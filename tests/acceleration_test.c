#include <assert.h>
#include <stdio.h>
#include "../swipe.h"

static void near(double a, double b) { assert(fabs(a-b)<1e-9); }
static Swipe begin(double acceleration, double base, int index, int count) {
    iss_settings = iss_defaults;
    iss_settings.releaseOnly = 1;
    iss_settings.gestureAcceleration = acceleration;
    iss_settings.gestureMultiplier = base;
    Swipe s = {0};
    swipe_begin(&s, (SwipeEvent){.phase=SwipeBegin}, index, count, 0);
    return s;
}
static Swipe move(Swipe s, double travel, double seconds, int samples) {
    for (int i=1; i<=samples; i++) {
        SwipeEvent e=swipe_move(&s, (SwipeEvent){.phase=SwipeChange, .progress=travel*i/samples},
                                seconds*i/samples, s.index, true);
        assert(!e.drop && e.phase==SwipeChange && isfinite(e.progress));
    }
    return s;
}
int main(void) {
    // At zero acceleration, timing cannot alter the existing base multiplier.
    for (int direction=-1; direction<=1; direction+=2) {
        Swipe a=move(begin(0,2.5,4,10),direction*.3,.1,24);
        Swipe b=move(begin(0,2.5,4,10),direction*.3,1,240);
        near(a.position,direction*.75); near(a.position,b.position);
        Swipe slow=move(begin(2,1,4,10),direction*.3,1,120);
        Swipe fast=move(begin(2,1,4,10),direction*.3,.1,24);
        assert(fabs(fast.position)>fabs(slow.position)*1.4);
        assert(fabs(slow.position)>.3 && fabs(fast.position)<.9);
        // Base speed composes with acceleration, but doesn't feed its estimator.
        Swipe doubled=move(begin(2,2,4,10),direction*.3,.1,24);
        near(doubled.position,2*fast.position);
        // Same movement on devices with different event frequencies is similar.
        Swipe at60=move(begin(2,1,4,10),direction*.3,.5,30);
        Swipe at240=move(begin(2,1,4,10),direction*.3,.5,120);
        assert(fabs(at60.position-at240.position)<.01);
        // Pausing never moves the existing preview, and decays the old speed.
        double position=fast.position;
        swipe_move(&fast,(SwipeEvent){.phase=SwipeChange,.progress=direction*.3},1,4,true);
        near(fast.position,position);
        assert(fast.speed<1e-6);
        // A gentle reversal doesn't inherit the previous fast forward boost.
        fast=move(begin(5,1,4,10),direction*.3,.1,24);
        position=fast.position;
        swipe_move(&fast,(SwipeEvent){.phase=SwipeChange,.progress=direction*.299},.11,4,true);
        assert((fast.position-position)*direction<0);
        assert(fabs(fast.position-position)<.0011);
    }
    Swipe forward=move(begin(2,1,4,10),.3,.2,30);
    Swipe backward=move(begin(2,1,4,10),-.3,.2,30);
    near(forward.position,-backward.position);
    // Settings are captured at Begin; live edits apply to the next gesture.
    Swipe snapshot=begin(1.375,1.25,0,10);
    iss_settings.gestureAcceleration=99; iss_settings.gestureMultiplier=99;
    snapshot=move(snapshot,.2,.1,20);
    Swipe reference=move(begin(1.375,1.25,0,10),.2,.1,20);
    near(snapshot.position,reference.position);
    // Fast motion can cross several centers; release uses that boosted position.
    Swipe s=move(begin(5,2,0,10),.7,.1,24);
    assert(s.position>slide_target(10,1)*2);
    double visible=s.position;
    int expected=(int)ceil(visible/slide_target(10,1));
    swipe_finish(&s,(SwipeEvent){.phase=SwipeCancel,.velocity=.1},.11);
    assert(s.target==expected);
    // Even extreme decimal settings remain bounded at the final Space.
    s=move(begin(1e308,1e308,0,10),1,.1,24);
    near(s.position,10);
    swipe_move(&s,(SwipeEvent){.phase=SwipeChange,.progress=1},.2,0,true);
    near(s.position,10);
    // Returning to an edge never accumulates hidden overshoot.
    s=move(begin(3,2,0,3),10,.1,24);
    near(s.position,3);
    swipe_move(&s,(SwipeEvent){.phase=SwipeChange,.progress=9.999},.11,0,true);
    assert(s.position<3 && s.position>2.99);
    // Non-increasing sample timestamps cannot amplify movement or produce NaN.
    s=begin(2,1,0,3);
    SwipeEvent e=swipe_move(&s,(SwipeEvent){.phase=SwipeChange,.progress=.1},0,0,true);
    near(e.progress,.1);
    e=swipe_move(&s,(SwipeEvent){.phase=SwipeChange,.progress=.2},-.01,0,true);
    near(e.progress,.2);
    puts("gesture acceleration tests passed");
}
