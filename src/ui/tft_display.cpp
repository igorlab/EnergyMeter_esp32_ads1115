#include "ui/tft_display.h"

#include <Adafruit_ST7789_KMR18.h>
#include <SPI.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace meas;

namespace ui {

namespace {

// Розкладка. Шрифт GFX: 6×8 при розмірі 1, 12×16 при 2, 18×24 при 3.
constexpr int16_t X        = 4;

// --- LIVE, верхній блок ---
constexpr int16_t Y_V      = 2;     // розмір 3, 6 символів = 108 px
constexpr int16_t X_V_UNIT = 116;
constexpr int16_t X_FLAGS  = 132;   // SD / NET / SIM у стовпчик
constexpr int16_t Y_I      = 30;    // розмір 2, 6 символів = 72 px
constexpr int16_t X_I_UNIT = 80;
constexpr int16_t X_RIGHT  = 98;
constexpr int16_t Y_STATE  = 30;
constexpr int16_t Y_DIR    = 40;
constexpr int16_t Y_P      = 50;
constexpr int16_t Y_SUM    = 60;

// --- LIVE, поле графіка ---
// Ліві 31 px — під підписи осі напруги, нижні 10 — під вісь часу.
constexpr int16_t GX       = 32;    // ліва межа поля побудови
constexpr int16_t GY       = 70;    // верх поля
constexpr int16_t GH       = 48;    // висота поля: 70..117
constexpr int16_t AXIS_X   = GX - 1;
constexpr int16_t AXIS_Y   = GY + GH;   // 118
constexpr int16_t LBL_Y    = AXIS_Y + 2;
constexpr int16_t LBL_RIGHT = GX - 2;   // праворуч до цієї межі підписи по Y

// --- слоти кешу полів ---
enum : uint8_t {
    F_V = 0, F_VU, F_I, F_IU, F_STATE, F_DIR, F_P, F_TIME, F_SUM,
    F_SD, F_NET, F_SIM, F_Y0, F_Y1, F_Y2, F_XSPAN,
};

constexpr uint16_t C_BG    = ST77XX_BLACK;
constexpr uint16_t C_LABEL = 0x8410;            // сірий 50%
constexpr uint16_t C_GRID  = 0x2124;            // осі та сітка
constexpr uint16_t C_VALUE = ST77XX_WHITE;
// Напруга синя, струм зелений — і в числах, і в підписах осей, і в кривій.
constexpr uint16_t C_VOLT  = 0x451F;            // блакитно-синій, читається на чорному
constexpr uint16_t C_CURR  = ST77XX_GREEN;
constexpr uint16_t C_WARN  = ST77XX_YELLOW;
constexpr uint16_t C_FAULT = ST77XX_RED;
constexpr uint16_t C_SIM   = ST77XX_MAGENTA;

void formatHms(char *out, size_t n, uint32_t secs) {
    snprintf(out, n, "%02u:%02u:%02u",
             secs / 3600u, (secs / 60u) % 60u, secs % 60u);
}

// Ємність і енергія в зручній одиниці. Десяті Ah — те, що просив користувач,
// але при 56 мА до першої десятої треба 107 хвилин, і весь цей час індикація
// показувала б нуль. Тому нижче 1 Ah переходимо на мілі: це інша одиниця, а
// не зайва точність.
//
// Ширина завжди 8 символів — інакше непрозорий фон не стер би попереднє
// значення при переході 999 mAh -> 1.0 Ah.
void formatCharge(char *out, size_t n, float value, const char *unit) {
    const float a = fabsf(value);
    if (a >= 1.0f)      snprintf(out, n, "%5.1f %s", value, unit);
    else if (a >= 0.1f) snprintf(out, n, "%5.0fm%s", value * 1000.0f, unit);
    else                snprintf(out, n, "%5.1fm%s", value * 1000.0f, unit);
}

// Підпис осі напруги: чотири символи за будь-якої шкали, щоб не з'їхала
// права межа при переході через 10 В.
void formatVolt(char *out, size_t n, float v) {
    if (fabsf(v) < 10.0f) snprintf(out, n, "%.2f", v);
    else                  snprintf(out, n, "%.1f", v);
}

}  // namespace

bool TftDisplay::begin(const Config &cfg, MeasurementCore &core, SemaphoreHandle_t bus) {
    cfg_ = cfg;
    core_ = &core;
    bus_ = bus;

    step_s_ = (cfg.graph_step_s > 0.0f) ? cfg.graph_step_s : 1.0f;
    resetGraph();

    if (cfg_.button_pin >= 0) pinMode(cfg_.button_pin, INPUT_PULLUP);

    tft_ = new Adafruit_ST7789_KMR18(cfg_.cs, cfg_.dc, cfg_.rst);
    if (!tft_) return false;

    // Панель після подачі живлення не одразу приймає команди, а бібліотека
    // своїх затримок майже не має — деталі в README.
    delay(150);
    tft_->initKMR18(cfg_.spi_hz);   // rotation 1 = ландшафт 160×128
    tft_->setTextWrap(false);

    return xTaskCreatePinnedToCore(&TftDisplay::taskEntry, "tft", cfg_.task_stack,
                                   this, cfg_.task_priority, &task_,
                                   cfg_.task_core) == pdPASS;
}

void TftDisplay::setPage(int page) {
    const Page next = (page < 0)
        ? static_cast<Page>((page_ + 1) % PAGE_COUNT)
        : static_cast<Page>(page % PAGE_COUNT);
    if (next == page_) return;
    page_ = next;
    frame_dirty_ = true;
}

void TftDisplay::taskEntry(void *arg) {
    static_cast<TftDisplay *>(arg)->run();
}

void TftDisplay::run() {
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        // Кнопка опитується частіше за перемальовування: на 3 Гц половина
        // натискань просто не потрапила б у вікно.
        pollButton();

        const uint32_t now = millis();
        if (cfg_.auto_rotate_ms && now >= next_rotate_ms_) {
            next_rotate_ms_ = now + cfg_.auto_rotate_ms;
            setPage(-1);
        }

        if (frame_dirty_ || now >= next_redraw_ms_) {
            next_redraw_ms_ = now + cfg_.refresh_ms;

            // Мьютекс береться на кадр і віддається між кадрами: SD Logger
            // (ТЗ §42) отримує шину в паузі, а не воює за неї на кожному полі.
            // Не дочекавшись за 100 мс — пропускаємо кадр: індикація має право
            // відстати, вимірювання від цього не залежить (ТЗ §64).
            if (!bus_ || xSemaphoreTake(bus_, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (frame_dirty_) {
                    frame_dirty_ = false;
                    drawPageFrame();
                }
                switch (page_) {
                    case PAGE_TEST: drawTest(); break;
                    case PAGE_DIAG: drawDiag(); break;
                    default:        drawLive(); break;
                }
                if (bus_) xSemaphoreGive(bus_);
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(40));
    }
}

// Коротке натискання перемикає сторінку, довге (>= kLongPressMs) — старт/стоп
// тесту. Рішення відкладене до відпускання: якби коротке спрацьовувало одразу
// на натисканні (як робилось раніше, коли жест був лише один), довге тримання
// встигло б і перемкнути сторінку, і застартувати тест тим самим натисканням.
void TftDisplay::pollButton() {
    if (cfg_.button_pin < 0) return;

    const bool down = digitalRead(cfg_.button_pin) == LOW;
    const uint32_t now = millis();
    if (down == button_down_) return;
    if (now - button_edge_ms_ < 30) return;   // дребезг контакту

    button_edge_ms_ = now;
    button_down_ = down;
    if (down) {
        button_press_ms_ = now;
    } else if (now - button_press_ms_ >= kLongPressMs) {
        if (toggle_cb_) toggle_cb_();
    } else {
        setPage(-1);
    }
}

void TftDisplay::clearFieldCache() {
    for (uint8_t i = 0; i < kMaxFields; i++) {
        fields_[i].text[0] = '\0';
        fields_[i].color = 0;
    }
}

// Поле фіксованої ширини поверх попереднього. Рядки доповнені пробілами до
// сталої довжини, тому непрозорий фон стирає попереднє значення повністю —
// чистити прямокутник окремо не треба.
void TftDisplay::field(uint8_t slot, int16_t x, int16_t y, uint8_t size,
                       uint16_t color, const char *text) {
    Field &f = fields_[slot];
    if (color == f.color && strcmp(text, f.text) == 0) return;

    tft_->setTextSize(size);
    tft_->setTextColor(color, C_BG);
    tft_->setCursor(x, y);
    tft_->print(text);

    strncpy(f.text, text, sizeof(f.text) - 1);
    f.text[sizeof(f.text) - 1] = '\0';
    f.color = color;
}

// Єдине місце, де викликається fillScreen: зміна сторінки. У циклі — ніколи.
void TftDisplay::drawPageFrame() {
    tft_->fillScreen(C_BG);
    clearFieldCache();
    tft_->setTextSize(1);
    tft_->setTextColor(C_LABEL, C_BG);

    switch (page_) {
        case PAGE_LIVE:
            drawGraphAxes();
            redrawGraph();
            updateGraphLabels();
            break;
        case PAGE_TEST:
            tft_->setCursor(X, 2);
            tft_->print(F("TEST SUMMARY"));
            tft_->drawFastHLine(0, 12, 160, C_GRID);
            break;
        case PAGE_DIAG:
            tft_->setCursor(X, 2);
            tft_->print(F("DIAGNOSTICS"));
            tft_->drawFastHLine(0, 12, 160, C_GRID);
            break;
        default: break;
    }
}

// ---------------------------------------------------------------- LIVE

void TftDisplay::drawLive() {
    Measurement m;
    if (!core_->latest(m)) return;

    Diagnostics d;
    core_->diagnostics(d);
    const Fault fault = core_->fault();
    char buf[24];

    // Старт тесту скидає графік: і буфер, і межі шкали. Інакше нова крива
    // малюється в масштабі попереднього тесту, а стара висить поруч і
    // виглядає як частина нової. Лічильники при цьому не зачіпаються —
    // графік це вид, а не дані.
    const bool integrating = (m.flags & FLAG_INTEGRATING) != 0;
    if (integrating && !prev_integrating_) {
        resetGraph();
        redrawGraph();
    }
    prev_integrating_ = integrating;

    // --- V великим, синім ---
    snprintf(buf, sizeof(buf), "%6.3f", m.voltage);
    field(F_V, X, Y_V, 3, C_VOLT, buf);
    field(F_VU, X_V_UNIT, Y_V + 8, 2, C_VOLT, "V");

    // --- I зеленим ---
    // Без знака: напрямок несе рядок CHARGE/DISCHARGE нижче (§28).
    snprintf(buf, sizeof(buf), "%6.3f", fabsf(m.current));
    field(F_I, X, Y_I, 2, C_CURR, buf);
    field(F_IU, X_I_UNIT, Y_I + 8, 1, C_CURR, "A");

    // --- стан і напрямок ---
    uint16_t state_color = C_LABEL;
    const char *state = "PAUSED ";
    if (fault != Fault::NONE) {
        state = "FAULT  ";
        state_color = C_FAULT;
    } else if (m.flags & FLAG_INTEGRATING) {
        state = "RUNNING";
        state_color = C_CURR;
    }
    field(F_STATE, X_RIGHT, Y_STATE, 1, state_color, state);

    const char *dir = "IDLE     ";
    uint16_t dir_color = C_LABEL;
    if (fabsf(m.current) >= 0.005f) {
        if (m.flags & FLAG_CHARGE) { dir = "CHARGE   "; dir_color = C_CURR; }
        else                       { dir = "DISCHARGE"; dir_color = C_WARN; }
    }
    field(F_DIR, X_RIGHT, Y_DIR, 1, dir_color, dir);

    // --- P і час ---
    snprintf(buf, sizeof(buf), "P %7.3f W", fabsf(m.power));
    field(F_P, X, Y_P, 1, C_VALUE, buf);

    const float elapsed = core_->elapsedSeconds();
    formatHms(buf, sizeof(buf), static_cast<uint32_t>(elapsed));
    field(F_TIME, X_RIGHT, Y_P, 1, C_VALUE, buf);

    // --- Ah, Wh, температура одним рядком ---
    // Ємність і енергія — до десятих. Ширина полів стала (%5.1f), інакше
    // при переході 9.9 -> 10.0 непрозорий фон не стер би зайвий символ.
    char ah[16], wh[16];
    formatCharge(ah, sizeof(ah), fabsf(m.Ah), "Ah");
    formatCharge(wh, sizeof(wh), fabsf(m.Wh), "Wh");
    // Замість температури, поки датчика немає, — відсоток заряду.
    if (m.flags & FLAG_TEMP_VALID) {
        snprintf(buf, sizeof(buf), "%s %s %4.1fC", ah, wh, m.temperature);
    } else if (!isnan(soc_)) {
        snprintf(buf, sizeof(buf), "%s %s %3.0f%%  ", ah, wh, soc_);
    } else {
        snprintf(buf, sizeof(buf), "%s %s  --  ", ah, wh);
    }
    field(F_SUM, X, Y_SUM, 1, C_VALUE, buf);

    // --- індикатори збоку від великої напруги ---
    field(F_SD,  X_FLAGS, Y_V, 1, sd_present_ ? C_CURR : C_LABEL, "SD");
    field(F_NET, X_FLAGS, Y_V + 10, 1, net_up_ ? C_CURR : C_LABEL, "NET");
    // Симуляція мусить бути видимою: мовчазні синтетичні дані — гірше,
    // ніж їх відсутність.
    field(F_SIM, X_FLAGS, Y_V + 20, 1, C_SIM, d.simulated ? "SIM" : "   ");

    pushGraphPoint(m.voltage, elapsed);
}

// ---------------------------------------------------------------- графік

void TftDisplay::resetGraph() {
    for (uint16_t i = 0; i < kGraphW; i++) trace_[i] = NAN;
    count_ = 0;
    g_scaled_ = false;
    last_elapsed_ = 0.0f;
}

// Осі малюються один раз на кадр сторінки: лінії й нерухома нуль-позначка
// часу. Числові підписи живуть окремо, бо міняються разом зі шкалою.
void TftDisplay::drawGraphAxes() {
    tft_->drawFastVLine(AXIS_X, GY, GH + 2, C_GRID);
    tft_->drawFastHLine(AXIS_X, AXIS_Y, 160 - AXIS_X, C_GRID);

    // Засічки на осі напруги: верх, середина, низ.
    tft_->drawFastHLine(AXIS_X - 2, GY, 2, C_GRID);
    tft_->drawFastHLine(AXIS_X - 2, GY + GH / 2, 2, C_GRID);
    tft_->drawFastHLine(AXIS_X - 2, GY + GH - 1, 2, C_GRID);

    tft_->setTextSize(1);
    tft_->setTextColor(C_LABEL, C_BG);
    tft_->setCursor(GX, LBL_Y);
    tft_->print(F("0"));
}

// Підписи осей. Вісь напруги — синім, бо це та сама величина, що й крива.
void TftDisplay::updateGraphLabels() {
    char buf[12];

    formatVolt(buf, sizeof(buf), g_max_);
    field(F_Y0, LBL_RIGHT - static_cast<int16_t>(strlen(buf)) * 6, GY, 1, C_VOLT, buf);

    formatVolt(buf, sizeof(buf), 0.5f * (g_min_ + g_max_));
    field(F_Y1, LBL_RIGHT - static_cast<int16_t>(strlen(buf)) * 6,
          GY + GH / 2 - 4, 1, C_VOLT, buf);

    formatVolt(buf, sizeof(buf), g_min_);
    field(F_Y2, LBL_RIGHT - static_cast<int16_t>(strlen(buf)) * 6,
          GY + GH - 8, 1, C_VOLT, buf);

    // Права межа осі часу — повна тривалість, яку зараз вміщає графік.
    const float span_s = (count_ > 1) ? (count_ - 1) * step_s_ : 0.0f;
    if (span_s < 90.0f)        snprintf(buf, sizeof(buf), "%.0fs", span_s);
    else if (span_s < 5400.0f) snprintf(buf, sizeof(buf), "%.0fmin", span_s / 60.0f);
    else                       snprintf(buf, sizeof(buf), "%.1fh", span_s / 3600.0f);
    field(F_XSPAN, 160 - static_cast<int16_t>(strlen(buf)) * 6, LBL_Y, 1, C_LABEL, buf);
}

// Межі шкали з полем 8%. Повертає true, якщо межі змінились і криву треба
// перемалювати цілком.
bool TftDisplay::rescaleGraph(float v) {
    if (!g_scaled_) {
        g_min_ = v - 0.05f;
        g_max_ = v + 0.05f;
        g_scaled_ = true;
        return true;
    }
    if (v >= g_min_ && v <= g_max_) return false;

    float lo = v, hi = v;
    for (uint16_t i = 0; i < count_; i++) {
        if (isnan(trace_[i])) continue;
        if (trace_[i] < lo) lo = trace_[i];
        if (trace_[i] > hi) hi = trace_[i];
    }
    const float margin = fmaxf((hi - lo) * 0.08f, 0.02f);
    g_min_ = lo - margin;
    g_max_ = hi + margin;
    return true;
}

void TftDisplay::pushGraphPoint(float v, float elapsed_s) {
    // Час пішов назад — тест перезапустили, крива від попереднього не наша.
    if (elapsed_s + 0.5f < last_elapsed_) {
        resetGraph();
        redrawGraph();
    }
    last_elapsed_ = elapsed_s;

    // Колонка i відповідає моменту i * step_s_. Поки цей момент не настав,
    // нової точки немає — і графік стоїть, якщо тест на паузі.
    if (count_ > 0 && elapsed_s < count_ * step_s_) return;

    bool full_redraw = false;

    if (count_ >= kGraphW) {
        // Ущільнення: сусідні пари зливаються в одну колонку, крок по часу
        // подвоюється. Крива стає грубішою, але видно її цілком — від старту
        // тесту до поточного моменту, скільки б той не тривав.
        for (uint16_t i = 0; i < kGraphW / 2; i++) {
            trace_[i] = 0.5f * (trace_[2 * i] + trace_[2 * i + 1]);
        }
        for (uint16_t i = kGraphW / 2; i < kGraphW; i++) trace_[i] = NAN;
        count_ = kGraphW / 2;
        step_s_ *= 2.0f;
        full_redraw = true;
    }

    trace_[count_++] = v;
    if (rescaleGraph(v)) full_redraw = true;

    if (full_redraw) redrawGraph();
    else             plotColumn(static_cast<uint8_t>(count_ - 1));

    updateGraphLabels();
}

// Одна колонка: відрізок від попередньої точки до поточної.
void TftDisplay::plotColumn(uint8_t i) {
    const float span = g_max_ - g_min_;
    if (span <= 0.0f || isnan(trace_[i])) return;

    auto toY = [&](float v) -> int16_t {
        float t = (v - g_min_) / span;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        return static_cast<int16_t>(GY + GH - 1 - t * (GH - 1));
    };

    const int16_t y = toY(trace_[i]);
    if (i > 0 && !isnan(trace_[i - 1])) {
        tft_->drawLine(GX + i - 1, toY(trace_[i - 1]), GX + i, y, C_VOLT);
    } else {
        tft_->drawPixel(GX + i, y, C_VOLT);
    }
}

void TftDisplay::redrawGraph() {
    tft_->fillRect(GX, GY, kGraphW, GH, C_BG);
    // Середня лінія сітки: без неї око не бачить, де половина шкали.
    tft_->drawFastHLine(GX, GY + GH / 2, kGraphW, C_GRID);
    for (uint16_t i = 0; i < count_; i++) plotColumn(static_cast<uint8_t>(i));
}

// ---------------------------------------------------------------- TEST

void TftDisplay::drawTest() {
    Measurement m;
    if (!core_->latest(m)) return;

    TestStats st;
    core_->stats(st);
    const float elapsed = core_->elapsedSeconds();
    char buf[24];

    formatCharge(buf, sizeof(buf), fabsf(m.Ah), "Ah");
    field(F_V, X, 16, 2, C_CURR, buf);
    formatCharge(buf, sizeof(buf), fabsf(m.Wh), "Wh");
    field(F_VU, X, 34, 2, C_VALUE, buf);

    snprintf(buf, sizeof(buf), "V start %8.4f", st.v_start);
    field(F_I, X, 56, 1, C_VOLT, buf);
    // Min і max в один рядок: кожне окремо займало б рядок заради восьми
    // символів, а звільнене місце потрібне під внутрішній опір.
    snprintf(buf, sizeof(buf), "V %5.3f..%5.3f", st.v_min, st.v_max);
    field(F_IU, X, 66, 1, C_VOLT, buf);
    // §68. Заміряний із пульсацій навантаження, тому є вже під час тесту.
    if (r_int_ > 0.0f) snprintf(buf, sizeof(buf), "R int %5.0f mOhm", r_int_ * 1000.0f);
    else               snprintf(buf, sizeof(buf), "R int      -    ");
    field(F_STATE, X, 76, 1, r_int_ > 0.0f ? C_VALUE : C_LABEL, buf);

    // Середній струм — це Ah, поділені на час: не інтегрування наново,
    // а перерахунок уже готової величини в інші одиниці (ТЗ §52).
    const float i_avg = (elapsed > 1.0f) ? fabsf(m.Ah * 3600.0f / elapsed) : 0.0f;
    snprintf(buf, sizeof(buf), "I avg   %8.4f", i_avg);
    field(F_DIR, X, 86, 1, C_CURR, buf);
    snprintf(buf, sizeof(buf), "I max   %8.4f", st.i_max);
    field(F_P, X, 96, 1, C_CURR, buf);

    // Ємність до робочого cutoff — окремо від повної. Датчика температури
    // немає, а це число потрібне, щоб порівнювати з паспортом елемента.
    if (isnan(st.ah_cutoff)) {
        snprintf(buf, sizeof(buf), "cutoff      -   ");
        field(F_TIME, X, 106, 1, C_LABEL, buf);
    } else {
        char c[16];
        formatCharge(c, sizeof(c), fabsf(st.ah_cutoff), "Ah");
        snprintf(buf, sizeof(buf), "cutoff %s", c);
        field(F_TIME, X, 106, 1, C_CURR, buf);
    }

    char hms[16];
    formatHms(hms, sizeof(hms), static_cast<uint32_t>(elapsed));
    const Fault fault = core_->fault();
    snprintf(buf, sizeof(buf), "%s %-12s", hms, faultName(fault));
    field(F_SUM, X, 118, 1, fault == Fault::NONE ? C_LABEL : C_FAULT, buf);
}

// ---------------------------------------------------------------- DIAG

void TftDisplay::drawDiag() {
    Measurement m;
    core_->latest(m);
    Diagnostics d;
    core_->diagnostics(d);
    char buf[24];

    snprintf(buf, sizeof(buf), "PGA   %u  +/-%5.3f V", d.v_pga, d.v_fs);
    field(F_V, X, 18, 1, d.simulated ? C_SIM : C_VALUE, buf);
    snprintf(buf, sizeof(buf), "V raw    %8d", d.v_raw);
    field(F_VU, X, 28, 1, C_VOLT, buf);
    snprintf(buf, sizeof(buf), "I raw    %8d", d.i_raw);
    field(F_I, X, 38, 1, C_CURR, buf);

    snprintf(buf, sizeof(buf), "cycle %5.1f ms  avg%u", d.cycle_us / 1000.0f, d.average);
    field(F_IU, X, 52, 1, C_VALUE, buf);
    snprintf(buf, sizeof(buf), "cyc max  %6.1f ms", d.cycle_max_us / 1000.0f);
    field(F_STATE, X, 62, 1, d.cycle_max_us > 40000 ? C_WARN : C_VALUE, buf);

    snprintf(buf, sizeof(buf), "ADC err  %8u", d.adc_errors);
    field(F_DIR, X, 76, 1, d.adc_errors ? C_FAULT : C_VALUE, buf);
    snprintf(buf, sizeof(buf), "range ch %8u", d.range_changes);
    field(F_P, X, 86, 1, C_VALUE, buf);
    snprintf(buf, sizeof(buf), "dropped  %8u", d.dropped);
    field(F_TIME, X, 96, 1, d.dropped ? C_WARN : C_VALUE, buf);
    snprintf(buf, sizeof(buf), "seq      %8u", m.sequence);
    field(F_SUM, X, 106, 1, C_VALUE, buf);

    snprintf(buf, sizeof(buf), "heap  %6u KB  %s",
             ESP.getFreeHeap() / 1024u, d.simulated ? "SIM" : "   ");
    field(F_SD, X, 118, 1, d.simulated ? C_SIM : C_LABEL, buf);
}

}  // namespace ui
