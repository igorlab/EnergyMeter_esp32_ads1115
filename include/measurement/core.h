/*
 * MEASUREMENT CORE (ТЗ §3, §24, §71).
 *
 * Окрема FreeRTOS-задача з власним планувальником. Знає тільки про ADS1115,
 * калібрування, інтегрування та safety. Не знає про Wi-Fi, HTTP, SD, дисплей
 * і не викликає нічого, що може заблокуватись.
 *
 * Споживачі отримують дані двома шляхами:
 *   latest()  — миттєвий знімок останнього record (для TFT, /api/status, web);
 *   setSink() — черга з усіма record підряд (для SD logger).
 * Черга ніколи не блокує ядро: якщо споживач не встигає, record рахується
 * втраченим у Diagnostics::dropped, а не чекає (ТЗ §70 — пропуски мають бути
 * зареєстровані, а не приховані).
 */
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "measurement/ads1115.h"
#include "measurement/autorange.h"
#include "measurement/calibration.h"
#include "measurement/integration.h"
#include "measurement/safety.h"
#include "measurement/types.h"

namespace meas {

struct CoreConfig {
    int      i2c_sda = 21;            // 21/22 лишені вільними під I2C (README)
    int      i2c_scl = 22;
    uint32_t i2c_hz  = 400000;
    uint8_t  ads_addr = 0x48;         // ADDR → GND

    ads::Rate rate = ads::Rate::SPS_128;   // конверсія ~7.8 мс на канал
    uint32_t  sample_period_us = 31250;    // 32 SPS повного циклу V+I (ТЗ §17)

    // Полярність струму — у Calibration::invert_current: вона залежить від
    // монтажу конкретного приладу, тому має зберігатись у NVS (ТЗ §28).
    float current_deadband = 0.005f;       // А, нижче — напрямок не оголошуємо

    // Як часто record іде в чергу споживача. Вимірювання від цього не
    // залежить — воно завжди на sample_period_us; це лише темп логування
    // (§43: 0.5–1 Гц у штатному режимі). 0 = кожен sample.
    uint32_t sink_interval_us = 1000000;

    UBaseType_t task_priority = 10;   // вище за loopTask (1) і за UI-задачі
    BaseType_t  task_core = 1;        // 1 — щоб не ділити ядро з Wi-Fi/lwIP
    uint32_t    task_stack = 4096;

    // Живлення ADS1115. Потрібне не для розрахунків, а для чесної відповіді
    // на питання «яку максимальну напругу прилад бачить»: одноканальне
    // вимірювання обмежене не шкалою PGA, а самим живленням АЦП. При шині
    // 3.1 В і дільнику 7.83 стеля становить 24 В, а не 32, як дає PGA.
    float adc_vdd = 3.1f;

    // Ковзне середнє по V та I (ТЗ §23). 1 = вимкнено.
    //
    // 16 — не з теорії, а заміряно на платі: СКВ падає в 7.7 раза, і далі
    // не падає (вікно 32 дає той самий результат). Отже все, що лишилось
    // після 16, — дрейф джерела нижче смуги фільтра, а не шум.
    // Вікно 500 мс, групова затримка (N-1)/2 = 234 мс: для індикації
    // непомітно, а для Ah за годинний тест це похибка порядку 10^-5 Ah.
    // §23 прямо дозволяє інтегрувати з фільтрованого сигналу, тому фільтр
    // стоїть до розрахунку потужності й до інтегратора.
    uint8_t average = 16;

    // Симуляція: ядро не торкається I2C і генерує правдоподібний розряд
    // Li-ion 1S. Потрібна рівно для одного — піднімати UI, поки АЦП ще не
    // припаяний. Дані позначені Diagnostics::simulated, і кожен споживач
    // зобов'язаний показати це користувачеві: мовчазна симуляція гірша за
    // відсутність даних.
    bool     simulate = false;
    uint16_t simulate_seconds = 600;   // повний розряд 4.2 -> 3.0 В за цей час
    float    simulate_current = 1.0f;  // А, поки йде інтегрування
};

class MeasurementCore {
public:
    // Піднімає I2C, перевіряє ADS1115 і стартує задачу.
    bool begin(const CoreConfig &cfg, const Calibration &cal);

