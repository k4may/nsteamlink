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
static void *desktop_slots[142];
static object desktop={desktop_slots};
static unsigned char desktop_cache[0x10960c];
static bool desktop_opened, haptics_enabled;
static unsigned native_calls,native_flags,native_on,native_off,native_repeat;
static uint64_t handle=0x123456789;
static unsigned low,high,calls,mask=1,empty,type=10,shutdown_count;
static bool initialized,shutdown_done,released;
static unsigned pipe_attempts;
static unsigned user_attempts;
__declspec(dllexport) uint64_t NslMockRead(unsigned which) {
    switch(which) { case 0:return low; case 1:return high; case 2:return calls;
        case 3:return handle; case 4:return shutdown_count;case 5:return native_calls;case 6:return native_flags;case 7:return native_on;case 8:return native_off;case 9:return native_repeat;case 10:return haptics_enabled; default:return 0; }
}
__declspec(dllexport) void NslMockSet(unsigned which,uint64_t n) {
    if(which==0) mask=(unsigned)n;
    else if(which==1) empty=(unsigned)n;
    else if(which==2) handle=n;
    else if(which==3) type=(unsigned)n;
    else if(which==4) {uint32_t appid=(uint32_t)n;memcpy(desktop_cache+0x109608,&appid,4);}
}
__declspec(dllexport) DWORD WINAPI XInputGetState(DWORD index,XINPUT_STATE *value) {
    memset(value,0,sizeof(*value));
    return index<4 && mask&(1u<<index)?ERROR_SUCCESS:ERROR_DEVICE_NOT_CONNECTED;
}
__declspec(dllexport) DWORD WINAPI XInputSetState(DWORD index,XINPUT_VIBRATION *v) {
    low=v->wLeftMotorSpeed;high=v->wRightMotorSpeed;++calls;
    return index<4 && mask&(1u<<index)?ERROR_SUCCESS:ERROR_DEVICE_NOT_CONNECTED;
}
static int pipe_create(void *self) {
    assert(self==&client);
    if(GetEnvironmentVariableA("NSL_PANEL_INIT_FAIL_ONCE",NULL,0) && !pipe_attempts++) return 0;
    return 7;
}
static int user_create(void *self,int pipe) {
    assert(self==&client && pipe==7);
    if(GetEnvironmentVariableA("NSL_PANEL_USER_FAIL_ONCE",NULL,0) && !user_attempts++) return 0;
    return 11;
}
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
static void *desktop_destroy(void *self,unsigned flags) {
    assert(self==&desktop && flags==1 && desktop_opened && !low && !high);
    assert(!native_calls || (native_flags==2 && !native_on));desktop_opened=false;return NULL;
}
static bool desktop_details_mock(void *self,unsigned index,unsigned char *out) {
    assert(self==&desktop && desktop_opened);
    if(index!=0 || !mask) return false;
    memset(out,0,88);memcpy(out+0x20,&handle,8);uint32_t device_type=38;memcpy(out+0x34,&device_type,4);out[0x40]=1;return true;
}
static void desktop_rumble_mock(void *self,uint32_t appid,unsigned index,uint16_t a,uint16_t b) {
    uint32_t active;memcpy(&active,desktop_cache+0x109608,4);
    assert(self==&desktop && desktop_opened && index==0 && appid==active);
    low=a;high=b;++calls;
}
static bool desktop_haptics_mock(void *self,unsigned index,bool enabled) {
    assert(self==&desktop && desktop_opened && index==0);haptics_enabled=enabled;return true;
}
static void desktop_pulse_mock(void *self,unsigned index,int side,uint16_t on,uint16_t off,uint16_t repeat,unsigned flags) {
    assert(self==&desktop && desktop_opened && index==0 && (side==0 || side==1));
    assert((flags==0 && haptics_enabled) || (flags==2 && on==0 && off==0 && repeat==0));
    ++native_calls;native_flags=flags;native_on=on;native_off=off;native_repeat=repeat;
}
__declspec(dllexport) void *NslMockDesktop(void **controller) {
    char env[32];GetEnvironmentVariableA("SteamAppId",env,32);assert(!strcmp(env,"0"));
    GetEnvironmentVariableA("SteamGameId",env,32);assert(!strcmp(env,"0"));
    uint32_t appid=769;memcpy(desktop_cache+0x109608,&appid,4);*controller=desktop_cache;
    desktop_slots[0x468/8]=desktop_haptics_mock;desktop_slots[0x2a8/8]=desktop_pulse_mock;desktop_slots[0]=desktop_destroy;desktop_slots[0x338/8]=desktop_details_mock;desktop_slots[0x2b8/8]=desktop_rumble_mock;
    desktop_opened=true;return &desktop;
}
static bool input_shutdown(void *self) {
    assert(self==&input && initialized && !shutdown_done && !low && !high && !desktop_opened);
    shutdown_done=true;++shutdown_count;return true;
}
static void user_release(void *self,int pipe,int user) {
    assert(self==&client && pipe==7 && user==11 && (!initialized || shutdown_done)); released=true;
}
static bool pipe_release(void *self,int pipe) { assert(self==&client && pipe==7 && (!initialized || released));return true; }
__declspec(dllexport) void *CreateInterface(const char *name,int *rc) {
    (void)rc;assert(!strcmp(name,"SteamClient023"));
    client_slots[0]=pipe_create;client_slots[1]=pipe_release;client_slots[2]=user_create;
    client_slots[4]=user_release;client_slots[12]=interface_get;
    input_slots[0]=input_init;input_slots[1]=input_shutdown;input_slots[3]=input_frame;
    input_slots[6]=input_controllers;input_slots[30]=input_rumble;
    input_slots[37]=input_type;input_slots[38]=index_handle;input_slots[45]=input_remote;
    return &client;
}
