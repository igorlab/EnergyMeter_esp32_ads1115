/*
 * Мінімальний драйвер ADS1115 — тільки те, що потрібно вимірювальному ядру.
 *
 * Навмисно без Adafruit_ADS1X15: там конверсія запускається і читається
 * всередині одного блокуючого виклику з фіксованою затримкою, а ТЗ §16 вимагає
 * явного контролю над тим, яка конверсія вважається дійсною після зміни PGA.
 * Тут запуск і читання розділені, а очікування віддає процесор планувальнику.
 *
 * Регістри (datasheet Table 6):
 *   0x00 conversion, 0x01 config, 0x02 lo_thresh, 0x03 hi_thresh
 */
#pragma once

#include <Arduino.h>
#include <Wire.h>

namespace ads {

// MUX[14:12] конфігураційного регістра.
enum class Mux : uint16_t {
    DIFF_0_1 = 0,   // струм: AIN0 - AIN1 (ТЗ §6)
    DIFF_0_3 = 1,
    DIFF_1_3 = 2,
    DIFF_2_3 = 3,
    SINGLE_0 = 4,
    SINGLE_1 = 5,
    SINGLE_2 = 6,   // напруга: AIN2 після дільника
    SINGLE_3 = 7,   // резерв
};

// PGA[11:9]. Назва — реальна межа шкали, а не «gain 16»: множник у ТЗ §10/§14
// походить з іменування Adafruit і легко читається навпаки.
enum class Pga : uint8_t {
    FS_6144 = 0,    // ±6.144 V — вище живлення ADS1115, практичного сенсу немає
    FS_4096 = 1,    // ±4.096 V
    FS_2048 = 2,    // ±2.048 V
    FS_1024 = 3,    // ±1.024 V
    FS_0512 = 4,    // ±0.512 V
    FS_0256 = 5,    // ±0.256 V — «GAIN 16», робочий режим шунта
};

// DR[7:5].
enum class Rate : uint8_t {
    SPS_8 = 0, SPS_16, SPS_32, SPS_64, SPS_128, SPS_250, SPS_475, SPS_860,
};

// Повна шкала в вольтах для заданого PGA.
float pgaFullScale(Pga p);

// Ціна молодшого розряду, В/відлік. 16 біт зі знаком → FS/32768.
inline float pgaLsb(Pga p) { return pgaFullScale(p) / 32768.0f; }

// Номінальна тривалість конверсії, мкс (без запасу).
uint32_t rateConversionUs(Rate r);

class Ads1115 {
public:
    // wire має бути вже налаштований (begin + setClock).
    bool begin(TwoWire &wire, uint8_t addr = 0x48);

    // Запускає одиничну конверсію. Попередній результат після цього недійсний.
    bool startSingle(Mux mux, Pga pga, Rate rate);

    // Чекає завершення конверсії: спить очікувану тривалість, далі опитує біт OS.
    // Повертає false за таймаутом або помилкою шини.
    bool waitReady(Rate rate, uint32_t extra_timeout_us = 20000);

    // Читає регістр конверсії. Валідний лише після успішного waitReady().
    bool readRaw(int16_t &out);

    // Зручна обгортка: старт + очікування + читання.
    bool convert(Mux mux, Pga pga, Rate rate, int16_t &out);


private:
    bool write16(uint8_t reg, uint16_t value);
    bool read16(uint8_t reg, uint16_t &value);

    TwoWire *wire_ = nullptr;
    uint8_t  addr_ = 0x48;
};

}  // namespace ads
