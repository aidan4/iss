#ifndef ISS_FOCUS_EVENT_H
#define ISS_FOCUS_EVENT_H
#include "native_event.h"
#include "iohid.h"

// Build complete synthetic Dock gestures, including the macOS 27 IOHID queue.
// Unlike a finger-following slide, a focus jump uses tiny nonzero progress and
// a high terminal velocity. Build all phases before posting any of them.
static inline CGEventRef focus_event_create(enum SwipePhase phase, int direction,
                                            bool augment, bool natural, CGPoint point,
                                            int64_t tag) {
    if (direction != 1 && direction != -1) return NULL;
    double sign = direction * (augment && natural ? -1.0 : 1.0);
    CGEventRef event = CGEventCreate(NULL);
    if (!event) return NULL;
    CGEventSetIntegerValueField(event, kEventType, kDockControl);
    CGEventSetIntegerValueField(event, kHIDType, kDockSwipe);
    CGEventSetIntegerValueField(event, kMotion, kHorizontal);
    CGEventSetIntegerValueField(event, kPhase, phase);
    CGEventSetDoubleValueField(event, kProgress, sign * 0.0001);
    CGEventSetDoubleValueField(event, (CGEventField)125, 0.1);
    CGEventSetDoubleValueField(event, kVelocityX, phase == SwipeEnd ? sign * 9999.0 : 0);
    CGEventSetDoubleValueField(event, kVelocityY, 0);
    CGEventSetLocation(event, point);
    if (augment) {
        CFDataRef original = CGEventCreateData(NULL, event);
        if (!original) { CFRelease(event); return NULL; }
        if (CFDataGetLength(original) < 4 || memcmp(CFDataGetBytePtr(original), "\0\0\0\2", 4)) {
            CFRelease(original); CFRelease(event); return NULL;
        }
        bool velocity = phase == SwipeEnd;
        uint8_t record[4 + 28 + 40 + 28] = {0};
        size_t size = velocity ? 96 : 68;
        record[1] = (uint8_t)size; record[2] = 0x10; record[3] = 0x6d;
        uint8_t *queue = record + 4, *fluid = queue + 28;
        uint64_t timestamp = CGEventGetTimestamp(event);
        memcpy(queue, &timestamp, 8);
        hid_put32(queue + 24, velocity ? 2 : 1);
        hid_put32(fluid, 40); hid_put32(fluid + 4, 23);
        hid_put32(fluid + 8, (uint32_t)phase << 24);
        hid_put32(fluid + 16, (uint32_t)hid_fixed(0.1));
        fluid[32] = 1; fluid[34] = 3; // horizontal motion, Dock primary
        hid_put32(fluid + 36, (uint32_t)hid_fixed(sign * 0.0001));
        if (velocity) {
            uint8_t *child = fluid + 40;
            hid_put32(child, 28); hid_put32(child + 4, 9); child[12] = 1;
            hid_put32(child + 16, (uint32_t)hid_fixed(sign * 9999.0));
        }
        CFMutableDataRef bytes = CFDataCreateMutableCopy(NULL, 0, original);
        CFRelease(original);
        if (!bytes) { CFRelease(event); return NULL; }
        CFDataAppendBytes(bytes, record, (CFIndex)(size + 4));
        CGEventRef replacement = CGEventCreateFromData(NULL, bytes);
        CFRelease(bytes); CFRelease(event);
        event = replacement;
        if (!event) return NULL;
    }
    // Serialization can lose UserData; tag the final object.
    CGEventSetIntegerValueField(event, kCGEventSourceUserData, tag);
    return event;
}
#endif
