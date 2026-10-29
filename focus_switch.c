// Focus-driven Space jumps: the SIP-compatible approach documented by yabai,
// with macOS 27 serialization described by noswoosh. See THIRD_PARTY_NOTICES.md.
#include <Carbon/Carbon.h>
#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "focus_switch.h"
#include "focus_event.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static struct {
    int (*connection)(void);
    CFArrayRef (*displays)(int);
    uint64_t (*current)(int, CFStringRef);
    CFArrayRef (*window_spaces)(int, int, CFArrayRef);
    AXError (*window_id)(AXUIElementRef, uint32_t *);
    CGError (*front_psn)(ProcessSerialNumber *);
    CGError (*set_front)(int, uint64_t, ProcessSerialNumber);
} api;
static struct {
    bool available, enabled, augment, verbose;
    bool (*busy)(void);
    int64_t tag;
    EventHandlerRef handler;
    AXObserverRef observer;
    AXUIElementRef app;
    pid_t pid;
    ProcessSerialNumber psn;
    CFRunLoopTimerRef request_timer, restore_timer;
    CFStringRef last_display;
    uint64_t last_target, last_order;
    CFAbsoluteTime last_posted;
    CGPoint point, saved_cursor;
    bool moved_cursor;
} focus;

static bool load_symbol(void *target, const char *name) {
    void *symbol = dlsym(RTLD_DEFAULT, name);
    if (!symbol) return false;
    memcpy(target, &symbol, sizeof(symbol));
    return true;
}
bool focus_switch_available(void) {
    if (focus.available) return true;
#define LOAD(field, name) if (!load_symbol(&api.field, name)) return false
    LOAD(connection, "SLSMainConnectionID");
    LOAD(displays, "SLSCopyManagedDisplaySpaces");
    LOAD(current, "SLSManagedDisplayGetCurrentSpace");
    LOAD(window_spaces, "SLSCopySpacesForWindows");
    LOAD(window_id, "_AXUIElementGetWindow");
    LOAD(front_psn, "_SLPSGetFrontProcess");
    LOAD(set_front, "SLSSpaceSetFrontPSN");
#undef LOAD
    focus.available = true;
    return true;
}
static uint64_t dictionary_id(CFDictionaryRef dict) {
    if (!dict || CFGetTypeID(dict) != CFDictionaryGetTypeID()) return 0;
    CFTypeRef n = CFDictionaryGetValue(dict, CFSTR("id64"));
    if (!n) n = CFDictionaryGetValue(dict, CFSTR("ManagedSpaceID"));
    int64_t result = 0;
    if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue(n, kCFNumberSInt64Type, &result);
    return (uint64_t)result;
}
// Retains the matching display. Shared/sticky windows already visible anywhere
// are ignored; never guess a destination when the layout cannot be resolved.
typedef struct {
    CFStringRef display;
    int current, target, count;
    uint64_t order, sid;
} FocusLayout;
static FocusLayout layout_for_windows(CFArrayRef spaces) {
    FocusLayout result = { .current = -1, .target = -1 };
    CFArrayRef displays = api.displays(api.connection());
    if (!displays) return result;
    bool visible = false;
    for (CFIndex i = 0; i < CFArrayGetCount(displays); i++) {
        CFDictionaryRef display = CFArrayGetValueAtIndex(displays, i);
        if (CFGetTypeID(display) != CFDictionaryGetTypeID()) continue;
        CFStringRef name = CFDictionaryGetValue(display, CFSTR("Display Identifier"));
        CFArrayRef list = CFDictionaryGetValue(display, CFSTR("Spaces"));
        if (!name || CFGetTypeID(name) != CFStringGetTypeID() || !list || CFGetTypeID(list) != CFArrayGetTypeID()) continue;
        uint64_t current = api.current(api.connection(), name);
        int current_index = -1, target_index = -1;
        uint64_t sid = 0, order = 14695981039346656037ULL;
        bool valid = true;
        for (CFIndex j = 0; j < CFArrayGetCount(list); j++) {
            uint64_t id = dictionary_id(CFArrayGetValueAtIndex(list, j));
            if (!id) { valid = false; break; }
            order = (order ^ id) * 1099511628211ULL;
            if (id == current) current_index = (int)j;
            CFNumberRef number = CFNumberCreate(NULL, kCFNumberSInt64Type, &id);
            bool match = CFArrayContainsValue(spaces, CFRangeMake(0, CFArrayGetCount(spaces)), number);
            CFRelease(number);
            if (match) { target_index = (int)j; sid = id; if (id == current) visible = true; }
        }
        if (valid && current_index >= 0 && target_index >= 0 && !result.display) {
            result = (FocusLayout){ CFRetain(name), current_index, target_index,
                                    (int)CFArrayGetCount(list), order, sid };
        }
    }
    CFRelease(displays);
    if (visible && result.display) { CFRelease(result.display); result.display = NULL; }
    return result;
}
static CFArrayRef spaces_for_window(uint32_t wid) {
    CFNumberRef number = CFNumberCreate(NULL, kCFNumberSInt32Type, &wid);
    CFArrayRef list = CFArrayCreate(NULL, (const void **)&number, 1, &kCFTypeArrayCallBacks);
    CFArrayRef spaces = api.window_spaces(api.connection(), 0x7, list);
    CFRelease(list); CFRelease(number);
    return spaces;
}
// Read the actual Accessibility focus. The app's frontmost WindowServer
// window can still be its OLD window while AltTab is selecting another one.
// AX notification elements can also be stale by the time we process them.
static uint32_t focused_window(void) {
    uint32_t wid = 0;
    CFTypeRef window = NULL;
    if (focus.app && AXUIElementCopyAttributeValue(focus.app, kAXFocusedWindowAttribute, &window) == kAXErrorSuccess && window) {
        if (CFGetTypeID(window) == AXUIElementGetTypeID()) api.window_id((AXUIElementRef)window, &wid);
        CFRelease(window);
    }
    return wid;
}
static bool same_front(void) {
    ProcessSerialNumber front = {0};
    return api.front_psn(&front) == noErr && front.highLongOfPSN == focus.psn.highLongOfPSN &&
           front.lowLongOfPSN == focus.psn.lowLongOfPSN;
}
static bool layout_matches(FocusLayout a, FocusLayout b) {
    return a.display && b.display && CFEqual(a.display, b.display) &&
           a.current == b.current && a.target == b.target && a.count == b.count &&
           a.order == b.order && a.sid == b.sid;
}
static bool recently_posted(FocusLayout layout, CFAbsoluteTime now) {
    return focus.last_display && layout.sid == focus.last_target && layout.order == focus.last_order &&
           CFEqual(layout.display, focus.last_display) && now - focus.last_posted < 0.25;
}
static void restore_cursor(void) {
    if (!focus.moved_cursor) return;
    CGEventRef event = CGEventCreate(NULL);
    if (event) {
        CGPoint current = CGEventGetLocation(event); CFRelease(event);
        if (hypot(current.x - focus.point.x, current.y - focus.point.y) < 2)
            CGWarpMouseCursorPosition(focus.saved_cursor);
    }
    focus.moved_cursor = false;
}
static void cancel_timer(CFRunLoopTimerRef *timer) {
    if (*timer) { CFRunLoopTimerInvalidate(*timer); CFRelease(*timer); *timer = NULL; }
}
void focus_switch_cancel(void) {
    cancel_timer(&focus.request_timer);
    cancel_timer(&focus.restore_timer);
    restore_cursor();
}
static void restore_tick(CFRunLoopTimerRef timer, void *context) {
    (void)timer; (void)context;
    cancel_timer(&focus.restore_timer);
    restore_cursor();
}
// All phases for the complete jump are allocated and validated up front.
// Never add another adjacent jump based on the active-space query while the
// switcher's own focus transition is still in flight. That could overshoot.
typedef struct { CGEventRef *frames, companion; int count; } FocusBatch;
static void release_batch(FocusBatch *batch) {
    if (batch->frames) {
        for (int i = 0; i < batch->count; i++) if (batch->frames[i]) CFRelease(batch->frames[i]);
        free(batch->frames);
    }
    if (batch->companion) CFRelease(batch->companion);
    *batch = (FocusBatch){0};
}
static bool build_batch(FocusBatch *batch, int direction, int steps, CGPoint point) {
    *batch = (FocusBatch){0};
    if (steps < 1 || steps > 64 || (direction != 1 && direction != -1)) return false;
    batch->count = steps * 3;
    batch->frames = calloc((size_t)batch->count, sizeof(CGEventRef));
    batch->companion = CGEventCreate(NULL);
    if (!batch->frames || !batch->companion) goto fail;
    CGEventSetIntegerValueField(batch->companion, kEventType, kGesture);
    CGEventSetIntegerValueField(batch->companion, kCGEventSourceUserData, focus.tag);
    CGEventSetLocation(batch->companion, point);
    CFPropertyListRef value = CFPreferencesCopyAppValue(CFSTR("com.apple.swipescrolldirection"), kCFPreferencesAnyApplication);
    bool natural = !value || CFEqual(value, kCFBooleanTrue);
    if (value) CFRelease(value);
    enum SwipePhase phases[] = {SwipeBegin, SwipeChange, SwipeEnd};
    for (int i = 0; i < batch->count; i++) {
        batch->frames[i] = focus_event_create(phases[i % 3], direction, focus.augment, natural, point, focus.tag);
        if (!batch->frames[i]) goto fail;
    }
    return true;
fail:
    release_batch(batch);
    return false;
}
static bool display_bounds(CFStringRef name, CGRect *bounds) {
    CGDirectDisplayID displays[32]; uint32_t count = 0;
    if (CGGetActiveDisplayList(32, displays, &count) != kCGErrorSuccess) return false;
    for (uint32_t i = 0; i < count; i++) {
        CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(displays[i]);
        CFStringRef identifier = uuid ? CFUUIDCreateString(NULL, uuid) : NULL;
        bool match = identifier && CFEqual(name, identifier);
        if (identifier) CFRelease(identifier);
        if (uuid) CFRelease(uuid);
        if (match || (count == 1 && CFEqual(name, CFSTR("Main")))) {
            *bounds = CGDisplayBounds(displays[i]); return true;
        }
    }
    return false;
}
static FocusLayout layout_for_window(uint32_t wid) {
    CFArrayRef ids = wid ? spaces_for_window(wid) : NULL;
    FocusLayout layout = { .current = -1, .target = -1 };
    if (ids) { layout = layout_for_windows(ids); CFRelease(ids); }
    return layout;
}
// These small indirections let the regression test simulate focus changing
// between planning and posting, without posting events on the user's desktop.
static uint32_t (*read_focus)(void) = focused_window;
static bool (*get_bounds)(CFStringRef, CGRect *) = display_bounds;
static void (*post_event)(CGEventTapLocation, CGEventRef) = CGEventPost;

