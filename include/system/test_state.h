/*
 * Збереження стану тесту (ТЗ §50) і відновлення після рестарту (ТЗ §51).
 *
 * Пише не вимірювальне ядро: запис у NVS блокує на кілька мілісекунд, а §71
 * забороняє блокуючі операції в ядрі. Стан знімається з ядра готовим і
 * зберігається з loopTask — задачі пріоритету 1.
 *
 * §51 вимагає прямо: після рестарту тест НЕ продовжується автоматично.
 * Пристрій повідомляє про перерваний тест і чекає рішення — resume або abort.
 */
#pragma once

#include <stdint.h>

namespace sys {

struct TestState {
    uint32_t magic;
    uint16_t version;
    uint16_t test_id;
    uint8_t  active;        // 1 = тест не був закритий штатно
    uint8_t  reserved[3];
    float    ah;
    float    wh;
    float    elapsed_s;
    float    v_start;
    float    v_min;
    float    v_max;
    float    i_max;
    float    t_max;
    float    soc_start;
    uint32_t samples;
    uint32_t crc;
};

// Скільки чекати між збереженнями (§50: «наприклад раз на 30 секунд»).
constexpr uint32_t kSaveIntervalMs = 30000;

bool saveTestState(const TestState &st);
// Повертає true, лише якщо запис цілий: magic, version і CRC збігаються.
bool loadTestState(TestState &out);
bool clearTestState();

}  // namespace sys
