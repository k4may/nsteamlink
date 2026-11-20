#include "../../tools/haptics-panel-state.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    nsl_panel_state s = {.low=123, .high=45678, .on_ms=100, .off_ms=200,
        .limit_ms=10000, .mode=NSL_PANEL_HOLD};
    uint16_t out[2];
    assert(nsl_panel_start(&s,1000));
    assert(nsl_panel_next(&s,1000,out) && out[0]==123 && out[1]==45678);
    assert(!nsl_panel_next(&s,1099,out));
    assert(nsl_panel_next(&s,1100,out)); /* refresh held output */
    s.low=1; s.high=65535;
    assert(nsl_panel_next(&s,1101,out) && out[0]==1 && out[1]==65535);
    nsl_panel_stop(&s);
    assert(nsl_panel_next(&s,1102,out) && !out[0] && !out[1]);
    assert(!nsl_panel_next(&s,1202,out)); /* stopped output doesn't spam */
    s.mode=NSL_PANEL_REPEAT; assert(nsl_panel_start(&s,2000));
    assert(nsl_panel_next(&s,2000,out) && out[0]==1);
    assert(nsl_panel_next(&s,2100,out) && !out[0]);
    assert(!nsl_panel_next(&s,2299,out) && !out[0]);
    assert(nsl_panel_next(&s,2300,out) && out[1]==65535);
    s.on_ms=500; /* changing timing while playing takes effect */
    assert(nsl_panel_next(&s,2500,out) && !out[0]);
    assert(nsl_panel_next(&s,2700,out) && out[0]==1);
    s.mode=NSL_PANEL_ONCE; s.on_ms=100; assert(nsl_panel_start(&s,3000));
    assert(nsl_panel_next(&s,3000,out) && out[0]);
    assert(nsl_panel_next(&s,3100,out) && !out[0] && !s.active);
    s.mode=NSL_PANEL_HOLD; s.limit_ms=100;
    assert(nsl_panel_start(&s,4000)); assert(nsl_panel_next(&s,4000,out));
    assert(nsl_panel_next(&s,4100,out) && !out[0] && !s.active);
    assert(nsl_panel_start(&s,5000)); assert(nsl_panel_next(&s,5000,out));
    assert(nsl_panel_next(&s,4999,out) && !out[0] && !s.active);
    s.limit_ms=600000; s.mode=NSL_PANEL_HOLD; assert(nsl_panel_start(&s,0));
    for(unsigned i=0;i<=65535;++i) {
        s.low=(uint16_t)i; s.high=(uint16_t)(65535-i);
        assert(nsl_panel_next(&s,i,out));
        assert(out[0]==i && out[1]==65535-i);
    }
    s.on_ms=0; assert(!nsl_panel_start(&s,0));
    s.on_ms=100; s.mode=(nsl_panel_mode)3; assert(!nsl_panel_start(&s,0));
    s.mode=NSL_PANEL_HOLD;s.limit_ms=0;assert(nsl_panel_start(&s,0));
    assert(nsl_panel_next(&s,UINT64_C(86400000),out) && s.active && out[0]);
    puts("haptics panel scheduling, live changes, precision and stops passed");
}
