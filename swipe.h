#ifndef ISS_SWIPE_H
#define ISS_SWIPE_H

#include <stdbool.h>
#include <math.h>

#define ISS_SPACE_DISTANCE 0.125
#define ISS_CENTER_SECONDS 0.12
#define ISS_LONG_SWIPE_SECONDS 0.18
#define ISS_FLICK_VELOCITY 0.08
#define ISS_EDGE_REVERSAL_DISTANCE 0.04
#define ISS_FINISH_VELOCITY 999999.0
#define ISS_CONFIRM_SECONDS 0.6

enum SwipePhase { SwipeBegin = 1, SwipeChange = 2, SwipeEnd = 4, SwipeCancel = 8 };
enum SwipeMode { SwipeTracking, SwipeWaiting, SwipeBetween, SwipeFailed };
typedef struct {
    enum SwipePhase phase;
    double progress, velocity;
    bool drop;
} SwipeEvent;
typedef struct {
    int index, count, target;
    enum SwipeMode mode;
    double started, waiting_since, last, position, pending, centered_since, edge_travel;
    bool active, open, changed, rebased, passthrough, paging, edge_blocked;
} Swipe;

// Native events already include the scroll preference. On 26 AND 27 positive
// native progress means increasing space index. Synthetic events on 27 have a
// different, preference-dependent sign, handled by the timed-frame adapter.
static inline double swipe_input_sign(int major) {
    return major >= 26 ? 1.0 : -1.0;
}
static inline bool swipe_allowed(const Swipe *s, double direction) {
    return direction > 0 ? s->index + 1 < s->count : direction < 0 && s->index > 0;
}
static inline double swipe_preview(double position) {
    // Preview travel and paging sensitivity are independent. Changing how
    // far each step takes must not shorten an ordinary quick flick preview.
    return fmax(-1.0, fmin(1.0, position));
}
static inline SwipeEvent swipe_drop(SwipeEvent e) {
    e.drop = true;
    return e;
}
static inline SwipeEvent swipe_zero(SwipeEvent e) {
    e.progress = e.velocity = 0;
    return e;
}

// Preserve a pending confirmation across quick successive physical gestures.
// The caller resets this state if the display or ordered space IDs change.
static inline SwipeEvent swipe_begin(Swipe *s, SwipeEvent e, int index, int count, double now) {
    bool waiting = s->mode == SwipeWaiting && index == s->index && count == s->count &&
                   now - s->waiting_since < ISS_CONFIRM_SECONDS;
    int target = s->target;
    double waiting_since = s->waiting_since;
    *s = (Swipe){ .index = index, .count = count, .target = -1,
                 .started = now, .last = e.progress, .position = e.progress,
                 .active = true, .open = true,
                 .passthrough = false };
    // During a topology update, never guess a boundary from an invalid index.
    if (index < 0 || count < 1 || index >= count) {
        s->mode = SwipeFailed;
        s->open = false;
        return swipe_drop(e);
    }
    if (waiting) {
        s->mode = SwipeWaiting;
        s->target = target;
        s->waiting_since = waiting_since;
        s->open = false;
        s->rebased = true;
        return swipe_drop(e);
    }
    // Began already carries real travel. Clamp it before the Dock can start
    // an edge rubber-band, and ignore an opposite release velocity afterward.
    if (e.progress != 0 && !swipe_allowed(s, e.progress)) {
        s->edge_blocked = s->rebased = true;
        s->position = 0;
        s->open = false;
        return swipe_drop(e);
    }
    return e;
}

