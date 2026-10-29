#include <assert.h>
#include <stdio.h>
#include "../focus_event.h"

int main(void) {
    const enum SwipePhase phases[] = {SwipeBegin, SwipeChange, SwipeEnd};
    for (int augment = 0; augment <= 1; augment++) {
        for (int natural = 0; natural <= 1; natural++) {
            for (int direction = -1; direction <= 1; direction += 2) {
                for (int i = 0; i < 3; i++) {
                    CGEventRef event = focus_event_create(phases[i], direction, augment, natural, CGPointMake(50, 60), 1234);
                    assert(event);
                    assert(CGEventGetIntegerValueField(event, kCGEventSourceUserData) == 1234);
                    assert(CGEventGetIntegerValueField(event, kPhase) == phases[i]);
                    double sign = direction * (augment && natural ? -1 : 1);
                    double progress = CGEventGetDoubleValueField(event, kProgress);
                    double velocity = CGEventGetDoubleValueField(event, kVelocityX);
                    assert(progress * sign > 0 && fabs(progress) < .001);
                    assert(i == 2 ? velocity * sign == 9999 : velocity == 0);
                    if (augment) {
                        // Independently inspect the serialized queue and signed
                        // 16.16 values. Nothing in these tests posts gestures.
                        CFDataRef data = CGEventCreateData(NULL, event);
                        const uint8_t *bytes = CFDataGetBytePtr(data);
                        size_t length = (size_t)CFDataGetLength(data), at = 4;
                        bool found = false;
                        while (at + 4 <= length) {
                            size_t count = ((size_t)bytes[at] << 8) | bytes[at+1];
                            unsigned tag = ((unsigned)bytes[at+2] << 8) | bytes[at+3];
                            size_t unit = (tag & 0xf000) == 0x1000 ? 1 : (tag & 0xf000) == 0 ? 8 : 4;
                            assert(count <= (length-at-4)/unit);
                            if (tag == 4205) {
                                const uint8_t *q = bytes+at+4, *fluid = q+28;
                                assert(count == (i == 2 ? 96 : 68));
                                assert(hid_u32(q+24) == (i == 2 ? 2 : 1));
                                assert(hid_u32(fluid) == 40 && hid_u32(fluid+4) == 23);
                                assert(hid_u32(fluid+8) >> 24 == (uint32_t)phases[i]);
                                assert((int32_t)hid_u32(fluid+36) * sign > 0);
                                if (i == 2) assert((int32_t)hid_u32(fluid+40+16) == hid_fixed(velocity));
                                found = true; break;
                            }
                            at += 4 + count*unit;
                        }
                        assert(found); CFRelease(data);
                    }
                    CFRelease(event);
                }
            }
        }
    }
    assert(!focus_event_create(SwipeBegin, 0, true, true, CGPointZero, 0));
    puts("focus event tests passed");
}
