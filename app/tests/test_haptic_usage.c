#include "platform/haptic_stream.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const uint64_t base=UINT64_C(1000000000), ms=UINT64_C(1000000);
static void encode(uint8_t b[9], unsigned type, uint16_t low, uint16_t high, unsigned duration) {
    memset(b,0,9); b[0]=(uint8_t)type;
    b[1]=(uint8_t)low;b[2]=(uint8_t)(low>>8);b[3]=(uint8_t)high;b[4]=(uint8_t)(high>>8);
    for(unsigned i=0;i<4;++i)b[5+i]=(uint8_t)(duration>>(i*8));
}
typedef struct capture { sl_haptic_frame frames[8]; uint64_t sent[8]; unsigned count; uint64_t now; } capture;
static void pump(sl_haptic_stream *s,capture *c,uint64_t until) {
    for(;;) {
        uint64_t next=sl_haptic_deadline(s);
        if(!next || next>until)break;
        if(next<c->now)next=c->now;
        assert(c->count<8);
        assert(sl_haptic_advance(s,next,&c->frames[c->count]));
        c->sent[c->count]=next;
        sl_haptic_output_complete(s,&c->frames[c->count],next,next,true,true);
        ++c->count;c->now=next;
    }
    c->now=until;
}
/* The owner applies an already received backlog while the worker may run
 * between ANY two callbacks. All inputs arrived before the initial drain.
 * Check source order, values and minimum receive intervals independently of
 * internal epochs. A slow worker may stretch, but must never compress them. */
static void backlog_interleavings(void) {
    const unsigned received_ms[]={0,10,20,50};
    const uint16_t levels[]={10000,20000,30000,0};
    unsigned cases=0;
    for(unsigned kind=0;kind<3;++kind)
        for(unsigned cost=0;cost<=15;cost+=5)
            for(unsigned mask=0;mask<16;++mask) {
                sl_haptic_stream s;sl_haptic_init(&s);
                capture c={.now=base+80*ms};
                for(unsigned i=0;i<4;++i) {
                    uint64_t apply=base+(80+i*cost)*ms;
                    c.now=apply;
                    uint8_t b[69];size_t size=9;
                    if(kind<2) encode(b,kind?6:1,levels[i],0,0);
                    else {
                        memset(b,0,sizeof(b));b[0]=13;b[1]=47;
                        if(i<3){b[5]=3;b[8]=(uint8_t)(40+i*40);}
                        size=69;
                    }
                    assert(sl_haptic_command(&s,b,size,base+received_ms[i]*ms,apply,true));
                    if(mask&(1u<<i))pump(&s,&c,apply);
                }
                pump(&s,&c,base+240*ms);
                assert(c.count==4 && !s.count && !s.stats.stale_frames);
                for(unsigned i=0;i<4;++i) {
                    const sl_haptic_frame *f=&c.frames[i];
                    assert(f->sequence==i+1);
                    float expected=kind==2 ? (i<3?(float)(40+i*40)/255*.5571934f:0) :
                        (float)levels[i]/65535*.5571934f;
                    float actual=kind==1?f->values[0].amp_high:f->values[0].amp_low;
                    assert(fabsf(actual-expected)<1e-7f);
                    if(i && c.sent[i]-c.sent[i-1]<(received_ms[i]-received_ms[i-1])*ms) {
                        fprintf(stderr,"backlog compressed kind=%u cost=%u mask=%u event=%u gap=%llu\n",
                                kind,cost,mask,i,(unsigned long long)(c.sent[i]-c.sent[i-1]));
                        assert(!"worker interleaving compressed source interval");
                    }
                }
                /* Catching up a backlog must not add a permanent delay to
                 * genuinely new live input arriving after that playback. */
                uint8_t live[9];encode(live,1,10000,0,0);
                uint64_t live_now=base+250*ms;
                assert(sl_haptic_command(&s,live,9,live_now,live_now,true));
                assert(sl_haptic_deadline(&s)==live_now);
                sl_haptic_frame fresh;
                assert(sl_haptic_advance(&s,live_now,&fresh));
                ++cases;
            }
    assert(cases==192);
    /* The owner can be on time but the worker itself late: arrivals already
     * received before actual playback still belong to the catch-up batch. */
    sl_haptic_stream s;sl_haptic_init(&s);uint8_t on[9],off[9];
    encode(on,1,65535,0,0);encode(off,1,0,0,0);
    assert(sl_haptic_command(&s,on,9,base,base,true));
    sl_haptic_frame f;assert(sl_haptic_advance(&s,base+60*ms,&f));
    sl_haptic_output_complete(&s,&f,base+60*ms,base+61*ms,true,true);
    assert(sl_haptic_command(&s,off,9,base+50*ms,base+62*ms,true));
    assert(sl_haptic_deadline(&s)==base+110*ms);
}
/* Independent two-channel live-use reference: fixed source levels + documented
 * layout, max-band mixing and actual playback-duration expiry. No renderer,
 * scheduler internals, or inverse of their code supplies expected values. */
