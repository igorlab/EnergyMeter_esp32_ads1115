#include "measurement/integration.h"

namespace meas {

void Integrator::reset() {
    ah_ = wh_ = elapsed_s_ = 0.0;
    prev_i_ = prev_p_ = 0.0f;
    prev_t_us_ = 0;
    have_prev_ = false;
}

void Integrator::restore(float ah, float wh, float elapsed_s) {
    ah_ = ah;
    wh_ = wh;
    elapsed_s_ = elapsed_s;
    have_prev_ = false;   // перша точка після відновлення не утворює трапецію
}

bool Integrator::update(uint64_t timestamp_us, float current, float power, bool running) {
    if (!running) {
        // Пауза: точку запам'ятовуємо, площу не додаємо. Інакше після
        // поновлення перша трапеція охопила б усю паузу.
        prev_i_ = current;
        prev_p_ = power;
        prev_t_us_ = timestamp_us;
        have_prev_ = true;
        return false;
    }

    if (!have_prev_ || timestamp_us <= prev_t_us_) {
        prev_i_ = current;
        prev_p_ = power;
        prev_t_us_ = timestamp_us;
        have_prev_ = true;
        return false;
    }

    const uint64_t dt_us = timestamp_us - prev_t_us_;
    const double dt_s = static_cast<double>(dt_us) * 1e-6;

    // Трапеція: (x[n] + x[n-1]) / 2 * dt / 3600
    ah_ += static_cast<double>(current + prev_i_) * 0.5 * dt_s / 3600.0;
    wh_ += static_cast<double>(power   + prev_p_) * 0.5 * dt_s / 3600.0;
    elapsed_s_ += dt_s;

    prev_i_ = current;
    prev_p_ = power;
    prev_t_us_ = timestamp_us;

    return dt_us > max_dt_us_;
}

}  // namespace meas
