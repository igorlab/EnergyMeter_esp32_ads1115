/*
 * Етап 2 (ТЗ §75): те саме вимірювальне ядро + TFT з трьома сторінками.
 *
 * Модулі measurement/ спільні з `-e measure`. У цьому й сенс етапу: числа
 * V, I, Ah, Wh мають лишитись ті самі, а Diagnostics::cycle_max_us — не
 * вирости. Якщо виросло, винен дисплей, і це видно ще до появи SD та
 * Wi-Fi (ТЗ §70). Порівняти можна прямо на екрані: сторінка DIAG.
 *
 * Збірка:  pio run -e tester -t upload && pio device monitor
 *
 * Сторінки: кнопка на GPIO27 (замикає на GND) або команда `page` у консолі.
 *
 * Розводка — README (дисплей і картка) плюс ADS1115:
 *   ADS1115 VDD -> 3V3   SDA -> GPIO21   SCL -> GPIO22   ADDR -> GND (0x48)
 *   AIN0/AIN1 -> SENSE+/SENSE- шунта 10 mΩ (Kelvin, low-side)
 *   AIN2      -> середня точка дільника 90k/10k
 * I²C і SPI не перетинаються: 21/22 у README лишені саме під це.
 *
 * Якщо ADS1115 не відповідає, прошивка не зупиняється, а піднімає ядро в
 * режимі симуляції: розкладку й таймінги дисплея можна доводити до того, як
 * припаяний АЦП. Синтетичні дані позначені міткою SIM на екрані й у консолі.
 */

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>

#include "measurement/core.h"
#include "storage/sd_logger.h"
#include "system/battery_profile.h"
#include "system/test_session.h"
#include "system/test_state.h"
#include "ui/console.h"
#include "ui/tft_display.h"
#include "ui/web_ui.h"

#define SPI_SCK  18
#define SPI_MISO 19
#define SPI_MOSI 23
#define TFT_CS    5
#define SD_CS    15
#define BTN_PIN  27      // вільний за README; замикається на GND

static meas::MeasurementCore core;
static ui::Console           console;
static ui::TftDisplay        display;
static storage::SdLogger     sdlog;
static ui::WebUi             web;

// Мьютекс шини SPI (ТЗ §33.1). Поки на ній лише дисплей, але створюється він
// уже тут: SD Logger має отримати готову точку синхронізації, а не привід
// переписувати Display Task.
static SemaphoreHandle_t spi_bus = nullptr;

static sys::BatteryProfile profile;
static sys::TestSession    session;
static float soc_pct   = NAN;   // те, що показуємо
static float soc_rest  = NAN;   // остання оцінка за напругою спокою
// Внутрішній опір: або з профілю, або заміряний на стрибку струму.
static float r_measured = 0.0f;
static float rest_v = NAN;          // остання напруга у справжньому спокої
static uint32_t rest_ms = 0;
static float prev_i = 0.0f;
static float prev_v = NAN;

// Кільце оцінок R для медіани. Саме медіана, а не середнє: окремі пари
// шумні — на живих даних міжквартильний розмах вийшов 0.27..0.50 при
// істинних 0.35, — і одна промашка зіпсувала б середнє, а медіану ні.
constexpr uint8_t kRRing = 31;
static float r_ring[kRRing];
static uint8_t r_head = 0, r_count = 0;

static float medianR() {
    if (r_count < 7) return 0.0f;
    float t[kRRing];
    for (uint8_t k = 0; k < r_count; k++) t[k] = r_ring[k];
    for (uint8_t a = 1; a < r_count; a++) {          // вставками, 31 елемент
        const float x = t[a];
        int8_t b = static_cast<int8_t>(a - 1);
        while (b >= 0 && t[b] > x) { t[b + 1] = t[b]; b--; }
        t[b + 1] = x;
    }
    return t[r_count / 2];
}
static float soc_ocv = NAN;         // напруга спокою, з якої взято відсоток

