// iss — finger-following space swipes with fast landings and clamped edges.
// Keep the real native event and its IOHID metadata. Synthetic macOS 27
// gestures have a separate, scroll-preference-dependent direction encoding.

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <dlfcn.h>
#include "native_event.h"
#include "iohid.h"
#include "slide.h"

extern int CGSMainConnectionID(void);
extern CFArrayRef CGSCopyManagedDisplaySpaces(int cid);
extern uint64_t CGSGetActiveSpace(int cid);

typedef struct {
    CFStringRef display;
    uint64_t order;
    int index, count;
} SpaceInfo;

static CFMachPortRef tap;
static Swipe swipe;
static SpaceInfo spaces = { .index = -1 };
static bool tap_trusted, tap_enabled, probe, verbose, suppressCompanion;
static double inputSign, nextSpaceCheck;
static int observedIndex;
static double rawSign = 1;
static bool macOS27;
static const int64_t animationTag = 0x495353534c494445LL;
static struct {
    CGEventRef source, companion, cleanup;
    CFRunLoopTimerRef timer;
    double started, from, target, posted_sign;
} animation;
static bool sameSpaces;
static volatile sig_atomic_t running = 1;

static double now_seconds(void) {
    static mach_timebase_info_data_t timebase;
    if (!timebase.denom) mach_timebase_info(&timebase);
    return (double)mach_continuous_time() * timebase.numer / timebase.denom / 1e9;
}

static uint64_t space_id(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFDictionaryGetTypeID()) return 0;
    CFDictionaryRef dict = value;
    CFNumberRef number = CFDictionaryGetValue(dict, CFSTR("id64"));
    if (!number) number = CFDictionaryGetValue(dict, CFSTR("ManagedSpaceID"));
    int64_t id = 0;
    if (number && CFGetTypeID(number) == CFNumberGetTypeID())
        CFNumberGetValue(number, kCFNumberSInt64Type, &id);
    return (uint64_t)id;
}

static CFStringRef cursor_display(void) {
    CGEventRef event = CGEventCreate(NULL);
    if (!event) return NULL;
    CGPoint point = CGEventGetLocation(event);
    CFRelease(event);
    CGDirectDisplayID display;
    uint32_t count = 0;
    if (CGGetDisplaysWithPoint(point, 1, &display, &count) != kCGErrorSuccess || !count)
        return NULL;
    CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(display);
    if (!uuid) return NULL;
    CFStringRef result = CFUUIDCreateString(NULL, uuid);
    CFRelease(uuid);
    return result;
}

// Query the actual display, rather than the keyboard-focused global space or
// the stale Current Space dictionary observed on macOS 27.
static uint64_t display_active_space(CFStringRef display, bool single_display) {
    typedef uint64_t (*GetCurrent)(int, CFStringRef);
    static GetCurrent get_current;
    static bool resolved;
    if (!resolved) {
        void *symbol = dlsym(RTLD_DEFAULT, "SLSManagedDisplayGetCurrentSpace");
        if (!symbol) symbol = dlsym(RTLD_DEFAULT, "CGSManagedDisplayGetCurrentSpace");
        if (symbol) memcpy(&get_current, &symbol, sizeof(get_current));
        resolved = true;
    }
    if (get_current) return get_current(CGSMainConnectionID(), display);
    return single_display ? CGSGetActiveSpace(CGSMainConnectionID()) : 0;
}

