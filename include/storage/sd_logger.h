/*
 * SD Logger (ТЗ §41–§48).
 *
 * Ядро ніколи не пише на картку саме (§42). Воно кладе record у чергу, а ця
 * задача їх забирає, накопичує в RAM і скидає блоком. Запис на картку може
 * заблокуватись на сотні мілісекунд — вимірювання при цьому не зупиняється.
 *
 * Шина SPI спільна з дисплеєм (§33.1), тому кожне звернення до картки
 * береться під той самий мьютекс. Ініціалізація — строго після дисплея.
 *
 * Відсутня картка не є аварією (§65): logging просто вимикається.
 */
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "measurement/core.h"

namespace storage {

// ТЗ §44. Рівно 32 байти: три байти добивки, щоб record не перетинав межу
// сектора і читався з файлу простим індексуванням.
struct LogRecord {
    uint32_t timestamp_ms;   // від старту тесту, не uptime
    float    voltage;
    float    current;
    float    power;
    float    Ah;
    float    Wh;
    float    temperature;
    uint8_t  flags;
    uint8_t  reserved[3];
};
static_assert(sizeof(LogRecord) == 32, "LogRecord має бути 32 байти");

class SdLogger {
public:
    struct Config {
        int8_t   cs = 15;
        uint32_t spi_hz = 16000000;
        uint16_t queue_len = 64;         // §48
        uint8_t  buffer_records = 32;    // скільки накопичити до одного запису
        UBaseType_t task_priority = 1;   // нижче дисплея (2) і ядра (10)
        BaseType_t  task_core = 1;
        uint32_t    task_stack = 5120;   // SD/FAT їдять стек помітно
    };

    // Викликати ПІСЛЯ display.begin(): у зворотному порядку панель не
    // ініціалізується (README, §33.1).
    bool begin(const Config &cfg, meas::MeasurementCore &core, SemaphoreHandle_t bus);

    bool     cardPresent() const { return card_present_; }
    bool     logging() const { return file_open_; }
    uint16_t testId() const { return test_id_; }
    uint32_t recordsWritten() const { return records_written_; }
    uint32_t writeErrors() const { return write_errors_; }
    uint64_t cardSizeMB() const { return card_mb_; }
    uint64_t usedMB() const { return used_mb_; }
    uint64_t freeMB() const { return card_mb_ > used_mb_ ? card_mb_ - used_mb_ : 0; }
    // Перерахунок зайнятого місця обходить FAT, тому робиться лише за подією:
    // при монтуванні, після закриття файлу й після видалення. У гарячому
    // шляху його немає.
    void refreshUsage();
    // Видаляє TEST_XXXX.BIN. Відмовляє, якщо це файл поточного тесту.
    bool deleteTest(uint16_t id);

    // Перемонтувати картку — після того, як її вставили (§65).
    void remount();
    // Причина, з якою файл піде в INDEX.TXT (§58). Вказівник має жити
    // довше за виклик — беремо лише рядкові літерали.
    void setStopReason(const char *r) { stop_reason_ = r; }

private:
    static void taskEntry(void *arg);
    void run();
    bool mount();
    void wakeCard();
    bool openFile();
    void closeFile();
    void flush();
    void appendIndex(const char *reason);

    bool take(uint32_t ms = 2000);
    void give();

    Config cfg_;
    meas::MeasurementCore *core_ = nullptr;
    SemaphoreHandle_t bus_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t  task_ = nullptr;

    LogRecord *buffer_ = nullptr;
    uint8_t    buffered_ = 0;
    uint32_t   first_buffered_ms_ = 0;

    volatile bool card_present_ = false;
    volatile bool file_open_ = false;
    volatile bool remount_requested_ = false;
    uint16_t test_id_ = 0;
    uint64_t test_start_us_ = 0;
    uint64_t card_mb_ = 0;
    uint64_t used_mb_ = 0;
    uint32_t records_written_ = 0;
    uint32_t write_errors_ = 0;
    char     path_[40] = {0};
    const char *stop_reason_ = "stopped";
};

}  // namespace storage