static void applyProfile() {
    meas::SafetyLimits lim;
    // Аварія — на абсолютному мінімумі хімії (2.50 В на елемент для літію),
    // а не на робочому cutoff. Розряд рве плата захисту на своєму порозі;
    // якщо напруга дійшла до абсолютного мінімуму, значить не розірвала —
    // і це справді несправність, а не штатне завершення тесту.
    lim.v_min = sys::chemFloor(profile.chemistry) * profile.cells;
    lim.v_max = sys::packFull(profile) * 1.10f;
    lim.i_max = profile.i_max;
    lim.t_max = profile.t_max;
    core.setSafetyLimits(lim);
    // Cutoff лише позначається в результаті, нічого не зупиняє (§57).
    core.setCutoff(sys::packCutoff(profile));
    web.setProfile(sys::chemName(profile.chemistry),
                   sys::packCutoff(profile), profile.capacity_ah, profile.cells,
                   sys::chemFloor(profile.chemistry) * profile.cells,
                   profile.i_max, profile.t_max, profile.r_internal);
}

// Профілем володіє system-шар, веб лише просить його змінити (§52).
static bool applyProfileFromWeb(const char *chem, int cells, float cap,
                                float imax, float tmax, float r_int) {
    profile.chemistry = (chem && !strcmp(chem, "lfp")) ? sys::Chemistry::LIFEPO4
                                                       : sys::Chemistry::LI_ION;
    profile.cells = (cells >= 1 && cells <= 8) ? static_cast<uint8_t>(cells) : 1;
    profile.capacity_ah = cap > 0.0f ? cap : 0.0f;
    profile.i_max = imax > 0.0f ? imax : 5.0f;
    profile.t_max = tmax > 0.0f ? tmax : 60.0f;
    profile.r_internal = r_int >= 0.0f ? r_int : 0.0f;
    profile.v_cutoff_cell = sys::chemCutoff(profile.chemistry);
    sys::saveProfile(profile);
    applyProfile();
    return true;
}

static void printProfile() {
    Serial.printf("профіль: %s %uS  ємність %.4f Ah%s\n",
                  sys::chemName(profile.chemistry), profile.cells,
                  profile.capacity_ah,
                  profile.capacity_ah > 0 ? "" : "  (невідома)");
    Serial.printf("  на елемент: повний %.2f  номінал %.2f  cutoff %.2f  мінімум %.2f В\n",
                  sys::chemFull(profile.chemistry), sys::chemNominal(profile.chemistry),
                  profile.v_cutoff_cell, sys::chemFloor(profile.chemistry));
    Serial.printf("  на збірку:  повний %.2f  cutoff %.2f В   Imax %.2f А  Tmax %.0f C\n",
                  sys::packFull(profile), sys::packCutoff(profile),
                  profile.i_max, profile.t_max);
    Serial.printf("  cutoff лише позначає ємність у результаті; аварія на %.2f В\n",
                  sys::chemFloor(profile.chemistry) * profile.cells);
}

// Внутрішній опір із стрибка струму: R = dV / dI. Момент для цього дає сама
// робота — щойно навантаження або зарядник вмикається після спокою.
// Це §68 «internal resistance measurement», але задарма: окремого режиму не
// треба, достатньо зловити подію, яка й так відбувається.
static void measureResistance(const meas::Measurement &m) {
    const float i = m.current;
    const uint32_t now = millis();

    if (fabsf(i) < 0.01f) {
        // Спокій: запам'ятовуємо напругу для замір на майбутньому стрибку.
        rest_v = m.voltage;
        rest_ms = now;
    } else {
        // Спосіб перший: стрибок зі спокою під струм. Найточніший, але
        // трапляється лише на початку роботи.
        if (!isnan(rest_v) && fabsf(prev_i) < 0.01f && fabsf(i) > 0.1f &&
            now - rest_ms < 30000) {
            const float r = fabsf(m.voltage - rest_v) / fabsf(i);
            if (r > 0.02f && r < 3.0f) {
                r_measured = r;
                Serial.printf("R внутрішній: %.3f Ом (стрибок %.3f В на %.3f А)\n",
                              r, fabsf(m.voltage - rest_v), fabsf(i));
            }
        }

        // Спосіб другий, безперервний: R = -dV/dI між сусідніми відліками.
        // Дрейф напруги від самого розряду скорочується різницею, тому це
        // працює й посеред тесту. Пари дає саме навантаження: у ESP32-C3
        // кожен сплеск Wi-Fi — стрибок струму на десятки міліампер.
        //
        // Перевірено на живих даних: медіана 0.356 Ом проти 0.350, знятих
        // незалежно способом першим.
        if (!isnan(prev_v) && fabsf(i) > 0.02f) {
            const float di = i - prev_i;
            const float dv = m.voltage - prev_v;
            if (fabsf(di) > fmaxf(0.005f, 0.02f * fabsf(i))) {
                const float r = -dv / di;
                if (r > 0.02f && r < 3.0f) {
                    r_ring[r_head] = r;
                    r_head = static_cast<uint8_t>((r_head + 1) % kRRing);
                    if (r_count < kRRing) r_count++;
                    const float med = medianR();
                    if (med > 0.0f) r_measured = med;
                }
            }
        }
    }

    prev_v = m.voltage;
    prev_i = i;
}

