/*
 * Етап 1 (ТЗ §75): ESP32 + ADS1115 + шунт + дільник. Ні дисплея, ні SD, ні Wi-Fi.
 *
 * Мета цієї прошивки — довести характеристики вимірювального ядра до того, як
 * навколо нього з'явиться периферія: точність V та I, роботу auto-GAIN,
 * калібрування, Ah та Wh (ТЗ §69). Прошивка етапу 2 (`-e tester`) додає до
 * тих самих модулів TFT; числа мають лишитись ті самі — це критерій §70.
 *
 * Збірка:  pio run -e measure -t upload && pio device monitor
 * Консоль: 115200, команда "?" друкує довідку.
 *
 * Розводка:
 *   ADS1115 VDD -> 3V3      SDA -> GPIO21     ADDR -> GND (адреса 0x48)
 *   ADS1115 GND -> GND      SCL -> GPIO22
 *   AIN0/AIN1 -> SENSE+/SENSE- шунта 10 mΩ (Kelvin, low-side, ТЗ §9/§11)
 *   AIN2      -> середня точка дільника 90k/10k (ТЗ §12)
 *   AIN3      -> резерв
 */

#include <Arduino.h>

#include "measurement/core.h"
#include "ui/console.h"

static meas::MeasurementCore core;
static ui::Console console;

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println(F("\nBATTERY TESTER — measurement core, етап 1"));

    meas::Calibration cal;
    Serial.println(meas::loadCalibration(cal)
                       ? F("калібрування завантажено з NVS")
                       : F("калібрування в NVS немає — беремо номінали"));

    meas::CoreConfig cfg;   // дефолти: I2C 21/22, 0x48, 128 SPS, 32 SPS цикл, ядро 1

    if (!core.begin(cfg, cal)) {
        Serial.printf("ADS1115 не відповів на 0x%02X — перевір I2C (SDA %d, SCL %d)\n",
                      cfg.ads_addr, cfg.i2c_sda, cfg.i2c_scl);
        // Без ADC далі йти нема куди, але й перезавантажуватись у циклі теж:
        // хай повідомлення лишиться на екрані монітора.
        for (;;) delay(1000);
    }

    core.setSafetyLimits(meas::SafetyLimits{});   // 30 В / 5.5 А / 60 °C, без таймауту
    console.begin(core);
}

void loop() {
    console.poll();
}
