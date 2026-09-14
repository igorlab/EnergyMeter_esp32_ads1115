#include "system/test_session.h"

#include <Arduino.h>
#include <math.h>

namespace sys {

void TestSession::begin(meas::MeasurementCore &core, storage::SdLogger &sdlog,
                        const BatteryProfile &profile) {
    core_ = &core;
    sdlog_ = &sdlog;
    profile_ = &profile;
}

void TestSession::loadPending() {
    TestState st;
    if (loadTestState(st) && st.active) {
        pending_ = st;
        has_pending_ = true;
    }
}

// Старт = НОВИЙ тест: лічильники обнуляються. Інакше файл і тест
// розходяться — логер відкриває новий файл на кожному старті, а інтегратор
// продовжує рахувати, і колонка Ah у новому файлі починається не з нуля.
// Саме так у TEST_0022 з'явилися успадковані 20.6 мАг від TEST_0021.
//
// Виняток один: після resume() наступний старт продовжує відновлений тест.
void TestSession::start() {
    if (resume_armed_) {
        resume_armed_ = false;
    } else {
        core_->setIntegrating(false);
        core_->resetIntegration();
    }
    core_->setIntegrating(true);
}

// §51. Відновлюємо стан, але тест НЕ запускаємо — рішення за користувачем.
bool TestSession::resume() {
    if (!has_pending_) return false;

    meas::TestStats st{};
    st.v_start = pending_.v_start; st.v_min = pending_.v_min;
    st.v_max = pending_.v_max;     st.i_max = pending_.i_max;
    st.t_max = pending_.t_max;     st.samples = pending_.samples;
    core_->restoreStats(st);
    core_->restoreIntegration(pending_.ah, pending_.wh, pending_.elapsed_s);
    soc_start_ = pending_.soc_start;
    has_pending_ = false;
    resume_armed_ = true;   // наступний start() не обнулить відновлене
    return true;
}

void TestSession::abort() {
    clearTestState();
    has_pending_ = false;
    resume_armed_ = false;   // інакше наступний start() мовчки пропустить
                             // свій resetIntegration(), поклавшись на збіг,
                             // що abort() і так щойно обнулив лічильники
    core_->resetIntegration();
}

TestSession::State TestSession::state() const {
    if (has_pending_) return State::PENDING_RESUME;
    return prev_running_ ? State::RUNNING : State::STOPPED;
}

// Завершення тесту визначається ПАРОЮ умов: струм зник І напруга каже, що
// саме сталося. Нульовий струм сам по собі означає ще й обрив навантаження
// чи відійшлий контакт, і зупиняти по ньому означало б убити багатогодинний
// розряд від секундного дребезгу.
//
// Повертає причину або nullptr, якщо зупиняти нема за чим.
const char *TestSession::checkAutoStop(const meas::Measurement &m) {
    meas::TestStats st;
    core_->stats(st);

    // «Струм зник» — це 2% від піку цього тесту, але не менше 5 мА.
    // Абсолютний поріг не годиться: у тесті на 60 мА і в тесті на 5 А
    // «майже нуль» це різні величини.
    const float floor_i = fmaxf(0.02f * st.i_max, 0.005f);
    if (fabsf(m.current) > floor_i) { quiet_since_ = 0; warned_break_ = false; return nullptr; }

    const uint32_t now = millis();
    if (!quiet_since_) {
        quiet_since_ = now;
        // Напруга саме в момент зникнення струму, а не через хвилину. Без
        // цього навантаження з власним відсіченням (ESP32-C3 гасне близько
        // 3.0 В) виглядало б як обрив: струм зник, а напруга без навантаження
        // піднялась до 3.6 В і опинилась посередині діапазону.
        v_at_quiet_ = m.voltage;
        return nullptr;
    }
    if (now - quiet_since_ < kQuietHoldMs) return nullptr;

    const float full = packFull(*profile_);
    const float cut  = packCutoff(*profile_);
    const float band = 0.10f * profile_->cells;

    const float v = isnan(v_at_quiet_) ? m.voltage : v_at_quiet_;
    if (v >= full - band) return "CHARGE_COMPLETE";
    // Ширше вікно, ніж для заряду: навантаження може згаснути саме, не
    // дотягнувши до порогу плати захисту, і це теж завершення розряду.
    if (v <= cut + 5.0f * band) return "DISCHARGE_END";

    // Напруга посередині діапазону: струму немає, а батарея не повна й не
    // розряджена. Це обрив, а не завершення — попереджаємо й не зупиняємо.
    if (!warned_break_) {
        warned_break_ = true;
        Serial.printf("!!! струму немає, а напруга при його зникненні була "
                      "%.3f В посередині діапазону — схоже на обрив, "
                      "тест не зупиняю\n", v);
    }
    return nullptr;
}

// §50. Пише loopTask, а не ядро: запис у NVS блокує на кілька мілісекунд,
// що для §71 неприйнятно всередині вимірювального циклу.
void TestSession::saveState(bool active) {
    meas::Measurement m;
    core_->latest(m);
    meas::TestStats st;
    core_->stats(st);

    TestState s{};
    s.test_id   = sdlog_->testId();
    s.active    = active ? 1 : 0;
    s.ah        = m.Ah;
    s.wh        = m.Wh;
    s.elapsed_s = core_->elapsedSeconds();
    s.v_start   = st.v_start;
    s.v_min     = st.v_min;
    s.v_max     = st.v_max;
    s.i_max     = st.i_max;
    s.t_max     = st.t_max;
    s.samples   = st.samples;
    s.soc_start = soc_start_;
    saveTestState(s);
}

void TestSession::poll(float soc_pct, float soc_rest) {
    // --- §50: збереження стану тесту ---
    const bool running = core_->integrating();
    const meas::Fault fault = core_->fault();

    if (running && !prev_running_) {
        // Старт: фіксуємо, з якого відсотка почали. Якщо спокою не бачили,
        // беремо оцінку прямо зараз — вона гірша, але краще за нічого.
        soc_start_ = isnan(soc_rest) ? soc_pct : soc_rest;
        saveState(true);
        next_state_save_ = millis() + kSaveIntervalMs;
    } else if (!running && prev_running_) {
        saveState(false);                     // §50: обов'язково при STOP
    } else if (running && millis() >= next_state_save_) {
        next_state_save_ = millis() + kSaveIntervalMs;
        saveState(true);
    }

    // §29: тест має завершуватись сам, а не писати нулі годинами.
    if (auto_stop_ && running) {
        meas::Measurement m;
        if (core_->latest(m)) {
            const char *reason = checkAutoStop(m);
            if (reason) {
                sdlog_->setStopReason(reason);
                core_->setIntegrating(false);
                saveState(false);
                Serial.printf(">>> тест завершено: %s  (%.4f Ah, %.4f Wh, %.0f с)\n",
                              reason, m.Ah, m.Wh, core_->elapsedSeconds());
            }
        }
    }

    if (fault != prev_fault_ && fault != meas::Fault::NONE) {
        // §27: STOP TEST, MARK FAULT, SAVE STATE. Досі виконувались лише
        // другий і третій пункти, і це коштувало реальних даних: після
        // відрізання захисту на 6.86 годині прилад ще сім годин інтегрував
        // залишковий струм 0.7 мА і намотав 4.8 мАг з нічого.
        //
        // Зупинка тут, а не в ядрі: ядро не володіє станом тесту (§73),
        // а loopTask — саме та задача, що ним керує.
        // Причина зупинки — назва аварії (§58), а не дефолтне "stopped".
        sdlog_->setStopReason(meas::faultName(fault));
        core_->setIntegrating(false);
        // Свіже значення, а не `running` знятий на початку цього poll():
        // якщо автозупинка вище в цьому ж проході вже зупинила тест,
        // `running` лишився б застарілим `true` і переписав би щойно
        // збережений коректний active=false на помилковий active=true.
        // У звичайному випадку (без такого збігу) це той самий результат.
        saveState(core_->integrating());
        Serial.printf("!!! АВАРІЯ: %s — тест зупинено\n", meas::faultName(fault));
    }
    prev_running_ = running;
    prev_fault_ = fault;
}

}  // namespace sys
