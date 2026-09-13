#include "storage/sd_logger.h"

#include <Preferences.h>
#include <SD.h>
#include <SPI.h>

using namespace meas;

namespace storage {

namespace {
constexpr char kDir[]      = "/BATTERY_TESTS";
constexpr char kIndex[]    = "/BATTERY_TESTS/INDEX.TXT";
constexpr char kNamespace[] = "battlog";
constexpr char kKeyTest[]   = "testid";

// Скільки record можуть чекати в RAM, перш ніж їх запишуть попри неповний
// буфер. Компроміс: більше — менше звернень до картки, але більше втратиться
// при раптовій втраті живлення.
constexpr uint32_t kMaxHoldMs = 30000;
}  // namespace

bool SdLogger::take(uint32_t ms) {
    if (!bus_) return true;
    return xSemaphoreTake(bus_, pdMS_TO_TICKS(ms)) == pdTRUE;
}

void SdLogger::give() {
    if (bus_) xSemaphoreGive(bus_);
}

// Пробудження картки перед SD.begin(). Без цього стара SDSC лишається в
// проміжному стані, віддає R1 = 0x05 замість 0x01, і f_mount падає з error 3.
// Послідовність перевірена на цій платі — подробиці в README.
void SdLogger::wakeCard() {
    SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));

    digitalWrite(cfg_.cs, HIGH);
    for (uint8_t i = 0; i < 10; i++) SPI.transfer(0xFF);   // 80 тактів

    digitalWrite(cfg_.cs, LOW);
    SPI.transfer(0xFF);
    SPI.transfer(0x40);                                    // CMD0
    SPI.transfer(0x00); SPI.transfer(0x00);
    SPI.transfer(0x00); SPI.transfer(0x00);
    SPI.transfer(0x95);
    for (uint8_t i = 0; i < 16; i++) if (!(SPI.transfer(0xFF) & 0x80)) break;
    digitalWrite(cfg_.cs, HIGH);
    SPI.transfer(0xFF);

    digitalWrite(cfg_.cs, LOW);
    SPI.transfer(0xFF);
    SPI.transfer(0x48);                                    // CMD8
    SPI.transfer(0x00); SPI.transfer(0x00);
    SPI.transfer(0x01); SPI.transfer(0xAA);
    SPI.transfer(0x87);
    for (uint8_t i = 0; i < 16; i++) if (!(SPI.transfer(0xFF) & 0x80)) break;
    for (uint8_t i = 0; i < 4; i++) SPI.transfer(0xFF);
    digitalWrite(cfg_.cs, HIGH);
    SPI.transfer(0xFF);

    SPI.endTransaction();
}

bool SdLogger::mount() {
    if (!take(3000)) return false;

    SD.end();
    wakeCard();

    bool ok = false;
    for (uint8_t attempt = 1; attempt <= 3 && !ok; attempt++) {
        ok = SD.begin(cfg_.cs, SPI, cfg_.spi_hz);
        if (!ok) { SD.end(); vTaskDelay(pdMS_TO_TICKS(300)); }
    }

    if (ok) {
        card_mb_ = SD.cardSize() / (1024ULL * 1024ULL);
        if (!SD.exists(kDir)) SD.mkdir(kDir);
        used_mb_ = SD.usedBytes() / (1024ULL * 1024ULL);
    }
    give();

    card_present_ = ok;
    return ok;
}

void SdLogger::refreshUsage() {
    if (!card_present_) return;
    if (!take(3000)) return;
    used_mb_ = SD.usedBytes() / (1024ULL * 1024ULL);
    give();
}

bool SdLogger::deleteTest(uint16_t id) {
    if (!card_present_) return false;
    char path[40];
    snprintf(path, sizeof(path), "%s/TEST_%04u.BIN", kDir, id);
    // Файл поточного тесту не віддаємо: логер тримає його відкритим порціями,
    // і видалення посеред запису лишило б битий FAT.
    if (file_open_ && strcmp(path, path_) == 0) return false;

    if (!take(3000)) return false;
    const bool ok = SD.remove(path);
    if (ok) used_mb_ = SD.usedBytes() / (1024ULL * 1024ULL);
    give();
    return ok;
}

void SdLogger::remount() { remount_requested_ = true; }