    // --- дані для споживачів ---
    bool latest(Measurement &out) const;
    void diagnostics(Diagnostics &out) const;
    void stats(TestStats &out) const;
    // Черга приймача; nullptr відключає. Ядро шле неблокуюче.
    void setSink(QueueHandle_t sink) { sink_ = sink; }

    // --- керування (викликається state machine, не з HTTP-колбека) ---
    void setIntegrating(bool on);
    void resetIntegration();
    void restoreIntegration(float ah, float wh, float elapsed_s);
    void restoreStats(const TestStats &st);

    void setCalibration(const Calibration &cal);
    Calibration calibration() const;

    // -1 = автоматика, 0..4 = зафіксований діапазон каналу напруги (§69).
    void setVoltageRange(int index);
    // Разовий обхід усіх чотирьох входів однобічно (§69, монтаж). Виконує
    // сама вимірювальна задача в кінці циклу — щоб не переконфігурувати АЦП
    // з-під неї з іншої задачі. Результат забирати pinScan().
    void requestPinScan() { scan_request_ = true; }
    bool pinScan(float out[4]) const;

    // Ширина вікна усереднення, 1..32. Скидає накопичену історію.
    void setAverage(uint8_t n);
    uint8_t average() const { return cfg_.average; }
    // Тільки за явною командою користувача, ніколи автоматично.
    void setSimulate(bool on);
    // Робочий cutoff у вольтах збірки. 0 = не позначати.
    void setCutoff(float pack_volts) { cutoff_v_ = pack_volts; }
    void setSafetyLimits(const SafetyLimits &l);
    Fault fault() const;
    void clearFault();

    float elapsedSeconds() const;
    bool  integrating() const { return integrating_; }
    // Верхня межа за шкалою PGA — теоретична.
    float maxBatteryVolts() const { return range_.maxBatteryVolts(); }
    // Реальна стеля: менше з двох — шкала PGA і живлення АЦП.
    float usableMaxVolts() const;

private:
    static void taskEntry(void *arg);
    void run();
    void resetStats();
    void simulateOnce();
    void finishSample(uint64_t now_us, float voltage, float current,
                      float temperature, uint8_t flags);
    void sampleOnce();
    void publish(const Measurement &m);

    CoreConfig  cfg_;
    ads::Ads1115 ads_;
    VoltageAutoRange range_;
    Integrator  integrator_;
    Safety      safety_;

    // Все, що читають інші задачі, — під цим спінлоком. Копія структури
    // коштує десятки тактів, тримати критичну секцію довше нема потреби.
    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    Measurement latest_{};
    Diagnostics diag_{};
    TestStats   stats_{};
    Calibration cal_{};
    bool        have_sample_ = false;

    // Історія для ковзного середнього. Сума перераховується щоцикла, а не
    // ведеться накопичувально: 32 додавання на 32 Гц нічого не варті, зате
    // немає ні дрейфу суми, ні мороки при зміні ширини вікна на ходу.
    static constexpr uint8_t kMaxAverage = 32;
    float   v_hist_[kMaxAverage] = {0};
    float   i_hist_[kMaxAverage] = {0};
    uint8_t hist_head_ = 0;
    uint8_t hist_count_ = 0;

    volatile bool scan_request_ = false;
    volatile bool scan_valid_ = false;
    float         scan_[4] = {0};

    volatile float cutoff_v_ = 0.0f;
    uint16_t cutoff_hits_ = 0;
    volatile bool integrating_ = false;
    QueueHandle_t sink_ = nullptr;
    uint64_t      last_sink_us_ = 0;
    TaskHandle_t  task_ = nullptr;
    uint32_t      sequence_ = 0;
};

// Середнє по сирих, ще не каліброваних відліках. Потрібне і консолі, і
// вебу для двоточкового калібрування §54; блокує задачу-виклику приблизно
// на секунду, а ядра не торкається.
float averageUncalibrated(MeasurementCore &core, bool voltage,
                          uint16_t samples = 32);

}  // namespace meas
