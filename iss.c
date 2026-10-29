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
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include "settings_io.h"
#include "runtime_watch.h"
#include "focus_switch.h"
#include "native_event.h"
#include "iohid.h"
#include "slide.h"
#include "slide_commit.h"
#include "release_segment.h"

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
static ReleaseSegment release_segment;
static double release_posted_sign = 1;
static SpaceInfo spaces = { .index = -1 };
static bool tap_trusted, tap_enabled, probe, verbose, suppressCompanion;
static double inputSign, nextSpaceCheck;
static int observedIndex;
static double rawSign = 1;
static bool macOS27;
static const int64_t animationTag = 0x495353534c494445LL;
static struct {
    CGEventRef source, companion, cleanup;
    CGEventRef *finish_frames;
    int finish_count;
    CFRunLoopTimerRef timer;
    double started, from, target, posted_sign;
} animation;
static bool sameSpaces;
static double last_user_gesture;
static bool permission_lost;
static double now_seconds(void);
static bool gesture_busy(void) {
    return swipe.active || animation.source || now_seconds() - last_user_gesture < 0.25;
}


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
    release_segment = (ReleaseSegment){0};
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
static CGEventRef create_gesture_frame(CGEventRef source, enum SwipePhase phase,
                             double progress, double velocity, double posted_sign) {
    CGEventRef frame = CGEventCreateCopy(source);
    if (!frame) return NULL;
    double original_p = CGEventGetDoubleValueField(frame, kProgress);
    double original_v = CGEventGetDoubleValueField(frame, kVelocityX);
    CGEventSetTimestamp(frame, (CGEventTimestamp)(now_seconds() * 1e9));
    CGEventSetIntegerValueField(frame, kPhase, phase);
    CGEventSetDoubleValueField(frame, kProgress, progress * posted_sign);
    CGEventSetDoubleValueField(frame, kVelocityX, velocity * posted_sign);
    CGEventSetDoubleValueField(frame, kVelocityY, 0);
    double payload_sign = rawSign;
    CGEventRef synced = sync_native_payload(frame, original_p, original_v, &payload_sign);
    if (synced) { CFRelease(frame); frame = synced; }
    else if (macOS27) { CFRelease(frame); return NULL; }
    CGEventSetIntegerValueField(frame, kCGEventSourceUserData, animationTag);
    return frame;
}

static CGEventRef create_slide_frame(CGEventRef source, enum SwipePhase phase,
                                     double progress, double velocity) {
    return create_gesture_frame(source, phase, progress, velocity, animation.posted_sign);
}

static double gesture_posted_sign(void) {
    if (!macOS27) return 1;
    CFPropertyListRef natural = CFPreferencesCopyAppValue(
        CFSTR("com.apple.swipescrolldirection"), kCFPreferencesAnyApplication);
    bool enabled = !natural || CFEqual(natural, kCFBooleanTrue);
    if (natural) CFRelease(natural);
    return enabled ? -1 : 1;
}

// Native previews and timed frames use the same local coordinate system.
// Posting through this tap preserves the order of boundary End/Begin phases.
static bool post_release_preview(CGEventTapProxy proxy, CGEventRef source, SwipeEvent output) {
    int capacity = 3*swipe.count + 6;
    SwipeEvent *plan = calloc((size_t)capacity, sizeof(*plan));
    CGEventRef *frames = calloc((size_t)capacity, sizeof(*frames));
    CGEventRef companion = CGEventCreate(NULL);
    bool success = false;
    if (!plan || !frames || !companion) goto done;
    CGEventSetIntegerValueField(companion, kEventType, kGesture);
    CGEventSetIntegerValueField(companion, kCGEventSourceUserData, animationTag);
    ReleaseSegment next = release_segment;
    int count = release_segment_move(&next, swipe.index, swipe.count, output.progress, plan, capacity);
    if (count < 0) goto done;
    double sign = release_posted_sign;
    for (int i=0; i<count; i++) {
        frames[i] = create_gesture_frame(source, plan[i].phase, plan[i].progress, plan[i].velocity, sign);
        if (!frames[i]) goto done;
    }
    for (int i=0; i<count; i++) {
        CGEventTapPostEvent(proxy, frames[i]);
        CGEventTapPostEvent(proxy, companion);
    }
    if (verbose && next.index != release_segment.index)
        fprintf(stderr, "preview crossed from=%d to=%d local=%+.6f\n",
                release_segment.index+1, next.index+1, next.position);
    release_segment = next;
    success = true;
done:
    if (frames) { for (int i=0; i<capacity; i++) if (frames[i]) CFRelease(frames[i]); free(frames); }
    free(plan);
    if (companion) CFRelease(companion);
    return success;
}

