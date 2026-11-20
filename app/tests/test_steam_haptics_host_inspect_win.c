/* Real Windows memory-query/read operations over a synthetic pinned layout.
 * This tests discovery and state reading, not Steam's real ownership/layout. */
#define WIN32_LEAN_AND_MEAN
#include "../../tools/steam-haptics-host-inspect-win.h"
#include <assert.h>
#include <stdio.h>

static void put(void *memory,size_t offset,uint64_t value,size_t size) {
    memcpy((unsigned char *)memory+offset,&value,size);
}
int main(void) {
    const uint64_t base=UINT64_C(0x138000000), handle=UINT64_C(0x0057e2009f4e844f);
    unsigned char *parent=VirtualAlloc(NULL,0x11c000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    unsigned char *io=VirtualAlloc(NULL,0x3000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    unsigned char *device=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    unsigned char *config=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    assert(parent && io && device && config);
    put(parent,0,base+0x1304dc0,8);put(parent,0x10922d,1,1);put(parent,0x180,(uintptr_t)io,8);
    put(io,0,base+0x1304c68,8);put(io,0x1c0,(uintptr_t)device,8);put(io,0x2a0,(uintptr_t)config,8);
    put(device,0,base+0x1318fd0,8);put(device,8,38,4);put(device,0x98,handle,8);put(device,0xb8,1,1);
    put(device,0x80,0x8b6bff,8);put(device,0x588,1,1);put(io,0x1d1,1,1);
    put(config,0x240,0x8b6bff,8);put(config,0x230,38,4);put(config,0x258,413080,4);put(config,0x661,1,1);
    put(parent,0x1f0,1,1);put(parent,0x2bc,1,1);
    nsl_host_haptics_state state={0};nsl_host_inspection report;uint64_t cached=0;
    assert(nsl_host_inspect_process(GetCurrentProcess(),base,handle,&cached,true,&state,&report)==1);
    assert(!strcmp(report.source,"private_memory_candidate") && report.candidates>=1);
    assert(state.appid==413080 && state.pulse_gate && state.enhanced && state.haptics_preference);
    put(io,0x1d1,0,1);put(config,0x661,0,1);
    assert(nsl_host_inspect_process(GetCurrentProcess(),base,handle,&cached,false,&state,&report)==1);
    assert(!strcmp(report.source,"validated_parent_cache") && !state.pulse_gate && !state.config_pulse);
    put(device,0x98,handle+1,8);
    assert(nsl_host_inspect_process(GetCurrentProcess(),base,handle,&cached,false,&state,&report)==0);
    assert(!cached && report.bounded);
    VirtualFree(config,0,MEM_RELEASE);VirtualFree(device,0,MEM_RELEASE);
    VirtualFree(io,0,MEM_RELEASE);VirtualFree(parent,0,MEM_RELEASE);
    puts("{\"passed\":true,\"real_process_memory_operations\":true,\"actual_steam_verified\":false}");
    return 0;
}
