#include "platform/rumble.h"
#include <SDL.h>
#include <switch.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int __wrap_SDL_GameControllerRumble(SDL_GameController *, Uint16, Uint16, Uint32);
static u32 styles[33];
static bool connected = true;
static atomic_uint send_error;
static Result init_error;
static HidNpadIdType destination;
static unsigned initializes;
static pthread_mutex_t capture_lock = PTHREAD_MUTEX_INITIALIZER;
static HidVibrationValue last[2];
static unsigned last_count;

SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *c) { return (SDL_Joystick *)c; }
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *j) { return (int)(uintptr_t)j - 1; }
SDL_Joystick *SDL_JoystickFromInstanceID(SDL_JoystickID id) {
    return connected && id == 0 ? (SDL_Joystick *)1 : NULL;
}
u32 hidGetNpadStyleSet(HidNpadIdType id) { return styles[id]; }
Result hidInitializeVibrationDevices(HidVibrationDeviceHandle *h, int n, HidNpadIdType id,
                                     HidNpadStyleTag style) {
    (void)style;
    destination = id;
    ++initializes;
    for (int i = 0; i < n; ++i)
        h[i].type_value = (unsigned)id * 2 + (unsigned)i;
    return init_error;
}
Result hidSendVibrationValues(const HidVibrationDeviceHandle *h, const HidVibrationValue *v, int n) {
    (void)h;
    for (int i = 0; i < n; ++i) {
        assert(isfinite(v[i].amp_low) && v[i].amp_low >= 0 && v[i].amp_low <= 1);
        assert(isfinite(v[i].amp_high) && v[i].amp_high >= 0 && v[i].amp_high <= 1);
        assert(v[i].freq_low >= 40 && v[i].freq_low <= 640);
        assert(v[i].freq_high >= 80 && v[i].freq_high <= 1280);
    }
    pthread_mutex_lock(&capture_lock);
    memcpy(last, v, (size_t)n * sizeof(*v));
    last_count = (unsigned)n;
    pthread_mutex_unlock(&capture_lock);
    return atomic_load(&send_error);
}
static void pause_ms(unsigned ms) {
    struct timespec delay = {.tv_nsec = (long)ms * 1000000};
    nanosleep(&delay, NULL);
}
static sl_rumble_snapshot snapshot(void) {
    sl_rumble_snapshot s;
    sl_rumble_get_snapshot(&s);
    return s;
}
static void await_sends(uint64_t n) {
    for (unsigned i = 0; i < 500; ++i) {
        if (snapshot().sends >= n)
            return;
        pause_ms(1);
    }
    assert(!"worker did not produce expected output");
}
static int rumble(unsigned low, unsigned high, unsigned ms) {
    return __wrap_SDL_GameControllerRumble((SDL_GameController *)1, low, high, ms);
}
int main(void) {
    assert(sl_rumble_init());
    styles[32] = HidNpadStyleTag_NpadHandheld;
    const uint8_t invalid[] = {13};
    assert(sl_rumble_command(0, invalid, sizeof(invalid), 0, NULL) == -1);
    assert(initializes == 0 && snapshot().sends == 0);
    assert(rumble(65535, 32768, 30) == 0);
    await_sends(1);
    pthread_mutex_lock(&capture_lock);
    assert(destination == HidNpadIdType_Handheld && last_count == 2);
    assert(last[0].amp_low == 1 && last[0].amp_high == 0);
    assert(last[1].amp_low == 0 && fabsf(last[1].amp_high - .5f) < .00002f);
    pthread_mutex_unlock(&capture_lock);
    await_sends(2);
    pthread_mutex_lock(&capture_lock);
    assert(last[0].amp_low == 0 && last[1].amp_high == 0);
    pthread_mutex_unlock(&capture_lock);

    uint64_t before = snapshot().sends;
    assert(rumble(65535, 0, 0) == 0);
    await_sends(before + 1);
    pause_ms(20);
    assert(snapshot().sends == before + 1);
    connected = false;
    sl_rumble_tick(true);
    await_sends(before + 2);
    connected = true;

    /* Dock transition, then an actual three-sample effect on the worker. */
    styles[32] = 0;
    styles[0] = HidNpadStyleTag_NpadFullKey;
    uint8_t pulse[69] = {13, 10, 0, 0, 0, 0x10, 0,
        0, 1, 0x40, 0x40, 0, 0, 5, 0xc0};
    before = snapshot().sends;
    assert(sl_rumble_command(0, pulse, sizeof(pulse), 0, NULL) == 0);
    for (unsigned i = 0; i < 500 && snapshot().stream[0].emitted_samples < 3; ++i)
        pause_ms(1);
    assert(snapshot().stream[0].emitted_samples == 3);
    await_sends(before + 3); /* trace records are committed with the send count */
    sl_rumble_trace trace[128];
    size_t n = sl_rumble_copy_trace(trace, 128);
    unsigned samples = 0;
    uint64_t previous = 0;
    for (size_t i = 0; i < n; ++i) {
        if (trace[i].kind != 2 || trace[i].sequence != snapshot().stream[0].commands)
            continue;
        assert(trace[i].sample == samples);
        if (samples)
            assert(trace[i].submitted_ns - previous >= 4000000);
        previous = trace[i].submitted_ns;
        assert((samples == 1) == (trace[i].values[1].amp_low > 0));
        ++samples;
    }
    assert(samples == 3);

    /* Failed explicit stops are retried, without logging or SDL on worker. */
    before = snapshot().sends;
    assert(rumble(65535, 65535, 0) == 0);
    await_sends(before + 1);
    atomic_store(&send_error, 123);
    assert(rumble(0, 0, 0) == 0);
    for (unsigned i = 0; i < 500 && !snapshot().send_failures; ++i)
        pause_ms(1);
    assert(snapshot().send_failures > 0);
    atomic_store(&send_error, 0);
    pause_ms(5);
    before = snapshot().sends;
    sl_rumble_tick(false);
    await_sends(before + 1);
    init_error = 456;
    assert(rumble(1, 2, 3) == -1);
    init_error = 0;
    assert(__wrap_SDL_GameControllerRumble(NULL, 1, 2, 3) == -1);
    sl_rumble_shutdown();
    sl_rumble_snapshot s = snapshot();
    assert(!s.worker_running && s.worker_starts == 1 && s.worker_joins == 1);
    assert(initializes > 0);
    /* Reinitialization is tested on the host, with no device time required. */
    assert(sl_rumble_init());
    assert(rumble(10, 20, 0) == 0);
    await_sends(1);
    sl_rumble_shutdown();
    s = snapshot();
    assert(!s.worker_running && s.worker_starts == s.worker_joins);
    puts("PASS native output worker, three-sample pulse, expiry, device changes, retry and join");
}
