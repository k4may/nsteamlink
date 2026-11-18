/* Windows-only ABI/lifecycle fixture for the public-interface source tool.
 * A fake steamclient DLL, never shipped with the tool or used as Valve evidence.
 * Run with NSL_MOCK_EXPECT_PROBE=1 for the bounded pulse test, 0 for --list.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct object { void **vtable; void *controller; } object;
static void *client_slots[13];
#ifdef NSL_MOCK_PRIVATE_LAYOUT
static struct { char padding[0x1e0]; void *slots[46]; }
    input_storage __attribute__((section(".nslpub"), aligned(8)));
static struct { char padding[0xdc0]; void *slots[13]; }
    controller_storage __attribute__((section(".nslctl"), aligned(8)));
#define input_slots input_storage.slots
#define controller_slots controller_storage.slots
extern int query_fixed(void *, uint32_t, uint64_t *);
__asm__(".section .nslqry,\"xr\"\n.space 0x9a0,0x90\n.globl query_fixed\nquery_fixed:\njmp query_impl\n.text");
static object controller = {controller_slots, NULL};
static void *bad_layout[46];
#else
static void *input_slots[46];
#endif
static object client = {client_slots, NULL}, input = {input_slots, NULL};
static unsigned ordinary, pulses[2];
static bool initialized, shut_down, user_released;
static unsigned rpc_pulses[2];
static unsigned bypass_pulses[2];
static bool map_created, map_destroyed;
static const uint64_t handle = UINT64_C(0x112233445566);
#ifdef NSL_MOCK_PRIVATE_LAYOUT
static struct { char padding[0x980]; void *slots[104]; }
    map_storage __attribute__((section(".nslmap"), aligned(8)));
extern void *factory_fixed(int, int);
extern void *destroy_fixed(void *, unsigned);
extern bool details_fixed(void *, unsigned, unsigned char *);
extern void rpc_pulse_fixed(void *, unsigned, int, uint16_t, uint16_t, uint16_t, unsigned);
__asm__(".section .nslfac,\"xr\"\n.space 0x560,0x90\n.globl factory_fixed\nfactory_fixed:\njmp factory_impl\n.text");
__asm__(".section .nsldel,\"xr\"\n.space 0x3d0,0x90\n.globl destroy_fixed\ndestroy_fixed:\njmp destroy_impl\n.text");
__asm__(".section .nsldet,\"xr\"\n.space 0xb0,0x90\n.globl details_fixed\ndetails_fixed:\njmp details_impl\n.text");
__asm__(".section .nslpls,\"xr\"\n.space 0xb60,0x90\n.globl rpc_pulse_fixed\nrpc_pulse_fixed:\njmp rpc_pulse_impl\n.text");
static object map = {map_storage.slots, NULL};
void *factory_impl(int pipe, int user) {
    assert(!pipe && user == 11 && !map_created);
    map_created = true;
    map_storage.slots[0] = destroy_fixed;
    map_storage.slots[0x2a8 / 8] = rpc_pulse_fixed;
    map_storage.slots[0x338 / 8] = details_fixed;
    if (GetEnvironmentVariableA("NSL_MOCK_BAD_MAP_METHOD", NULL, 0))
        map_storage.slots[0x338 / 8] = NULL;
    return &map;
}
void *destroy_impl(void *self, unsigned flags) {
    assert(self == &map && flags == 1 && map_created && !map_destroyed && !user_released);
    map_destroyed = true;
    return self;
}
bool details_impl(void *self, unsigned index, unsigned char *info) {
    assert(self == &map && map_created && !map_destroyed && index < 16);
    if (index != 3) return false;
    uint64_t id = handle, caps = 0x836bff;
    uint32_t type = 38;
    if (GetEnvironmentVariableA("NSL_MOCK_WRONG_RPC_HANDLE", NULL, 0)) ++id;
    memcpy(info + 0x20, &id, 8); memcpy(info + 8, &caps, 8); memcpy(info + 0x34, &type, 4);
    info[0x40] = 1;
    return true;
}
void rpc_pulse_impl(void *self, unsigned index, int side, uint16_t duration,
                    uint16_t off, uint16_t repeat, unsigned flags) {
    assert(self == &map && index == 3 && side >= 0 && side <= 1);
    assert(!off && repeat == 1 && flags <= 1 && (duration == 0 || duration == 1000));
    assert(map_created && !map_destroyed && initialized && !shut_down);
    if (duration) { if (flags) ++bypass_pulses[side]; else ++rpc_pulses[side]; }
}
#endif
static int make_pipe(void *self) { assert(self == &client); return 7; }
static int connect_user(void *self, int pipe) {
    assert(self == &client && pipe == 7);
    return 11;
}
static void *get_interface(void *self, int user, int pipe, const char *name) {
    assert(self == &client && user == 11 && pipe == 7 && !strcmp(name, "SteamInput006"));
    return &input;
}
static bool init(void *self, bool explicit_frame) {
    assert(self == &input && explicit_frame && !initialized);
    initialized = true;
    return true;
}
static void run_frame(void *self, bool reserved) {
    assert(self == &input && reserved && initialized && !shut_down);
}
static int controllers(void *self, uint64_t *out) {
    assert(self == &input);
    if (GetEnvironmentVariableA("NSL_MOCK_EMPTY_PUBLIC", NULL, 0)) return 0;
    out[0] = handle;
    return 1;
}
#ifdef NSL_MOCK_PRIVATE_LAYOUT
int query_impl(void *self, uint32_t appid, uint64_t *out) {
    assert(self == &controller);
    assert(appid == 0 || appid == 769 || appid == 413080 || appid == 1049320);
    if (GetEnvironmentVariableA("NSL_MOCK_BAD_COUNT", NULL, 0)) return 17;
    if (appid == 1049320) return 0;
    out[0] = handle;
    if (GetEnvironmentVariableA("NSL_MOCK_MULTIPLE", NULL, 0)) {
        out[1] = handle + 1;
        return 2;
    }
    return 1;
}
#endif
static int input_type(void *self, uint64_t id) {
    assert(self == &input && (id == handle || id == handle + 1));
    return GetEnvironmentVariableA("NSL_MOCK_NO_SWITCH", NULL, 0) ? 4 : 10;
}
static uint32_t remote_session(void *self, uint64_t id) {
    assert(self == &input && (id == handle || id == handle + 1));
    return 1;
}
static void pulse(void *self, uint64_t id, int side, uint16_t duration) {
    assert(self == &input && id == handle && side >= 0 && side <= 1);
    assert(initialized && !shut_down && (duration == 0 || duration == 1000));
    if (duration) ++pulses[side];
}
static void rumble(void *self, uint64_t id, uint16_t left, uint16_t right) {
    assert(self == &input && id == handle && !right && (left == 0 || left == 16384));
    assert(initialized && !shut_down);
    if (left) ++ordinary;
}
static bool shutdown_input(void *self) {
    assert(self == &input && initialized && !shut_down);
    bool expected = GetEnvironmentVariableA("NSL_MOCK_EXPECT_PROBE", NULL, 0) != 0;
    bool isolated = GetEnvironmentVariableA("NSL_MOCK_EXPECT_ISOLATION", NULL, 0) != 0;
    assert(ordinary == (expected && !isolated ? 1u : 0u));
    assert(pulses[0] == (expected ? 3u : 0u) && pulses[1] == (expected ? 3u : 0u));
    bool rpc_expected = GetEnvironmentVariableA("NSL_MOCK_EXPECT_RPC", NULL, 0) != 0;
    assert(rpc_pulses[0] == (rpc_expected ? 3u : 0u) && rpc_pulses[1] == (rpc_expected ? 3u : 0u));
    assert(bypass_pulses[0] == (isolated ? 3u : 0u) && bypass_pulses[1] == (isolated ? 3u : 0u));
    assert(!map_created || map_destroyed);
    shut_down = true;
    return true;
}
static void release_user(void *self, int pipe, int user) {
    assert(self == &client && pipe == 7 && user == 11 && shut_down);
    user_released = true;
}
static bool release_pipe(void *self, int pipe) {
    assert(self == &client && pipe == 7 && user_released);
    return true;
}
__declspec(dllexport) void *CreateInterface(const char *name, int *result) {
    (void)result;
    assert(!strcmp(name, "SteamClient023"));
    client_slots[0] = make_pipe;
    client_slots[1] = release_pipe;
    client_slots[2] = connect_user;
    client_slots[4] = release_user;
    client_slots[12] = get_interface;
    input_slots[0] = init;
    input_slots[1] = shutdown_input;
    input_slots[3] = run_frame;
    input_slots[6] = controllers;
    input_slots[30] = rumble;
    input_slots[34] = pulse;
    input_slots[37] = input_type;
    input_slots[45] = remote_session;
#ifdef NSL_MOCK_PRIVATE_LAYOUT
    input.controller = &controller;
    controller_slots[12] = query_fixed;
    if (GetEnvironmentVariableA("NSL_MOCK_BAD_METHOD", NULL, 0)) controller_slots[12] = query_impl;
    if (GetEnvironmentVariableA("NSL_MOCK_BAD_LAYOUT", NULL, 0)) {
        memcpy(bad_layout, input_slots, sizeof(bad_layout));
        input.vtable = bad_layout;
    }
#endif
    return &client;
}
