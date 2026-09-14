/*
 * Життєвий цикл тесту (ТЗ §73) — окремо від MeasurementCore і від UI.
 *
 * Раніше цей стан жив як десяток файлових глобальних змінних у
 * src/main_tester.cpp упереміш із SoC/внутрішнім опором/статусом SD.
 * TestSession збирає його в одне місце: старт/resume/abort, §50 збереження
 * стану в NVS, §29 автозавершення тесту, §27 реакція на аварію.
 *
 * MeasurementCore і далі володіє самим бітом "інтегрування триває" —
 * TestSession лише реагує на нього й вирішує, коли його виставити.
 *
 * Усі методи викликаються лише з loopTask (з console.poll()/web.pump()/
 * loop()), ніколи з задачі вимірювання чи з HTTP-задачі — тому без мьютекса.
 */
#pragma once

#include <stdint.h>

#include "measurement/core.h"
#include "storage/sd_logger.h"
#include "system/battery_profile.h"
#include "system/test_state.h"

namespace sys {

class TestSession {
public:
    // Один раз, з setup(). Зберігає лише вказівники: profile_ читає той
    // самий живий sys::BatteryProfile, який міняють команда `prof` і веб —
    // зміна профілю на льоту видно без повторного begin().
    void begin(meas::MeasurementCore &core, storage::SdLogger &sdlog,
               const BatteryProfile &profile);

    // §51: перевірити NVS на перерваний тест. Раз, із setup().
    void loadPending();
    bool hasPending() const { return has_pending_; }
    const TestState &pending() const { return pending_; }

    // Старт = НОВИЙ тест, лічильники обнуляються. Виняток один: після
    // resume() наступний старт продовжує відновлений тест (§51).
    void start();

    // §51 команда `resume`: відновлює лічильники, але не запускає тест —
    // рішення про `run` лишається за користувачем. false = нема що відновлювати.
    bool resume();
    // §51 команда `abort`: закидає перерваний тест і обнуляє лічильники.
    void abort();

    void setAutoStop(bool on) { auto_stop_ = on; }
    bool autoStop() const { return auto_stop_; }
    float socStart() const { return soc_start_; }

    // Викликати з loop() БЕЗУМОВНО, щоразу — не всередині 500-мс гейта
    // дисплея: зсув у часі затримав би §50 збереження й §29 автозавершення.
    // soc_pct/soc_rest — з окремого SoC-шару, тут лише читаються, щоб
    // зафіксувати, з якого відсотка почався тест.
    void poll(float soc_pct, float soc_rest);

    // Похідний стан, суто для читання (діагностика, майбутній /api/status).
    // Не бере участі в жодній гілці керування вище.
    enum class State : uint8_t { STOPPED, RUNNING, PENDING_RESUME };
    State state() const;

private:
    void saveState(bool active);
    // Було testEndReason(): §29 автозавершення вимагає ПАРИ умов — струм
    // зник І напруга каже, що саме сталося. Нульовий струм сам по собі
    // означає ще й обрив навантаження чи відійшлий контакт.
    const char *checkAutoStop(const meas::Measurement &m);

    meas::MeasurementCore *core_ = nullptr;
    storage::SdLogger     *sdlog_ = nullptr;
    const BatteryProfile  *profile_ = nullptr;

    TestState pending_{};
    bool  has_pending_ = false;
    bool  resume_armed_ = false;   // наступний старт продовжує, а не починає
    float soc_start_ = NAN;        // з чого починався поточний тест

    bool  prev_running_ = false;
    meas::Fault prev_fault_ = meas::Fault::NONE;
    uint32_t next_state_save_ = 0;

    // Автозупинка тесту (§29, §32). Вимикається командою `autostop off`.
    bool     auto_stop_ = true;
    uint32_t quiet_since_ = 0;     // коли струм зник
    float    v_at_quiet_ = NAN;    // напруга в ТОЙ момент, не після
    bool     warned_break_ = false;
    static constexpr uint32_t kQuietHoldMs = 60000;   // хвилина тиші до рішення
};

}  // namespace sys
