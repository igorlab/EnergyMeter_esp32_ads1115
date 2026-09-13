/*
 * Інтегрування Ah/Wh методом трапецій (ТЗ §21–§22).
 *
 * Δt береться з реальних міток часу (ТЗ §18): esp_timer_get_time() у мкс.
 * Припущення dt = 1 s не робиться ніде — при джитері планувальника воно дало б
 * систематичну похибку ємності, а не випадкову.
 */
#pragma once

#include <stdint.h>

namespace meas {

class Integrator {
public:
    void reset();

    // Викликати кожен цикл. running=false зупиняє накопичення, але зберігає
    // останню точку, щоб після поновлення трапеція не з'їхала на паузу.
    // Повертає true, якщо Δt перевищив max_dt_us (пропуск циклу).
    bool update(uint64_t timestamp_us, float current, float power, bool running);

    float Ah() const { return static_cast<float>(ah_); }
    float Wh() const { return static_cast<float>(wh_); }
    // Час під інтегруванням, с — не «час від старту», а сума врахованих Δt.
    float elapsedSeconds() const { return static_cast<float>(elapsed_s_); }

    void setMaxDt(uint32_t max_dt_us) { max_dt_us_ = max_dt_us; }

    // Відновлення після рестарту (ТЗ §51): продовжити з збереженого стану.
    void restore(float ah, float wh, float elapsed_s);

private:
    // Накопичувачі — double, і це не перестраховка. На float сума 115 200
    // приростів по 8.7 мкAh (година при 32 SPS) дає систематичний зсув
    // +0.11%: коли ah_ доростає до 1.0, приріст лежить біля межі роздільності
    // float, і округлення перестає компенсуватись. 0.11% з'їдає половину
    // бюджету точності струму (ТЗ §55). Ціна — програмний double раз на цикл.
    double   ah_ = 0.0;
    double   wh_ = 0.0;
    double   elapsed_s_ = 0.0;
    float    prev_i_ = 0.0f;
    float    prev_p_ = 0.0f;
    uint64_t prev_t_us_ = 0;
    bool     have_prev_ = false;
    uint32_t max_dt_us_ = 250000;   // 0.25 с — вчетверо більше за цикл 32 SPS
};

}  // namespace meas