// §52: відсоток рахується тут, у system-шарі. Дисплей і веб лише показують.
static void updateSoc() {
    meas::Measurement m;
    if (!core.latest(m)) return;

    measureResistance(m);

    // Крива OCV описує напругу СПОКОЮ. Питати її про напругу на клемі під
    // струмом — груба помилка: при 0.8 А і R = 0.35 Ом клема піднята на
    // 0.28 В, і крива віддає 89% там, де кулонометрія бачить 40%.
    // Тому спершу знімаємо I*R_вн, і аж тоді дивимось у таблицю.
    const float r = profile.r_internal > 0.0f ? profile.r_internal : r_measured;
    soc_ocv = sys::ocvFromLoaded(m.voltage, m.current, r, profile.cells);
    soc_pct = sys::socFromOcv(profile, soc_ocv);

    if (!(m.flags & meas::FLAG_INTEGRATING) && fabsf(m.current) < 0.01f) {
        soc_rest = soc_pct;
    }
}

// Життєвий цикл тесту (старт/resume/abort/автозупинка/збереження стану) —
// у sys::TestSession (§73). Тут лишається лише тонкий трамплін: колбеки
// console/web — прості C-вказівники на функцію без user-data, тож потрібен
// один глобальний session і однорядкова обгортка, як і раніше з startTest().
static void startTest() { session.start(); }

static void switchPage(int page) { display.setPage(page); }

