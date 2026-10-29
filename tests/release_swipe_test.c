#include <assert.h>
#include <stdio.h>
#include "../swipe.h"
#include "../slide_commit.h"
#include "../release_segment.h"

static SwipeEvent event(enum SwipePhase phase, double progress, double velocity) {
    return (SwipeEvent){.phase=phase, .progress=progress, .velocity=velocity};
}
static void near(double a, double b) { assert(fabs(a-b) < 1e-9); }
static Swipe begin(int index, int count) {
    Swipe s = {0};
    SwipeEvent e = swipe_begin(&s, event(SwipeBegin, 0, 0), index, count, 0);
    assert(!e.drop && e.phase == SwipeBegin);
    return s;
}
int main(void) {
    iss_settings = iss_defaults;
    iss_settings.releaseOnly = 1;
    for (int count=2; count<=8; count++) for (int direction=-1; direction<=1; direction+=2) {
        int origin = direction > 0 ? 0 : count-1;
        Swipe s = begin(origin, count);
        double span = slide_target(count, 1);
        // Holding never starts a timed slide, no matter duration or distance.
        for (int sample=1; sample<=20; sample++) {
            SwipeEvent e = swipe_move(&s, event(SwipeChange, direction*sample, direction), sample, origin, true);
            assert(!e.drop && e.phase == SwipeChange && e.velocity == 0);
            near(e.progress, direction * fmin(sample, (count-1)*span));
            assert(!s.paging && s.target == -1);
        }
        // Overshoot is discarded: reversal immediately changes the preview.
        double end = s.position;
        SwipeEvent e = swipe_move(&s, event(SwipeChange, direction*19.9, -direction), 20.1, origin, true);
        near(e.progress, end-direction*.1);
        // Reversed momentum selects the nearest center behind the visible position.
        e = swipe_finish(&s, event(SwipeEnd, 0, -direction*.2), 20.11);
        assert(e.phase == SwipeEnd && s.target == (direction > 0 ? count-2 : 1));
        near(e.progress, end-direction*.1);
        // A short flick in either direction advances even before halfway.
        s = begin(origin, count);
        swipe_move(&s, event(SwipeChange, direction*.01, 0), .01, origin, true);
        e = swipe_finish(&s, event(SwipeEnd, 0, direction*.001), .02);
        assert(!e.drop && s.target == origin+direction);
        near(e.progress, direction*.01);
        // Devices with zero End velocity use recent travel, not a reset End position.
        s = begin(origin, count);
        swipe_move(&s, event(SwipeChange, direction*.01, 0), .01, origin, true);
        swipe_finish(&s, event(SwipeEnd, 0, 0), .02);
        assert(s.target == origin+direction);
        // A stationary release chooses the closest center; tie stays at origin.
        for (int offset=-1; offset<=1; offset++) {
            s = begin(origin, count);
            swipe_move(&s, event(SwipeChange, direction*(span/2+offset*.01), 0), .1, origin, true);
            swipe_finish(&s, event(SwipeEnd, 0, 0), .5);
            assert(s.target == origin+(offset > 0 ? direction : 0));
        }
        // Suppress outward Begin/Changed/End, including opposing release jitter.
        s = (Swipe){0};
        assert(swipe_begin(&s, event(SwipeBegin, -direction*.1, 0), origin, count, 0).drop);
        assert(swipe_move(&s, event(SwipeChange, -direction*.2, 0), .1, origin, true).drop);
        assert(swipe_finish(&s, event(SwipeEnd, 0, direction), .2).drop);
        // A real inward reversal can reopen immediately, with visible travel.
        s = (Swipe){0};
        swipe_begin(&s, event(SwipeBegin, -direction*.1, 0), origin, count, 0);
        e = swipe_move(&s, event(SwipeChange, -direction*.09, 0), .1, origin, true);
        assert(!e.drop && e.phase == SwipeBegin); near(e.progress, direction*.01);
        swipe_finish(&s, event(SwipeEnd, 0, direction), .11);
        assert(s.target == origin+direction);
    }
    // A Begin/End-only flick can still use the movement in Begin.
    Swipe short_swipe = {0};
    swipe_begin(&short_swipe, event(SwipeBegin, .02, 0), 0, 3, 1);
    swipe_finish(&short_swipe, event(SwipeEnd, 0, 0), 1.01);
    assert(short_swipe.target == 1);
    double gains[] = {.5, 1, 1.275, 2.5};
    for (unsigned i=0; i<sizeof gains/sizeof *gains; i++) {
        iss_settings.gestureMultiplier = gains[i];
        Swipe s = {0};
        SwipeEvent e = swipe_begin(&s, event(SwipeBegin, .01, 1), 1, 3, 0);
        near(e.progress, .01*gains[i]); assert(e.velocity == 0);
        // Changes made mid-gesture must not alter its mode or gain.
        iss_settings.gestureMultiplier = 99; iss_settings.releaseOnly = 0;
        e = swipe_move(&s, event(SwipeChange, .1, 1), .2, 1, true);
        near(e.progress, .1*gains[i]); assert(e.phase == SwipeChange);
        iss_settings.releaseOnly = 1;
    }
    iss_settings.gestureMultiplier = 1;
    Swipe s = begin(1, 3);
    assert(swipe_move(&s, event(SwipeChange, .2, 0), .1, 1, false).phase == SwipeCancel);
    assert(swipe_finish(&s, event(SwipeEnd, 0, 1), .2).drop);
    s = begin(1, 3);
    swipe_move(&s, event(SwipeChange, .8, 0), .1, 1, true);
    assert(swipe_finish(&s, event(SwipeCancel, 0, 1), .2).phase == SwipeEnd);
    assert(s.target == 2);
    s = (Swipe){0};
    assert(swipe_begin(&s, event(SwipeBegin, 0, 0), 0, 1, 0).drop);
    assert(swipe_finish(&s, event(SwipeEnd, 0, 1), .1).drop);
    // A fast second gesture waits for confirmation, then previews without paging.
    s = begin(0, 3);
    swipe_move(&s, event(SwipeChange, .2, 0), .1, 0, true);
    swipe_finish(&s, event(SwipeEnd, 0, .1), .11);
    assert(swipe_begin(&s, event(SwipeBegin, 0, 0), 0, 3, .12).drop);
    assert(swipe_move(&s, event(SwipeChange, .1, 0), .13, 0, true).drop);
    assert(swipe_move(&s, event(SwipeChange, .2, 0), .14, 1, true).phase == SwipeBegin);
    assert(swipe_move(&s, event(SwipeChange, .4, 0), 1, 1, true).phase == SwipeChange);
    swipe_finish(&s, event(SwipeEnd, 0, 1), 1.01);
    assert(s.target == 2);
    assert(slide_context_valid(1, 1, 3, 1, 3, false));
    assert(!slide_context_valid(1, 1, 3, 2, 3, false));
    // Both native terminal decisions must land from SCALED preview travel.
    // macOS Cancel uses raw progress and is not authoritative after scaling.
    for (int phase=SwipeEnd; phase<=SwipeCancel; phase+=4) {
        for (int direction=-1; direction<=1; direction+=2) {
            for (int multiplier=1; multiplier<=4; multiplier++) {
                iss_settings.gestureMultiplier = multiplier;
                int origin = direction > 0 ? 0 : 5;
                s = begin(origin, 6);
                // Raw movement is always 0.6 Spaces; visible travel is 0.6–2.4.
                double raw = direction * slide_target(6, 1) * .6;
                SwipeEvent e = swipe_move(&s, event(SwipeChange, raw, 0), .1, origin, true);
                near(e.progress, raw * multiplier);
                e = swipe_finish(&s, event(phase, 0, 0), .5);
                assert(e.phase == SwipeEnd);
                assert(s.target == origin + direction * (int)round(.6*multiplier));
                near(e.progress, raw * multiplier);
                assert(slide_context_valid(origin, s.target, 6, origin, 6, true));
            }
        }
    }
    iss_settings.gestureMultiplier = 2.5;
    for (int direction=-1; direction<=1; direction+=2) {
        // Several centers crossed, then reversal, all during the same gesture.
        s = begin(3, 7);
        double span = slide_target(7, 1);
        swipe_move(&s, event(SwipeChange, direction*span, 0), .1, 3, true);
        near(s.position, direction*span*2.5);
        swipe_move(&s, event(SwipeChange, direction*span*.9, 0), .2, 3, true);
        swipe_finish(&s, event(SwipeEnd, 0, -direction), .21);
        assert(s.target == 3 + direction*2);
    }
    // Validate commit distance independently from the native one-Space End.
    for (int origin=0; origin<7; origin++) for (int target=0; target<7; target++) {
        if (abs(target-origin) > 1) { assert(slide_commit_count(origin,target) == 0); continue; }
        int commits=0, begins=0, cancels=0;
        int segment_origin=origin;
        int direction = (target > origin) - (target < origin);
        int frames = slide_commit_count(origin, target);
        for (int f=0; f<frames; f++) {
            SwipeEvent e = slide_commit_event(origin, target, 7, f);
            if (!f) {
                assert(e.phase == SwipeChange);
                near(e.progress, (target-origin)*slide_target(7, 1));
            }
            // World position must remain at the rendered target across ALL
            // commit frames, not snap to epsilon at each native segment.
            near(segment_origin + e.progress / slide_target(7, 1), target);
            if (e.phase == SwipeEnd) {
                commits++; assert(e.velocity*direction > 0);
                assert(fabs(e.velocity) <= 1.0/65536);
                segment_origin += direction;
            }
            if (e.phase == SwipeBegin) begins++;
            if (e.phase == SwipeCancel) { cancels++; near(e.progress, 0); }
        }
        assert(commits == abs(target-origin));
        assert(begins == (commits > 0 ? commits-1 : 0));
        assert(cancels == (origin == target));
    }
    assert(!slide_context_valid(0, 3, 5, 0, 5, false));
    assert(slide_context_valid(0, 3, 5, 0, 5, true));
    assert(!slide_context_valid(0, 3, 5, 0, 6, true));
    assert(!slide_context_valid(0, 3, 5, 4, 5, true));
    // Native segment bookkeeping follows continuous finger travel. Neither
    // forward crossings nor reversals ever send an End beyond one full Space.
    for (int origin=0; origin<10; origin++) {
        ReleaseSegment segment = release_segment_begin(origin);
        SwipeEvent frames[40];
        int base=origin;
        for (int sample=0; sample<=180; sample++) {
            double absolute = sample <= 90 ? sample/10.0 : (180-sample)/10.0;
            int n = release_segment_move(&segment, origin, 10,
                (absolute-origin)*slide_target(10,1), frames, 40);
            assert(n >= 0);
            for (int f=0; f<n; f++) {
                SwipeEvent e=frames[f];
                assert(fabs(e.progress) <= slide_target(10,1)+1e-9);
                if (e.phase == SwipeEnd) {
                    base += e.progress > 0 ? 1 : -1;
                    assert(base >= 0 && base < 10);
                }
            }
            near(base+segment.position/slide_target(10,1), absolute);
            assert(base == segment.index);
        }
    }
    near(slide_frame_interval(.06), .0075);
    assert(isfinite(slide_frame_interval(1e308)));
    puts("release swipe tests passed");
}
