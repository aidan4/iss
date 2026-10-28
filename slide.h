#ifndef ISS_SLIDE_H
#define ISS_SLIDE_H
#include <math.h>
#include <stdbool.h>
#define ISS_INITIAL_SLIDE_SECONDS 0.060
// FasterSwiper's space-state conversion: N/(N-1) progress per full space.
static inline double slide_target(int spaces, double direction) {
    return spaces > 1 ? copysign((double)spaces / (spaces - 1), direction) : 0;
}
static inline double slide_position(double start, double target, double elapsed) {
    double t = fmax(0, fmin(1, elapsed / ISS_INITIAL_SLIDE_SECONDS));
    return start + (target - start) * t;
}
// The active space may become the target before the final frame commits.
// A third space, invalid destination, or new space count invalidates the slide.
static inline bool slide_context_valid(int origin, int target, int count,
                                       int observed, int observed_count) {
    return count > 1 && count == observed_count && origin >= 0 && origin < count &&
           target >= 0 && target < count && (target-origin == 1 || origin-target == 1) &&
           (observed == origin || observed == target);
}
#endif
