#include "measurement/core.h"

#include <Wire.h>
#include <esp_timer.h>
#include <math.h>

namespace meas {

namespace {
// Ознака упирання в шкалу: 32000 з 32768 — це 97.7% діапазону.
constexpr int16_t kSaturationCount = 32000;
constexpr ads::Pga kCurrentPga = ads::Pga::FS_0256;   // GAIN 16, ТЗ §10
// Скільки відліків поспіль напруга має простояти нижче cutoff, щоб це
// вважалось досягненням cutoff, а не провалом. 32 при 32 SPS — секунда.
constexpr uint16_t kCutoffHits = 32;
}  // namespace

float averageUncalibrated(MeasurementCore &core, bool voltage, uint16_t samples) {
    double acc = 0;
    uint16_t taken = 0;
    uint32_t last_seq = 0;
    while (taken < samples) {
        Measurement m;
        Diagnostics d;
        core.latest(m);
        core.diagnostics(d);
        // Чекаємо саме зміни sequence, а не просто паузи: інакше при
        // повільному циклі та сама вибірка потрапила б у середнє кілька разів.
        if (m.sequence != last_seq) {
            last_seq = m.sequence;
            acc += voltage ? d.v_uncal : d.i_uncal;
            taken++;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return static_cast<float>(acc / taken);
}

bool MeasurementCore::begin(const CoreConfig &cfg, const Calibration &cal) {
    cfg_ = cfg;
    cal_ = cal;

    range_.setDividerRatio(cal_.divider_k);
    integrator_.reset();
    integrator_.setMaxDt(cfg_.sample_period_us * 4);
    safety_.reset();
    resetStats();
    diag_.simulated = cfg_.simulate;
    if (cfg_.average < 1) cfg_.average = 1;
    if (cfg_.average > kMaxAverage) cfg_.average = kMaxAverage;
    diag_.average = cfg_.average;

    if (!cfg_.simulate) {
        if (!Wire.begin(cfg_.i2c_sda, cfg_.i2c_scl, cfg_.i2c_hz)) return false;
        // Якщо ADS1115 не відповідає, задача все одно стартує. Мовчазний АЦП —
        // це Fault::ADC_ERROR за §32/§58, і обробляти його має safety, а не
        // відмова піднятись: інакше пристрій не покаже користувачеві нічого,
        // тоді як аварія має бути видимою.
        ads_.begin(Wire, cfg_.ads_addr);
    }

    const BaseType_t ok = xTaskCreatePinnedToCore(
        &MeasurementCore::taskEntry, "meas", cfg_.task_stack, this,
        cfg_.task_priority, &task_, cfg_.task_core);

    return ok == pdPASS;
}

void MeasurementCore::taskEntry(void *arg) {
    static_cast<MeasurementCore *>(arg)->run();
}

void MeasurementCore::run() {
    TickType_t last_wake = xTaskGetTickCount();

    TickType_t period = pdMS_TO_TICKS(cfg_.sample_period_us / 1000);
    if (period < 1) period = 1;

    for (;;) {
        const uint64_t t0 = esp_timer_get_time();
        if (cfg_.simulate) simulateOnce();
        else               sampleOnce();
        // Обхід входів — після робочого циклу, щоб не рвати вимірювання.
        if (scan_request_ && !cfg_.simulate) {
            scan_request_ = false;
            float v[4];
            bool ok = true;
            for (uint8_t ch = 0; ch < 4; ch++) {
                int16_t raw = 0;
                const auto mux = static_cast<ads::Mux>(
                    static_cast<uint8_t>(ads::Mux::SINGLE_0) + ch);
                if (ads_.convert(mux, ads::Pga::FS_4096, cfg_.rate, raw)) {
                    v[ch] = raw * ads::pgaLsb(ads::Pga::FS_4096);
                } else {
                    ok = false;
                    v[ch] = NAN;
                }
            }
            taskENTER_CRITICAL(&lock_);
            for (uint8_t ch = 0; ch < 4; ch++) scan_[ch] = v[ch];
            taskEXIT_CRITICAL(&lock_);
            scan_valid_ = ok;
        }

        const uint32_t cycle = static_cast<uint32_t>(esp_timer_get_time() - t0);

        taskENTER_CRITICAL(&lock_);
        diag_.cycle_us = cycle;
        if (cycle > diag_.cycle_max_us) diag_.cycle_max_us = cycle;
        taskEXIT_CRITICAL(&lock_);

        // Тік планувальника — 1 мс, тож період витримується з точністю до тіка.
        // Це не проблема: Δt для інтегрування береться з реальних міток часу
        // (ТЗ §18), а не з припущення про рівномірність циклу.
        vTaskDelayUntil(&last_wake, period);
    }
}

void MeasurementCore::sampleOnce() {
    Calibration cal;
    taskENTER_CRITICAL(&lock_);
    cal = cal_;
    taskEXIT_CRITICAL(&lock_);

    uint8_t flags = 0;
    uint32_t adc_errors = 0;
    uint32_t range_changes = 0;

    // --- струм: AIN0-AIN1, фіксований ±0.256 В ---
    int16_t i_raw = 0;
    if (!ads_.convert(ads::Mux::DIFF_0_1, kCurrentPga, cfg_.rate, i_raw)) {
        flags |= FLAG_ADC_ERROR;
        adc_errors++;
    }
    if (abs(i_raw) > kSaturationCount) flags |= FLAG_I_SATURATED;

    // --- напруга: AIN2 з auto-range ---
    ads::Pga v_pga = range_.pga();
    int16_t  v_raw = 0;
    if (!ads_.convert(ads::Mux::SINGLE_2, v_pga, cfg_.rate, v_raw)) {
        flags |= FLAG_ADC_ERROR;
        adc_errors++;
    }
    float v_uncal = static_cast<float>(v_raw) * ads::pgaLsb(v_pga) * cal.divider_k;

    if (range_.update(v_uncal, abs(v_raw) > kSaturationCount)) {
        // ТЗ §16: після зміни PGA попередній результат недійсний — і його
        // не можна ні використати, ні "підправити". Тільки нова конверсія.
        flags |= FLAG_V_RANGE_CHG;
        range_changes++;
        v_pga = range_.pga();
        if (!ads_.convert(ads::Mux::SINGLE_2, v_pga, cfg_.rate, v_raw)) {
            flags |= FLAG_ADC_ERROR;
            adc_errors++;
        }
        v_uncal = static_cast<float>(v_raw) * ads::pgaLsb(v_pga) * cal.divider_k;
    }
    if (abs(v_raw) > kSaturationCount) flags |= FLAG_V_SATURATED;

    // Мітка часу — одна на record, після обох конверсій. Канали розділені
    // приблизно на тривалість конверсії (~8 мс на 128 SPS); на робочих
    // швидкостях зміни струму це дає похибку P нижче за похибку калібрування.
    const uint64_t now_us = esp_timer_get_time();

    // --- фізичні величини ---
    float i_uncal = static_cast<float>(i_raw) * ads::pgaLsb(kCurrentPga) / cal.shunt_ohms;
    if (cal.invert_current) i_uncal = -i_uncal;

    const float voltage = cal.v_gain * v_uncal + cal.v_offset;
    const float current = cal.i_gain * i_uncal + cal.i_offset;

    taskENTER_CRITICAL(&lock_);
    diag_.v_uncal = v_uncal;
    diag_.i_uncal = i_uncal;
    diag_.v_raw = v_raw;
    diag_.i_raw = i_raw;
    diag_.v_pga = range_.index();
    diag_.v_fs  = ads::pgaFullScale(v_pga);
    diag_.adc_errors += adc_errors;
    diag_.range_changes += range_changes;
    taskEXIT_CRITICAL(&lock_);

    finishSample(now_us, voltage, current, NAN, flags);
}

// Синтетичний розряд Li-ion 1S. Жодного звернення до I2C: коли ADS1115 ще не
// припаяний, UI треба на чомусь піднімати, а «нулі замість даних» дають
// неправдоподібну картинку й ховають помилки розкладки.
void MeasurementCore::simulateOnce() {
    const uint64_t now_us = esp_timer_get_time();

    // Годинник симуляції — час під інтегруванням, а не uptime: до команди
    // "run" батарея просто лежить із напругою спокою.
    const float t = integrator_.elapsedSeconds();
    float frac = t / static_cast<float>(cfg_.simulate_seconds);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    // 4.20 -> 3.00 В: швидкий початковий спад, плато, обвал у кінці.
    const float x = 1.0f - frac;
    const float noise = (static_cast<float>(esp_random() & 0xFF) - 128.0f) * 8e-6f;
    const float voltage = 3.00f + 1.20f * (0.25f * x + 0.75f * powf(x, 0.35f)) + noise;

    // Розряд — додатній струм: саме таку полярність дає low-side шунт §11.
    const bool running = integrating_ && frac < 1.0f;
    const float current = running
        ? cfg_.simulate_current + noise * 3.0f
        : 0.0f;

    const float temperature = 24.0f + 6.0f * frac;

    taskENTER_CRITICAL(&lock_);
    diag_.v_uncal = voltage;
    diag_.i_uncal = current;
    diag_.v_raw = static_cast<int16_t>(voltage / cal_.divider_k / ads::pgaLsb(range_.pga()));
    diag_.i_raw = static_cast<int16_t>(current * cal_.shunt_ohms / ads::pgaLsb(kCurrentPga));
    diag_.v_pga = range_.index();
    diag_.v_fs  = ads::pgaFullScale(range_.pga());
    taskEXIT_CRITICAL(&lock_);

    range_.update(voltage, false);

    finishSample(now_us, voltage, current, temperature, FLAG_TEMP_VALID);
}

// Спільний хвіст обох джерел: інтегрування, статистика тесту, safety, публікація.
void MeasurementCore::finishSample(uint64_t now_us, float voltage, float current,
                                   float temperature, uint8_t flags) {
    // Ковзне середнє (§23). Стоїть до розрахунку потужності й до інтегратора:
    // P = V*I з двох незалежно шумних величин шумить сильніше за кожну з них,
    // а множення відфільтрованих дає чесніший результат. Сирі значення нікуди
    // не діваються — вони в Diagnostics, і калібрування працює з ними.
    const uint8_t n = (cfg_.average > kMaxAverage) ? kMaxAverage
                    : (cfg_.average < 1 ? 1 : cfg_.average);
    if (n > 1 && !(flags & FLAG_ADC_ERROR)) {
        v_hist_[hist_head_] = voltage;
        i_hist_[hist_head_] = current;
        hist_head_ = static_cast<uint8_t>((hist_head_ + 1) % n);
        if (hist_count_ < n) hist_count_++;

        float sv = 0.0f, si = 0.0f;
        for (uint8_t k = 0; k < hist_count_; k++) { sv += v_hist_[k]; si += i_hist_[k]; }
        voltage = sv / hist_count_;
        current = si / hist_count_;
    }

    // Дільник міряє батарею разом із падінням на шунті: його низ сидить на
    // спільній землі, а мінус батареї — по той бік шунта (§11). При 5 А це
    // 50 мВ, у десять разів більше за ціль §55.
    //
    // Знак виведений для конвенції «додатний струм = розряд»: тоді при
    // розряді мінус батареї нижче землі, виміряне занижене, і додавання
    // однаково правильне для обох напрямків.
    // Diagnostics::v_uncal лишається без компенсації — щоб її можна було
    // перевірити прямим порівнянням.
    float shunt_ohms;
    taskENTER_CRITICAL(&lock_);
    shunt_ohms = cal_.shunt_ohms;
    taskEXIT_CRITICAL(&lock_);
    voltage += current * shunt_ohms;

    const float power = voltage * current;
    if (current < -cfg_.current_deadband) flags |= FLAG_CHARGE;

    const bool integrate = integrating_ && !(flags & FLAG_ADC_ERROR);
    if (integrate) flags |= FLAG_INTEGRATING;
    if (integrator_.update(now_us, current, power, integrate)) flags |= FLAG_TIME_GAP;

    // --- статистика тесту (ТЗ §57) ---
    if (integrate) {
        TestStats st = stats_;   // пише лише ця задача, читання без блокування безпечне
        if (st.samples == 0) {
            st.v_start = voltage;
            st.v_min = st.v_max = voltage;
            st.i_max = fabsf(current);
            st.t_max = temperature;
        } else {
            if (voltage < st.v_min) st.v_min = voltage;
            if (voltage > st.v_max) st.v_max = voltage;
            if (fabsf(current) > st.i_max) st.i_max = fabsf(current);
            if ((flags & FLAG_TEMP_VALID) &&
                (isnan(st.t_max) || temperature > st.t_max)) st.t_max = temperature;
        }
        st.samples++;

        // Перший перетин cutoff фіксується один раз і більше не рухається.
        //
        // Двi умови, обидві з реального випадку: батарею від'єднали посеред
        // тесту, напруга пішла в нуль через 3.00 В, і позначка залипла на
        // 0.8 мАг. Тому потрібен і струм (без нього це не розряд, а обрив),
        // і витримка — короткий провал не є досягненням cutoff.
        // Струм має бути саме РОЗРЯДНИЙ, тобто додатний. Cutoff — поняття
        // розряду; під час заряду його не існує. Без цієї умови глибоко
        // розряджений елемент ставив позначку на першій секунді заряду, коли
        // напруга ще нижче 3.00 В, а струм уже тече — так у тесті #20
        // з'явився ah_cutoff = -0.00003.
        const bool below = cutoff_v_ > 0.0f && voltage <= cutoff_v_ &&
                           current > cfg_.current_deadband;
        cutoff_hits_ = below ? static_cast<uint16_t>(cutoff_hits_ + 1) : 0;
        if (isnan(st.ah_cutoff) && cutoff_hits_ >= kCutoffHits) {
            st.ah_cutoff = integrator_.Ah();
            st.wh_cutoff = integrator_.Wh();
            st.s_cutoff  = integrator_.elapsedSeconds();
        }

        taskENTER_CRITICAL(&lock_);
        stats_ = st;
        taskEXIT_CRITICAL(&lock_);
    }

    Measurement m{};
    m.timestamp_us = now_us;
    m.voltage = voltage;
    m.current = current;
    m.power   = power;
    m.Ah      = integrator_.Ah();
    m.Wh      = integrator_.Wh();
    m.temperature = temperature;
    m.sequence = ++sequence_;
    m.flags = flags;

    // Safety до публікації: споживач не побачить record без вердикту.
    safety_.check(m, integrator_.elapsedSeconds(), integrating_);

    taskENTER_CRITICAL(&lock_);
    latest_ = m;
    have_sample_ = true;
    taskEXIT_CRITICAL(&lock_);

    publish(m);
}

void MeasurementCore::publish(const Measurement &m) {
    QueueHandle_t sink = sink_;
    if (!sink) return;

    // Проріджування до темпу логування (§43). Робиться тут, а не в логері,
    // щоб черга на 64 record не переповнювалась від 32 SPS і лічильник
    // dropped показував справжні втрати, а не заплановане проріджування.
    if (cfg_.sink_interval_us) {
        if (m.timestamp_us - last_sink_us_ < cfg_.sink_interval_us) return;
        last_sink_us_ = m.timestamp_us;
    }

    // Нуль таймауту — принципово: черга споживача не має права затримати ядро.
    if (xQueueSend(sink, &m, 0) != pdTRUE) {
        taskENTER_CRITICAL(&lock_);
        diag_.dropped++;
        taskEXIT_CRITICAL(&lock_);
    }
}

bool MeasurementCore::latest(Measurement &out) const {
    taskENTER_CRITICAL(&lock_);
    const bool ok = have_sample_;
    out = latest_;
    taskEXIT_CRITICAL(&lock_);
    return ok;
}

void MeasurementCore::diagnostics(Diagnostics &out) const {
    taskENTER_CRITICAL(&lock_);
    out = diag_;
    taskEXIT_CRITICAL(&lock_);
}

void MeasurementCore::stats(TestStats &out) const {
    taskENTER_CRITICAL(&lock_);
    out = stats_;
    taskEXIT_CRITICAL(&lock_);
}

bool MeasurementCore::pinScan(float out[4]) const {
    taskENTER_CRITICAL(&lock_);
    for (uint8_t ch = 0; ch < 4; ch++) out[ch] = scan_[ch];
    taskEXIT_CRITICAL(&lock_);
    return scan_valid_;
}

void MeasurementCore::restoreStats(const TestStats &st) {
    taskENTER_CRITICAL(&lock_);
    stats_ = st;
    taskEXIT_CRITICAL(&lock_);
}

void MeasurementCore::resetStats() {
    taskENTER_CRITICAL(&lock_);
    stats_ = TestStats{NAN, NAN, NAN, 0.0f, NAN, 0, NAN, NAN, NAN};
    taskEXIT_CRITICAL(&lock_);
}

// Симуляція вмикається тільки явно. Автоматичного переходу в неї немає й не
// повинно бути: підміна відмови АЦП синтетикою робить непрацюючий прилад
// схожим на працюючий.
void MeasurementCore::setAverage(uint8_t n) {
    if (n < 1) n = 1;
    if (n > kMaxAverage) n = kMaxAverage;
    taskENTER_CRITICAL(&lock_);
    cfg_.average = n;
    diag_.average = n;
    // Історію скидаємо: залишки старого вікна дали б стрибок показань.
    hist_head_ = 0;
    hist_count_ = 0;
    taskEXIT_CRITICAL(&lock_);
}

void MeasurementCore::setSimulate(bool on) {
    cfg_.simulate = on;
    taskENTER_CRITICAL(&lock_);
    diag_.simulated = on;
    taskEXIT_CRITICAL(&lock_);
    // Аварія знімається в обидві сторони: вмикаючи симуляцію, ми більше не
    // читаємо АЦП, тож старий ADC_ERROR уже ні про що не говорить.
    safety_.reset();
}

void MeasurementCore::setIntegrating(bool on) { integrating_ = on; }

void MeasurementCore::resetIntegration() {
    // Викликається з іншої задачі. Порядок важливий: спершу зупинити
    // накопичення, потім обнулити — інакше цикл встигне додати трапецію
    // між скиданням і зупинкою.
    const bool was = integrating_;
    integrating_ = false;
    vTaskDelay(pdMS_TO_TICKS(cfg_.sample_period_us / 1000 + 2));
    integrator_.reset();
    resetStats();
    integrating_ = was;
}

void MeasurementCore::restoreIntegration(float ah, float wh, float elapsed_s) {
    const bool was = integrating_;
    integrating_ = false;
    vTaskDelay(pdMS_TO_TICKS(cfg_.sample_period_us / 1000 + 2));
    integrator_.restore(ah, wh, elapsed_s);
    integrating_ = was;
}

void MeasurementCore::setCalibration(const Calibration &cal) {
    taskENTER_CRITICAL(&lock_);
    cal_ = cal;
    taskEXIT_CRITICAL(&lock_);
    range_.setDividerRatio(cal.divider_k);
}

Calibration MeasurementCore::calibration() const {
    taskENTER_CRITICAL(&lock_);
    const Calibration c = cal_;
    taskEXIT_CRITICAL(&lock_);
    return c;
}

// Викликається з іншої задачі. index_ усередині — один байт, розірватись
// на запису не може, а зайвий цикл на старому діапазоні нічого не псує.
void MeasurementCore::setVoltageRange(int index) { range_.setOverride(index); }

void MeasurementCore::setSafetyLimits(const SafetyLimits &l) { safety_.setLimits(l); }
Fault MeasurementCore::fault() const { return safety_.fault(); }
void MeasurementCore::clearFault() { safety_.reset(); }
float MeasurementCore::elapsedSeconds() const { return integrator_.elapsedSeconds(); }

float MeasurementCore::usableMaxVolts() const {
    taskENTER_CRITICAL(&lock_);
    const float k = cal_.divider_k;
    taskEXIT_CRITICAL(&lock_);
    const float by_pga = range_.maxBatteryVolts();
    const float by_vdd = cfg_.adc_vdd * k;
    return (by_vdd < by_pga) ? by_vdd : by_pga;
}

}  // namespace meas