static inline SwipeEvent swipe_move(Swipe *s, SwipeEvent e, double now,
                                     int observed_index, bool same_spaces) {
    if (!s->active || s->passthrough) return e;
    if (!isfinite(e.progress) || !isfinite(e.velocity)) return swipe_drop(e);
    if (!same_spaces) {
        s->mode = SwipeFailed;
        if (s->open) {
            s->open = false;
            e.phase = SwipeCancel;
            return swipe_zero(e);
        }
    }
    if (s->mode == SwipeFailed) return swipe_drop(e);

    double delta = e.progress - s->last;
    s->last = e.progress;
    if (s->mode == SwipeWaiting || s->mode == SwipeBetween) {
        s->pending += delta;
        if (s->mode == SwipeWaiting) {
            if (observed_index == s->target) {
                s->index = observed_index; // Only advance after the Dock confirms.
                s->target = -1;
                s->mode = SwipeBetween;
                s->centered_since = now;
            } else {
                if ((observed_index >= 0 && observed_index != s->index) ||
                    now - s->waiting_since >= ISS_CONFIRM_SECONDS)
                    s->mode = SwipeFailed;
                return swipe_drop(e);
            }
        }
        if (!swipe_allowed(s, s->pending)) {
            s->pending = 0; // Discard edge overshoot; reversal responds immediately.
            return swipe_drop(e);
        }
        // Once the first preview has landed, keep the current space centered.
        // Require both fresh travel and a short dwell before another page.
        if (fabs(s->pending) < ISS_SPACE_DISTANCE ||
            now - s->centered_since < ISS_CENTER_SECONDS) return swipe_drop(e);
        s->paging = true;
        s->position = s->pending;
        s->pending = 0;
        s->mode = SwipeTracking;
        s->open = true;
        s->changed = false;
        s->rebased = true;
        e.phase = SwipeBegin;
        return swipe_zero(e);
    }

    if (s->edge_blocked) {
        s->edge_travel += delta;
        if (!swipe_allowed(s, s->edge_travel)) s->edge_travel = 0;
        if (fabs(s->edge_travel) < ISS_EDGE_REVERSAL_DISTANCE) return swipe_drop(e);
        s->edge_blocked = false;
        s->position = s->edge_travel;
        s->edge_travel = 0;
        if (!s->open) {
            s->open = true;
            s->changed = false;
            e.phase = SwipeBegin;
            return swipe_zero(e);
        }
    } else {
        s->position += delta;
    }
    if (s->position != 0 && !swipe_allowed(s, s->position)) {
        s->position = 0;
        s->rebased = s->edge_blocked = true;
        s->edge_travel = 0;
        if (s->open) {
            s->open = false;
            e.phase = SwipeCancel;
            return swipe_zero(e);
        }
        return swipe_drop(e);
    }
    double preview = s->paging ? copysign(0.0001, s->position) : swipe_preview(s->position);
    if (preview != e.progress) s->rebased = true;
    e.progress = preview;
    // Emit at most one phase per hardware sample. Never send End/Begin/End
    // back-to-back, or predict several switches the Dock hasn't processed yet.
    if (s->changed && now - s->started >= ISS_LONG_SWIPE_SECONDS &&
        fabs(s->position) >= ISS_SPACE_DISTANCE) {
        double direction = copysign(1, s->position);
        s->target = s->index + (int)direction;
        s->pending = s->position - direction * ISS_SPACE_DISTANCE;
        s->mode = SwipeWaiting;
        s->waiting_since = now;
        s->open = false;
        s->rebased = true;
        e.phase = SwipeEnd;
        e.velocity = direction * ISS_FINISH_VELOCITY;
        return e;
    }
    s->changed = true;
    return e;
}

static inline SwipeEvent swipe_finish(Swipe *s, SwipeEvent e, double now) {
    if (!s->active) return e;
    s->active = false;
    bool was_open = s->open;
    s->open = false;
    if (s->passthrough) return e;
    // An edge gesture that never opened a Dock segment must not deliver an
    // orphan End. Even End(0,0) can make the Dock bounce back on macOS 27.
    if (!was_open && (s->edge_blocked || s->mode == SwipeFailed)) return swipe_drop(e);
    // Always deliver the physical terminal event, including after an early
    // commit. macOS 27 needs it to close the hardware gesture's native state.
    if (s->mode == SwipeFailed) e.phase = SwipeCancel;
    if (s->mode != SwipeTracking || s->edge_blocked) return swipe_zero(e);
    if (s->rebased) e.progress = s->paging ? copysign(0.0001, s->position) : swipe_preview(s->position);
    if (e.phase == SwipeCancel) {
        e.velocity = 0;
        return e;
    }
    if (!isfinite(e.velocity)) e.velocity = 0;
    double direction = 0;
    if (fabs(e.velocity) >= ISS_FLICK_VELOCITY)
        direction = copysign(1, e.velocity);
    else if (fabs(s->position) >= 0.5)
        direction = copysign(1, s->position);
    if (direction != 0 && !swipe_allowed(s, direction)) {
        return swipe_zero(e);
    }
    // Let an actual reversal or small peek settle natively. For a commit,
    // change ONLY velocity; resetting progress here caused blank landings.
    if (direction != 0 && direction * s->position >= 0) {
        e.velocity = direction * ISS_FINISH_VELOCITY;
        s->target = s->index + (int)direction;
        s->mode = SwipeWaiting;
        s->waiting_since = now;
        s->pending = 0;
    }
    return e;
}
#endif