static SpaceInfo read_spaces(CFStringRef wanted) {
    SpaceInfo result = { .index = -1 };
    CFArrayRef displays = CGSCopyManagedDisplaySpaces(CGSMainConnectionID());
    if (!displays) return result;
    for (CFIndex i = 0; i < CFArrayGetCount(displays); i++) {
        CFDictionaryRef display = CFArrayGetValueAtIndex(displays, i);
        if (CFGetTypeID(display) != CFDictionaryGetTypeID()) continue;
        CFStringRef name = CFDictionaryGetValue(display, CFSTR("Display Identifier"));
        if (!name || CFGetTypeID(name) != CFStringGetTypeID()) continue;
        if (CFArrayGetCount(displays) > 1 && (!wanted || !CFEqual(name, wanted))) continue;
        CFArrayRef list = CFDictionaryGetValue(display, CFSTR("Spaces"));
        uint64_t current = display_active_space(name, CFArrayGetCount(displays) == 1);
        if (verbose) {
            uint64_t global = CGSGetActiveSpace(CGSMainConnectionID());
            if (global != current)
                fprintf(stderr, "space-query display=%llu global=%llu\n",
                        (unsigned long long)current, (unsigned long long)global);
        }
        if (!list || CFGetTypeID(list) != CFArrayGetTypeID() || !current) continue;
        result.count = (int)CFArrayGetCount(list);
        result.order = 14695981039346656037ULL;
        for (int j = 0; j < result.count; j++) {
            uint64_t id = space_id(CFArrayGetValueAtIndex(list, j));
            if (!id) { result.index = -1; break; }
            result.order = (result.order ^ id) * 1099511628211ULL;
            if (id == current) result.index = j;
        }
        if (result.index >= 0) result.display = CFRetain(name);
        break;
    }
    CFRelease(displays);
    return result;
}

static bool same_space_list(SpaceInfo a, SpaceInfo b) {
    return a.display && b.display && CFEqual(a.display, b.display) &&
           a.order == b.order && a.count == b.count;
}

static void reset_gesture_state(void) {
    swipe = (Swipe){0};
    suppressCompanion = false;
}

static void begin_spaces(void) {
    CFStringRef wanted = cursor_display();
    SpaceInfo current = read_spaces(wanted);
    if (wanted) CFRelease(wanted);
    if (!same_space_list(current, spaces)) reset_gesture_state();
    if (spaces.display) CFRelease(spaces.display);
    spaces = current;
    observedIndex = current.index;
    sameSpaces = true;
    nextSpaceCheck = 0;
}

static void trace_event(const char *kind, SwipeEvent e) {
    fprintf(stderr, "%s phase=%d progress=%+.6f velocity=%+.6f space=%d/%d target=%d mode=%d\n",
            kind, e.phase, e.progress, e.velocity, swipe.index + 1, swipe.count,
            swipe.target + 1, swipe.mode);
}

// Timer frames are marked after serialization, because userData does not
// survive the raw-payload round trip. Only our own frames bypass the tap.
static bool post_slide_frame(CGEventRef source, enum SwipePhase phase,
                             double progress, double velocity) {
    CGEventRef frame = CGEventCreateCopy(source);
    if (!frame) return false;
    double original_p = CGEventGetDoubleValueField(frame, kProgress);
    double original_v = CGEventGetDoubleValueField(frame, kVelocityX);
    CGEventSetTimestamp(frame, (CGEventTimestamp)(now_seconds() * 1e9));
    CGEventSetIntegerValueField(frame, kPhase, phase);
    CGEventSetDoubleValueField(frame, kProgress, progress * animation.posted_sign);
    CGEventSetDoubleValueField(frame, kVelocityX, velocity * animation.posted_sign);
    CGEventSetDoubleValueField(frame, kVelocityY, 0);
    double payload_sign = rawSign;
    CGEventRef synced = sync_native_payload(frame, original_p, original_v, &payload_sign);
    if (synced) { CFRelease(frame); frame = synced; }
    else if (macOS27) { CFRelease(frame); return false; }
    CGEventSetIntegerValueField(frame, kCGEventSourceUserData, animationTag);
    CGEventPost(kCGSessionEventTap, frame);
    CGEventPost(kCGSessionEventTap, animation.companion);
    CFRelease(frame);
    return true;
}

static void dispose_slide(void) {
    if (animation.timer) {
        CFRunLoopTimerInvalidate(animation.timer);
        CFRelease(animation.timer);
    }
    if (animation.source) CFRelease(animation.source);
    if (animation.companion) CFRelease(animation.companion);
    if (animation.cleanup) CFRelease(animation.cleanup);
    memset(&animation, 0, sizeof(animation));
}

