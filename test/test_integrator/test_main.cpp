/*
 * Тести інтегратора на хості (ТЗ §69: інтегрування перевіряється окремо).
 * Модуль навмисно не залежить від Arduino, тому рахується без плати:
 *   pio test -e native
 */
#include <unity.h>

#include "measurement/integration.h"

using meas::Integrator;

// 1 А рівно годину при кроці 31.25 мс → рівно 1 Ah.
static void test_constant_current_one_hour() {
    Integrator it;
    const uint64_t step_us = 31250;
    uint64_t t = 1000000;

    it.update(t, 1.0f, 4.2f, true);          // перша точка лише запам'ятовується
    for (uint32_t n = 0; n < 3600u * 32u; n++) {
        t += step_us;
        it.update(t, 1.0f, 4.2f, true);
    }

    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 1.0f, it.Ah());
    TEST_ASSERT_FLOAT_WITHIN(0.0021f, 4.2f, it.Wh());
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 3600.0f, it.elapsedSeconds());
}

// На лінійно спадному струмі трапеція точна: середнє (I0+I1)/2 за годину.
static void test_linear_ramp_is_exact() {
    Integrator it;
    const uint64_t step_us = 1000000;        // 1 с
    uint64_t t = 0;

    it.update(t, 2.0f, 0.0f, true);
    for (uint32_t n = 1; n <= 3600; n++) {
        t += step_us;
        const float i = 2.0f - 2.0f * (static_cast<float>(n) / 3600.0f);   // 2 A → 0 A
        it.update(t, i, 0.0f, true);
    }

    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, it.Ah());   // площа трикутника
}

// Нерівномірний крок: результат не залежить від того, як розбито час.
static void test_uneven_dt() {
    Integrator it;
    uint64_t t = 0;
    const uint64_t steps[] = {10000, 90000, 250000, 5000, 645000};   // сума 1 с

    it.update(t, 1.0f, 1.0f, true);
    for (uint64_t s : steps) {
        t += s;
        it.update(t, 1.0f, 1.0f, true);
    }

    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f / 3600.0f, it.Ah());
}

// Пауза не накопичує і не «доганяє» після поновлення.
static void test_pause_does_not_accumulate() {
    Integrator it;
    uint64_t t = 0;

    it.update(t, 1.0f, 1.0f, true);
    t += 1000000;
    it.update(t, 1.0f, 1.0f, true);          // +1 с під струмом
    const float after_first = it.Ah();

    t += 3600ull * 1000000ull;               // година паузи
    it.update(t, 1.0f, 1.0f, false);
    TEST_ASSERT_FLOAT_WITHIN(1e-9f, after_first, it.Ah());

    t += 1000000;
    it.update(t, 1.0f, 1.0f, true);          // ще 1 с — саме 1 с, не 3601
    TEST_ASSERT_FLOAT_WITHIN(1e-7f, 2.0f / 3600.0f, it.Ah());
}

// Пропуск циклу має бути видимим — на ньому ставиться FLAG_TIME_GAP.
static void test_time_gap_reported() {
    Integrator it;
    it.setMaxDt(125000);
    uint64_t t = 0;

    it.update(t, 1.0f, 1.0f, true);
    t += 31250;
    TEST_ASSERT_FALSE(it.update(t, 1.0f, 1.0f, true));
    t += 500000;
    TEST_ASSERT_TRUE(it.update(t, 1.0f, 1.0f, true));
}

// Час, що йде назад, не повинен віднімати ємність.
static void test_backwards_time_is_ignored() {
    Integrator it;
    it.update(1000000, 1.0f, 1.0f, true);
    it.update(2000000, 1.0f, 1.0f, true);
    const float before = it.Ah();
    it.update(1500000, 1.0f, 1.0f, true);
    TEST_ASSERT_FLOAT_WITHIN(1e-9f, before, it.Ah());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_constant_current_one_hour);
    RUN_TEST(test_linear_ramp_is_exact);
    RUN_TEST(test_uneven_dt);
    RUN_TEST(test_pause_does_not_accumulate);
    RUN_TEST(test_time_gap_reported);
    RUN_TEST(test_backwards_time_is_ignored);
    return UNITY_END();
}

void setUp() {}
void tearDown() {}