// §51. Відновлюємо стан, але тест НЕ запускаємо — рішення за користувачем.
static bool extraCommand(const char *cmd, char *arg) {
    if (!strcmp(cmd, "?")) {
        Serial.println(F(
            "  prof              показати профіль батареї\n"
            "  prof li|lfp N C   хімія, кількість банок, ємність в Ah\n"
            "  soc               відсоток заряду і звідки він\n"
            "  resume / abort    перерваний тест: відновити / закинути\n"
            "  autostop on|off   завершувати тест самому за напругою і струмом"));
        return true;
    }
    if (!strcmp(cmd, "prof")) {
        if (arg) {
            char *p2 = strchr(arg, ' ');
            if (p2) {
                *p2++ = '\0';
                char *p3 = strchr(p2, ' ');
                if (p3) *p3++ = '\0';
                profile.chemistry = !strcmp(arg, "lfp") ? sys::Chemistry::LIFEPO4
                                                        : sys::Chemistry::LI_ION;
                const int n = atoi(p2);
                profile.cells = (n >= 1 && n <= 8) ? static_cast<uint8_t>(n) : 1;
                profile.capacity_ah = p3 ? atof(p3) : 0.0f;
                profile.v_cutoff_cell = sys::chemCutoff(profile.chemistry);
                sys::saveProfile(profile);
                applyProfile();
            } else {
                Serial.println(F("формат: prof li|lfp <банок> <ємність Ah>"));
            }
        }
        printProfile();
        return true;
    }
    if (!strcmp(cmd, "autostop")) {
        if (arg) session.setAutoStop(!strcmp(arg, "on"));
        Serial.printf("автозупинка %s\n", session.autoStop() ? "увімкнена" : "вимкнена");
        return true;
    }
    if (!strcmp(cmd, "soc")) {
        if (isnan(soc_pct)) Serial.println(F("відсоток ще невідомий"));
        else Serial.printf("заряд %.1f%% за напругою спокою %.4f В\n",
                           soc_pct, soc_ocv);
        const float r = profile.r_internal > 0.0f ? profile.r_internal : r_measured;
        Serial.printf("R внутрішній %.3f Ом (%s)\n", r,
                      profile.r_internal > 0.0f ? "з профілю"
                          : (r_measured > 0.0f ? "заміряно" : "невідомий, поправки немає"));
        if (!isnan(session.socStart())) Serial.printf("на старті тесту було %.1f%%\n", session.socStart());
        if (profile.capacity_ah <= 0.0f)
            Serial.println(F("ємність профілю не задана — кулонометрія недоступна"));
        return true;
    }
    if (!strcmp(cmd, "resume")) {
        if (!session.resume()) { Serial.println(F("перерваного тесту немає")); return true; }
        const sys::TestState &p = session.pending();
        Serial.printf("відновлено: %.5f Ah, %.5f Wh, %.0f с\n",
                      p.ah, p.wh, p.elapsed_s);
        Serial.println(F("тест на паузі — `run` або кнопка в браузері, щоб продовжити"));
        return true;
    }
    if (!strcmp(cmd, "abort")) {
        session.abort();
        Serial.println(F("перерваний тест закинуто, лічильники обнулено"));
        return true;
    }
    return false;
}

static void printNetStatus() {
    if (!web.up()) { Serial.println(F("мережа ще піднімається")); return; }
    Serial.printf("%s \"%s\"  ->  http://%s/  (або http://%s.local/)\n",
                  web.isAp() ? "точка доступу" : "мережа",
                  web.ssid(), web.ip().toString().c_str(), web.hostname());
}

static bool saveWifi(const char *ssid, const char *pass) {
    return ui::WebUi::saveCredentials(ssid, pass);
}

static void printSdStatus() {
    Serial.printf("картка: %s", sdlog.cardPresent() ? "є" : "НЕМАЄ");
    if (sdlog.cardPresent()) Serial.printf(", %llu МБ", sdlog.cardSizeMB());
    Serial.println();
    Serial.printf("лог: %s  тест #%u  записів %u  помилок запису %u\n",
                  sdlog.logging() ? "пишеться" : "зупинено",
                  sdlog.testId(), sdlog.recordsWritten(), sdlog.writeErrors());
    if (!sdlog.cardPresent()) {
        sdlog.remount();
        Serial.println(F("спроба перемонтувати..."));
        return;
    }
    File dir = SD.open("/BATTERY_TESTS");
    if (!dir) return;
    while (File f = dir.openNextFile()) {
        Serial.printf("  %-16s %8u Б\n", f.name(), (unsigned)f.size());
        f.close();
    }
    dir.close();
}