static bool slide_still_valid(void) {
    SpaceInfo current = read_spaces(spaces.display);
    bool valid = same_space_list(current, spaces) &&
                 slide_context_valid(swipe.index, swipe.target, swipe.count,
                                     current.index, current.count);
    if (current.display) CFRelease(current.display);
    return valid;
}

static void abort_slide(void) {
    if (!animation.source) return;
    // Never complete a destination computed from a removed/reordered space.
    post_slide_frame(animation.source, SwipeCancel, 0, 0);
    if (animation.cleanup) post_slide_frame(animation.cleanup, SwipeEnd, 0, 0);
    dispose_slide();
    swipe.mode = SwipeFailed;
    swipe.target = -1;
    swipe.pending = swipe.position = 0;
    swipe.open = false;
    sameSpaces = false;
    if (verbose) fprintf(stderr, "slide cancelled: space layout or active space changed\n");
}

static void finish_slide(void) {
    if (!animation.source) return;
    if (!slide_still_valid()) { abort_slide(); return; }
    // Full visual travel first; then commit with no slow native ease-out.
    bool drawn = post_slide_frame(animation.source, SwipeChange, animation.target, 0);
    // The visual position is already at the adjacent-space boundary. Keep
    // that endpoint on End; resetting it to epsilon can return the Dock to
    // the origin after the space layout changes. This is FasterSwiper's
    // segmented commit: full progress plus a minimal nonzero velocity.
    bool ended = post_slide_frame(animation.source, SwipeEnd,
                                  animation.target, copysign(1.0 / 65536, animation.target));
    if (animation.cleanup)
        post_slide_frame(animation.cleanup, SwipeEnd, 0, 0);
    swipe.waiting_since = now_seconds();
    if (verbose)
        fprintf(stderr, "slide finished elapsed_ms=%.1f drawn=%d committed=%d\n",
                (now_seconds() - animation.started) * 1000, drawn, ended);
    if (!ended) fprintf(stderr, "iss: failed to commit timed slide\n");
    dispose_slide();
}

static void slide_tick(CFRunLoopTimerRef timer, void *context) {
    (void)timer; (void)context;
    if (!slide_still_valid()) { abort_slide(); return; }
    double elapsed = now_seconds() - animation.started;
    if (elapsed >= ISS_INITIAL_SLIDE_SECONDS) { finish_slide(); return; }
    if (!post_slide_frame(animation.source, SwipeChange,
                         slide_position(animation.from, animation.target, elapsed), 0))
        finish_slide();
}

static bool start_slide(CGEventRef source, SwipeEvent output) {
    if (!slide_still_valid()) {
        swipe.mode = SwipeFailed;
        swipe.target = -1;
        return false;
    }
    animation.source = CGEventCreateCopy(source);
    animation.companion = CGEventCreate(NULL);
    if (!animation.source || !animation.companion) { dispose_slide(); return false; }
    CGEventSetIntegerValueField(animation.companion, kEventType, kGesture);
    CGEventSetIntegerValueField(animation.companion, kCGEventSourceUserData, animationTag);
    animation.from = output.progress;
    animation.target = slide_target(swipe.count, output.velocity);
    animation.started = now_seconds();
    animation.posted_sign = 1;
    if (macOS27) {
        CFPropertyListRef natural = CFPreferencesCopyAppValue(
            CFSTR("com.apple.swipescrolldirection"), kCFPreferencesAnyApplication);
        bool enabled = !natural || CFEqual(natural, kCFBooleanTrue);
        if (natural) CFRelease(natural);
        animation.posted_sign = enabled ? -1 : 1;
    }
    // Divide the duration into whole frames so the last tick lands on the
    // deadline rather than rounding a 60 ms animation up to 66.7 ms.
    double interval = ISS_INITIAL_SLIDE_SECONDS / ceil(ISS_INITIAL_SLIDE_SECONDS * 120);
    animation.timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + interval,
                                         interval, 0, 0, slide_tick, NULL);
    if (!animation.timer) { dispose_slide(); return false; }
    CFRunLoopAddTimer(CFRunLoopGetMain(), animation.timer, kCFRunLoopCommonModes);
    if (verbose) fprintf(stderr, "slide started from=%+.6f target=%+.6f duration_ms=%.0f\n",
                         animation.from, animation.target, ISS_INITIAL_SLIDE_SECONDS * 1000);
    return true;
}

