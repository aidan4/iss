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
#include "native_event.h"
#include "iohid.h"

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
        uint64_t current = CFArrayGetCount(displays) == 1
            ? CGSGetActiveSpace(CGSMainConnectionID())
            : space_id(CFDictionaryGetValue(display, CFSTR("Current Space")));
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

static CGEventRef cb(CGEventTapProxy proxy, CGEventType type, CGEventRef ev, void *ctx) {
    (void)proxy; (void)ctx;
    if (type == kCGEventTapDisabledByUserInput || type == kCGEventTapDisabledByTimeout) {
        // Do not feed unrebased samples into a segment whose progress we
        // already changed. Drain it through its neutral terminal event.
        if (swipe.active && !swipe.passthrough) swipe.mode = SwipeFailed;
        else reset_gesture_state();
        tap_enabled = AXIsProcessTrusted();
        CGEventTapEnable(tap, tap_enabled);
        return ev;
    }
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
    SwipeEvent output = input;
    if (input.phase == SwipeBegin) {
        begin_spaces();
        output = swipe_begin(&swipe, input, spaces.index, spaces.count, now);
    } else if (input.phase == SwipeChange) {
        if (swipe.mode == SwipeWaiting && now >= nextSpaceCheck) {
            SpaceInfo current = read_spaces(spaces.display);
            // A transient failed query is not a topology change. Wait for the
            // next sample; the bounded confirmation timeout still applies.
            if (current.index >= 0) {
                sameSpaces = same_space_list(spaces, current);
                observedIndex = current.index;
            }
            if (current.display) CFRelease(current.display);
            nextSpaceCheck = now + 1.0 / 60.0;
        }
        output = swipe_move(&swipe, input, now, observedIndex, sameSpaces);
    } else if (input.phase == SwipeEnd || input.phase == SwipeCancel) {
        output = swipe_finish(&swipe, input, now);
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
            reset_gesture_state();
            tap_trusted = trusted;
            fprintf(stderr, "iss: Accessibility %s\n", trusted ? "restored" : "removed");
        }
        if (trusted != tap_enabled) {
            CGEventTapEnable(tap, trusted);
            tap_enabled = trusted;
        }
    }
    CGEventTapEnable(tap, false);
    CFRunLoopRemoveSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
    CFRelease(source); CFRelease(tap);
    if (spaces.display) CFRelease(spaces.display);
    return 0;
}
#endif
