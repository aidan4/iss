#ifndef ISS_TEST_NATIVE_FIXTURE_H
#define ISS_TEST_NATIVE_FIXTURE_H
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../native_event.h"
#include "../iohid.h"

static CGEventRef fixture(enum SwipePhase phase, double progress, double velocity) {
    CGEventRef e = CGEventCreate(NULL);
    assert(e);
    CGEventSetIntegerValueField(e, kEventType, kDockControl);
    CGEventSetIntegerValueField(e, kHIDType, kDockSwipe);
    CGEventSetIntegerValueField(e, kMotion, kHorizontal);
    CGEventSetIntegerValueField(e, kPhase, phase);
    CGEventSetDoubleValueField(e, kProgress, progress);
    CGEventSetDoubleValueField(e, kVelocityX, velocity);
    CGEventSetDoubleValueField(e, (CGEventField)125, .17);
    CGEventSetDoubleValueField(e, (CGEventField)126, .83);
    CGEventSetIntegerValueField(e, kCGEventSourceUserData, 1234567);
    CGEventSetTimestamp(e, 1000000000);
    CGEventSetLocation(e, CGPointMake(200, 300));

    // Give the event an opaque raw IOHID queue payload (tag 4205). The native
    // path must keep the driver's payload, rather than generating a new one
    // with the synthetic direction convention. No events are posted by tests.
    CFDataRef data = CGEventCreateData(NULL, e);
    CFMutableDataRef bytes = CFDataCreateMutableCopy(NULL, 0, data);
    uint8_t record[4 + 28 + 40] = { 0, 68, 0x10, 0x6d };
    record[4 + 24] = 1; // one event in the queue
    record[4 + 28] = 40; // fluid-touch record size
    record[4 + 28 + 4] = 23; // fluid-touch event type
    record[4 + 28 + 11] = (uint8_t)phase;
    record[4 + 28 + 32] = 1; // horizontal motion
    record[4 + 28 + 34] = 3; // Dock-primary flavor
    hid_put32(record+4+28+36, (uint32_t)hid_fixed(progress));
    CFDataAppendBytes(bytes, record, sizeof(record));
    CGEventRef result = CGEventCreateFromData(NULL, bytes);
    assert(result);
    CFDataRef roundtrip = CGEventCreateData(NULL, result);
    bool kept_payload = false;
    for (CFIndex i = 0; i + (CFIndex)sizeof(record) <= CFDataGetLength(roundtrip); i++) {
        if (!memcmp(CFDataGetBytePtr(roundtrip) + i, record, sizeof(record))) kept_payload = true;
    }
    assert(kept_payload);
    CFRelease(roundtrip);
    CFRelease(bytes); CFRelease(data); CFRelease(e);
    return result;
}
#endif