static CGEventRef cb(CGEventTapProxy proxy, CGEventType type, CGEventRef ev, void *ctx) {
    (void)proxy; (void)ctx;
    if (type == kCGEventTapDisabledByUserInput || type == kCGEventTapDisabledByTimeout) {
        // Do not feed unrebased samples into a segment whose progress we
        // already changed. Drain it through its neutral terminal event.
        if (swipe.active && !swipe.passthrough) swipe.mode = SwipeFailed;
        else reset_gesture_state();
        tap_enabled = AXIsProcessTrusted();
        if (tap_enabled) finish_slide(); else dispose_slide();
        CGEventTapEnable(tap, tap_enabled);
        return ev;
    }
    if (CGEventGetIntegerValueField(ev, kCGEventSourceUserData) == animationTag) return ev;
    int et = (int)CGEventGetIntegerValueField(ev, kEventType);
    if (et == kGesture) {
        bool suppress = suppressCompanion;
        suppressCompanion = false;
        return !probe && suppress ? NULL : ev;
    }
    if (et != kDockControl) return ev;
    if (CGEventGetIntegerValueField(ev, kHIDType) != kDockSwipe ||
        CGEventGetIntegerValueField(ev, kMotion) != kHorizontal) {
        suppressCompanion = false;
        return ev;
    }

    SwipeEvent input = {
        .phase = (enum SwipePhase)CGEventGetIntegerValueField(ev, kPhase),
        .progress = CGEventGetDoubleValueField(ev, kProgress) * inputSign,
        .velocity = CGEventGetDoubleValueField(ev, kVelocityX) * inputSign,
    };
    double now = now_seconds();
    if (probe) {
        if (input.phase == SwipeBegin) begin_spaces();
        fprintf(stderr, "native phase=%d progress=%+.6f velocity=%+.6f flags=%lld space=%d/%d\n",
                input.phase, input.progress, input.velocity,
                (long long)CGEventGetIntegerValueField(ev, (CGEventField)135),
                spaces.index + 1, spaces.count);
        CFDataRef data = CGEventCreateData(NULL, ev);
        if (data) {
            fprintf(stderr, "serialized ");
            for (CFIndex i = 0; i < CFDataGetLength(data); i++)
                fprintf(stderr, "%02x", CFDataGetBytePtr(data)[i]);
            fprintf(stderr, "\n");
            CFRelease(data);
        }
        return ev;
    }
    if (animation.source) {
        if (input.phase == SwipeBegin) {
            finish_slide();
        } else {
            if (input.phase == SwipeChange) {
                swipe.pending += input.progress - swipe.last;
                swipe.last = input.progress;
            } else if (input.phase == SwipeEnd || input.phase == SwipeCancel) {
                swipe.active = false;
                if (animation.cleanup) CFRelease(animation.cleanup);
                animation.cleanup = CGEventCreateCopy(ev);
            }
            suppressCompanion = true;
            return NULL;
        }
    }
    SwipeEvent output = input;
    if (input.phase == SwipeBegin) {
        begin_spaces();
        output = swipe_begin(&swipe, input, spaces.index, spaces.count, now);
    } else if (input.phase == SwipeChange || input.phase == SwipeEnd || input.phase == SwipeCancel) {
        // Validate during tracking as well as waiting, and always at release.
        if (swipe.active && (now >= nextSpaceCheck || input.phase != SwipeChange)) {
            SpaceInfo current = read_spaces(spaces.display);
            sameSpaces = same_space_list(spaces, current);
            observedIndex = current.index;
            if (sameSpaces && swipe.mode == SwipeTracking && observedIndex != swipe.index)
                sameSpaces = false;
            if (current.display) CFRelease(current.display);
            nextSpaceCheck = now + 1.0 / 60.0;
        }
        if (input.phase == SwipeChange) {
            output = swipe_move(&swipe, input, now, observedIndex, sameSpaces);
        } else {
            if (!sameSpaces) { swipe.mode = SwipeFailed; swipe.target = -1; }
            output = swipe_finish(&swipe, input, now);
        }
    }
    if (!output.drop && output.phase == SwipeEnd && output.velocity != 0 &&
        swipe.mode == SwipeWaiting && !swipe.paging && !swipe.passthrough &&
        start_slide(ev, output)) {
        // Keep the native gesture open while timed Changed frames complete
        // the visible slide. The timer supplies its terminal event at the animation deadline.
        output.phase = SwipeChange;
        output.velocity = 0;
    }
    if (swipe.mode == SwipeFailed && output.phase == SwipeEnd) {
        output.phase = SwipeCancel;
        output = swipe_zero(output);
    }
    suppressCompanion = output.drop;
    if (verbose) {
        trace_event("input", input);
        trace_event(output.drop ? "suppressed" : "output", output);
    }
    CGEventRef edited = apply_edit(ev, input, output, inputSign);
    if (edited && (input.phase != output.phase || input.progress != output.progress ||
                   input.velocity != output.velocity)) {
        CGEventRef replacement = sync_native_payload(edited, input.progress * inputSign,
                                                      input.velocity * inputSign, &rawSign);
        if (replacement) return replacement;
    }
    return edited;
}

