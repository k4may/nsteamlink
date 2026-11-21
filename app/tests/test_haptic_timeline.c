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
static void live_rumble_settings(void) {
    sl_haptic_stream s;
    sl_haptic_init(&s);
    const uint8_t body[9] = {1,255,255,255,255,10};
    const uint8_t trigger[9] = {6,255,255,0,0,15};
    assert(sl_haptic_command(&s, body, 9, t, t, true));
    assert(sl_haptic_command(&s, trigger, 9, t+ms, t, true));
    assert(sl_haptic_command(&s, off, 9, t+20*ms, t, true));
    play(&s, t);
    play(&s, t+ms);

    sl_rumble_render_config config = sl_rumble_render_default_config();
    config.low_max_amplitude = config.high_max_amplitude = 0;
    sl_haptic_configure_rumble(&s, &config, 3);
    sl_haptic_frame f = play(&s, t+2*ms);
    assert(f.source == SL_HAPTIC_PROFILE && f.values[0].amp_high > 0);
    assert(!f.values[0].amp_low && !f.values[1].amp_low && !f.values[1].amp_high);
    assert(s.count == 1 && s.expires_ns == t+10*ms && s.trigger_expires_ns == t+16*ms);

    config.low_hz = 100;
    config.high_hz = 500;
    config.low_max_amplitude = config.high_max_amplitude = .5f;
    sl_haptic_configure_rumble(&s, &config, 3);
    f = play(&s, t+4*ms);
    assert(f.source == SL_HAPTIC_PROFILE && f.values[0].amp_high > 0 && !f.values[0].amp_low);
    assert(f.values[1].amp_low == .5f && f.values[1].amp_high == .5f);
    assert(f.values[1].freq_low == 100 && f.values[1].freq_high == 500);
    f = play(&s, t+10*ms);
    assert(f.source == SL_HAPTIC_EXPIRY && f.values[0].amp_high > 0 && !f.values[1].amp_high);
    f = play(&s, t+16*ms);
    assert(f.source == SL_HAPTIC_EXPIRY && !sounding(&f));
    assert(sl_haptic_deadline(&s) == t+20*ms);
    assert(play(&s, t+20*ms).source == SL_HAPTIC_BODY);

    const uint8_t sony[69] = {13,47,0,0,0,3,0,64,128};
    assert(sl_haptic_command(&s, sony, 69, t+25*ms, t+25*ms, true));
    f = play(&s, t+25*ms);
    assert(f.source == SL_HAPTIC_SONY && !f.values[0].amp_low && !f.values[0].amp_high);
    assert(fabsf(f.values[1].amp_low - .5f*128/255) < 1e-6f);
    assert(fabsf(f.values[1].amp_high - .5f*64/255) < 1e-6f && f.values[1].freq_high == 500);

    sl_haptic_init(&s);
    assert(sl_haptic_command(&s, pulse, 69, t, t, true));
    assert(play(&s, t).native && s.count > 0);
    unsigned pending = s.count;
    uint64_t next = sl_haptic_deadline(&s);
    sl_haptic_configure_rumble(&s, &config, 3);
    assert(s.count == pending && sl_haptic_deadline(&s) == next);
    assert(!sl_haptic_advance(&s, next-1, &f));
    f = play(&s, next);
    assert(f.native && sounding(&f));
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
    /* A failed composition must honor either channel's stop. Keep the
     * other-channel frame as a retry, including its sequence/deadline. */
    for (unsigned failed_trigger = 0; failed_trigger < 2; ++failed_trigger) {
        sl_haptic_init(&s);
        const uint8_t trigger_on[9]={6,255,255}, trigger_zero[9]={6};
        assert(sl_haptic_command(&s,on,9,t,t,true)); f=play(&s,t);
        assert(sl_haptic_command(&s,trigger_on,9,t+ms,t+ms,true)); f=play(&s,t+ms);
        const uint8_t *retry = failed_trigger ? trigger_on : on;
        const uint8_t *stop_other = failed_trigger ? off : trigger_zero;
        assert(sl_haptic_command(&s,retry,9,t+2*ms,t+2*ms,true));
        assert(sl_haptic_advance(&s,t+2*ms,&f));
        uint64_t retry_sequence=f.sequence;
        sl_haptic_output_complete(&s,&f,t+2*ms,t+2*ms,false,true);
        assert(sl_haptic_command(&s,stop_other,9,t+4*ms,t+4*ms,true));
        assert(sl_haptic_advance(&s,t+3*ms,&f));
        assert(f.values[0].amp_low>0 && f.values[0].amp_high>0);
        sl_haptic_output_complete(&s,&f,t+3*ms,t+3*ms,false,true);
        f=play(&s,t+4*ms);
        assert(f.sequence==retry_sequence);
        assert(failed_trigger ? !f.values[0].amp_low && f.values[0].amp_high>0 :
                               f.values[0].amp_low>0 && !f.values[0].amp_high);
        assert(!s.count);
    }
    /* A trigger stop during failed native output retains the native identity
     * and holds the next sample 5ms after successful completion. */
    sl_haptic_init(&s);
    const uint8_t trigger_on[9]={6,255,255}, trigger_zero[9]={6};
    const uint8_t native_on[69]={13,10,0,0,0,16,0,128,129,32,96,128,129,32,96};
    assert(sl_haptic_command(&s,trigger_on,9,t,t,true)); f=play(&s,t);
    assert(sl_haptic_command(&s,native_on,69,t+ms,t+ms,true));
    assert(sl_haptic_advance(&s,t+ms,&f) && f.native);
    assert(sounding(&f));
    uint64_t native_sequence=f.sequence;
    sl_haptic_output_complete(&s,&f,t+ms,t+ms,false,true);
    assert(sl_haptic_command(&s,trigger_zero,9,t+2*ms,t+2*ms,true));
    assert(sl_haptic_advance(&s,t+3*ms,&f) && f.native && f.sequence==native_sequence);
    assert(f.values[0].amp_low>0 && f.values[1].amp_low>0);
    assert(!s.triggers[0].amp_high && !s.triggers[1].amp_high);
    sl_haptic_output_complete(&s,&f,t+3*ms,t+4*ms,true,true);
    assert(sl_haptic_command(&s,pulse,69,t+6*ms,t+6*ms,true));
    assert(sl_haptic_deadline(&s)>=t+9*ms);
    assert(s.stats.emitted_samples==1 && !s.stats.superseded_samples);
    /* A native zero selected to replace a failed native sample is played,
     * so only the failed sample belongs in the superseded counter. */
    sl_haptic_init(&s);
    const uint8_t native_off[69]={13,10,0,0,0,16,0,0,1,64,64,0,1,64,64};
    assert(sl_haptic_command(&s,native_on,69,t,t,true));
    assert(sl_haptic_advance(&s,t,&f) && sounding(&f));
    sl_haptic_output_complete(&s,&f,t,t,false,true);
    assert(sl_haptic_command(&s,native_off,69,t+ms,t+ms,true));
    f=play(&s,t+5*ms); assert(f.native && !sounding(&f));
    assert(s.stats.emitted_samples==1 && s.stats.superseded_samples==1);
    /* A new opposite-channel stop cannot extend the failed source's stale
     * budget and accidentally retry old nonzero body output forever. */
    sl_haptic_init(&s);
    assert(sl_haptic_command(&s,on,9,t,t,true));
    assert(sl_haptic_advance(&s,t,&f));
    sl_haptic_output_complete(&s,&f,t,t,false,true);
    uint64_t late_stop=t+SL_HAPTIC_MAX_LATENCY_NS+1;
    assert(sl_haptic_command(&s,trigger_zero,9,late_stop,late_stop,true));
    f=play(&s,late_stop);
    assert(!sounding(&f) && s.stats.stale_frames==1);
    /* Stopping only one hand/band while the other remains active is also a
     * stop; it must not wait for a whole-channel zero to unblock retries. */
    for(unsigned failed_trigger=0;failed_trigger<2;++failed_trigger) {
        sl_haptic_init(&s);
        const uint8_t full_body[9]={1,255,255,255,255};
        const uint8_t partial_body[9]={1,0,0,255,255}, partial_trigger[9]={6,0,0,255,255};
        assert(sl_haptic_command(&s,full_body,9,t,t,true)); f=play(&s,t);
        assert(sl_haptic_command(&s,trigger,9,t+ms,t+ms,true)); f=play(&s,t+ms);
        assert(sl_haptic_command(&s,failed_trigger?trigger:full_body,9,t+2*ms,t+2*ms,true));
        assert(sl_haptic_advance(&s,t+2*ms,&f));
        sl_haptic_output_complete(&s,&f,t+2*ms,t+2*ms,false,true);
        assert(sl_haptic_command(&s,failed_trigger?partial_body:partial_trigger,9,t+3*ms,t+3*ms,true));
        f=play(&s,t+3*ms);
        assert(failed_trigger ? !s.body[0].amp_low && s.body[1].amp_high>0 :
                               !s.triggers[0].amp_high && s.triggers[1].amp_high>0);
    }
    sl_haptic_init(&s);
    const uint8_t full_body[9]={1,255,255,255,255}, partial_body[9]={1,0,0,255,255,5};
    assert(sl_haptic_command(&s,full_body,9,t,t,true));
    assert(sl_haptic_advance(&s,t,&f));
    sl_haptic_output_complete(&s,&f,t,t,false,true);
    assert(sl_haptic_command(&s,partial_body,9,t+ms,t+ms,true));
    f=play(&s,t+ms); assert(!f.values[0].amp_low && f.values[1].amp_high>0);
    assert(s.expires_ns==t+6*ms);
    f=play(&s,t+6*ms); assert(!sounding(&f));
    live_rumble_settings();
    puts("PASS ordinary envelopes, live rumble settings, bounded retries, independent channels, source gaps and desync recovery");
}
