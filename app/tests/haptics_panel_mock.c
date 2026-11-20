/* Test-only Steam/XInput DLL. Never distributed or used as Valve evidence. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

typedef struct object { void **vtable; } object;
static void *client_slots[13], *input_slots[46];
static object client={client_slots},input={input_slots};
static uint64_t handle=0x123456789;
static unsigned low,high,calls,mask=1,empty,type=10,shutdown_count;
static bool initialized,shutdown_done,released;
__declspec(dllexport) uint64_t NslMockRead(unsigned which) {
    switch(which) { case 0:return low; case 1:return high; case 2:return calls;
        case 3:return handle; case 4:return shutdown_count; default:return 0; }
}
__declspec(dllexport) void NslMockSet(unsigned which,uint64_t n) {
    if(which==0) mask=(unsigned)n;
    else if(which==1) empty=(unsigned)n;
    else if(which==2) handle=n;
    else if(which==3) type=(unsigned)n;
}
__declspec(dllexport) DWORD WINAPI XInputGetState(DWORD index,XINPUT_STATE *value) {
    memset(value,0,sizeof(*value));
    return index<4 && mask&(1u<<index)?ERROR_SUCCESS:ERROR_DEVICE_NOT_CONNECTED;
}
__declspec(dllexport) DWORD WINAPI XInputSetState(DWORD index,XINPUT_VIBRATION *v) {
    low=v->wLeftMotorSpeed;high=v->wRightMotorSpeed;++calls;
    return index<4 && mask&(1u<<index)?ERROR_SUCCESS:ERROR_DEVICE_NOT_CONNECTED;
}
static int pipe_create(void *self) { assert(self==&client);return 7; }
static int user_create(void *self,int pipe) { assert(self==&client && pipe==7);return 11; }
static void *interface_get(void *self,int user,int pipe,const char *version) {
    assert(self==&client && pipe==7 && user==11 && !strcmp(version,"SteamInput006")); return &input;
}
static bool input_init(void *self,bool explicit_frame) {
    assert(self==&input && explicit_frame && !initialized); initialized=true;return true;
}
static void input_frame(void *self,bool reserved) {
    assert(self==&input && reserved && initialized && !shutdown_done);
}
static int input_controllers(void *self,uint64_t *out) {
    assert(self==&input && initialized && !shutdown_done);
    if(empty) return 0;
    out[0]=handle; return 1;
}
static void input_rumble(void *self,uint64_t id,uint16_t a,uint16_t b) {
    assert(self==&input && id && initialized && !shutdown_done); low=a;high=b;++calls;
}
static int input_type(void *self,uint64_t id) { assert(self==&input && id==handle);return (int)type; }
static uint32_t input_remote(void *self,uint64_t id) { assert(self==&input && id==handle);return 1; }
static uint64_t index_handle(void *self,int index) { assert(self==&input);return index==0?handle:0; }
static bool input_shutdown(void *self) {
    assert(self==&input && initialized && !shutdown_done && !low && !high);
    shutdown_done=true;++shutdown_count;return true;
}
static void user_release(void *self,int pipe,int user) {
    assert(self==&client && pipe==7 && user==11 && shutdown_done); released=true;
}
static bool pipe_release(void *self,int pipe) { assert(self==&client && pipe==7 && released);return true; }
__declspec(dllexport) void *CreateInterface(const char *name,int *rc) {
    (void)rc;assert(!strcmp(name,"SteamClient023"));
    client_slots[0]=pipe_create;client_slots[1]=pipe_release;client_slots[2]=user_create;
    client_slots[4]=user_release;client_slots[12]=interface_get;
    input_slots[0]=input_init;input_slots[1]=input_shutdown;input_slots[3]=input_frame;
    input_slots[6]=input_controllers;input_slots[30]=input_rumble;
    input_slots[37]=input_type;input_slots[38]=index_handle;input_slots[45]=input_remote;
    return &client;
}
