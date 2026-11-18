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

typedef struct object { void **vtable; } object;
static void *client_slots[13], *input_slots[46];
static object client = {client_slots}, input = {input_slots};
static unsigned ordinary, pulses[2];
static bool initialized, shut_down, user_released;
static const uint64_t handle = UINT64_C(0x112233445566);
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
    out[0] = handle;
    return 1;
}
static int input_type(void *self, uint64_t id) {
    assert(self == &input && id == handle);
    return GetEnvironmentVariableA("NSL_MOCK_NO_SWITCH", NULL, 0) ? 4 : 10;
}
static uint32_t remote_session(void *self, uint64_t id) {
    assert(self == &input && id == handle);
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
    assert(ordinary == (expected ? 1u : 0u));
    assert(pulses[0] == (expected ? 3u : 0u) && pulses[1] == (expected ? 3u : 0u));
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
    return &client;
}