bool SdLogger::begin(const Config &cfg, MeasurementCore &core, SemaphoreHandle_t bus) {
    cfg_ = cfg;
    core_ = &core;
    bus_ = bus;

    pinMode(cfg_.cs, OUTPUT);
    digitalWrite(cfg_.cs, HIGH);

    buffer_ = static_cast<LogRecord *>(malloc(sizeof(LogRecord) * cfg_.buffer_records));
    if (!buffer_) return false;

    queue_ = xQueueCreate(cfg_.queue_len, sizeof(Measurement));
    if (!queue_) return false;
    core_->setSink(queue_);

    // Номер тесту переживає перезавантаження (§49).
    Preferences prefs;
    if (prefs.begin(kNamespace, true)) {
        test_id_ = prefs.getUShort(kKeyTest, 0);
        prefs.end();
    }

    mount();   // відсутня картка — не привід не стартувати (§65)

    return xTaskCreatePinnedToCore(&SdLogger::taskEntry, "sdlog", cfg_.task_stack,
                                   this, cfg_.task_priority, &task_,
                                   cfg_.task_core) == pdPASS;
}

void SdLogger::taskEntry(void *arg) { static_cast<SdLogger *>(arg)->run(); }

bool SdLogger::openFile() {
    if (!card_present_) return false;

    Preferences prefs;
    if (prefs.begin(kNamespace, false)) {
        test_id_ = static_cast<uint16_t>(test_id_ + 1);
        prefs.putUShort(kKeyTest, test_id_);
        prefs.end();
    }

    snprintf(path_, sizeof(path_), "%s/TEST_%04u.BIN", kDir, test_id_);

    if (!take()) return false;
    File f = SD.open(path_, FILE_WRITE);
    const bool ok = static_cast<bool>(f);
    if (ok) f.close();
    give();

    if (!ok) { write_errors_++; return false; }

    buffered_ = 0;
    records_written_ = 0;
    test_start_us_ = 0;
    file_open_ = true;
    return true;
}

// Один блочний запис на кілька десятків record (§48).
void SdLogger::flush() {
    if (!buffered_ || !card_present_) { buffered_ = 0; return; }

    if (!take()) { write_errors_++; return; }
    File f = SD.open(path_, FILE_APPEND);
    if (f) {
        const size_t bytes = sizeof(LogRecord) * buffered_;
        if (f.write(reinterpret_cast<uint8_t *>(buffer_), bytes) == bytes) {
            records_written_ += buffered_;
        } else {
            write_errors_++;      // §64: подія в діагностику, вимірювання йде далі
        }
        f.close();
    } else {
        write_errors_++;
    }
    give();
    buffered_ = 0;
}

void SdLogger::appendIndex(const char *reason) {
    if (!card_present_) return;
    if (!take()) return;
    File f = SD.open(kIndex, FILE_APPEND);
    if (f) {
        f.printf("TEST_%04u.BIN  records=%u  errors=%u  %s\n",
                 test_id_, records_written_, write_errors_, reason);
        f.close();
    }
    give();
}

void SdLogger::closeFile() {
    if (!file_open_) return;
    flush();
    appendIndex(stop_reason_);
    stop_reason_ = "stopped";
    file_open_ = false;
    refreshUsage();
}

void SdLogger::run() {
    Measurement m;

    for (;;) {
        if (remount_requested_) {
            remount_requested_ = false;
            if (file_open_) file_open_ = false;
            mount();
        }

        if (xQueueReceive(queue_, &m, pdMS_TO_TICKS(500)) != pdTRUE) {
            // Тиша в черзі сама по собі не привід писати: при 1 записі на
            // секунду таймаут спрацьовує між кожною парою record, і блочний
            // запис §48 виродився б у відкриття файлу щосекунди. Зливаємо
            // лише те, що залежалось.
            if (file_open_ && buffered_ &&
                millis() - first_buffered_ms_ > kMaxHoldMs) flush();
            continue;
        }

        const bool running = (m.flags & FLAG_INTEGRATING) != 0;

        if (running && !file_open_) {
            if (!openFile()) continue;
            test_start_us_ = m.timestamp_us;
        }
        if (!running) {
            if (file_open_) closeFile();
            continue;
        }

        if (buffered_ == 0) first_buffered_ms_ = millis();

        LogRecord &r = buffer_[buffered_++];
        r.timestamp_ms = static_cast<uint32_t>((m.timestamp_us - test_start_us_) / 1000ULL);
        r.voltage = m.voltage;
        r.current = m.current;
        r.power   = m.power;
        r.Ah      = m.Ah;
        r.Wh      = m.Wh;
        r.temperature = m.temperature;
        r.flags   = m.flags;
        r.reserved[0] = r.reserved[1] = r.reserved[2] = 0;

        if (buffered_ >= cfg_.buffer_records) flush();
    }
}

}  // namespace storage
