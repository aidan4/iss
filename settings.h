#ifndef ISS_SETTINGS_H
#define ISS_SETTINGS_H
#include <stdbool.h>
#include <math.h>

enum ISSSettingDomain { ISSPositive, ISSNonnegative, ISSToggle };
static inline bool settings_value_valid(double value, enum ISSSettingDomain domain) {
    return isfinite(value) && (domain == ISSToggle ? (value == 0 || value == 1) :
                              domain == ISSNonnegative ? value >= 0 : value > 0);
}
static inline const char *settings_domain_name(enum ISSSettingDomain domain) {
    return domain == ISSToggle ? "toggle" : domain == ISSNonnegative ? "nonnegative" : "positive";
}
// Shared schema: key, default, SLIDER range, valid numeric domain. Text fields
// may use any finite value in that domain, independently of slider ranges.
#define ISS_SETTINGS(X) \
    X(slideSeconds,         0.060,  0.020, 0.300, ISSPositive) \
    X(initialDistance,      0.0625, 0.015, 0.300, ISSPositive) \
    X(initialSeconds,       0.040,  0.000, 0.250, ISSNonnegative) \
    X(spaceDistance,        0.160,  0.060, 0.600, ISSPositive) \
    X(centerSeconds,        0.150,  0.040, 0.500, ISSNonnegative) \
    X(reverseDistance,      0.040,  0.015, 0.150, ISSPositive) \
    X(edgeReverseDistance,  0.040,  0.015, 0.150, ISSPositive) \
    X(flickVelocity,        0.080,  0.020, 0.400, ISSNonnegative) \
    X(longSeconds,          0.180,  0.060, 0.500, ISSNonnegative) \
    X(instantFocus,         1.0,    0.0,   1.0,   ISSToggle) \
    X(releaseOnly,          0.0,    0.0,   1.0,   ISSToggle) \
    X(gestureMultiplier,    1.0,    0.1,   5.0,   ISSPositive) \
    X(gestureAcceleration,  0.0,    0.0,   5.0,   ISSNonnegative)

typedef struct {
#define ISS_FIELD(name, value, low, high, domain) double name;
    ISS_SETTINGS(ISS_FIELD)
#undef ISS_FIELD
} ISSSettings;
#define ISS_DEFAULT(name, value, low, high, domain) .name = value,
static const ISSSettings iss_defaults = { ISS_SETTINGS(ISS_DEFAULT) };
static ISSSettings iss_settings = { ISS_SETTINGS(ISS_DEFAULT) };
#undef ISS_DEFAULT
#endif
