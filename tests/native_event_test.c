#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../native_event.h"
#include "../iohid.h"
#include "../slide.h"

#include "native_event_fixture.h"

static void same_serialized(CGEventRef a, CGEventRef b) {
    CFDataRef da = CGEventCreateData(NULL, a), db = CGEventCreateData(NULL, b);
    assert(da && db && CFEqual(da, db));
    CFRelease(da); CFRelease(db);
}
int main(void) {
    // Normal drag: exact native event bytes survive, including the IOHID blob.
    for (int direction = -1; direction <= 1; direction += 2) {
        for (int major = 26; major <= 27; major++) {
            double sign = swipe_input_sign(major);
            Swipe s = {0};
            SwipeEvent begin = { .phase = SwipeBegin };
            swipe_begin(&s, begin, 1, 3, 0);
            SwipeEvent input = { .phase = SwipeChange, .progress = direction * .2, .velocity = direction * .4 };
            CGEventRef e = fixture(input.phase, input.progress, input.velocity);
            CGEventRef original = CGEventCreateCopy(e);
            SwipeEvent output = swipe_move(&s, input, .1, 1, true);
            assert(apply_edit(e, input, output, sign) == e);
            same_serialized(e, original);
            CFRelease(original); CFRelease(e);

            // Release: ONLY field 129 changes, with the native sign preserved.
            input = (SwipeEvent){ .phase = SwipeEnd, .progress = direction * .2, .velocity = direction * 3 };
            e = fixture(input.phase, input.progress, input.velocity);
            CGEventRef expected = CGEventCreateCopy(e);
            CGEventSetDoubleValueField(expected, kVelocityX, direction * ISS_FINISH_VELOCITY);
            output = swipe_finish(&s, input, .12);
            assert(apply_edit(e, input, output, sign) == e);
            same_serialized(e, expected);
            CFRelease(expected); CFRelease(e);
        }
    }
    // The actual native direction used by the boundary gate: at the first
    // space, positive (next space) passes through and negative is flattened.
    for (int direction = -1; direction <= 1; direction += 2) {
        Swipe s = {0};
        swipe_begin(&s, (SwipeEvent){ .phase = SwipeBegin }, 0, 3, 0);
        SwipeEvent input = { .phase = SwipeChange, .progress = direction * .2, .velocity = direction * .4 };
        CGEventRef e = fixture(input.phase, input.progress, input.velocity);
        CGEventRef expected = CGEventCreateCopy(e);
        if (direction < 0) {
            CGEventSetIntegerValueField(expected, kPhase, SwipeCancel);
            CGEventSetDoubleValueField(expected, kProgress, 0);
            CGEventSetDoubleValueField(expected, kVelocityX, 0);
        }
        SwipeEvent output = swipe_move(&s, input, .1, 0, true);
        assert(apply_edit(e, input, output, 1) == e);
        assert(fabs(CGEventGetDoubleValueField(e, kProgress) - (direction > 0 ? .2 : 0)) < 1e-6);
        // Field 135 aliases progress's float bits; changing 124 changes 135
        // too. Compare against the exact intended edits, not an invented flag.
        same_serialized(e, expected);
        CFRelease(expected); CFRelease(e);
    }
    // Long-swipe phase edits retain all other native metadata, and a dropped
    // sample emits nothing. This covers the adapter missing from the old tests.
    SwipeEvent input = { .phase = SwipeChange, .progress = .65, .velocity = 2 };
    CGEventRef e = fixture(input.phase, input.progress, input.velocity);
    CGEventRef expected = CGEventCreateCopy(e);
    SwipeEvent output = input;
    output.phase = SwipeEnd; output.velocity = ISS_FINISH_VELOCITY;
    CGEventSetIntegerValueField(expected, kPhase, SwipeEnd);
    CGEventSetDoubleValueField(expected, kVelocityX, ISS_FINISH_VELOCITY);
    assert(apply_edit(e, input, output, 1) == e);
    same_serialized(e, expected);
    output.drop = true;
    assert(apply_edit(e, input, output, 1) == NULL);
    CFRelease(expected); CFRelease(e);
    // macOS 27 consumes the embedded phase/progress/velocity as well. An
    // early End must append the missing velocity child to a Changed sample.
    for (int polarity = -1; polarity <= 1; polarity += 2) {
        CGEventRef source = fixture(SwipeChange, polarity * .65, 0);
        CGEventSetDoubleValueField(source, kProgress, .65);
        CGEventSetIntegerValueField(source, kPhase, SwipeEnd);
        CGEventSetDoubleValueField(source, kVelocityX, ISS_FINISH_VELOCITY);
        double raw_sign = 1;
        CGEventRef synced = sync_native_payload(source, .65, 0, &raw_sign);
        assert(synced && raw_sign == polarity);
        CFDataRef bytes = CGEventCreateData(NULL, synced);
        const uint8_t *b = CFDataGetBytePtr(bytes);
        bool found = false;
        for (CFIndex i=4; i+100<=CFDataGetLength(bytes); i++) {
            if (b[i]!=0 || b[i+1]!=96 || b[i+2]!=0x10 || b[i+3]!=0x6d) continue;
            const uint8_t *q=b+i+4;
            assert(hid_u32(q+24)==2);
            assert((hid_u32(q+28+8)>>24)==SwipeEnd);
            int32_t progress, velocity;
            memcpy(&progress,q+28+36,4);
            memcpy(&velocity,q+68+16,4);
            assert(progress==hid_fixed(polarity*.65));
            assert(velocity==hid_fixed(polarity*ISS_FINISH_VELOCITY));
            assert(velocity * (double)polarity > 0); // No fixed-point overflow.
            found=true;
        }
        assert(found);
        // Clearing the real terminal must clear its raw velocity too.
        CGEventSetDoubleValueField(synced,kProgress,0);
        CGEventSetDoubleValueField(synced,kVelocityX,0);
        CGEventRef cleared=sync_native_payload(synced,.65,ISS_FINISH_VELOCITY,&raw_sign);
        assert(cleared);
        CFDataRef cleared_data=CGEventCreateData(NULL,cleared);
        const uint8_t *c=CFDataGetBytePtr(cleared_data);
        for(CFIndex i=4;i+100<=CFDataGetLength(cleared_data);i++) {
            if(c[i]==0 && c[i+1]==96 && c[i+2]==0x10 && c[i+3]==0x6d) {
                assert(hid_u32(c+i+4+68+16)==0);
                assert(hid_u32(c+i+4+28+36)==0);
            }
        }
        CFRelease(cleared_data); CFRelease(cleared); CFRelease(bytes);
        CFRelease(synced); CFRelease(source);
    }
    // Full-boundary commits must survive 16.16 serialization after the space
    // count changes; the minimal terminal velocity must not flush to zero.
    for (int count=2; count<=8; count++) for (int direction=-1; direction<=1; direction+=2) {
        double target=slide_target(count,direction);
        CGEventRef source=fixture(SwipeEnd,target,direction/65536.0);
        double sign=1;
        CGEventRef ended=sync_native_payload(source,target,direction/65536.0,&sign);
        assert(ended);
        CFDataRef data=CGEventCreateData(NULL,ended);
        const uint8_t *b=CFDataGetBytePtr(data);
        bool found=false;
        for(CFIndex i=4;i+100<=CFDataGetLength(data);i++) {
            if(b[i]==0 && b[i+1]==96 && b[i+2]==0x10 && b[i+3]==0x6d) {
                int32_t p,v;
                memcpy(&p,b+i+4+28+36,4); memcpy(&v,b+i+4+68+16,4);
                assert(p==hid_fixed(target) && v==direction);
                found=true;
            }
        }
        assert(found);
        CFRelease(data); CFRelease(ended); CFRelease(source);
    }
    puts("native event tests passed");
}