static void commit_focus(void) {
    if (!focus.enabled || focus.busy() || !same_front()) return;
    // Finish pointer cleanup before a new request can replace its saved state.
    cancel_timer(&focus.restore_timer);
    restore_cursor();
    uint32_t wid = read_focus();
    if (!wid) return;
    FocusLayout layout = layout_for_window(wid);
    if (!layout.display) return;
    if (recently_posted(layout, CFAbsoluteTimeGetCurrent())) { CFRelease(layout.display); return; }
    CGRect bounds;
    if (!get_bounds(layout.display, &bounds)) { CFRelease(layout.display); return; }
    CGEventRef pointer = CGEventCreate(NULL);
    if (!pointer) { CFRelease(layout.display); return; }
    CGPoint saved = CGEventGetLocation(pointer); CFRelease(pointer);
    CGPoint point = CGRectContainsPoint(bounds, saved) ? saved : CGPointMake(CGRectGetMidX(bounds), CGRectGetMidY(bounds));
    FocusBatch batch;
    if (!build_batch(&batch, layout.target > layout.current ? 1 : -1, abs(layout.target - layout.current), point)) {
        CFRelease(layout.display); return;
    }
    // AltTab may have selected a different window, or macOS may already have
    // reached the target, while we built these events. In either case post none.
    FocusLayout fresh = layout_for_window(wid);
    bool valid = layout_matches(layout, fresh) && read_focus() == wid && same_front() && !focus.busy();
    if (fresh.display) CFRelease(fresh.display);
    if (valid && api.set_front(api.connection(), layout.sid, focus.psn) == kCGErrorSuccess) {
        bool move = !CGRectContainsPoint(bounds, saved);
        if (!move || CGWarpMouseCursorPosition(point) == kCGErrorSuccess) {
            focus.saved_cursor = saved; focus.point = point; focus.moved_cursor = move;
            for (int i = 0; i < batch.count; i++) {
                post_event(kCGSessionEventTap, batch.frames[i]);
                if (focus.augment) post_event(kCGSessionEventTap, batch.companion);
            }
            if (focus.last_display) CFRelease(focus.last_display);
            focus.last_display = CFRetain(layout.display);
            focus.last_target = layout.sid; focus.last_order = layout.order;
            focus.last_posted = CFAbsoluteTimeGetCurrent();
            if (focus.verbose) fprintf(stderr, "focus jump pid=%d window=%u from=%d to=%d count=%d\n",
                focus.pid, wid, layout.current+1, layout.target+1, layout.count);
            if (move) {
                focus.restore_timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent()+0.06, 0, 0, 0, restore_tick, NULL);
                if (focus.restore_timer) CFRunLoopAddTimer(CFRunLoopGetMain(), focus.restore_timer, kCFRunLoopCommonModes);
                else restore_cursor();
            }
        }
    }
    release_batch(&batch); CFRelease(layout.display);
}
static void request_tick(CFRunLoopTimerRef timer, void *context) {
    (void)timer; (void)context;
    cancel_timer(&focus.request_timer);
    commit_focus();
}
static void request_focus(void) {
    if (!focus.enabled || focus.busy()) return;
    cancel_timer(&focus.request_timer);
    // Coalesce activation/focus notifications; never act on the activation's
    // historical window. New requests during a landing wait briefly, then
    // query the latest real focus rather than adding to an old switch queue.
    double when = fmax(CFAbsoluteTimeGetCurrent()+0.004, focus.last_posted+0.06);
    focus.request_timer = CFRunLoopTimerCreate(NULL, when, 0, 0, 0, request_tick, NULL);
    if (focus.request_timer) CFRunLoopAddTimer(CFRunLoopGetMain(), focus.request_timer, kCFRunLoopCommonModes);
}
static void focus_changed(AXObserverRef observer, AXUIElementRef element, CFStringRef name, void *context) {
    (void)observer; (void)element; (void)name; (void)context;
    if (same_front()) request_focus();
}
static void unobserve(void) {
    if (focus.observer) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(focus.observer), kCFRunLoopCommonModes);
        CFRelease(focus.observer); focus.observer = NULL;
    }
    if (focus.app) { CFRelease(focus.app); focus.app = NULL; }
}
static void observe_front(ProcessSerialNumber psn, bool act) {
    focus_switch_cancel(); unobserve();
    focus.psn = psn; focus.pid = 0;
    if (GetProcessPID(&psn, &focus.pid) != noErr || focus.pid <= 0 || focus.pid == getpid()) return;
    focus.app = AXUIElementCreateApplication(focus.pid);
    if (!focus.app) return;
    AXUIElementSetMessagingTimeout(focus.app, 0.03f);
    if (AXObserverCreate(focus.pid, focus_changed, &focus.observer) == kAXErrorSuccess) {
        AXObserverAddNotification(focus.observer, focus.app, kAXFocusedWindowChangedNotification, NULL);
        CFRunLoopAddSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(focus.observer), kCFRunLoopCommonModes);
    }
    if (act) request_focus();
}
static OSStatus front_changed(EventHandlerCallRef handler, EventRef event, void *context) {
    (void)handler; (void)context;
    ProcessSerialNumber psn = {0};
    if (focus.enabled && GetEventParameter(event, kEventParamProcessID, typeProcessSerialNumber,
                                          NULL, sizeof(psn), NULL, &psn) == noErr)
        observe_front(psn, true);
    return noErr;
}
bool focus_switch_start(bool augment, int64_t event_tag, bool (*gesture_busy)(void), bool verbose) {
    if (!focus_switch_available()) return false;
    focus.augment = augment; focus.tag = event_tag; focus.busy = gesture_busy; focus.verbose = verbose;
    EventTypeSpec event = { kEventClassApplication, kEventAppFrontSwitched };
    return InstallEventHandler(GetApplicationEventTarget(), front_changed, 1, &event, NULL, &focus.handler) == noErr;
}
void focus_switch_enable(bool enabled) {
    if (!focus.handler || focus.enabled == enabled) return;
    focus.enabled = enabled;
    if (!enabled) {
        focus_switch_cancel(); unobserve();
        if (focus.last_display) { CFRelease(focus.last_display); focus.last_display = NULL; }
    } else {
        ProcessSerialNumber psn = {0};
        if (api.front_psn(&psn) == noErr) observe_front(psn, false);
    }
}
void focus_switch_stop(void) {
    focus_switch_enable(false);
    if (focus.handler) { RemoveEventHandler(focus.handler); focus.handler = NULL; }
}
#pragma clang diagnostic pop
