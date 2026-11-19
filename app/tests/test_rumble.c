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
static atomic_uint call_number, block_call, blocked_call, blocked_device, stale_nonzero;
static int controllers = 1;
static Result init_error;
static HidNpadIdType destination;
static unsigned initializes;
static pthread_mutex_t capture_lock = PTHREAD_MUTEX_INITIALIZER;
static HidVibrationValue last[2];
static unsigned last_count;

SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *c) { return (SDL_Joystick *)c; }
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *j) { return (int)(uintptr_t)j - 1; }
SDL_Joystick *SDL_JoystickFromInstanceID(SDL_JoystickID id) {
    return connected && id >= 0 && id < controllers ? (SDL_Joystick *)(uintptr_t)(id + 1) : NULL;
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
    unsigned call = atomic_fetch_add(&call_number, 1) + 1;
    if (atomic_load(&block_call) == call) {
        atomic_store(&blocked_device, h[0].type_value);
        atomic_store(&blocked_call, call);
        while (atomic_load(&block_call) == call) {
            struct timespec pause = {.tv_nsec = 1000000};
            nanosleep(&pause, NULL);
        }
    }
    if (h[0].type_value == 2 && (v[0].amp_low || v[0].amp_high))
        atomic_fetch_add(&stale_nonzero, 1);
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
static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
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
static void await_block(unsigned call) {
    for (unsigned i = 0; i < 500; ++i) {
        if (atomic_load(&blocked_call) == call)
            return;
        pause_ms(1);
    }
    assert(!"mock IPC did not reach the concurrency barrier");
}
static int rumble(unsigned low, unsigned high, unsigned ms) {
    return __wrap_SDL_GameControllerRumble((SDL_GameController *)1, low, high, ms);
}
int main(void) {
    assert(!sl_rumble_supports_native_haptics(0, NULL));
    assert(sl_rumble_init());
    styles[32] = HidNpadStyleTag_NpadHandheld;
    sl_rumble_refresh_capabilities();
    assert(sl_rumble_supports_native_haptics(0, NULL));
    assert(!sl_rumble_supports_native_haptics(-1, NULL));
    assert(!sl_rumble_supports_native_haptics(1, NULL));
    connected = false;
    sl_rumble_refresh_capabilities();
    assert(!sl_rumble_supports_native_haptics(0, NULL));
    connected = true;
    styles[32] = 0;
    sl_rumble_refresh_capabilities();
    assert(!sl_rumble_supports_native_haptics(0, NULL));
    styles[32] = HidNpadStyleTag_NpadHandheld;
    sl_rumble_refresh_capabilities();
    const uint8_t invalid[] = {13};
    assert(sl_rumble_command(0, invalid, sizeof(invalid), 0, NULL) == -1);
    assert(initializes == 0 && snapshot().sends == 0);
    sl_rumble_trace rejected[2];
    assert(sl_rumble_copy_trace(rejected, 2) == 1);
    assert(rejected[0].kind == 1 && rejected[0].result == UINT32_MAX &&
           rejected[0].size == 1 && rejected[0].command[0] == 13);
    /* Rejection diagnostics must not dereference a missing payload. */
    assert(sl_rumble_command(0, NULL, 69, 0, NULL) == -1);
    assert(sl_rumble_copy_trace(rejected, 2) == 2);
    assert(rejected[1].result == UINT32_MAX && rejected[1].size == 0);
    assert(rumble(65535, 32768, 30) == 0);
    await_sends(1);
    pthread_mutex_lock(&capture_lock);
    assert(destination == HidNpadIdType_Handheld && last_count == 2);
    assert(fabsf(last[0].amp_low - .557193398f) < 1e-7f && last[0].amp_high == 0);
    assert(last[1].amp_low == 0 && fabsf(last[1].amp_high - .27860094f) < 1e-7f);
    assert(last[0].freq_low == 160 && last[1].freq_high == 320);
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
    assert(sl_rumble_command(0, pulse, sizeof(pulse), monotonic_ns(), NULL) == 0);
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
    n = sl_rumble_copy_trace(trace, 128);
    bool activation_traced = false;
    for (size_t i = 0; i < n; ++i)
        activation_traced |= trace[i].kind == 1 && trace[i].result == 456 && trace[i].size == 9;
    assert(activation_traced);
    init_error = 0;
    assert(__wrap_SDL_GameControllerRumble(NULL, 1, 2, 3) == -1);
    sl_rumble_shutdown();
    sl_rumble_snapshot s = snapshot();
    assert(!s.worker_running && s.worker_starts == 1 && s.worker_joins == 1);
    assert(!sl_rumble_supports_native_haptics(0, NULL));
    assert(initializes > 0);
    /* Reinitialization is tested on the host, with no device time required. */
    assert(sl_rumble_init());
    assert(rumble(10, 20, 0) == 0);
    await_sends(1);
    sl_rumble_shutdown();
    s = snapshot();
    assert(!s.worker_running && s.worker_starts == s.worker_joins);

    /* Hold one IPC so the following iteration selects both slots. While
     * slot 0's next IPC is in flight, cancel the session: slot 1's selected
     * but not yet submitted nonzero output must never reach HID. */
    controllers = 2;
    styles[1] = HidNpadStyleTag_NpadFullKey;
    atomic_store(&call_number, 0);
    atomic_store(&block_call, 1);
    atomic_store(&blocked_call, 0);
    atomic_store(&blocked_device, 0);
    atomic_store(&stale_nonzero, 0);
    assert(sl_rumble_init());
    const uint8_t first_body[9] = {1, 255, 255};
    const uint8_t second_body[9] = {1, 0, 128};
    const uint64_t batch_received = monotonic_ns();
    assert(sl_rumble_command(0, first_body, sizeof(first_body), batch_received, NULL) == 0);
    await_block(1);
    /* The concurrent source events share a receive instant. Otherwise source
     * gap preservation may legitimately select slot 1 before slot 0's next
     * event, invalidating this selected-but-unsubmitted cancellation fixture. */
    assert(sl_rumble_command(0, second_body, sizeof(second_body), batch_received, NULL) == 0);
    const uint8_t ordinary[9] = {1, 255, 255};
    assert(sl_rumble_command(1, ordinary, sizeof(ordinary), batch_received, NULL) == 0);
    atomic_store(&block_call, 2);
    await_block(2);
    assert(atomic_load(&blocked_device) == 0); /* slot 0, not slot 1's own IPC */
    sl_rumble_tick(false);
    atomic_store(&block_call, 0);
    sl_rumble_shutdown();
    assert(atomic_load(&stale_nonzero) == 0);
    assert(snapshot().canceled_outputs == 1);
    /* The dedicated local PCM frontend reaches the same worker/IPC sink,
     * preserves authored left/right, and refuses a one-actuator topology. */
    controllers = 1;
    assert(sl_rumble_init());
    sl_pcm_frame pcm = {.epoch = 1, .flags = SL_PCM_FRAME_RESET,
        .values = {{0,160,0,320},{0,160,0,320}}};
    assert(sl_rumble_pcm_frame(0, &pcm, monotonic_ns()) == 0);
    await_sends(1);
    pcm.flags = 0; pcm.available_pts_ns += 5000000;
    pcm.values[0] = (sl_hd_value){.2f,123,0,320};
    pcm.values[1] = (sl_hd_value){0,160,.1f,500};
    assert(sl_rumble_pcm_frame(0, &pcm, monotonic_ns()) == 0);
    await_sends(2);
    pthread_mutex_lock(&capture_lock);
    assert(last_count == 2 && last[0].amp_low == .2f && last[0].freq_low == 123);
    assert(last[1].amp_high == .1f && last[1].freq_high == 500 && !last[1].amp_low);
    pthread_mutex_unlock(&capture_lock);
    pcm.flags = SL_PCM_FRAME_EOS; pcm.available_pts_ns += 5000000;
    pcm.values[0] = pcm.values[1] = (sl_hd_value){0,160,0,320};
    assert(sl_rumble_pcm_frame(0, &pcm, monotonic_ns()) == 0);
    await_sends(3);
    styles[0] = HidNpadStyleTag_NpadJoyRight;
    assert(sl_rumble_pcm_frame(0, &pcm, monotonic_ns()) == -1);
    assert(snapshot().stream[0].unsupported_effects == 1);
    sl_rumble_shutdown();
    /* Actual worker: keep IPC failing while stopping the other channel.
     * Observe attempted parameters before allowing any successful retry. */
    styles[0] = HidNpadStyleTag_NpadFullKey;
    for (unsigned failed_trigger=0; failed_trigger<2; ++failed_trigger) {
        assert(sl_rumble_init());
        const uint8_t body_on[9]={1,255,255}, trigger_on[9]={6,255,255};
        const uint8_t body_off[9]={1}, trigger_off[9]={6};
        assert(sl_rumble_command(0,body_on,9,monotonic_ns(),NULL)==0);
        await_sends(1);
        assert(sl_rumble_command(0,trigger_on,9,monotonic_ns(),NULL)==0);
        await_sends(2);
        atomic_store(&send_error,123);
        assert(sl_rumble_command(0,failed_trigger?trigger_on:body_on,9,monotonic_ns(),NULL)==0);
        for (unsigned i=0;i<500 && !snapshot().send_failures;++i) pause_ms(1);
        assert(snapshot().send_failures);
        assert(sl_rumble_command(0,failed_trigger?body_off:trigger_off,9,monotonic_ns(),NULL)==0);
        bool stopped=false;
        for (unsigned i=0;i<500 && !stopped;++i) {
            pthread_mutex_lock(&capture_lock);
            stopped=failed_trigger ? last[0].amp_low==0 && last[0].amp_high>0 :
                                     last[0].amp_low>0 && last[0].amp_high==0;
            pthread_mutex_unlock(&capture_lock);
            if (!stopped) pause_ms(1);
        }
        assert(stopped && atomic_load(&send_error)==123);
        atomic_store(&send_error,0);
        sl_rumble_shutdown();
        assert(snapshot().worker_starts==snapshot().worker_joins);
    }
    /* Slot 1 is selected before slot 0's blocking IPC. Deadline guards must
     * run at submission too, not just at the earlier selection time. */
    controllers=2;
    styles[1]=HidNpadStyleTag_NpadFullKey;
    for(unsigned overdue=0;overdue<2;++overdue) {
        atomic_store(&call_number,0);
        atomic_store(&block_call,1);
        atomic_store(&blocked_call,0);
        atomic_store(&stale_nonzero,0);
        assert(sl_rumble_init());
        uint64_t received=monotonic_ns();
        const uint8_t first[9]={1,255,255}, next[9]={1,0,128};
        uint8_t short_body[9]={1,255,255,0,0,10};
        if(overdue) short_body[5]=0;
        assert(sl_rumble_command(0,first,9,received,NULL)==0);
        await_block(1);
        assert(sl_rumble_command(0,next,9,received,NULL)==0);
        assert(sl_rumble_command(1,short_body,9,received,NULL)==0);
        atomic_store(&block_call,2);
        await_block(2);
        assert(atomic_load(&blocked_device)==0);
        pause_ms(overdue?270:20);
        atomic_store(&block_call,0);
        await_sends(3);
        assert(atomic_load(&stale_nonzero)==0);
        assert(snapshot().canceled_outputs>=1);
        assert(snapshot().stream[1].stale_frames==overdue);
        sl_rumble_shutdown();
    }
    puts("PASS native output worker, three-sample pulse, expiry, device changes, retry and join");
}
