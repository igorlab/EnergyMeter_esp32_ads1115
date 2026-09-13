/*
 * Профіль батареї (ТЗ §59) і оцінка заряду.
 *
 * Відсоток заряду рахується гібридно, і це не ускладнення заради краси:
 *
 *   - напруга спокою (OCV) дає стартову точку, коли струму немає;
 *   - далі рахує кулонометрія: SoC = SoC_старт − Ah/ємність.
 *
 * Чисто за напругою відсоток порахувати не можна. Крива літію між 3.9 і
 * 3.6 В майже плоска: там лежить ~60% ємності в смузі 300 мВ, тобто похибка
 * 30 мВ дає 6% заряду. У LiFePO4 у 60 мВ вміщається 80% ємності — там
 * відсоток за напругою не існує в принципі. Плюс під навантаженням напруга
 * занижена на I·R_вн, а після зняття струму встоюється хвилин десять.
 */
#pragma once

#include <stdint.h>

namespace sys {

enum class Chemistry : uint8_t { LI_ION = 0, LIFEPO4 = 1 };

struct BatteryProfile {
    Chemistry chemistry = Chemistry::LI_ION;
    uint8_t   cells = 1;              // 1S..4S
    float     capacity_ah = 0.0f;     // номінальна; 0 = невідома
    float     v_cutoff_cell = 3.00f;  // робочий cutoff на елемент
    float     i_max = 5.0f;
    float     t_max = 60.0f;
    // Внутрішній опір, Ом. Потрібен, щоб питати криву OCV про напругу
    // СПОКОЮ, а не про напругу на клемі під струмом. 0 = брати заміряний
    // автоматично.
    float     r_internal = 0.0f;
};

// Напруга спокою з показань під струмом. Знак виведений для конвенції
// «додатний струм = розряд»: при розряді клема нижча за OCV, при заряді вища,
// і вираз однаковий для обох напрямків.
inline float ocvFromLoaded(float pack_v, float current, float r_int_cell,
                           uint8_t cells) {
    return pack_v + current * r_int_cell * (cells ? cells : 1);
}

// Межі хімії на елемент: повний заряд, номінал, робочий cutoff,
// абсолютний мінімум.
float chemFull(Chemistry c);
float chemNominal(Chemistry c);
float chemCutoff(Chemistry c);
float chemFloor(Chemistry c);
const char *chemName(Chemistry c);

// Відсоток заряду за напругою СПОКОЮ. pack_v — напруга всієї збірки.
// Повертає 0..100. Валідне лише без струму й після встоювання.
float socFromOcv(const BatteryProfile &p, float pack_v);

// Cutoff і повна напруга для всієї збірки.
inline float packCutoff(const BatteryProfile &p) {
    return p.v_cutoff_cell * p.cells;
}
inline float packFull(const BatteryProfile &p) {
    return chemFull(p.chemistry) * p.cells;
}

bool loadProfile(BatteryProfile &out);
bool saveProfile(const BatteryProfile &p);

}  // namespace sys
