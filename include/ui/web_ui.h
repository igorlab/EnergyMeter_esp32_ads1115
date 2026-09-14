/*
 * Веб-інтерфейс (ТЗ §35–§39).
 *
 * Поки що та частина, яка стосується архіву: перелік тестів (§38 /tests),
 * вміст окремого тесту (§38 /test?id=), CSV-експорт (§46) і /api/status (§37).
 * Realtime WebSocket (§36) і графіки (§40) — окремим кроком.
 *
 * Обмеження, які тут дотримані:
 *   - HTTP-задача живе на ядрі 0, разом зі стеком Wi-Fi, подалі від
 *     вимірювального ядра на ядрі 1 (§3, §24);
 *   - HTTP-обробник нічого не запускає сам, лише читає готові дані (§36);
 *   - будь-яке звернення до картки береться під мьютекс шини SPI (§33.1),
 *     і файл віддається порціями, щоб не тримати шину весь час передачі.
 */
#pragma once

#include <IPAddress.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "measurement/core.h"
#include "storage/sd_logger.h"

namespace ui {

class WebUi {
public:
    struct Config {
        const char *ap_ssid = "BatteryTester";
        const char *ap_pass = nullptr;   // nullptr = відкрита мережа
        // mDNS: http://<hostname>.local/ — працює і в режимі AP, і в
        // домашній мережі, тож адресу не треба щоразу шукати заново.
        // Підтримується "з коробки" на macOS/iOS (Bonjour); на Android і
        // Windows потрібен окремий клієнт mDNS — там лишається IP.
        const char *hostname = "battery-tester";
        uint16_t    port = 80;
        uint32_t    sta_timeout_ms = 8000;
        UBaseType_t task_priority = 3;
        BaseType_t  task_core = 0;       // 0 — там же, де Wi-Fi та lwIP
        uint32_t    task_stack = 8192;
    };

    bool begin(const Config &cfg, meas::MeasurementCore &core,
               storage::SdLogger &sd, SemaphoreHandle_t bus);

    // §39: HTTP-обробник не виконує керування сам, а лише кладе команду в
    // чергу. Застосовує її pump(), який викликається з loopTask — задачі
    // пріоритету 1, нижчої і за ядро, і за дисплей. Так навіть зависла
    // мережа не може ні запустити, ні зупинити тест.
    void pump();

    // Відсоток заряду рахує system-шар (§52), веб лише показує.
    void setSoc(float pct) { soc_ = pct; }
    // Внутрішній опір і напруга спокою, з якої взято відсоток. Рахує
    // system-шар, веб лише показує (§52).
    void setDiag(float r_int, float ocv) { r_int_ = r_int; ocv_ = ocv; }
    void setProfile(const char *name, float cutoff_v, float capacity_ah,
                    uint8_t cells, float floor_v, float imax, float tmax,
                    float r_profile) {
        prof_name_ = name; prof_cutoff_ = cutoff_v; prof_cap_ = capacity_ah;
        prof_cells_ = cells; prof_floor_ = floor_v;
        prof_imax_ = imax; prof_tmax_ = tmax; prof_r_ = r_profile;
    }

    // Зміна профілю з веба: сам WebUi профілю не володіє (§52), тому віддає
    // її тому, хто володіє — system-шару в main.
    using ProfileHandler = bool (*)(const char *chem, int cells, float cap,
                                    float imax, float tmax, float r_int);
    void setProfileHandler(ProfileHandler h) { prof_cb_ = h; }
    using StartHandler = void (*)();
    void setStartHandler(StartHandler h) { start_cb_ = h; }

    bool      up() const { return up_; }
    bool      isAp() const { return ap_mode_; }
    IPAddress ip() const;
    const char *ssid() const { return ssid_; }
    const char *hostname() const { return cfg_.hostname; }

    // Облікові дані домашньої мережі в NVS (§49). Порожній ssid — стерти.
    static bool saveCredentials(const char *ssid, const char *pass);
    static bool loadCredentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

private:
    enum Cmd : uint8_t { CMD_START, CMD_STOP, CMD_RESET };

    static void taskEntry(void *arg);
    void run();
    bool connectSta();
    void startAp();
    void routes();

    void handleRoot();
    void handleTests();
    void handleTest();
    void handleCsv();
    void handleStatus();
    void handleCommand(Cmd c);
    void handleSettingsPage();
    void handleSettingsGet();
    void handleSettingsPost();
    void handleNvs();
    void handleCalPoint();
    void handleRemove();
    void handleBin();
    void streamLog(const char *name, bool csv);

    bool take(uint32_t ms = 2000);
    void give();

    Config cfg_;
    meas::MeasurementCore *core_ = nullptr;
    storage::SdLogger *sd_ = nullptr;
    SemaphoreHandle_t bus_ = nullptr;
    TaskHandle_t task_ = nullptr;
    QueueHandle_t cmds_ = nullptr;
    volatile float soc_ = NAN;
    volatile float r_int_ = 0.0f;
    volatile float ocv_ = NAN;
    const char *prof_name_ = "?";
    float prof_cutoff_ = 0.0f, prof_cap_ = 0.0f;
    uint8_t prof_cells_ = 1;
    float prof_floor_ = 0.0f, prof_imax_ = 0.0f, prof_tmax_ = 0.0f, prof_r_ = 0.0f;
    ProfileHandler prof_cb_ = nullptr;
    StartHandler start_cb_ = nullptr;

    // Слоти двоточкового калібрування §54, свої для веба.
    float cal_m_[2][2] = {{0, 0}, {0, 0}};   // [канал][точка], виміряне
    float cal_r_[2][2] = {{0, 0}, {0, 0}};   // еталон
    bool  cal_have_[2][2] = {{false, false}, {false, false}};
    volatile bool up_ = false;
    volatile bool ap_mode_ = false;
    char ssid_[33] = {0};
};

}  // namespace ui
