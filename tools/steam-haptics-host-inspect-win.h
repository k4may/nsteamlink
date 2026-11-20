/* Read-only discovery of the pinned controller object when the API-user tree
 * does not contain it. No writes, injection, debugger or memory dumps.
 * A bounded scan may be incomplete; failure is never a closed-gate result. */
#ifndef NSL_STEAM_HAPTICS_HOST_INSPECT_WIN_H
#define NSL_STEAM_HAPTICS_HOST_INSPECT_WIN_H
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "steam-haptics-host-state.h"
#include <tlhelp32.h>

typedef struct nsl_host_inspection {
    const char *source;
    uint64_t scanned_bytes;
    unsigned candidates;
    int tree_result;
    unsigned candidate_read_failures;
    bool bounded;
} nsl_host_inspection;

static inline bool nsl_host_process_read(void *process, uint64_t address, void *out, size_t size) {
    SIZE_T actual = 0;
    return ReadProcessMemory((HANDLE)process, (const void *)(uintptr_t)address, out, size, &actual) && actual == size;
}

static inline int nsl_host_inspect_process(HANDLE process, uint64_t base, uint64_t handle,
        uint64_t *cached_parent, bool allow_scan, nsl_host_haptics_state *out, nsl_host_inspection *report) {
    *report = (nsl_host_inspection){.source="api_user_tree"};
    int result = nsl_host_haptics_state_read(nsl_host_process_read, process, base, handle, out);
    report->tree_result=result;
    if (result == 1) { *cached_parent=out->parent_address; return 1; }
    if (*cached_parent) {
        report->source="validated_parent_cache";
        result=nsl_host_haptics_parent_read(nsl_host_process_read,process,base,*cached_parent,handle,out);
        if(result==1) return 1;
        *cached_parent=0;
    }
    report->source="private_memory_candidate";
    if(!allow_scan) {report->bounded=true;return 0;}
    const uint64_t limit=UINT64_C(256)*1024*1024, deadline=GetTickCount64()+750;
    unsigned char *bytes=malloc(65536);
    if(!bytes) return -1;
    /* First check region prefixes: large controller allocations often start
     * near their allocation base. Then scan remaining bytes within the budget. */
    for(unsigned pass=0;pass<2;++pass) {
        uintptr_t address=0; MEMORY_BASIC_INFORMATION region;
        while(VirtualQueryEx(process,(void *)address,&region,sizeof(region))==sizeof(region)) {
            uintptr_t start=(uintptr_t)region.BaseAddress, end=start+region.RegionSize;
            if(end<=address) break;
            address=end;
            if(GetTickCount64()>=deadline || report->scanned_bytes>=limit) {report->bounded=true;goto done;}
            if(region.State!=MEM_COMMIT || region.Type!=MEM_PRIVATE ||
               (region.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
               !(region.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))) continue;
            uintptr_t cursor=pass?start+65536:start;
            uintptr_t stop=pass?end:(end-start>65536?start+65536:end);
            while(cursor<stop) {
                if(GetTickCount64()>=deadline || report->scanned_bytes>=limit) {report->bounded=true;goto done;}
                SIZE_T size=stop-cursor>65536?65536:stop-cursor, actual=0;
                BOOL read=ReadProcessMemory(process,(void *)cursor,bytes,size,&actual);
                report->scanned_bytes+=size;
                if(read && actual==size) for(SIZE_T offset=0;offset+8<=actual;offset+=8) {
                    uint64_t vt;memcpy(&vt,bytes+offset,8);
                    if(vt!=base+0x1304dc0) continue;
                    ++report->candidates;
                    result=nsl_host_haptics_parent_read(nsl_host_process_read,process,base,cursor+offset,handle,out);
                    if(result==1) {*cached_parent=out->parent_address;free(bytes);return 1;}
                    if(result<0) ++report->candidate_read_failures;
                }
                cursor+=size;
            }
        }
    }
done:
    free(bytes);return 0;
}
#endif
