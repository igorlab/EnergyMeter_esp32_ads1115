#include "measurement/autorange.h"

namespace meas {

namespace {
constexpr float kUpFraction   = 0.90f;   // перехід вгору на 90% шкали
constexpr float kDownFraction = 0.80f;   // повернення вниз на 80% шкали нижчого
}  // namespace

void VoltageAutoRange::setDividerRatio(float divider_k) {
    if (divider_k < 1.0f) divider_k = 1.0f;
    divider_k_ = divider_k;

    for (uint8_t i = 0; i < kSteps; i++) {
        const float fs_battery = ads::pgaFullScale(steps_[i].pga) * divider_k_;
        steps_[i].up = (i + 1 < kSteps) ? fs_battery * kUpFraction : 1e9f;
        steps_[i].down = (i > 0)
            ? ads::pgaFullScale(steps_[i - 1].pga) * divider_k_ * kDownFraction
            : -1e9f;
    }
    // Для дільника 10.0 виходить рівно таблиця ТЗ §14:
    //   2.30 / 4.61 / 9.22 / 18.43 В вгору, 2.05 / 4.10 / 8.19 / 16.38 вниз.
}

void VoltageAutoRange::setOverride(int index) {
    if (index < 0) {
        override_ = -1;
        return;
    }
    if (index >= kSteps) index = kSteps - 1;
    override_ = static_cast<int8_t>(index);
    index_ = static_cast<uint8_t>(index);
}

bool VoltageAutoRange::update(float battery_v, bool saturated) {
    // При зафіксованому діапазоні не рухаємось навіть у saturation: сенс
    // фіксації в тому, щоб побачити сирий результат саме цього PGA.
    if (override_ >= 0) return false;

    const uint8_t before = index_;

    // Saturation обробляємо першою: показанню вже не можна вірити, тому
    // рішення приймаємо не за ним, а за самим фактом упирання в шкалу.
    if (saturated && index_ + 1 < kSteps) {
        index_++;
        return true;
    }

    const float v = fabsf(battery_v);
    if (v > steps_[index_].up && index_ + 1 < kSteps) {
        index_++;
    } else if (v < steps_[index_].down && index_ > 0) {
        index_--;
    }
    return index_ != before;
}

float VoltageAutoRange::maxBatteryVolts() const {
    return ads::pgaFullScale(steps_[kSteps - 1].pga) * divider_k_;
}

}  // namespace meas
