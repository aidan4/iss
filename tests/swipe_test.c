#include <assert.h>
#include <stdio.h>
#include "../swipe.h"
static SwipeEvent frame(enum SwipePhase phase, double p, double v) {
    return (SwipeEvent){.phase=phase,.progress=p,.velocity=v};
}
static void start(Swipe *s,int index,int count) {
    *s=(Swipe){0}; swipe_begin(s,frame(SwipeBegin,0,0),index,count,0);
}
static SwipeEvent move(Swipe *s,double p,double t,int observed) {
    return swipe_move(s,frame(SwipeChange,p,0),t,observed,true);
}
static void near(double a,double b) { assert(fabs(a-b)<1e-8); }
int main(void) {
    Swipe s; SwipeEvent e;
    for(int direction=-1;direction<=1;direction+=2) {
        start(&s,2,5);
        e=move(&s,direction*.15,.05,2);
        near(e.progress,direction*.15);
        e=swipe_finish(&s,frame(SwipeEnd,direction*.15,direction*3),.1);
        near(e.progress,direction*.15); near(e.velocity,direction*ISS_FINISH_VELOCITY);
        assert(s.target==2+direction && s.index==2);
    }
    // Quick, far flick still lands once.
    start(&s,0,4); move(&s,.1,.02,0); e=move(&s,2,.1,0);
    assert(e.phase==SwipeChange);
    e=swipe_finish(&s,frame(SwipeEnd,2,3),.12);
    assert(s.target==1);
    // Long drag: preview first, then centered steps with travel AND dwell.
    start(&s,0,4); move(&s,.1,.05,0);
    e=move(&s,.3,.2,0); assert(e.phase==SwipeEnd && s.target==1);
    e=move(&s,.5,.22,0); assert(e.drop && s.index==0);
    e=move(&s,.55,.24,1); assert(e.drop && s.index==1);
    e=move(&s,.6,.3,1); assert(e.drop); // Centered; no premature next preview.
    e=move(&s,.65,.37,1); assert(e.phase==SwipeBegin); near(e.progress,0);
    e=move(&s,.67,.38,1); assert(e.phase==SwipeChange); near(e.progress,.0001);
    e=move(&s,.7,.39,1); assert(e.phase==SwipeEnd && s.target==2);
    near(e.progress,.0001);
    e=move(&s,.75,.42,2); assert(e.drop && s.index==2);
    e=move(&s,.76,.55,2); assert(e.phase==SwipeBegin);
    e=move(&s,.77,.56,2); assert(e.phase==SwipeChange);
    e=move(&s,.78,.57,2); assert(e.phase==SwipeEnd && s.target==3);
    e=move(&s,.8,.6,3); assert(e.drop && s.index==3);
    e=move(&s,2,.8,3); assert(e.drop); // Edge overshoot discarded.
    e=move(&s,1.7,.95,3); assert(e.phase==SwipeBegin);
    e=move(&s,1.65,.96,3); near(e.progress,-.0001);
    e=move(&s,1.6,.97,3); assert(e.phase==SwipeEnd && s.target==2);
    e=swipe_finish(&s,frame(SwipeEnd,1.6,-2),1);
    near(e.progress,0); near(e.velocity,0);
    // Holding still after a small residual movement must not advance a page.
    start(&s,0,3); move(&s,.1,.05,0); move(&s,ISS_SPACE_DISTANCE+.01,.2,0);
    e=move(&s,ISS_SPACE_DISTANCE+.02,.25,1); assert(e.drop);
    e=move(&s,ISS_SPACE_DISTANCE+.02,1,1); assert(e.drop && s.index==1);
    e=swipe_finish(&s,frame(SwipeEnd,.27,0),1.1); near(e.velocity,0);
    // Never continue based on a predicted landing.
    start(&s,0,3); move(&s,.1,.05,0); move(&s,.3,.2,0);
    e=move(&s,2,.4,0); assert(e.drop && s.index==0);
    e=move(&s,3,.9,0); assert(e.drop && s.mode==SwipeFailed);
    e=swipe_finish(&s,frame(SwipeEnd,3,5),1); near(e.velocity,0);
    // Both boundaries, reversal and unknown topology.
    for(int direction=-1;direction<=1;direction+=2) {
        int index=direction<0?0:2;
        start(&s,index,3); e=move(&s,direction*.5,.2,index); near(e.progress,0);
        e=move(&s,direction*.35,.3,index); near(e.progress,-direction*.15);
    }
    start(&s,0,1); e=move(&s,2,.3,0); near(e.progress,0);
    e=swipe_finish(&s,frame(SwipeEnd,2,5),.4); near(e.velocity,0);
    start(&s,-1,0); e=move(&s,.5,.3,-1); near(e.progress,.5);
    e=swipe_finish(&s,frame(SwipeEnd,.5,2),.4); near(e.velocity,2);
    // Small peeks, cancellations and external space changes stay safe.
    start(&s,1,3); move(&s,.1,.05,1);
    e=swipe_finish(&s,frame(SwipeEnd,.1,.005),.1); near(e.velocity,.005);
    start(&s,1,3); move(&s,.1,.05,1);
    e=swipe_finish(&s,frame(SwipeCancel,.1,5),.1); near(e.velocity,0);
    start(&s,1,3); move(&s,.1,.05,1);
    e=swipe_move(&s,frame(SwipeChange,.2,0),.1,1,false);
    assert(e.phase==SwipeCancel);
    puts("swipe state tests passed");
}
