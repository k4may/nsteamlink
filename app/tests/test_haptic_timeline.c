#include "platform/haptic_stream.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const uint64_t t = UINT64_C(1000000000), ms = UINT64_C(1000000);
static const uint8_t on[9] = {1,255,255}, off[9] = {1};
static const uint8_t pulse[69] = {13,10,0,0,0,16,0,0,1,64,64,0,0,5,192};
static bool sounding(const sl_haptic_frame *f) {
    return f->values[0].amp_low || f->values[0].amp_high ||
           f->values[1].amp_low || f->values[1].amp_high;
}
static sl_haptic_frame play(sl_haptic_stream *s, uint64_t when) {
    sl_haptic_frame f;
    assert(sl_haptic_advance(s, when, &f));
    sl_haptic_output_complete(s, &f, when, when, true, true);
    return f;
}
int main(void) {
    sl_haptic_stream s;
    sl_haptic_frame f;
    /* Same receive history, independent of one-by-one versus batched drain. */
    for (unsigned delay = 0; delay <= 100; delay += 10) {
        sl_haptic_init(&s);
        uint64_t now = t + delay * ms;
        assert(sl_haptic_command(&s, on, 9, t, now, true));
        assert(sl_haptic_command(&s, off, 9, t+50*ms, now, true));
        f = play(&s, now); assert(sounding(&f));
        assert(!sl_haptic_advance(&s, now+50*ms-1, &f));
        f = play(&s, now+50*ms); assert(!sounding(&f));
    }
    /* Submitted-to-submitted ordinary source gaps do not accumulate IPC time. */
    sl_haptic_init(&s);
    for (unsigned i = 0; i < 3; ++i)
        assert(sl_haptic_command(&s, on, 9, t+i*10*ms, t, true));
    for (unsigned i = 0; i < 3; ++i) {
        assert(sl_haptic_deadline(&s) == t+i*10*ms);
        assert(sl_haptic_advance(&s, t+i*10*ms, &f));
        sl_haptic_output_complete(&s, &f, t+i*10*ms, t+(i*10+1)*ms, true, true);
    }
    /* Retry after duration or stale budget cannot resurrect nonzero output. */
    sl_haptic_init(&s);
    uint8_t short_on[9] = {1,255,255,0,0,10};
    assert(sl_haptic_command(&s, short_on, 9, t, t, true));
    assert(sl_haptic_advance(&s,t,&f));
    sl_haptic_output_complete(&s,&f,t,t,false,true);
    f=play(&s,t+20*ms); assert(!sounding(&f));
    sl_haptic_init(&s);
    assert(sl_haptic_command(&s,short_on,9,t,t,true));
    assert(sl_haptic_advance(&s,t,&f));
    sl_haptic_output_complete(&s,&f,t,t,false,true);
    uint8_t overdue_trigger[9]={6,255,255};
    assert(sl_haptic_command(&s,overdue_trigger,9,t+5*ms,t+5*ms,true));
    f=play(&s,t+20*ms); assert(!f.values[0].amp_low && f.values[0].amp_high>0);
    /* A newly received stop overrides failed same-channel nonzero retry. */
    sl_haptic_init(&s);
    assert(sl_haptic_command(&s,on,9,t,t,true)); f=play(&s,t);
    assert(sl_haptic_command(&s,on,9,t+10*ms,t+10*ms,true));
    assert(sl_haptic_advance(&s,t+10*ms,&f));
    sl_haptic_output_complete(&s,&f,t+10*ms,t+10*ms,false,true);
    assert(sl_haptic_command(&s,off,9,t+11*ms,t+11*ms,true));
    f=play(&s,t+12*ms); assert(!sounding(&f) && f.source==SL_HAPTIC_BODY);
    sl_haptic_init(&s);
    assert(sl_haptic_command(&s,on,9,t,t,true));
    assert(sl_haptic_advance(&s,t,&f));
    sl_haptic_output_complete(&s,&f,t,t,false,true);
    f=play(&s,t+SL_HAPTIC_MAX_LATENCY_NS+1); assert(!sounding(&f) && s.stats.stale_frames==1);
    /* Trigger-only stop leaves the body intact, including at expiry boundary. */
    sl_haptic_init(&s);
    uint8_t trigger[9] = {6,255,255,255,255};
    assert(sl_haptic_command(&s,on,9,t,t,true)); f=play(&s,t);
    assert(sl_haptic_command(&s,trigger,9,t+10*ms,t+10*ms,true));
    f=play(&s,t+10*ms); assert(f.values[0].amp_low>0 && f.values[0].amp_high>0);
    const uint8_t trigger_off[9]={6};
    assert(sl_haptic_command(&s,trigger_off,9,t+20*ms,t+20*ms,true));
    f=play(&s,t+20*ms); assert(f.values[0].amp_low>0 && !f.values[0].amp_high);
    sl_haptic_init(&s);
    assert(sl_haptic_command(&s,short_on,9,t,t,true)); f=play(&s,t);
    assert(sl_haptic_command(&s,trigger,9,t+10*ms,t+10*ms,true));
    f=play(&s,t+10*ms); assert(!f.values[0].amp_low && f.values[0].amp_high>0);
    /* Independent trigger control does not move Nintendo's 0/5/10ms samples. */
    sl_haptic_init(&s);
    assert(sl_haptic_command(&s,pulse,69,t,t,true));
    assert(sl_haptic_command(&s,trigger,9,t+ms,t,true));
    f=play(&s,t); assert(f.native && f.sample==0);
    f=play(&s,t+ms); assert(f.source==SL_HAPTIC_TRIGGERS);
    assert(sl_haptic_deadline(&s)==t+5*ms);
    f=play(&s,t+5*ms); assert(f.native && f.sample==1);
    f=play(&s,t+10*ms); assert(f.native && f.sample==2);
    /* Full/reset and source discontinuity stop promptly and recover without
     * treating a reset's source=0 as a huge new source-time interval. */
    sl_haptic_stop(&s,t+20*ms);
    assert(sl_haptic_command(&s,on,9,t+20*ms,t+20*ms,true));
    f=play(&s,t+20*ms); assert(!sounding(&f));
    assert(sl_haptic_deadline(&s)==t+20*ms);
    f=play(&s,t+20*ms); assert(sounding(&f));
    sl_haptic_discontinuity(&s,t+30*ms); f=play(&s,t+30*ms);
    assert(!sounding(&f));
    assert(!sl_haptic_command(&s,pulse,69,t+31*ms,t+31*ms,true));
    assert(s.stats.desync_rejected==1);
    uint8_t absolute[69]={13,10,0,0,0,16,0,0,1,64,64,0,1,64,64};
    assert(sl_haptic_command(&s,absolute,69,t+32*ms,t+32*ms,true));
    assert(!s.desynchronized);
    /* Stale feedback is observable and never replays arbitrarily late. */
    sl_haptic_init(&s);
    assert(sl_haptic_command(&s,on,9,t,t+300*ms,true));
    f=play(&s,t+300*ms); assert(!sounding(&f) && s.stats.stale_frames==1);
    /* A future ordinary event cannot erase earlier received native samples. */
    sl_haptic_init(&s);
    assert(sl_haptic_command(&s,pulse,69,t,t,true));
    assert(sl_haptic_command(&s,off,9,t+50*ms,t,true));
    assert(s.count==4);
    for(unsigned i=0;i<3;i++){f=play(&s,t+i*5*ms);assert(f.native && f.sample==i);}
    f=play(&s,t+50*ms); assert(!sounding(&f));
    puts("PASS ordinary envelopes, bounded retries, independent channels, source gaps and desync recovery");
}