static void stop(int signal_number) { (void)signal_number; running = 0; }

#ifndef ISS_TEST
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--probe")) probe = true;
        else if (!strcmp(argv[i], "--verbose")) verbose = true;
        else {
            fprintf(stderr, "Usage: iss [--verbose | --probe]\n"
                    "  --probe   log native horizontal gestures without modifying them\n");
            return !strcmp(argv[i], "--help") ? 0 : 2;
        }
    }
    char version[32] = {0};
    size_t version_size = sizeof(version);
    sysctlbyname("kern.osproductversion", version, &version_size, NULL, 0);
    inputSign = swipe_input_sign(atoi(version));
    macOS27 = atoi(version) >= 27;
    const void *keys[] = { kAXTrustedCheckOptionPrompt }, *values[] = { kCFBooleanTrue };
    CFDictionaryRef opts = CFDictionaryCreate(NULL, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    bool trusted = AXIsProcessTrustedWithOptions(opts);
    CFRelease(opts);
    if (!trusted) { fprintf(stderr, "Grant Accessibility permission, then re-run.\n"); return 1; }

    tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
        probe ? kCGEventTapOptionListenOnly : kCGEventTapOptionDefault,
        (1ULL << kGesture) | (1ULL << kDockControl), cb, NULL);
    if (!tap) { fprintf(stderr, "Failed to create event tap.\n"); return 1; }
    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(NULL, tap, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
    CGEventTapEnable(tap, true);
    tap_trusted = tap_enabled = true;
    signal(SIGINT, stop); signal(SIGTERM, stop);
    fprintf(stderr, "iss: %s (macOS %s, native event path)\n",
            probe ? "read-only gesture probe" : "swipe active", version);
    while (running) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
        trusted = AXIsProcessTrusted();
        if (trusted != tap_trusted) {
            dispose_slide();
            reset_gesture_state();
            tap_trusted = trusted;
            fprintf(stderr, "iss: Accessibility %s\n", trusted ? "restored" : "removed");
        }
        if (trusted != tap_enabled) {
            CGEventTapEnable(tap, trusted);
            tap_enabled = trusted;
        }
    }
    finish_slide();
    CGEventTapEnable(tap, false);
    CFRunLoopRemoveSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
    CFRelease(source); CFRelease(tap);
    if (spaces.display) CFRelease(spaces.display);
    return 0;
}
#endif
