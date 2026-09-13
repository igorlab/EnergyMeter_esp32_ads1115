#include "measurement/ads1115.h"

namespace ads {

namespace {

constexpr uint8_t REG_CONVERSION = 0x00;
constexpr uint8_t REG_CONFIG     = 0x01;

constexpr uint16_t CFG_OS_SINGLE = 1 << 15;  // запис 1 стартує конверсію
constexpr uint16_t CFG_MODE_SINGLE = 1 << 8;
constexpr uint16_t CFG_COMP_DISABLE = 0x0003;  // COMP_QUE = 11: компаратор вимкнено

const float kFullScale[6] = {6.144f, 4.096f, 2.048f, 1.024f, 0.512f, 0.256f};

// 1/DR у мікросекундах.
const uint32_t kConvUs[8] = {
    125000, 62500, 31250, 15625, 7813, 4000, 2105, 1163,
};

}  // namespace

float pgaFullScale(Pga p) {
    const uint8_t i = static_cast<uint8_t>(p);
    return kFullScale[i < 6 ? i : 5];
}

uint32_t rateConversionUs(Rate r) {
    return kConvUs[static_cast<uint8_t>(r) & 0x07];
}

bool Ads1115::begin(TwoWire &wire, uint8_t addr) {
    wire_ = &wire;
    addr_ = addr;

    // Перевірка присутності: config-регістр після reset читається як 0x8583.
    uint16_t cfg = 0;
    if (!read16(REG_CONFIG, cfg)) return false;
    return true;
}

bool Ads1115::write16(uint8_t reg, uint16_t value) {
    if (!wire_) return false;
    wire_->beginTransmission(addr_);
    wire_->write(reg);
    wire_->write(static_cast<uint8_t>(value >> 8));
    wire_->write(static_cast<uint8_t>(value & 0xFF));
    return wire_->endTransmission() == 0;
}

bool Ads1115::read16(uint8_t reg, uint16_t &value) {
    if (!wire_) return false;
    wire_->beginTransmission(addr_);
    wire_->write(reg);
    if (wire_->endTransmission() != 0) return false;
    if (wire_->requestFrom(static_cast<int>(addr_), 2) != 2) return false;
    const uint8_t hi = wire_->read();
    const uint8_t lo = wire_->read();
    value = (static_cast<uint16_t>(hi) << 8) | lo;
    return true;
}

bool Ads1115::startSingle(Mux mux, Pga pga, Rate rate) {
    const uint16_t cfg = CFG_OS_SINGLE
                       | (static_cast<uint16_t>(mux)  << 12)
                       | (static_cast<uint16_t>(pga)  << 9)
                       | CFG_MODE_SINGLE
                       | (static_cast<uint16_t>(rate) << 5)
                       | CFG_COMP_DISABLE;
    return write16(REG_CONFIG, cfg);
}

bool Ads1115::waitReady(Rate rate, uint32_t extra_timeout_us) {
    // Основну частину конверсії просто спимо: тримати шину опитуванням немає
    // сенсу, а задача вимірювання все одно найпріоритетніша і прокинеться вчасно.
    const uint32_t conv_us = rateConversionUs(rate);
    const uint32_t sleep_ms = conv_us / 1000;   // ціла частина, решта — на опитування
    if (sleep_ms > 0) vTaskDelay(pdMS_TO_TICKS(sleep_ms));

    const uint32_t deadline = micros() + conv_us + extra_timeout_us;
    for (;;) {
        uint16_t cfg = 0;
        if (!read16(REG_CONFIG, cfg)) return false;
        if (cfg & CFG_OS_SINGLE) return true;          // OS=1 → конверсія завершена
        if (static_cast<int32_t>(micros() - deadline) >= 0) return false;
        vTaskDelay(1);
    }
}

bool Ads1115::readRaw(int16_t &out) {
    uint16_t v = 0;
    if (!read16(REG_CONVERSION, v)) return false;
    out = static_cast<int16_t>(v);
    return true;
}

bool Ads1115::convert(Mux mux, Pga pga, Rate rate, int16_t &out) {
    if (!startSingle(mux, pga, rate)) return false;
    if (!waitReady(rate)) return false;
    return readRaw(out);
}

}  // namespace ads
