#ifndef ISS_RELEASE_SEGMENT_H
#define ISS_RELEASE_SEGMENT_H
#include "swipe.h"

typedef struct {
    int index, low, high;
    double position;
    bool open;
} ReleaseSegment;

static inline ReleaseSegment release_segment_begin(int index) {
    return (ReleaseSegment){.index=index, .low=index, .high=index};
}

// Follow a global finger position through adjacent native gestures. Commit a
// segment only at its exact center, so release never has to replay old crossings.
// The caller prepares all frames before applying the returned state.
static inline int release_segment_move(ReleaseSegment *segment, int origin, int count,
                                        double progress, SwipeEvent *frames, int capacity) {
    ReleaseSegment next = *segment;
    double span = slide_target(count, 1);
    double absolute = fmax(0, fmin(count-1, origin + progress/span));
    double local = (absolute-next.index)*span;
    int used = 0;
#define EMIT(phase_, progress_, velocity_) do { \
    if (used >= capacity) return -1; \
    frames[used++] = (SwipeEvent){.phase=(phase_), .progress=(progress_), .velocity=(velocity_)}; \
} while (0)
    // Reversing through this segment's origin closes the old side at zero.
    if (next.open && next.position*local < 0) {
        EMIT(SwipeChange, 0, 0);
        EMIT(SwipeCancel, 0, 0);
        next.open = false;
    }
    while (fabs(local) >= span - 1e-9) {
        int direction = local > 0 ? 1 : -1;
        if (!next.open) EMIT(SwipeBegin, 0, 0);
        EMIT(SwipeChange, direction*span, 0);
        EMIT(SwipeEnd, direction*span, direction/65536.0);
        next.index += direction;
        if (next.index < next.low) next.low = next.index;
        if (next.index > next.high) next.high = next.index;
        next.open = false;
        local = (absolute-next.index)*span;
    }
    if (fabs(local) < 1e-9) local = 0;
    if (local != 0) {
        if (!next.open) EMIT(SwipeBegin, 0, 0);
        EMIT(SwipeChange, local, 0);
        next.open = true;
    } else if (next.open) {
        EMIT(SwipeChange, 0, 0);
        EMIT(SwipeCancel, 0, 0);
        next.open = false;
    }
    next.position = local;
    *segment = next;
#undef EMIT
    return used;
}
#endif