// Чи відповідає ADS1115 по I2C. Перевіряється до старту ядра лише заради
// зрозумілого повідомлення: сама відсутність АЦП обробляється як аварія
// ADC_ERROR, а не як привід щось підмінити.
static bool adsPresent(const meas::CoreConfig &cfg) {
    Wire.begin(cfg.i2c_sda, cfg.i2c_scl, cfg.i2c_hz);
    Wire.beginTransmission(cfg.ads_addr);
    return Wire.endTransmission() == 0;
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println(F("\nBATTERY TESTER — ядро + TFT, етап 2"));

    // Обидва CS у HIGH до першої транзакції, щоб пристрої не заважали
    // один одному на спільній шині.
    pinMode(TFT_CS, OUTPUT); digitalWrite(TFT_CS, HIGH);
    pinMode(SD_CS,  OUTPUT); digitalWrite(SD_CS,  HIGH);

    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, -1);
    spi_bus = xSemaphoreCreateMutex();

    meas::CoreConfig cfg;
    if (!adsPresent(cfg)) {
        Serial.printf("!!! ADS1115 не відповів на 0x%02X (SDA %d, SCL %d)\n",
                      cfg.ads_addr, cfg.i2c_sda, cfg.i2c_scl);
        Serial.println(F("!!! вимірювання неможливе, буде аварія ADC_ERROR"));
        Serial.println(F("!!! перевір живлення модуля, SDA/SCL і ADDR"));
    }

    // Дисплей — до будь-якого звернення до картки (README, ТЗ §33.1).
    ui::TftDisplay::Config dcfg;
    dcfg.button_pin = BTN_PIN;
    if (!display.begin(dcfg, core, spi_bus)) {
        Serial.println(F("TFT: задача не створилась"));   // §64: не привід зупинятись
    }

    // Картка — строго ПІСЛЯ дисплея (README, §33.1) і на тому ж мьютексі.
    storage::SdLogger::Config scfg;
    scfg.cs = SD_CS;
    if (!sdlog.begin(scfg, core, spi_bus)) {
        Serial.println(F("SD: задача логера не створилась"));
    } else if (sdlog.cardPresent()) {
        Serial.printf("SD OK, %llu МБ, наступний тест #%u\n",
                      sdlog.cardSizeMB(), sdlog.testId() + 1);
    } else {
        // §65: відсутня картка не є аварією, лише вимикає логування.
        Serial.println(F("SD: картки немає, логування вимкнене"));
    }

    meas::Calibration cal;
    Serial.println(meas::loadCalibration(cal)
                       ? F("калібрування завантажено з NVS")
                       : F("калібрування в NVS немає — беремо номінали"));

    if (!core.begin(cfg, cal)) {
        Serial.println(F("ядро не піднялось"));
        for (;;) delay(1000);
    }

    if (sys::loadProfile(profile)) Serial.println(F("профіль завантажено з NVS"));
    else Serial.println(F("профілю в NVS немає — Li-ion 1S, ємність невідома"));
    applyProfile();
    printProfile();

    // §51: перерваний тест не продовжується сам.
    session.begin(core, sdlog, profile);
    session.loadPending();
    if (session.hasPending()) {
        const sys::TestState &st = session.pending();
        Serial.printf("\n!!! ПЕРЕРВАНИЙ ТЕСТ #%u: %.5f Ah, %.5f Wh, %.0f с\n",
                      st.test_id, st.ah, st.wh, st.elapsed_s);
        Serial.println(F("!!! `resume` — відновити лічильники, `abort` — закинути"));
    }

    console.begin(core);
    console.setPageHandler(&switchPage);
    console.setSdHook(&printSdStatus);
    console.setNetHook(&printNetStatus);
    console.setCredHandler(&saveWifi);
    console.setCommandHandler(&extraCommand);
    console.setStartHandler(&startTest);
    web.setProfileHandler(&applyProfileFromWeb);
    web.setStartHandler(&startTest);

    // Веб останнім: він нікому не потрібен для вимірювання (§64).
    ui::WebUi::Config wcfg;
    if (!web.begin(wcfg, core, sdlog, spi_bus)) {
        Serial.println(F("web: задача не створилась"));
    }
}

void loop() {
    console.poll();
    web.pump();          // команди з браузера стають діями тут, не в HTTP (§39)

    // Індикатор SD на екрані. Дисплей сам про картку нічого не знає (§33).
    static uint32_t next = 0;
    if (millis() >= next) {
        next = millis() + 500;
        display.setCardPresent(sdlog.cardPresent());
        display.setNetUp(web.up());
        updateSoc();
        display.setSoc(soc_pct);
        display.setInternalR(profile.r_internal > 0.0f ? profile.r_internal
                                                       : r_measured);
        web.setSoc(soc_pct);
        web.setDiag(profile.r_internal > 0.0f ? profile.r_internal : r_measured,
                    soc_ocv);
    }

    // Життєвий цикл тесту (§50 збереження стану, §29 автозавершення,
    // §27 реакція на аварію) — у sys::TestSession, безумовно щоразу.
    session.poll(soc_pct, soc_rest);
}
