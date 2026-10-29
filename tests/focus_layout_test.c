#include <assert.h>
#include <stdio.h>
// Exercise the actual target resolution using injected read-only space queries.
// None of these tests start observers or post gestures.
#include "../focus_switch.c"

static CFArrayRef topology;
static uint64_t active_a = 11, active_b = 21;
static bool land_during_planning, switch_during_planning, gesture_in_progress, fail_front;
static int current_reads, focus_reads, posted_dock, posted_companion, front_calls;
static uint32_t latest_window = 100;
static int fake_connection(void) { return 1; }
static CFArrayRef fake_displays(int c) { (void)c; return CFRetain(topology); }
static uint64_t fake_current(int c, CFStringRef d) {
    (void)c;
    if (CFEqual(d, CFSTR("A"))) {
        if (land_during_planning && current_reads++ > 0) return 13;
        return active_a;
    }
    return active_b;
}
static CFArrayRef number_list(const uint64_t *ids, int count) {
    CFMutableArrayRef result = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    for (int i = 0; i < count; i++) {
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt64Type, ids+i);
        CFArrayAppendValue(result, n); CFRelease(n);
    }
    return result;
}
static CFDictionaryRef display(CFStringRef name, const uint64_t *ids, int count) {
    CFMutableArrayRef list = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    for (int i = 0; i < count; i++) {
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt64Type, ids+i);
        const void *key = CFSTR("id64");
        CFDictionaryRef dict = CFDictionaryCreate(NULL, &key, (const void **)&n, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFArrayAppendValue(list, dict); CFRelease(dict); CFRelease(n);
    }
    const void *keys[] = {CFSTR("Display Identifier"), CFSTR("Spaces")};
    const void *values[] = {name, list};
    CFDictionaryRef result = CFDictionaryCreate(NULL, keys, values, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(list); return result;
}
static void setup(const uint64_t *a, int na, const uint64_t *b, int nb) {
    if (topology) CFRelease(topology);
    CFDictionaryRef da = display(CFSTR("A"), a, na), db = display(CFSTR("B"), b, nb);
    const void *items[] = {da, db};
    topology = CFArrayCreate(NULL, items, 2, &kCFTypeArrayCallBacks);
    CFRelease(da); CFRelease(db);
}
static FocusLayout target(const uint64_t *ids, int count) {
    CFArrayRef list = number_list(ids, count);
    FocusLayout plan = layout_for_windows(list); CFRelease(list); return plan;
}
static CGError fake_front(ProcessSerialNumber *psn) { *psn = focus.psn; return kCGErrorSuccess; }
static CGError fake_set_front(int cid, uint64_t sid, ProcessSerialNumber psn) {
    (void)cid; (void)psn; assert(sid == 13); ++front_calls;
    return fail_front ? kCGErrorFailure : kCGErrorSuccess;
}
static CFArrayRef fake_window_spaces(int cid, int selector, CFArrayRef windows) {
    (void)cid; (void)selector;
    uint32_t wid = 0;
    CFNumberGetValue(CFArrayGetValueAtIndex(windows, 0), kCFNumberSInt32Type, &wid);
    uint64_t sid = wid == 100 ? 13 : 12;
    return number_list(&sid, 1);
}
static uint32_t fake_focus(void) {
    if (switch_during_planning && focus_reads++ > 0) return 101;
    return latest_window;
}
static bool fake_bounds(CFStringRef display, CGRect *bounds) {
    (void)display; *bounds = CGRectMake(-1000000, -1000000, 2000000, 2000000); return true;
}
static bool fake_busy(void) { return gesture_in_progress; }
static void capture_post(CGEventTapLocation location, CGEventRef event) {
    assert(location == kCGSessionEventTap);
    assert(CGEventGetIntegerValueField(event, kCGEventSourceUserData) == 1234);
    if (CGEventGetIntegerValueField(event, kEventType) == kGesture) ++posted_companion;
    else {
        const int phases[] = {SwipeBegin, SwipeChange, SwipeEnd};
        assert(CGEventGetIntegerValueField(event, kPhase) == phases[posted_dock % 3]);
        ++posted_dock;
    }
}
static void reset_request(void) {
    if (focus.last_display) CFRelease(focus.last_display);
    focus.last_display = NULL; focus.last_posted = 0;
    posted_dock = posted_companion = front_calls = current_reads = focus_reads = 0;
    land_during_planning = switch_during_planning = gesture_in_progress = fail_front = false;
    latest_window = 100; active_a = 11;
}
int main(void) {
    api.connection = fake_connection; api.displays = fake_displays; api.current = fake_current;
    const uint64_t a[] = {11,12,13}, b[] = {21,22}, last[] = {13}, other[] = {22};
    setup(a,3,b,2);
    FocusLayout p = target(last,1);
    assert(p.display && CFEqual(p.display, CFSTR("A")) && p.current == 0 && p.target == 2 && p.count == 3);
    uint64_t order = p.order; CFRelease(p.display);
    p = target(other,1);
    assert(p.display && CFEqual(p.display, CFSTR("B")) && p.current == 0 && p.target == 1);
    CFRelease(p.display);
    active_a = 13;
    assert(!target(last,1).display); // already visible; no extra switch at an edge
    active_a = 11;
    const uint64_t sticky[] = {13,21};
    assert(!target(sticky,2).display); // visible on the other monitor
    const uint64_t missing[] = {99};
    assert(!target(missing,1).display);
    assert(!target(NULL,0).display);
    const uint64_t reordered[] = {11,13,12};
    setup(reordered,3,b,2);
    p = target(last,1); assert(p.display && p.order != order && p.target == 1); CFRelease(p.display);
    const uint64_t removed[] = {11,12};
    setup(removed,2,b,2); assert(!target(last,1).display);
    setup(a,3,b,2); active_a = 99; assert(!target(last,1).display); // stale active query
    // Full commit-path regressions: no actual desktop events are posted.
    api.front_psn = fake_front; api.set_front = fake_set_front; api.window_spaces = fake_window_spaces;
    read_focus = fake_focus; get_bounds = fake_bounds; post_event = capture_post;
    focus.enabled = focus.augment = true; focus.busy = fake_busy; focus.tag = 1234;
    focus.psn = (ProcessSerialNumber){0,123};
    reset_request(); switch_during_planning = true;
    commit_focus(); assert(posted_dock == 0 && front_calls == 0); // AltTab selects another window
    reset_request(); land_during_planning = true;
    commit_focus(); assert(posted_dock == 0 && front_calls == 0); // macOS already began landing
    reset_request(); gesture_in_progress = true;
    commit_focus(); assert(posted_dock == 0 && front_calls == 0);
    reset_request(); fail_front = true;
    commit_focus(); assert(posted_dock == 0 && front_calls == 1);
    reset_request(); latest_window = 0;
    commit_focus(); assert(posted_dock == 0 && front_calls == 0);
    reset_request();
    commit_focus(); assert(posted_dock == 6 && posted_companion == 6 && front_calls == 1);
    commit_focus(); assert(posted_dock == 6 && front_calls == 1); // duplicate focus notification, stale active query
    active_a = 13;
    commit_focus(); assert(posted_dock == 6 && front_calls == 1); // target now visible
    reset_request();
    focus.enabled = false;
    commit_focus(); assert(posted_dock == 0);
    CFRelease(topology);
    puts("focus layout and handoff tests passed");
}
