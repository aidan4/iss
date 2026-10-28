#ifndef ISS_SETTINGS_IO_H
#define ISS_SETTINGS_IO_H
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <math.h>
#include "settings.h"

// Atomic writers replace the file, so each read sees one complete revision.
// A malformed file preserves the current settings. Missing/invalid individual
// fields use defaults. No values can bypass the engine's supported ranges.
static inline bool settings_read(const char *path, ISSSettings *result) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    unsigned char bytes[65537];
    size_t count = fread(bytes, 1, sizeof(bytes), file);
    bool valid = !ferror(file) && count > 0 && count < sizeof(bytes);
    fclose(file);
    if (!valid) return false;
    CFDataRef data = CFDataCreate(NULL, bytes, (CFIndex)count);
    CFPropertyListRef plist = CFPropertyListCreateWithData(NULL, data,
        kCFPropertyListImmutable, NULL, NULL);
    CFRelease(data);
    if (!plist) return false;
    if (CFGetTypeID(plist) != CFDictionaryGetTypeID()) { CFRelease(plist); return false; }
    ISSSettings candidate = iss_defaults;
#define ISS_READ(name, value, low, high) { \
    CFTypeRef item = CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR(#name)); \
    double number; \
    if (item && CFGetTypeID(item) == CFNumberGetTypeID() && \
        CFNumberGetValue(item, kCFNumberDoubleType, &number) && isfinite(number)) \
        candidate.name = fmax(low, fmin(high, number)); \
}
    ISS_SETTINGS(ISS_READ)
#undef ISS_READ
    CFRelease(plist);
    *result = candidate;
    return true;
}
static inline void settings_print_schema(void) {
    const char *separator = "";
    printf("{\n");
#define ISS_PRINT(name, value, low, high) \
    printf("%s  \"%s\": {\"defaultValue\": %.8g, \"minimum\": %.8g, \"maximum\": %.8g}", \
           separator, #name, (double)value, (double)low, (double)high); separator = ",\n";
    ISS_SETTINGS(ISS_PRINT)
#undef ISS_PRINT
    printf("\n}\n");
}
#endif