static bool post_slide_frame(CGEventRef source, enum SwipePhase phase,
                             double progress, double velocity) {
    CGEventRef frame = create_slide_frame(source, phase, progress, velocity);
    if (!frame) return false;
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
    if (animation.finish_frames) {
        for (int i = 0; i < animation.finish_count; i++)
            if (animation.finish_frames[i]) CFRelease(animation.finish_frames[i]);
        free(animation.finish_frames);
    }
    if (animation.source) CFRelease(animation.source);
    if (animation.companion) CFRelease(animation.companion);
    if (animation.cleanup) CFRelease(animation.cleanup);
    memset(&animation, 0, sizeof(animation));
    CFRunLoopStop(CFRunLoopGetMain());
}

static bool slide_still_valid(void) {
    SpaceInfo current = read_spaces(spaces.display);
    // The Dock can keep reporting the physical gesture's starting Space
    // until the last native segment ends. Crossed centers are valid lagging
    // observations, not evidence that the user selected an unrelated Space.
    int observed = current.index;
    if (swipe.release_only && observed >= release_segment.low && observed <= release_segment.high)
        observed = swipe.index;
    bool valid = same_space_list(current, spaces) &&
                 slide_context_valid(swipe.index, swipe.target, swipe.count,
                                     observed, current.count, false);
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
    // Build the entire commit with fresh timestamps before posting any frame.
    // Allocation failure must not leave half a multi-Space commit posted.
    for (int i = 0; i < animation.finish_count; i++) {
        SwipeEvent frame = slide_commit_event(swipe.index, swipe.target, swipe.count, i);
        animation.finish_frames[i] = create_slide_frame(animation.source, frame.phase, frame.progress, frame.velocity);
        if (!animation.finish_frames[i]) { abort_slide(); return; }
    }
    if (!slide_still_valid()) { abort_slide(); return; }
    bool returning = swipe.target == swipe.index;
    for (int i = 0; i < animation.finish_count; i++) {
        CGEventRef frame = animation.finish_frames[i];
        CGEventPost(kCGSessionEventTap, frame);
        CGEventPost(kCGSessionEventTap, animation.companion);
    }
    if (animation.cleanup)
        post_slide_frame(animation.cleanup, SwipeEnd, 0, 0);
    swipe.waiting_since = now_seconds();
    if (returning) { swipe.mode = SwipeTracking; swipe.target = -1; }
    if (verbose)
        fprintf(stderr, "slide finished elapsed_ms=%.1f target=%d commit_frames=%d\n",
                (now_seconds() - animation.started) * 1000, swipe.target + 1, animation.finish_count);
    dispose_slide();
}

