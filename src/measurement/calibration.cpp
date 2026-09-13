#include "measurement/calibration.h"

#include <Preferences.h>
#include <math.h>

namespace meas {

namespace {
constexpr char kNamespace[] = "battcal";
constexpr char kKey[]       = "cal";
constexpr uint32_t kVersion = 2;   // +invert_current, старий запис не читаємо

struct Stored {
    uint32_t version;
    Calibration cal;
};
}  // namespace

bool solveTwoPoint(float measured_1, float real_1,
                   float measured_2, float real_2,
                   float min_span, float &gain, float &offset) {
    const float dm = measured_2 - measured_1;
    if (fabsf(dm) < min_span) return false;

    gain = (real_2 - real_1) / dm;
    offset = real_1 - gain * measured_1;
    return isfinite(gain) && isfinite(offset);
}

bool loadCalibration(Calibration &out) {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return false;

    Stored s{};
    const size_t got = prefs.getBytes(kKey, &s, sizeof(s));
    prefs.end();

    if (got != sizeof(s) || s.version != kVersion) return false;
    // Порожня NVS дає нулі — такий "шунт 0 Ом" зробив би струм нескінченним.
    if (!(s.cal.divider_k > 0.0f) || !(s.cal.shunt_ohms > 0.0f)) return false;

    out = s.cal;
    return true;
}

bool saveCalibration(const Calibration &cal) {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return false;

    Stored s{kVersion, cal};
    const size_t put = prefs.putBytes(kKey, &s, sizeof(s));
    prefs.end();
    return put == sizeof(s);
}

bool eraseCalibration() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return false;
    const bool ok = prefs.clear();
    prefs.end();
    return ok;
}

}  // namespace meas
