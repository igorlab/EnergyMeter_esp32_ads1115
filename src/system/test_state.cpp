#include "system/test_state.h"

#include <Preferences.h>
#include <string.h>

namespace sys {

namespace {
constexpr char kNamespace[] = "battstate";
constexpr char kKey[] = "state";
constexpr uint32_t kMagic = 0x42545354;   // 'BTST'
constexpr uint16_t kVersion = 1;

// CRC-32 (полінома IEEE). Потрібен не від злого умислу, а від обірваного
// на півдорозі запису: NVS може лишити частково оновлений блоб, якщо
// живлення зникло під час flash-операції — а саме в цьому сценарії
// recovery і має спрацювати.
uint32_t crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

uint32_t stateCrc(const TestState &st) {
    // CRC рахується по всьому, крім самого поля crc.
    return crc32(reinterpret_cast<const uint8_t *>(&st),
                 sizeof(TestState) - sizeof(uint32_t));
}
}  // namespace

bool saveTestState(const TestState &in) {
    TestState st = in;
    st.magic = kMagic;
    st.version = kVersion;
    st.reserved[0] = st.reserved[1] = st.reserved[2] = 0;
    st.crc = stateCrc(st);

    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return false;
    const size_t put = prefs.putBytes(kKey, &st, sizeof(st));
    prefs.end();
    return put == sizeof(st);
}

bool loadTestState(TestState &out) {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return false;
    TestState st{};
    const size_t got = prefs.getBytes(kKey, &st, sizeof(st));
    prefs.end();

    if (got != sizeof(st)) return false;
    if (st.magic != kMagic || st.version != kVersion) return false;
    if (st.crc != stateCrc(st)) return false;

    out = st;
    return true;
}

bool clearTestState() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return false;
    const bool ok = prefs.clear();
    prefs.end();
    return ok;
}

}  // namespace sys