static void slide_tick(CFRunLoopTimerRef timer, void *context) {
    (void)timer; (void)context;
    if (!slide_still_valid()) { abort_slide(); return; }
    double elapsed = now_seconds() - animation.started;
    if (elapsed >= ISS_SLIDE_SECONDS) { finish_slide(); return; }
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
    animation.target = (swipe.target - swipe.index) * slide_target(swipe.count, 1);
    animation.started = now_seconds();
    animation.posted_sign = swipe.release_only ? release_posted_sign : gesture_posted_sign();
    animation.finish_count = slide_commit_count(swipe.index, swipe.target);
    if (!animation.finish_count) { dispose_slide(); return false; }
    animation.finish_frames = calloc((size_t)animation.finish_count, sizeof(CGEventRef));
    if (!animation.finish_frames) { dispose_slide(); return false; }
    // Divide the duration into whole frames so the last tick lands on the
    // deadline rather than rounding a 60 ms animation up to 66.7 ms.
    double interval = slide_frame_interval(ISS_SLIDE_SECONDS);
    animation.timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + interval,
                                         interval, 0, 0, slide_tick, NULL);
    if (!animation.timer) { dispose_slide(); return false; }
    CFRunLoopAddTimer(CFRunLoopGetMain(), animation.timer, kCFRunLoopCommonModes);
    if (verbose) fprintf(stderr, "slide started from=%+.6f target=%+.6f duration_ms=%.0f\n",
                         animation.from, animation.target, ISS_SLIDE_SECONDS * 1000);
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
        if (tap_enabled != tap_trusted) {
            tap_trusted = tap_enabled;
            permission_lost = !tap_enabled;
            fprintf(stderr, "iss: Accessibility %s\n", tap_enabled ? "restored" : "removed");
        }
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
    if (!probe) {
        last_user_gesture = now_seconds();
        focus_switch_cancel();
    }
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
                swipe_accumulate_pending(&swipe, input.progress - swipe.last);
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
            if (sameSpaces && swipe.mode == SwipeTracking && observedIndex != swipe.index &&
                (!swipe.release_only || observedIndex < release_segment.low || observedIndex > release_segment.high))
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
    if (swipe.release_only && !output.drop) {
        if (output.phase == SwipeBegin || output.phase == SwipeChange) {
            if (output.phase == SwipeBegin) {
                release_segment = release_segment_begin(swipe.index);
                release_posted_sign = gesture_posted_sign();
            }
            if (!post_release_preview(proxy, ev, output)) {
                swipe.mode = SwipeFailed;
                output.phase = SwipeCancel;
                output = swipe_zero(output);
            } else {
                if (verbose) { trace_event("input", input); trace_event("preview", output); }
                suppressCompanion = true;
                return NULL;
            }
        } else if (output.phase == SwipeEnd && swipe.mode == SwipeWaiting) {
            // The destination was selected in global finger coordinates. The
            // Dock is now based at the last crossed center; animate only the
            // unfinished segment from the exact last preview position.
            output.progress = release_segment.position;
            swipe.position = output.progress;
            swipe.index = release_segment.index;
            if (!release_segment.open && swipe.target == swipe.index) {
                swipe.mode = SwipeTracking; swipe.target = -1;
                output = swipe_drop(output);
            }
        }
    }
    if (!output.drop && output.phase == SwipeEnd && (output.velocity != 0 || swipe.release_only) &&
        swipe.mode == SwipeWaiting && !swipe.passthrough &&
        start_slide(ev, output)) {
        // Animate every committed segment, including continued-drag paging.
        // Keep the native gesture open while timed Changed frames complete
        // the visible slide. The timer supplies its terminal event at the animation deadline.
        output.phase = SwipeChange;
        output.velocity = 0;
        // The preview already emitted this exact position. Do not reintroduce
        // a physical End relabelled Changed into the synthetic segment stream.
        if (swipe.release_only) output.drop = true;
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

#ifndef ISS_TEST
int main(int argc, char **argv) {
    const char *config_path = NULL;
    bool managed = false;
    pid_t parent = getppid();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--probe")) probe = true;
        else if (!strcmp(argv[i], "--verbose")) verbose = true;
        else if (!strcmp(argv[i], "--managed")) managed = true;
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) config_path = argv[++i];
        else if (!strcmp(argv[i], "--focus-check")) {
            bool available = focus_switch_available();
            printf("Focus transition APIs: %s\n", available ? "available" : "unavailable");
            return available ? 0 : 1;
        }
        else if (!strcmp(argv[i], "--settings-schema")) { settings_print_schema(); return 0; }
        else {
            fprintf(stderr, "Usage: iss [--verbose | --probe] [--config settings.plist]\n"
                    "  --probe   log native horizontal gestures without modifying them\n");
            return !strcmp(argv[i], "--help") ? 0 : 2;
        }
    }
    if (config_path && !settings_read(config_path, &iss_settings)) {
        fprintf(stderr, "iss: unable to read settings file\n"); return 2;
    }
    // Both app and CLI copies participate. Never install two modifying taps.
    char lock_path[128];
    snprintf(lock_path, sizeof(lock_path), "/tmp/iss-%u.lock", (unsigned)getuid());
    int lock = open(lock_path, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat lock_info;
    if (lock < 0 || fstat(lock, &lock_info) || !S_ISREG(lock_info.st_mode) ||
        lock_info.st_uid != getuid() || flock(lock, LOCK_EX | LOCK_NB)) {
        fprintf(stderr, "iss: another copy is running, or the instance lock is unavailable\n");
        if (lock >= 0) close(lock);
        return 73;
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
    RuntimeWatch watch;
    if (!runtime_watch_open(&watch, config_path, managed ? parent : 0, true)) {
        fprintf(stderr, "iss: failed to install runtime notifications\n");
        CGEventTapEnable(tap, false);
        CFRunLoopRemoveSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
        CFRelease(source); CFRelease(tap); close(lock);
        return 1;
    }
    if (!probe && !focus_switch_start(macOS27, animationTag, gesture_busy, verbose))
        fprintf(stderr, "iss: instant focus unavailable on this macOS version\n");
    if (!probe) focus_switch_enable(iss_settings.instantFocus >= 0.5);
    fprintf(stderr, "iss: %s (macOS %s, native event path)\n",
            probe ? "read-only gesture probe" : "swipe active", version);
    while (!watch.stop_requested && !permission_lost) {
        // Freeze settings through the physical gesture and its final slide.
        // No idle polling: the directory watcher marks actual edits dirty.
        if (config_path && watch.settings_dirty && !swipe.active && !animation.source) {
            ISSSettings updated;
            if (settings_read(config_path, &updated)) iss_settings = updated;
            watch.settings_dirty = false;
            if (!probe) focus_switch_enable(iss_settings.instantFocus >= 0.5);
        }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1e10, true);
    }
    runtime_watch_close(&watch);
    focus_switch_stop();
    finish_slide();
    CGEventTapEnable(tap, false);
    CFRunLoopRemoveSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
    CFRelease(source); CFRelease(tap);
    if (spaces.display) CFRelease(spaces.display);
    close(lock);
    return permission_lost ? 1 : 0;
}
#endif
