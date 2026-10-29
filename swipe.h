#ifndef ISS_SWIPE_H
#define ISS_SWIPE_H

#include <stdbool.h>
#include <math.h>

#include "settings.h"
#include "slide.h"
#define ISS_SPACE_DISTANCE iss_settings.spaceDistance
#define ISS_REVERSE_DISTANCE iss_settings.reverseDistance
#define ISS_INITIAL_TRIGGER_DISTANCE iss_settings.initialDistance
#define ISS_INITIAL_TRIGGER_SECONDS iss_settings.initialSeconds
#define ISS_CENTER_SECONDS iss_settings.centerSeconds
#define ISS_LONG_SWIPE_SECONDS iss_settings.longSeconds
#define ISS_FLICK_VELOCITY iss_settings.flickVelocity
#define ISS_EDGE_REVERSAL_DISTANCE iss_settings.edgeReverseDistance
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
    int index, count, target, last_direction, pending_direction;
    enum SwipeMode mode;
    double started, waiting_since, last, position, pending, centered_since, edge_travel, reverse_travel, preview_gain, motion_delta, motion_time;
    double acceleration, sample_time, speed;
    int speed_direction;
    bool active, open, changed, rebased, passthrough, paging, edge_blocked, fast_reverse, release_only;
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

// One continuous coordinate, relative to the physical gesture's origin.
// Clamp only at the display's ends, discarding overshoot for immediate reversal.
static inline double swipe_release_preview(const Swipe *s, double position) {
    double span = slide_target(s->count, 1);
    return fmax(-s->index * span, fmin((s->count - 1 - s->index) * span, position));
}

// Measure raw finger speed, independently of the base multiplier. Native
// Changed events usually carry zero velocity, so derive it from travel/time.
// A time-based filter smooths uneven event delivery without adding an idle timer.
static inline double swipe_scaled_delta(Swipe *s, double delta, double now) {
    double elapsed = now - s->sample_time;
    if (elapsed > 0) s->sample_time = now;
    if (s->acceleration == 0 || elapsed <= 0 || !isfinite(elapsed))
        return delta * s->preview_gain;
    int direction = (delta > 0) - (delta < 0);
    if (direction && direction != s->speed_direction) {
        s->speed = 0; // Don't carry a fast forward boost into a gentle reversal.
        s->speed_direction = direction;
    }
    double instant = fmin(1e6, fabs(delta) / fmax(elapsed, .001));
    double alpha = -expm1(-elapsed / .035);
    s->speed += alpha * (instant - s->speed);
    if (delta == 0) return 0; // A stopped finger never moves the preview.
    // Smoothly approach 1 + acceleration times the base speed. Slow travel
    // stays close to the base; the bounded curve avoids runaway gain spikes.
    double boost = 1 + s->acceleration * (s->speed / (s->speed + 2));
    return delta * s->preview_gain * boost;
}

static inline int swipe_release_target(const Swipe *s, double momentum) {
    double offset = s->position / slide_target(s->count, 1);
    // Avoid selecting an extra Space because of floating-point noise at a center.
    if (fabs(offset - round(offset)) < 1e-7) offset = round(offset);
    double steps = momentum > 0 ? ceil(offset) : momentum < 0 ? floor(offset) :
                   copysign(ceil(fabs(offset) - 0.5), offset);
    return (int)fmax(0, fmin(s->count - 1, s->index + steps));
}