typedef struct model { float body[2][2], trigger[2]; uint64_t body_end,trigger_end; } model;
static uint32_t random_state=0x53b2a719u;
static uint32_t random_u32(void) { random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state; }
static void expire(model *m,uint64_t now) {
    if(m->body_end && m->body_end<=now){memset(m->body,0,sizeof(m->body));m->body_end=0;}
    if(m->trigger_end && m->trigger_end<=now){memset(m->trigger,0,sizeof(m->trigger));m->trigger_end=0;}
}
static void update(model *m,sl_rumble_layout layout,unsigned type,uint16_t low,uint16_t high,unsigned duration,uint64_t now) {
    float a=(float)low/65535*.5571934f,b=(float)high/65535*.5571934f;
    if(type==1) {
        memset(m->body,0,sizeof(m->body));
        if(layout==SL_RUMBLE_LAYOUT_PRO){m->body[0][0]=m->body[1][0]=a;m->body[0][1]=m->body[1][1]=b;}
        else if(layout==SL_RUMBLE_LAYOUT_PAIRED){m->body[0][0]=a;m->body[1][1]=b;}
        else {unsigned side=layout==SL_RUMBLE_LAYOUT_SINGLE_RIGHT;m->body[side][0]=a;m->body[side][1]=b;}
        m->body_end=(low||high)&&duration?now+duration*ms:0;
    } else {
        m->trigger[0]=a;m->trigger[1]=b;
        if(layout==SL_RUMBLE_LAYOUT_SINGLE_LEFT || layout==SL_RUMBLE_LAYOUT_SINGLE_RIGHT) {
            unsigned side=layout==SL_RUMBLE_LAYOUT_SINGLE_RIGHT;
            m->trigger[side]=fmaxf(a,b);m->trigger[1-side]=0;
        }
        m->trigger_end=(low||high)&&duration?now+duration*ms:0;
    }
}
static void check(const model *m,const sl_haptic_frame *f) {
    for(unsigned i=0;i<2;++i) {
        assert(fabsf(f->values[i].amp_low-m->body[i][0])<1e-7f);
        assert(fabsf(f->values[i].amp_high-fmaxf(m->body[i][1],m->trigger[i]))<1e-7f);
        assert(f->values[i].freq_low==160 && f->values[i].freq_high==320);
    }
}
static void live_reference(void) {
    const uint16_t levels[]={0,1,10000,32768,65535};const unsigned durations[]={0,1,5,20};
    for(unsigned layout=SL_RUMBLE_LAYOUT_PRO;layout<=SL_RUMBLE_LAYOUT_SINGLE_RIGHT;++layout) {
        sl_haptic_stream s;sl_haptic_init(&s);model m={0};
        for(unsigned tick=0;tick<10000;++tick) {
            uint64_t now=base+tick*ms;
            bool must_emit=(m.body_end && m.body_end<=now) || (m.trigger_end && m.trigger_end<=now);
            expire(&m,now);
            if(random_u32()%3) {
                must_emit=true;
                unsigned type=random_u32()%2?1:6,duration=durations[random_u32()%4];
                uint16_t low=levels[random_u32()%5],high=levels[random_u32()%5];uint8_t b[9];
                encode(b,type,low,high,duration);
                assert(sl_haptic_command_layout(&s,b,9,now,now,(sl_rumble_layout)layout));
                update(&m,(sl_rumble_layout)layout,type,low,high,duration,now);
            }
            sl_haptic_frame f;
            bool emitted=sl_haptic_advance(&s,now,&f);
            assert(emitted==must_emit); /* Missing effects/expiry cannot pass silently. */
            if(emitted) {
                check(&m,&f);sl_haptic_output_complete(&s,&f,now,now,true,true);
                assert(!sl_haptic_advance(&s,now,&f));
            }
            assert(!s.count && !s.stats.rejected);
            uint64_t next=m.body_end;
            if(m.trigger_end && (!next || m.trigger_end<next))next=m.trigger_end;
            assert(sl_haptic_deadline(&s)==next);
        }
    }
}
int main(void) { backlog_interleavings();live_reference();puts("PASS 192 owner/worker backlog interleavings and 40000 independent live-use reference ticks"); }
