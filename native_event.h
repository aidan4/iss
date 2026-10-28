#ifndef ISS_NATIVE_EVENT_H
#define ISS_NATIVE_EVENT_H
#include <ApplicationServices/ApplicationServices.h>
#include "swipe.h"

static const CGEventField kEventType = 55;
static const CGEventField kHIDType = 110;
static const CGEventField kMotion = 123;
static const CGEventField kProgress = 124;
static const CGEventField kVelocityX = 129;
static const CGEventField kVelocityY = 130;
static const CGEventField kPhase = 132;
enum { kGesture = 29, kDockControl = 30, kDockSwipe = 23, kHorizontal = 1 };

// Modify only fields that changed. In particular, ordinary Begin/Changed
// events and all their device, direction, position and raw IOHID fields survive.
static inline CGEventRef apply_edit(CGEventRef ev, SwipeEvent input, SwipeEvent output, double sign) {
    if (output.drop) return NULL;
    if (output.phase != input.phase)
        CGEventSetIntegerValueField(ev, kPhase, output.phase);
    if (output.progress != input.progress)
        CGEventSetDoubleValueField(ev, kProgress, output.progress * sign);
    if (output.velocity != input.velocity)
        CGEventSetDoubleValueField(ev, kVelocityX, output.velocity * sign);
    // Neutral terminal events close the hardware gesture without a second fling.
    if ((output.phase == SwipeEnd || output.phase == SwipeCancel) && output.velocity == 0)
        CGEventSetDoubleValueField(ev, kVelocityY, 0);
    return ev;
}

#endif
