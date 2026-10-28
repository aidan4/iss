#include <assert.h>
#include <stdio.h>
#include "../swipe.h"
#include "../slide.h"
static SwipeEvent frame(enum SwipePhase phase, double p, double v) {
    return (SwipeEvent){.phase=phase,.progress=p,.velocity=v};
}
static void start(Swipe *s,int index,int count) {
    *s=(Swipe){0}; swipe_begin(s,frame(SwipeBegin,0,0),index,count,0);
}
static SwipeEvent move(Swipe *s,double p,double t,int observed) {
    return swipe_move(s,frame(SwipeChange,p,0),t,observed,true);
}
static void near(double a,double b) { if(fabs(a-b)>=1e-8) fprintf(stderr,"near failed actual=%g expected=%g\n",a,b); assert(fabs(a-b)<1e-8); }
int main(void) {
    Swipe s; SwipeEvent e;
    // A full visual space depends on the size of the space list. Timed
    // frames reach that exact endpoint at the configured deadline, independent of event rate.
    near(slide_target(1,1),0);
    near(slide_target(2,1),2);
    near(slide_target(3,1),1.5);
    near(slide_target(4,-1),-4.0/3);
    near(slide_position(.2,1.5,0),.2);
    near(slide_position(.2,1.5,ISS_SLIDE_SECONDS / 2),.85);
    near(slide_position(.2,1.5,ISS_SLIDE_SECONDS),1.5);
    near(slide_position(.2,1.5,.2),1.5);
    near(slide_position(-.2,-1.5,ISS_SLIDE_SECONDS / 2),-.85);
    for(int direction=-1;direction<=1;direction+=2) {
        start(&s,2,5);
        e=move(&s,direction*.15,.05,2);
        near(e.progress,direction*.15);
        e=swipe_finish(&s,frame(SwipeEnd,direction*.15,direction*3),.1);
        near(e.progress,direction*.15); near(e.velocity,direction*ISS_FINISH_VELOCITY);
        assert(s.target==2+direction && s.index==2);
    }
    // First trigger: both the shorter time and distance gates must pass.
    for(int direction=-1; direction<=1; direction+=2) {
        start(&s,1,3);
        e=move(&s,direction*.02,.01,1); assert(e.phase==SwipeChange);
        e=move(&s,direction*.07,.039,1); assert(e.phase==SwipeChange);
        e=move(&s,direction*.07,.04,1); assert(e.phase==SwipeEnd);
        assert(s.target==1+direction);
        near(s.pending,direction*(.07-ISS_INITIAL_TRIGGER_DISTANCE));
        start(&s,1,3);
        move(&s,direction*.02,.01,1);
        e=move(&s,direction*.03,.3,1); assert(e.phase==SwipeChange);
    }
    // Quick, far flick starts earlier but still requests only one landing.
    start(&s,0,4); move(&s,.1,.02,0); e=move(&s,2,.1,0);
    assert(e.phase==SwipeEnd);
    e=swipe_finish(&s,frame(SwipeEnd,2,3),.12);
    assert(s.target==1);
    // Long drag: preview first, then centered steps with travel AND dwell.
    start(&s,0,4); move(&s,.1,.05,0);
    e=move(&s,.3,.2,0); assert(e.phase==SwipeEnd && s.target==1);
    near(e.velocity,ISS_FINISH_VELOCITY); near(e.progress,.3);
    e=move(&s,.5,.22,0); assert(e.drop && s.index==0);
    e=move(&s,.55,.24,1); assert(e.drop && s.index==1);
    e=move(&s,.6,.3,1); assert(e.drop); // Centered; no premature next preview.
    e=move(&s,.65,.40,1); assert(e.phase==SwipeBegin); near(e.progress,0);
    e=move(&s,.67,.41,1); assert(e.phase==SwipeChange); near(e.progress,.0001);
    e=move(&s,.7,.42,1); assert(e.phase==SwipeEnd && s.target==2);
    near(e.velocity,ISS_FINISH_VELOCITY);
    near(e.progress,.0001);
    e=move(&s,.75,.45,2); assert(e.drop && s.index==2);
    e=move(&s,.76,.61,2); assert(e.phase==SwipeBegin);
    e=move(&s,.77,.62,2); assert(e.phase==SwipeChange);
    e=move(&s,.78,.63,2); assert(e.phase==SwipeEnd && s.target==3);
    e=move(&s,.8,.66,3); assert(e.drop && s.index==3);
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
    e=swipe_finish(&s,frame(SwipeEnd,3,5),1); assert(e.drop);
    // Both boundaries, reversal and unknown topology.
    for(int direction=-1;direction<=1;direction+=2) {
        int index=direction<0?0:2;
        start(&s,index,3); e=move(&s,direction*.5,.2,index); near(e.progress,0);
        e=move(&s,direction*.35,.3,index); assert(e.phase==SwipeBegin); near(e.progress,0);
        e=move(&s,direction*.34,.31,index); near(e.progress,-direction*.16);
    }
    start(&s,0,1); e=move(&s,2,.3,0); near(e.progress,0);
    e=swipe_finish(&s,frame(SwipeEnd,2,5),.4); assert(e.drop);
    start(&s,-1,0); e=move(&s,.5,.3,-1); assert(e.drop);
    e=swipe_finish(&s,frame(SwipeEnd,.5,2),.4);
    assert(e.drop);
    // Small peeks, cancellations and external space changes stay safe.
    start(&s,1,3); move(&s,.1,.05,1);
    e=swipe_finish(&s,frame(SwipeEnd,.1,.005),.1); near(e.velocity,.005);
    start(&s,1,3); move(&s,.1,.05,1);
    e=swipe_finish(&s,frame(SwipeCancel,.1,5),.1); near(e.velocity,0);
    start(&s,1,3); move(&s,.1,.05,1);
    e=swipe_move(&s,frame(SwipeChange,.2,0),.1,1,false);
    assert(e.phase==SwipeCancel);
    // Continued travel uses the wider distance and longer centered dwell.
    s=(Swipe){.index=2,.count=5,.target=-1,.mode=SwipeBetween,.active=true,
              .last_direction=1,.pending_direction=1,.centered_since=1};
    e=move(&s,.15,1.2,2); assert(e.drop); // Old 0.125 threshold no longer enough.
    e=move(&s,.17,1.21,2); assert(e.phase==SwipeBegin);
    s=(Swipe){.index=2,.count=5,.target=-1,.mode=SwipeBetween,.active=true,
              .last_direction=1,.pending_direction=1,.centered_since=1};
    e=move(&s,.2,1.13,2); assert(e.drop); // Old 120 ms pause no longer enough.
    e=move(&s,.21,1.16,2); assert(e.phase==SwipeBegin);

    // Reversal while the slide is playing discards a large forward backlog.
    // It still waits for the actual landing, then bypasses the centered dwell
    // and the long-drag delay, in either direction.
    for(int direction=-1; direction<=1; direction+=2) {
        start(&s,2,5);
        move(&s,direction*.02,.01,2);
        e=move(&s,direction*.1,.05,2); assert(e.phase==SwipeEnd);
        swipe_accumulate_pending(&s,direction*.9); s.last+=direction*.9;
        swipe_accumulate_pending(&s,-direction*.01); s.last-=direction*.01;
        assert(s.pending*direction>0 && s.pending_direction==direction);
        swipe_accumulate_pending(&s,-direction*.04); s.last-=direction*.04;
        near(s.pending,-direction*.05); // No need to unwind the prior 0.9 travel.
        e=move(&s,direction*.95,.06,2); assert(e.drop);
        e=move(&s,direction*.95,.07,2+direction);
        assert(e.phase==SwipeBegin && s.fast_reverse);
        e=move(&s,direction*.94,.08,2+direction); assert(e.phase==SwipeChange);
        e=move(&s,direction*.93,.09,2+direction);
        assert(e.phase==SwipeEnd && s.target==2);
        assert(e.velocity*direction<0);
    }
    // Small back-and-forth jitter cannot change direction or discard travel.
    s=(Swipe){.pending=.5,.last_direction=1,.pending_direction=1};
    swipe_accumulate_pending(&s,-.01);
    swipe_accumulate_pending(&s,.02);
    near(s.pending,.51); assert(s.pending_direction==1 && s.reverse_travel==0);

    // Nonzero Began must not start an edge rubber-band. A reversed release
    // (including a discrete swipe without Changed) must not fling backwards.
    for(int direction=-1;direction<=1;direction+=2) {
        int index=direction<0?0:2;
        s=(Swipe){0};
        e=swipe_begin(&s,frame(SwipeBegin,direction*.02,0),index,3,0);
        assert(e.drop && s.edge_blocked);
        e=move(&s,direction*.019,.02,index); assert(e.drop);
        e=swipe_finish(&s,frame(SwipeEnd,direction*.019,-direction*3),.05);
        assert(e.drop && s.target==-1);
        s=(Swipe){0};
        swipe_begin(&s,frame(SwipeBegin,direction*.02,0),index,3,0);
        e=swipe_finish(&s,frame(SwipeEnd,direction*.02,-direction*4),.02);
        assert(e.drop);
        // A deliberate reversal during the drag still works.
        s=(Swipe){0};
        swipe_begin(&s,frame(SwipeBegin,direction*.02,0),index,3,0);
        e=move(&s,-direction*.05,.05,index);
        assert(!s.edge_blocked && e.phase==SwipeBegin);
        e=move(&s,-direction*.06,.06,index);
        assert(e.phase==SwipeChange && e.progress*direction<0);
    }
    // Addition/removal (including removing the last target) invalidates the
    // whole animation, rather than recomputing a different destination.
    assert(slide_context_valid(1,2,3,1,3));
    assert(slide_context_valid(1,2,3,2,3));
    assert(!slide_context_valid(1,2,3,1,4));
    assert(!slide_context_valid(1,2,3,1,2));
    assert(!slide_context_valid(1,2,3,0,3));
    assert(!slide_context_valid(2,3,3,2,3));
    assert(!slide_context_valid(0,-1,3,0,3));
    assert(!slide_context_valid(0,1,1,0,1));
    puts("swipe state tests passed");
}
