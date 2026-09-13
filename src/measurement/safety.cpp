#include "measurement/safety.h"

#include <math.h>

namespace meas {

const char *faultName(Fault f) {
    switch (f) {
        case Fault::NONE:            return "NONE";
        case Fault::OVERVOLTAGE:     return "OVERVOLTAGE";
        case Fault::UNDERVOLTAGE:    return "UNDERVOLTAGE";
        case Fault::OVERCURRENT:     return "OVERCURRENT";
        case Fault::OVERTEMPERATURE: return "OVERTEMPERATURE";
        case Fault::TIMEOUT:         return "TIMEOUT";
        case Fault::SENSOR_ERROR:    return "SENSOR_ERROR";
        case Fault::ADC_ERROR:       return "ADC_ERROR";
        case Fault::NO_BATTERY:      return "NO_BATTERY";
    }
    return "?";
}

void Safety::reset() {
    fault_ = Fault::NONE;
    c_ov_ = c_uv_ = c_oc_ = c_ot_ = c_nb_ = 0;
    c_adc_ = 0;
}

bool Safety::trip(uint16_t &counter, bool violated, uint16_t limit) {
    if (!violated) {
        counter = 0;
        return false;
    }
    if (counter < 0xFFFF) counter++;
    return counter >= limit;
}

Fault Safety::check(const Measurement &m, float elapsed_s, bool running) {
    // «Немає батареї» оцінюється ДО перевірки на залиплу аварію, і має право
    // перебити вже виставлений UNDERVOLTAGE.
    //
    // Причина з реального випадку: при від'єднанні напруга проходить через
    // смугу недорозряду по дорозі в нуль і проводить у ній близько секунди,
    // тому UNDERVOLTAGE залипає першим. Але справжня причина — обрив, і на
    // екрані має стояти вона. Інші аварії не перебиваються: обрив після
    // перевантаження не скасовує перевантаження.
    if (!(m.flags & FLAG_ADC_ERROR)) {
        const bool nothing = m.voltage < limits_.no_battery_v &&
                             fabsf(m.current) < 0.005f;
        if (trip(c_nb_, nothing, limits_.uv_debounce)) {
            if (fault_ == Fault::NONE || fault_ == Fault::UNDERVOLTAGE) {
                fault_ = Fault::NO_BATTERY;
            }
            return fault_;
        }
    }

    // Аварія залипає: зняти її може лише явний reset із state machine.
    if (fault_ != Fault::NONE) return fault_;

    // ADC не залежить від debounce за напругою — рахуємо власною серією.
    if (m.flags & FLAG_ADC_ERROR) {
        if (++c_adc_ >= limits_.max_adc_errors) {
            fault_ = Fault::ADC_ERROR;
            return fault_;
        }
        // Дані цього циклу недостовірні — решту перевірок пропускаємо.
        return Fault::NONE;
    }
    c_adc_ = 0;

    if (!isfinite(m.voltage) || !isfinite(m.current)) {
        fault_ = Fault::SENSOR_ERROR;
        return fault_;
    }

    if (trip(c_ov_, m.voltage > limits_.v_max, limits_.debounce)) {
        fault_ = Fault::OVERVOLTAGE;
    } else if (running && limits_.v_min > 0.0f &&
               trip(c_uv_, m.voltage < limits_.v_min, limits_.uv_debounce)) {
        // Тільки під час тесту: v_min — це cutoff із §31/§32, а не заборона
        // тримати на клемах розряджений елемент. Інакше пристрій без батареї
        // йшов би в аварію одразу після старту.

        fault_ = Fault::UNDERVOLTAGE;
    } else if (trip(c_oc_, fabsf(m.current) > limits_.i_max, limits_.debounce)) {
        fault_ = Fault::OVERCURRENT;
    } else if (trip(c_ot_, (m.flags & FLAG_TEMP_VALID) && m.temperature > limits_.t_max,
                    limits_.debounce)) {
        fault_ = Fault::OVERTEMPERATURE;
    } else if (running && limits_.timeout_s > 0 &&
               elapsed_s > static_cast<float>(limits_.timeout_s)) {
        fault_ = Fault::TIMEOUT;
    }

    return fault_;
}

}  // namespace meas
