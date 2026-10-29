// Exercise the real slide adapter with synthetic native payloads and a fake
// Space layout. Capture every post: this test never switches the user's desktop.
#include "native_event_fixture.h"

static void capture_post(CGEventTapLocation location, CGEventRef event);
static void capture_tap_post(CGEventTapProxy proxy, CGEventRef event);
static void *fake_lookup(void *handle, const char *name);
#define CGEventPost capture_post
#define CGEventTapPostEvent capture_tap_post
#define dlsym fake_lookup
#define ISS_TEST
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../iss.c"
#pragma clang diagnostic pop
#undef CGEventPost
#undef CGEventTapPostEvent
#undef dlsym

static int fake_index, fake_count = 5;
static CGEventRef posted[128];
static int posted_count;
static bool simulate_commits, defer_space_query;
static int committed_index;
static void capture_post(CGEventTapLocation location, CGEventRef event) {
    assert(location == kCGSessionEventTap && posted_count < 128);
    assert(CGEventGetIntegerValueField(event, kCGEventSourceUserData) == animationTag);
    posted[posted_count++] = CGEventCreateCopy(event);
    if (simulate_commits && CGEventGetIntegerValueField(event,kPhase) == SwipeEnd &&
        CGEventGetIntegerValueField(event,kEventType) == kDockControl) {
        double progress = CGEventGetDoubleValueField(event,kProgress)*release_posted_sign;
        committed_index += progress > 0 ? 1 : -1;
        if (!defer_space_query) fake_index = committed_index;
    }
}
static void capture_tap_post(CGEventTapProxy proxy, CGEventRef event) {
    (void)proxy; capture_post(kCGSessionEventTap, event);
}
static void clear_posts(void) {
    for (int i=0; i<posted_count; i++) CFRelease(posted[i]);
    posted_count = 0;
}
int CGSMainConnectionID(void) { return 1; }
uint64_t CGSGetActiveSpace(int cid) { (void)cid; return 100 + fake_index; }
static uint64_t fake_current(int cid, CFStringRef display) {
    (void)display; return CGSGetActiveSpace(cid);
}
static void *fake_lookup(void *handle, const char *name) {
    (void)handle;
    assert(!strcmp(name, "SLSManagedDisplayGetCurrentSpace"));
    uint64_t (*function)(int, CFStringRef) = fake_current;
    void *pointer;
    memcpy(&pointer, &function, sizeof pointer);
    return pointer;
}
CFArrayRef CGSCopyManagedDisplaySpaces(int cid) {
    (void)cid;
    CFMutableArrayRef list = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    for (int i=0; i<fake_count; i++) {
        int value = 100 + i;
        CFNumberRef id = CFNumberCreate(NULL, kCFNumberIntType, &value);
        const void *keys[] = {CFSTR("id64")}, *values[] = {id};
        CFDictionaryRef space = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFArrayAppendValue(list, space); CFRelease(space); CFRelease(id);
    }
    const void *keys[] = {CFSTR("Display Identifier"), CFSTR("Spaces")};
    const void *values[] = {CFSTR("test-display"), list};
    CFDictionaryRef display = CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *items[] = {display};
    CFArrayRef result = CFArrayCreate(NULL, items, 1, &kCFTypeArrayCallBacks);
    CFRelease(display); CFRelease(list);
    return result;
}
static CGEventRef prepare(int origin, int target) {
    if (spaces.display) CFRelease(spaces.display);
    fake_count = 5; fake_index = origin;
    spaces = read_spaces(CFSTR("test-display"));
    swipe = (Swipe){.index=origin, .count=5, .target=target, .release_only=true, .mode=SwipeWaiting};
    macOS27 = true; rawSign = 1;
    double preview = (target-origin)*slide_target(5, 1)*.8;
    CGEventRef source = fixture(SwipeCancel, .2, 0);
    assert(start_slide(source, (SwipeEvent){.phase=SwipeEnd, .progress=preview}));
    assert(animation.source && posted_count == 0);
    assert(fabs(animation.from-preview) < 1e-9);
    assert(fabs(animation.target-(target-origin)*slide_target(5, 1)) < 1e-9);
    return source;
}
int main(void) {
    for (int origin=0; origin<5; origin++) for (int target=0; target<5; target++) {
        if (abs(target-origin) > 1) continue;
        CGEventRef source = prepare(origin, target);
        double sign = animation.posted_sign;
        CGEventTimestamp before = (CGEventTimestamp)(now_seconds()*1e9);
        finish_slide();
        assert(!animation.source && !animation.finish_frames);
        int distance = abs(target-origin);
        assert(posted_count == 2*(distance > 1 ? distance*2 : 2));
        int ends=0, segment_origin=origin;
        int direction = (target > origin) - (target < origin);
        for (int i=0; i<posted_count; i+=2) {
            CGEventRef frame = posted[i];
            assert(CGEventGetTimestamp(frame) >= before);
            assert(CGEventGetIntegerValueField(posted[i+1], kEventType) == kGesture);
            int phase = (int)CGEventGetIntegerValueField(frame, kPhase);
            // Compare the visible absolute position across native segments.
            // This fails for the old epsilon/velocity-500 catch-up sequence.
            double logical = CGEventGetDoubleValueField(frame,kProgress) * sign;
            assert(fabs(segment_origin + logical/slide_target(5,1) - target) < 1e-5);
            if (phase == SwipeEnd) {
                ends++; segment_origin += direction;
                assert(fabs(CGEventGetDoubleValueField(frame,kVelocityX)) <= 1.0/65536);
            }
            if (!i) {
                assert(phase == SwipeChange);
                assert(fabs(CGEventGetDoubleValueField(frame,kProgress) - sign*(target-origin)*slide_target(5,1)) < 1e-5);
            }
            // Serialized raw payload must agree with the phase and progress
            // presented to the Dock, including Cancel -> Changed conversion.
            CFDataRef bytes = CGEventCreateData(NULL, frame);
            const uint8_t *b = CFDataGetBytePtr(bytes);
            bool found = false;
            for (CFIndex j=4; j+72<=CFDataGetLength(bytes); j++) {
                if (b[j]!=0 || (b[j+1]!=96 && b[j+1]!=68) || b[j+2]!=0x10 || b[j+3]!=0x6d) continue;
                const uint8_t *q = b+j+4;
                assert((hid_u32(q+28+8)>>24) == (unsigned)phase);
                assert((int32_t)hid_u32(q+28+36) == hid_fixed(CGEventGetDoubleValueField(frame,kProgress)));
                if (b[j+1] == 96) {
                    assert(j+100<=CFDataGetLength(bytes));
                    assert((int32_t)hid_u32(q+68+16) == hid_fixed(CGEventGetDoubleValueField(frame,kVelocityX)));
                } else assert(CGEventGetDoubleValueField(frame,kVelocityX) == 0);
                found = true;
            }
            assert(found); CFRelease(bytes);
        }
        assert(ends == distance);
        clear_posts(); CFRelease(source);
    }
    // Replay the captured ten-Space gesture through the actual event callback,
    // including the preview crossing, physical release, timer, and final commit.
    iss_settings.releaseOnly = 1; inputSign = 1;
    for (int lag=0; lag<2; lag++) for (int direction=-1; direction<=1; direction+=2) {
        if (spaces.display) CFRelease(spaces.display);
        int origin = direction > 0 ? 0 : 9;
        fake_count=10; fake_index=committed_index=origin; simulate_commits=true; defer_space_query=lag;
        spaces=read_spaces(CFSTR("test-display"));
        swipe=(Swipe){0};
        CGEventRef begin=fixture(SwipeBegin,direction*.008423,0);
        assert(cb(NULL,kDockControl,begin,NULL)==NULL);
        CFRelease(begin); clear_posts();
        double samples[]={.5,1.0,1.2,1.4,1.551514};
        for (unsigned i=0; i<sizeof samples/sizeof *samples; i++) {
            CGEventRef move=fixture(SwipeChange,direction*samples[i],0);
            assert(cb(NULL,kDockControl,move,NULL)==NULL);
            CFRelease(move); clear_posts();
        }
        assert(committed_index == origin+direction);
        assert(fake_index == origin+(lag ? 0 : direction));
        assert(release_segment.index == committed_index);
        double local = direction*(1.551514-10.0/9);
        assert(fabs(release_segment.position-local)<1e-5);
        CGEventRef release=fixture(SwipeEnd,direction*1.551514,direction*.473602);
        assert(cb(NULL,kDockControl,release,NULL)==NULL);
        assert(posted_count==0 && animation.source);
        assert(fabs(animation.from-local)<1e-5);
        assert(fabs(animation.target-direction*10.0/9)<1e-5);
        assert(animation.finish_count==2);
        double previous=local, sign=animation.posted_sign;
        for (int tick=1; tick<=3; tick++) {
            animation.started=now_seconds()-ISS_SLIDE_SECONDS*tick/4;
            slide_tick(NULL,NULL);
            assert(animation.source);
            double p=CGEventGetDoubleValueField(posted[posted_count-2],kProgress)*sign;
            assert(direction*(p-previous)>=0 && direction*(direction*10.0/9-p)>=0);
            previous=p; clear_posts();
        }
        finish_slide();
        assert(posted_count==4 && committed_index==origin+2*direction);
        clear_posts(); CFRelease(release);
    }
    simulate_commits=false;
    // Layout changes cancel the whole planned commit, never a partial jump.
    CGEventRef source = prepare(0, 1);
    fake_count = 4;
    finish_slide();
    assert(swipe.mode == SwipeFailed && swipe.target == -1);
    assert(posted_count == 2 && CGEventGetIntegerValueField(posted[0], kPhase) == SwipeCancel);
    clear_posts(); CFRelease(source); CFRelease(spaces.display);
    puts("slide adapter tests passed");
}
