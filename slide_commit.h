#ifndef ISS_SLIDE_COMMIT_H
#define ISS_SLIDE_COMMIT_H
#include <stdlib.h>
#include "swipe.h"

// Fully crossed Spaces are committed at their centers during the preview.
// A timed landing must close just the remaining adjacent segment, never a
// batch of catch-up gestures from the physical gesture's original Space.
static inline int slide_commit_count(int origin, int target) {
    return abs(target-origin) <= 1 ? 2 : 0;
}
static inline SwipeEvent slide_commit_event(int origin, int target, int spaces, int frame) {
    double progress = (target-origin)*slide_target(spaces, 1);
    if (!frame) return (SwipeEvent){.phase=SwipeChange, .progress=progress};
    if (origin == target) return (SwipeEvent){.phase=SwipeCancel};
    return (SwipeEvent){.phase=SwipeEnd, .progress=progress,
                        .velocity=(target-origin)/65536.0};
}
#endif
