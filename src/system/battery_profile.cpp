#include "system/battery_profile.h"

#include <Preferences.h>
#include <math.h>

namespace sys {

namespace {
constexpr char kNamespace[] = "battprof";
constexpr char kKey[] = "prof";
// 2: додано r_internal. Версію треба піднімати при КОЖНІЙ зміні структури —
// інакше старий запис не підходить за розміром, профіль тихо скидається на
// дефолти, і користувач втрачає налаштування без жодного повідомлення.
constexpr uint32_t kVersion = 2;

struct Stored { uint32_t version; BatteryProfile p; };

// Крива напруги спокою на елемент, від 100% до 0% з кроком 10%
// (плюс 5% і 0% окремо, бо там крива обвалюється).
// Це узагальнені криві, а не паспорт конкретного елемента: реальні
// відрізняються, і точну криву прилад зможе побудувати сам після повного
// розряду з паузами на встоювання.
struct Point { float soc; float v; };

const Point kLiIon[] = {
    {100, 4.20f}, {90, 4.08f}, {80, 3.99f}, {70, 3.92f}, {60, 3.86f},
    {50, 3.81f},  {40, 3.77f}, {30, 3.73f}, {20, 3.68f}, {10, 3.58f},
    {5, 3.48f},   {0, 3.20f},
};

// LiFePO4: між 90% і 20% усього 120 мВ. Таблиця тут потрібна не щоб
// рахувати відсоток, а щоб було видно, чому за напругою його не порахуєш.
const Point kLiFePO4[] = {
    {100, 3.45f}, {90, 3.35f}, {80, 3.32f}, {70, 3.30f}, {60, 3.29f},
    {50, 3.28f},  {40, 3.27f}, {30, 3.26f}, {20, 3.23f}, {10, 3.15f},
    {5, 3.05f},   {0, 2.80f},
};

}  // namespace

float chemFull(Chemistry c)    { return c == Chemistry::LIFEPO4 ? 3.65f : 4.20f; }
float chemNominal(Chemistry c) { return c == Chemistry::LIFEPO4 ? 3.20f : 3.70f; }
float chemCutoff(Chemistry c)  { return c == Chemistry::LIFEPO4 ? 2.50f : 3.00f; }
float chemFloor(Chemistry c)   { return c == Chemistry::LIFEPO4 ? 2.00f : 2.50f; }

const char *chemName(Chemistry c) {
    return c == Chemistry::LIFEPO4 ? "LiFePO4" : "Li-ion";
}

float socFromOcv(const BatteryProfile &p, float pack_v) {
    const uint8_t cells = p.cells ? p.cells : 1;
    const float v = pack_v / cells;

    const Point *tab = (p.chemistry == Chemistry::LIFEPO4) ? kLiFePO4 : kLiIon;
    const uint8_t n = 12;

    if (v >= tab[0].v) return 100.0f;
    if (v <= tab[n - 1].v) return 0.0f;

    // Таблиця йде від високої напруги до низької.
    for (uint8_t k = 1; k < n; k++) {
        if (v >= tab[k].v) {
            const float t = (v - tab[k].v) / (tab[k - 1].v - tab[k].v);
            return tab[k].soc + t * (tab[k - 1].soc - tab[k].soc);
        }
    }
    return 0.0f;
}

bool loadProfile(BatteryProfile &out) {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return false;
    Stored s{};
    const size_t got = prefs.getBytes(kKey, &s, sizeof(s));
    prefs.end();
    if (got != sizeof(s) || s.version != kVersion) return false;
    if (s.p.cells < 1 || s.p.cells > 8) return false;
    out = s.p;
    return true;
}

bool saveProfile(const BatteryProfile &p) {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return false;
    Stored s{kVersion, p};
    const size_t put = prefs.putBytes(kKey, &s, sizeof(s));
    prefs.end();
    return put == sizeof(s);
}

}  // namespace sys
