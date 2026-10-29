#ifndef ISS_SETTINGS_H
#define ISS_SETTINGS_H

// Shared schema: field/key, default, minimum, maximum. The app gets this
// schema from --settings-schema at build time; defaults and ranges stay in sync.
#define ISS_SETTINGS(X) \
    X(slideSeconds,         0.060,  0.020, 0.300) \
    X(initialDistance,      0.0625, 0.015, 0.300) \
    X(initialSeconds,       0.040,  0.000, 0.250) \
    X(spaceDistance,        0.160,  0.060, 0.600) \
    X(centerSeconds,        0.150,  0.040, 0.500) \
    X(reverseDistance,      0.040,  0.015, 0.150) \
    X(edgeReverseDistance,  0.040,  0.015, 0.150) \
    X(flickVelocity,        0.080,  0.020, 0.400) \
    X(longSeconds,          0.180,  0.060, 0.500) \
    X(instantFocus,         1.0,    0.0,   1.0)

typedef struct {
#define ISS_FIELD(name, value, low, high) double name;
    ISS_SETTINGS(ISS_FIELD)
#undef ISS_FIELD
} ISSSettings;
#define ISS_DEFAULT(name, value, low, high) .name = value,
static const ISSSettings iss_defaults = { ISS_SETTINGS(ISS_DEFAULT) };
static ISSSettings iss_settings = { ISS_SETTINGS(ISS_DEFAULT) };
#undef ISS_DEFAULT
#endif
