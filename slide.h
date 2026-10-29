#ifndef ISS_SLIDE_H
#define ISS_SLIDE_H
#include <math.h>
#include <stdbool.h>
#include "settings.h"
#define ISS_SLIDE_SECONDS iss_settings.slideSeconds
// FasterSwiper's space-state conversion: N/(N-1) progress per full space.
static inline double slide_target(int spaces, double direction) {
    return spaces > 1 ? copysign((double)spaces / (spaces - 1), direction) : 0;
}
static inline double slide_position(double start, double target, double elapsed) {
    double t = fmax(0, fmin(1, elapsed / ISS_SLIDE_SECONDS));
    return start + (target - start) * t;
}
static inline double slide_frame_interval(double duration) {
    // Keep exact deadlines for short slides without overflowing on large
    // manually entered durations. Duration has a positive, finite domain.
    double frames = ceil(duration * 120);
    return isfinite(frames) ? duration / fmax(1, frames) : 1.0 / 120;
}
// The active space may become the target before the final frame commits.
// Default stepping still accepts only adjacent destinations. Continuous mode
// allows any bounded destination, with the origin retained until commitment.
static inline bool slide_context_valid(int origin, int target, int count,
                                       int observed, int observed_count, bool continuous) {
    return count > 1 && count == observed_count && origin >= 0 && origin < count &&
           target >= 0 && target < count && (continuous || (target-origin >= -1 && target-origin <= 1)) &&
           (observed == origin || observed == target);
}
#endif