// Track movement direction independently of accumulated forward distance.
// A deliberate reversal discards that backlog instead of making the fingers
// drag all the way back through it. Tiny jitter merely pauses forward paging.
static inline void swipe_accumulate_pending(Swipe *s, double delta) {
    if (!isfinite(delta) || delta == 0) return;
    if (!s->pending_direction)
        s->pending_direction = s->last_direction ? s->last_direction : (delta > 0 ? 1 : -1);
    if (delta * s->pending_direction < 0) {
        s->reverse_travel += fabs(delta);
        if (s->reverse_travel >= ISS_REVERSE_DISTANCE) {
            s->pending_direction = -s->pending_direction;
            s->pending = s->pending_direction * s->reverse_travel;
            s->reverse_travel = 0;
            return;
        }
    } else {
        s->reverse_travel = 0;
    }
    s->pending += delta;
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
                 .passthrough = false,
                 .release_only = iss_settings.releaseOnly >= 0.5,
                 .preview_gain = iss_settings.gestureMultiplier,
                 .acceleration = iss_settings.gestureAcceleration, .sample_time = now };
    // During a topology update, never guess a boundary from an invalid index.
    if (index < 0 || count < 1 || index >= count || (s->release_only && count == 1)) {
        s->mode = SwipeFailed;
        s->open = false;
        return swipe_drop(e);
    }
    if (waiting) {
        s->mode = SwipeWaiting;
        s->target = target;
        s->last_direction = s->pending_direction = target > index ? 1 : -1;
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
    if (s->release_only) {
        s->position = swipe_release_preview(s, e.progress * s->preview_gain);
        s->rebased = true;
        if (fabs(s->position) > 1e-6) { s->motion_delta = s->position; s->motion_time = now; }
        e.progress = s->position;
        e.velocity = 0;
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
    if (s->release_only && s->mode == SwipeWaiting) {
        // A new physical gesture may arrive before the previous landing is
        // confirmed. Start a fresh preview only after that confirmation.
        if (observed_index == s->target) {
            s->index = observed_index; s->target = -1; s->position = 0;
            s->mode = SwipeTracking; s->open = true; s->changed = false;
            s->sample_time = now; s->speed = 0; s->speed_direction = 0;
            e.phase = SwipeBegin;
            return swipe_zero(e);
        }
        if ((observed_index >= 0 && observed_index != s->index) ||
            now - s->waiting_since >= ISS_CONFIRM_SECONDS) s->mode = SwipeFailed;
        return swipe_drop(e);
    }
    if (s->release_only) {
        delta = swipe_scaled_delta(s, delta, now);
        if (fabs(delta) > 1e-6) { s->motion_delta = delta; s->motion_time = now; }
    }
    if (s->mode == SwipeWaiting || s->mode == SwipeBetween) {
        swipe_accumulate_pending(s, delta);
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
        bool reversing = s->last_direction && s->pending_direction != s->last_direction;
        double distance = reversing ? ISS_REVERSE_DISTANCE : ISS_SPACE_DISTANCE;
        if (s->reverse_travel > 0 || fabs(s->pending) < distance ||
            (!reversing && now - s->centered_since < ISS_CENTER_SECONDS)) return swipe_drop(e);
        s->fast_reverse = reversing;
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
        if (s->edge_travel == 0 || (!s->release_only && fabs(s->edge_travel) < ISS_EDGE_REVERSAL_DISTANCE)) return swipe_drop(e);
        s->edge_blocked = false;
        s->position = s->edge_travel;
        s->edge_travel = 0;
        if (!s->open) {
            s->open = true;
            s->changed = false;
            e.phase = SwipeBegin;
            if (s->release_only) {
                s->position = swipe_release_preview(s, s->position);
                e.progress = s->position; e.velocity = 0;
                return e;
            }
            return swipe_zero(e);
        }
    } else {
        s->position += delta;
    }
    if (s->release_only) {
        s->position = swipe_release_preview(s, s->position);
        s->changed = true;
        e.progress = s->position;
        e.velocity = 0;
        return e;
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
    // Start the first slide sooner; continued paging retains its own travel
    // threshold and timing so this does not increase multi-space sensitivity.
    double trigger_distance = s->fast_reverse ? ISS_REVERSE_DISTANCE :
                              s->paging ? ISS_SPACE_DISTANCE : ISS_INITIAL_TRIGGER_DISTANCE;
    double trigger_seconds = s->fast_reverse ? 0 :
                             s->paging ? ISS_LONG_SWIPE_SECONDS : ISS_INITIAL_TRIGGER_SECONDS;
    if (s->changed && now - s->started >= trigger_seconds &&
        fabs(s->position) >= trigger_distance) {
        double direction = copysign(1, s->position);
        s->target = s->index + (int)direction;
        s->last_direction = s->pending_direction = (int)direction;
        s->reverse_travel = 0;
        s->pending = s->position - direction * trigger_distance;
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
    if (s->release_only) {
        if (!was_open) return swipe_drop(e);
        if (s->mode == SwipeFailed) {
            s->target = -1;
            e.phase = SwipeCancel;
            return swipe_zero(e);
        }
        // End can reset progress to zero. Keep the last displayed position,
        // but use release momentum to choose the next center in that direction.
        // Some devices omit terminal velocity; recent physical movement is the
        // fallback. A held/stationary release instead chooses the nearest center.
        double momentum = isfinite(e.velocity) && fabs(e.velocity) > 1e-6 ? e.velocity :
                          now - s->motion_time <= 0.10 ? s->motion_delta : 0;
        // Native Cancel also represents a finger lift that macOS judged too
        // short using unscaled travel. Recompute both terminal phases from our
        // displayed position; real topology/tap failures are handled above.
        s->target = swipe_release_target(s, momentum);
        int direction = (s->target > s->index) - (s->target < s->index);
        e.phase = SwipeEnd;
        e.progress = s->position;
        e.velocity = 0;
        s->last_direction = s->pending_direction = direction;
        s->pending = s->reverse_travel = 0;
        s->mode = SwipeWaiting;
        s->waiting_since = now;
        return e;
    }
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
    if (e.velocity != 0 && fabs(e.velocity) >= ISS_FLICK_VELOCITY)
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
        s->last_direction = s->pending_direction = (int)direction;
        s->reverse_travel = 0;
        s->mode = SwipeWaiting;
        s->waiting_since = now;
        s->pending = 0;
    }
    return e;
}
#endif
