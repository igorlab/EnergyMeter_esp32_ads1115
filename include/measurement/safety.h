/*
 * Safety subsystem (ТЗ §27, §32).
 *
 * Живе всередині вимірювального циклу, а не в окремій задачі: перевірка коштує
 * кілька порівнянь, а винесення її в задачу нижчого пріоритету означало б, що
 * аварія чекає планувальника. Пріоритет над UI забезпечено самим місцем виклику.
 *
 * Кожна умова має лічильник послідовних порушень: одиничний викид на 32 SPS не
 * повинен зупиняти тест, стійке перевищення — повинно.
 */
#pragma once

#include "measurement/types.h"

namespace meas {

struct SafetyLimits {
    float    v_max = 30.0f;      // В, overvoltage
    float    v_min = 0.0f;       // В, undervoltage (0 = не перевіряти)
    float    i_max = 5.5f;       // А, по модулю
    float    t_max = 60.0f;      // °C, перевіряється лише при FLAG_TEMP_VALID
    uint32_t timeout_s = 0;      // 0 = без обмеження часу тесту
    uint8_t  debounce = 3;       // скільки поспіль порушень до аварії
    // Напруга — величина повільна, і 3 відліки (94 мс на 32 SPS) для неї
    // замало: пусковий струм мотора чи дребезг контакту дають короткий
    // провал, який не є недорозрядом. Секунда — розумна межа.
    uint16_t uv_debounce = 32;
    float    no_battery_v = 0.5f;   // нижче цього при нулі струму = немає АКБ
    uint32_t max_adc_errors = 10;  // поспіль невдалих конверсій → ADC_ERROR
};

class Safety {
public:
    void setLimits(const SafetyLimits &l) { limits_ = l; }

    void reset();

    // Викликати кожен цикл. running — чи йде тест (таймаут рахується лише в тесті).
    // Повертає активну аварію; після спрацювання тримає її до reset().
    Fault check(const Measurement &m, float elapsed_s, bool running);

    Fault fault() const { return fault_; }

private:
    bool trip(uint16_t &counter, bool violated, uint16_t limit);

    SafetyLimits limits_;
    Fault   fault_ = Fault::NONE;
    uint16_t c_ov_ = 0, c_uv_ = 0, c_oc_ = 0, c_ot_ = 0, c_nb_ = 0;
    uint32_t c_adc_ = 0;
};

}  // namespace meas
