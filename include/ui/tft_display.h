/*
 * Display Task на TFT 160×128 ST7789 (ТЗ §33, §34, §62).
 *
 * Три сторінки:
 *   LIVE — V, I, P, Ah, Wh, T плюс графік V(t) за весь тест: коли 160
 *          колонок закінчуються, крива ущільнюється вдвічі, а не з'їжджає;
 *   TEST — підсумок тесту за ТЗ §57;
 *   DIAG — метрики §69/§70: діапазон PGA, тривалість циклу, помилки, пропуски.
 *
 * Дисплей нічого не рахує і до ADS1115 не звертається (ТЗ §52): читає
 * latest(), diagnostics() і stats() — і все.
 *
 * Дві речі, що відрізняють цей дисплей від I²C-панелі (ТЗ §33.1):
 *   - шина SPI спільна з microSD, тому доступ береться через мьютекс;
 *   - повний кадр коштує ~20 ms шини, тому fillScreen() робиться лише при
 *     зміні сторінки, а в циклі перемальовуються тільки поля, що змінились.
 */
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <math.h>

#include "measurement/core.h"

class Adafruit_ST7789_KMR18;

namespace ui {

class TftDisplay {
public:
    enum Page : uint8_t { PAGE_LIVE = 0, PAGE_TEST, PAGE_DIAG, PAGE_COUNT };

    struct Config {
        int8_t   cs = 5, dc = 17, rst = 4;
        uint32_t spi_hz = 16000000;
        uint16_t refresh_ms = 300;       // ~3 Гц, у межах 2–5 Гц з ТЗ §33
        int8_t   button_pin = -1;        // -1 = кнопки немає
        uint32_t auto_rotate_ms = 0;     // 0 = сторінки самі не перемикаються
        float    graph_step_s = 1.0f;    // початкова роздільність графіка, с/колонка
        UBaseType_t task_priority = 2;   // нижче ядра (10), вище idle
        BaseType_t  task_core = 1;
        uint32_t    task_stack = 4096;
    };

    // SPI.begin() має бути викликаний до цього, а сам begin() — ДО першого
    // звернення до microSD: у зворотному порядку панель не ініціалізується.
    // bus = nullptr означає «шина поки нічия», мьютекс не береться.
    bool begin(const Config &cfg, meas::MeasurementCore &core,
               SemaphoreHandle_t bus = nullptr);

    // page < 0 — наступна за колом. Викликається з будь-якої задачі.
    void setPage(int page);

    // Індикатори нижнього рядка. Виставляються ззовні: сам дисплей про
    // картку й мережу нічого не знає і знати не повинен.
    void setCardPresent(bool present) { sd_present_ = present; }
    void setNetUp(bool up) { net_up_ = up; }
    // Відсоток заряду рахує system-шар, дисплей лише показує (§52).
    void setSoc(float pct) { soc_ = pct; }
    // Внутрішній опір, Ом. Рахує system-шар (§52).
    void setInternalR(float ohm) { r_int_ = ohm; }

private:
    // Поле фіксованої ширини з кешем: перемальовується, лише коли змінився
    // текст або колір.
    struct Field {
        char     text[24];
        uint16_t color;
    };
    static constexpr uint8_t kMaxFields = 20;
    static constexpr uint8_t kGraphW = 128;   // колонок у полі побудови

    static void taskEntry(void *arg);
    void run();
    void pollButton();
    void drawPageFrame();
    void drawLive();
    void drawTest();
    void drawDiag();
    void pushGraphPoint(float v, float elapsed_s);
    void drawGraphAxes();
    void updateGraphLabels();
    void resetGraph();
    bool rescaleGraph(float v);
    void redrawGraph();
    void plotColumn(uint8_t x);
    void field(uint8_t slot, int16_t x, int16_t y, uint8_t size,
               uint16_t color, const char *text);
    void clearFieldCache();

    Config cfg_;
    meas::MeasurementCore *core_ = nullptr;
    Adafruit_ST7789_KMR18 *tft_ = nullptr;
    SemaphoreHandle_t bus_ = nullptr;
    TaskHandle_t task_ = nullptr;

    Field fields_[kMaxFields] = {};
    volatile Page page_ = PAGE_LIVE;
    volatile bool frame_dirty_ = true;   // потрібен fillScreen + статика

    // Графік показує весь тест від старту до поточного моменту. Коли 160
    // колонок закінчуються, сусідні пари зливаються в одну, а крок по часу
    // подвоюється: крива ущільнюється, але з екрана не йде ніколи. Вісь X —
    // час під інтегруванням, а не uptime, тому пауза її не розтягує.
    float    trace_[kGraphW];
    uint16_t count_ = 0;             // скільки колонок зайнято
    float    step_s_ = 1.0f;         // поточна ціна колонки в секундах
    float    last_elapsed_ = 0.0f;   // для виявлення перезапуску тесту
    float    g_min_ = 0.0f, g_max_ = 0.0f;
    bool     g_scaled_ = false;

    bool     prev_integrating_ = false;
    uint32_t next_redraw_ms_ = 0;
    uint32_t next_rotate_ms_ = 0;
    uint32_t button_edge_ms_ = 0;
    bool     button_down_ = false;

    volatile float soc_ = NAN;
    volatile float r_int_ = 0.0f;
    volatile bool sd_present_ = false;
    volatile bool net_up_ = false;
};

}  // namespace ui
