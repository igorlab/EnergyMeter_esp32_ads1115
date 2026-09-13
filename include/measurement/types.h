/*
 * Єдиний формат вимірювальних даних (ТЗ §19).
 *
 * Це та сама структура, яку публікує Measurement Core і яку споживають
 * TFT / Web / SD. Жоден споживач не рахує Ah/Wh самостійно (ТЗ §52).
 */
#pragma once

#include <stdint.h>

namespace meas {

// Прапорці в Measurement::flags. Рівно 8 — весь байт зайнятий.
enum : uint8_t {
    FLAG_NONE          = 0,
    FLAG_INTEGRATING   = 1 << 0,   // інтегрування Ah/Wh активне
    FLAG_CHARGE        = 1 << 1,   // струм у напрямку заряду; без нього — розряд
    FLAG_V_RANGE_CHG   = 1 << 2,   // у цьому циклі змінився PGA каналу напруги
    FLAG_V_SATURATED   = 1 << 3,   // ADC каналу напруги близько до saturation
    FLAG_I_SATURATED   = 1 << 4,   // ADC каналу струму близько до saturation
    FLAG_ADC_ERROR     = 1 << 5,   // конверсія не завершилась / помилка I2C
    FLAG_TIME_GAP      = 1 << 6,   // Δt перевищив очікуваний — цикл затримався
    FLAG_TEMP_VALID    = 1 << 7,   // поле temperature містить реальні дані
};

// ТЗ §19 — структура задана дослівно, порядок полів не міняти.
struct Measurement {
    uint64_t timestamp_us;
    float    voltage;
    float    current;
    float    power;
    float    Ah;
    float    Wh;
    float    temperature;
    uint32_t sequence;
    uint8_t  flags;
};

// Службові дані для калібрування, приймальних тестів (ТЗ §69) і stress test
// (ТЗ §70). Не є частиною measurement record і не пишуться в лог.
struct Diagnostics {
    float    v_uncal;        // V після дільника, ДО gain/offset — для калібрування
    float    i_uncal;        // A після шунта,  ДО gain/offset
    int16_t  v_raw;          // сирі відліки ADC
    int16_t  i_raw;
    uint8_t  v_pga;          // поточний індекс PGA каналу напруги (0 = найчутливіший)
    float    v_fs;           // повна шкала цього діапазону на вході ADC, В
    uint32_t adc_errors;     // накопичені помилки конверсії
    uint32_t range_changes;  // скільки разів перемикався PGA
    uint32_t dropped;        // records, які не влізли в sink-чергу
    uint32_t cycle_us;       // тривалість останнього циклу вимірювання
    uint32_t cycle_max_us;   // найдовший цикл від старту — головна метрика §70
    bool     simulated;      // дані синтетичні, ADS1115 не опитується
    uint8_t  average;        // ширина вікна усереднення, 1 = вимкнено
};

// Похідні за тест величини (ТЗ §57). Веде Measurement Core, бо тільки він
// бачить кожен sample; UI їх лише читає й не перераховує (ТЗ §52).
struct TestStats {
    float    v_start;    // напруга на момент старту інтегрування
    float    v_min;
    float    v_max;
    float    i_max;      // за модулем
    float    t_max;
    uint32_t samples;    // 0 = тест ще не починався

    // Позначка робочого cutoff. Тест на ньому НЕ зупиняється — розряд рве
    // плата захисту на своєму порозі. Але ємність до cutoff треба знати
    // окремо: паспорти елементів дають її саме до 3.0 В, а не до 2.4.
    float    ah_cutoff;  // NAN, поки напруга не спускалась до cutoff
    float    wh_cutoff;
    float    s_cutoff;
};

// Причини зупинки тесту (ТЗ §58). FAULT-и safety мапляться сюди 1:1.
enum class Fault : uint8_t {
    NONE = 0,
    OVERVOLTAGE,
    UNDERVOLTAGE,
    OVERCURRENT,
    OVERTEMPERATURE,
    TIMEOUT,
    SENSOR_ERROR,
    ADC_ERROR,
    // Розширення переліку §58. Нуль вольт при нулі струму — це однозначно
    // від'єднана батарея, і називати це UNDERVOLTAGE означає збивати з
    // пантелику: недорозряд і відсутність елемента лікуються по-різному.
    NO_BATTERY,
};

const char *faultName(Fault f);

}  // namespace meas
