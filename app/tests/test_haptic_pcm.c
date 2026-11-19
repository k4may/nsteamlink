#include "platform/haptic_stream.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    sl_pcm_haptics *pcm = calloc(1, sizeof(*pcm));
    assert(pcm);
    sl_pcm_config config = {.sample_rate=8000,.format=SL_PCM_S16LE,
        .layout=SL_PCM_LEFT_GRIP_RIGHT_GRIP,.gain=1,.max_amplitude=.5571934f,
        .max_block_frames=800,.queue_frames=32};
    assert(sl_pcm_init(pcm,&config));
    uint8_t audio[400*4];
    for(unsigned i=0;i<400;i++) {
        int16_t sample[2]={(int16_t)(12000*sin(6.283185307179586*160*i/8000)),
                           (int16_t)(6000*sin(6.283185307179586*500*i/8000))};
        for(unsigned side=0;side<2;side++) {
            uint16_t v=(uint16_t)sample[side];
            audio[4*i+2*side]=(uint8_t)v; audio[4*i+2*side+1]=(uint8_t)(v>>8);
        }
    }
    const uint64_t t=UINT64_C(1000000000);
    sl_pcm_block block={.source=SL_PCM_DEDICATED_HAPTICS,.layout=config.layout,.format=config.format,
        .sample_rate=config.sample_rate,.frames=400,.data=audio,.bytes=sizeof(audio),
        .epoch=1,.sequence=0,.source_frame=0,.pts_ns=t,.flags=SL_PCM_EOS};
    assert(sl_pcm_push(pcm,&block)==SL_PCM_OK);
    sl_haptic_stream s; sl_haptic_init(&s);
    sl_pcm_frame p; sl_haptic_frame f; unsigned count=0; bool left=false,right=false;
    while(sl_pcm_pop(pcm,&p)) {
        uint64_t available=p.available_pts_ns;
        assert(sl_haptic_pcm_frame(&s,&p,available,available));
        uint64_t next=sl_haptic_deadline(&s);
        assert(next && sl_haptic_advance(&s,next,&f));
        assert(f.source==SL_HAPTIC_PCM && !f.native);
        for(unsigned i=0;i<2;i++) {
            assert(memcmp(&p.values[i],&f.values[i],sizeof(sl_hd_value))==0);
            assert(f.values[i].amp_low>=0 && f.values[i].amp_low<=1);
        }
        left|=f.values[0].amp_low>0; right|=f.values[1].amp_low>0 || f.values[1].amp_high>0;
        sl_haptic_output_complete(&s,&f,next,next,true,true);
        ++count;
    }
    assert(count>5 && left && right && s.stats.pcm_frames==count);
    assert(!f.values[0].amp_low && !f.values[0].amp_high && !f.values[1].amp_low && !f.values[1].amp_high);
    assert(s.pcm_ended);
    assert(!sl_haptic_pcm_frame(&s,&p,p.available_pts_ns,p.available_pts_ns));
    p.flags=SL_PCM_FRAME_RESET; p.epoch=2; p.available_pts_ns+=10000000;
    assert(sl_haptic_pcm_frame(&s,&p,p.available_pts_ns,p.available_pts_ns));
    assert(s.pcm_started && !s.pcm_ended && s.pcm_epoch==2);
    p.flags=0; p.epoch=1; p.available_pts_ns+=5000000;
    assert(!sl_haptic_pcm_frame(&s,&p,p.available_pts_ns,p.available_pts_ns));
    p.epoch=2; p.values[0].amp_low=NAN;
    assert(!sl_haptic_pcm_frame(&s,&p,p.available_pts_ns,p.available_pts_ns));
    free(pcm);
    puts("PASS dedicated PCM to source-timed native output frames, explicit EOS and epoch recovery");
}
