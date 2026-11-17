/* Diagnose system readback without weakening the native fidelity acceptance.
 * Compare public IPC 201, public IPC 206 and production private-worker IPC 206
 * on identical absolute stimuli. No Steam, SD resources, graphics or raw BT.
 * All timed records stay in RAM until stop/join; socket writes are bounded. */
#include "platform/rumble.h"
#include <switch.h>
#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct event {
    unsigned route, trial, step;
    uint64_t begin, end, sequence;
    uint32_t words[2];
    sl_hd_value values[2];
    Result rc[2];
} event;
typedef struct reading {
    unsigned route, trial, step;
    uint64_t begin, end;
    Result rc[2];
    HidVibrationValue values[2];
} reading;
static event events[64];
static reading readings[4096];
static sl_rumble_trace trace[512];
static unsigned event_count, reading_count;
static bool interrupted, overflowed;
static int log_fd = -1;
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void log_line(const char *format, ...) {
    if (log_fd < 0) return;
    char line[768] = "HCAL ";
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line + 5, sizeof(line) - 7, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof(line) - 7) return;
    size_t length = (size_t)n + 5;
    line[length++] = '\n';
    for (size_t offset = 0; offset < length;) {
        ssize_t sent = send(log_fd, line + offset, length - offset, 0);
        if (sent <= 0) { close(log_fd); log_fd = -1; return; }
        offset += (size_t)sent;
    }
}
static void observe(const HidVibrationDeviceHandle handles[2], unsigned route,
                    unsigned trial, unsigned step, unsigned duration_ms) {
    uint64_t until = now_ns() + (uint64_t)duration_ms * 1000000;
    while (now_ns() < until) {
        if (!appletMainLoop()) { interrupted = true; return; }
        SDL_PumpEvents();
        if (reading_count == sizeof(readings) / sizeof(readings[0])) {
            overflowed = true;
            return;
        }
        reading *r = &readings[reading_count++];
        r->route = route; r->trial = trial; r->step = step;
        r->begin = now_ns();
        for (unsigned i = 0; i < 2; ++i)
            r->rc[i] = hidGetActualVibrationValue(handles[i], &r->values[i]);
        r->end = now_ns();
        struct timespec pause = {.tv_nsec = 1000000};
        nanosleep(&pause, NULL);
    }
}
static void issue(const HidVibrationDeviceHandle handles[2], int32_t instance,
                  unsigned route, unsigned trial, unsigned step, const uint32_t words[2]) {
    if (event_count == sizeof(events) / sizeof(events[0])) { overflowed = true; return; }
    event *e = &events[event_count++];
    e->route = route; e->trial = trial; e->step = step;
    memcpy(e->words, words, sizeof(e->words));
    for (unsigned i = 0; i < 2; ++i) {
        sl_hd_state state;
        sl_hd_samples samples;
        sl_hd_reset(&state);
        sl_hd_decode(&state, words[i], &samples);
        e->values[i] = samples.values[0]; /* only absolute one-sample stimuli */
    }
    e->begin = now_ns();
    if (route == 0) {
        for (unsigned i = 0; i < 2; ++i) {
            HidVibrationValue value;
            memcpy(&value, &e->values[i], sizeof(value));
            e->rc[i] = hidSendVibrationValue(handles[i], &value);
        }
    } else if (route == 1) {
        HidVibrationValue values[2];
        memcpy(values, e->values, sizeof(values));
        e->rc[0] = e->rc[1] = hidSendVibrationValues(handles, values, 2);
    } else {
        uint8_t wire[69] = {13, 10, 0, 0, 0, 16};
        for (unsigned i = 0; i < 2; ++i)
            for (unsigned b = 0; b < 4; ++b)
                wire[7 + 4*i + b] = (uint8_t)(words[i] >> (8*b));
        int rc = sl_rumble_command(instance, wire, sizeof(wire), e->begin, NULL);
        e->rc[0] = e->rc[1] = rc ? UINT32_MAX : 0;
        sl_rumble_snapshot snapshot;
        sl_rumble_get_snapshot(&snapshot);
        e->sequence = snapshot.stream[instance].commands;
    }
    e->end = now_ns();
}
static void log_values(const char *stage, unsigned route, unsigned trial, unsigned step,
                       uint64_t begin, uint64_t end, const Result rc[2], const void *data) {
    HidVibrationValue v[2];
    memcpy(v, data, sizeof(v));
    log_line("{\"stage\":\"%s\",\"route\":%u,\"trial\":%u,\"step\":%u,\"begin_ns\":%llu,\"end_ns\":%llu,\"rc\":[%u,%u],\"values\":[[%.9g,%.9g,%.9g,%.9g],[%.9g,%.9g,%.9g,%.9g]]}",
        stage, route, trial, step, (unsigned long long)begin, (unsigned long long)end,
        rc[0], rc[1], v[0].amp_low, v[0].freq_low, v[0].amp_high, v[0].freq_high,
        v[1].amp_low, v[1].freq_low, v[1].amp_high, v[1].freq_high);
}
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    bool sdl = false, activated = false, ok = false;
    SDL_GameController *controller = NULL;
    HidVibrationDeviceHandle handles[2] = {0};
    if (R_FAILED(socketInitializeDefault())) return 1;
    log_fd = nxlinkConnectToHost(false, false);
    if (log_fd < 0) { socketExit(); return 1; }
    struct timeval timeout = {.tv_usec = 100000};
    setsockopt(log_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef NSL_HAPTICS_PROBE_HOST
    log_line("{\"stage\":\"begin\",\"platform\":\"host-simulation\",\"schema\":1}");
#else
    log_line("{\"stage\":\"begin\",\"platform\":\"switch\",\"schema\":1}");
#endif
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) goto cleanup;
    sdl = true;
    controller = SDL_GameControllerOpen(0);
    if (!controller) goto cleanup;
    int32_t instance = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
    if (instance < 0 || instance > 7 || !sl_rumble_init()) goto cleanup;
    HidNpadIdType id = instance == 0 && hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld
        ? HidNpadIdType_Handheld : (HidNpadIdType)(HidNpadIdType_No1 + instance);
    u32 style = hidGetNpadStyleSet(id);
    if (!(style & (HidNpadStyleTag_NpadHandheld | HidNpadStyleTag_NpadJoyDual | HidNpadStyleTag_NpadFullKey)))
        goto cleanup;
    Result activate = hidInitializeVibrationDevices(handles, 2, id, (HidNpadStyleTag)style);
    bool permitted = false;
    Result permission = hidIsVibrationPermitted(&permitted);
    u8 interface = 0;
    Result interface_rc = hidGetNpadInterfaceType(id, &interface);
    log_line("{\"stage\":\"device\",\"id\":%u,\"style\":%u,\"activate\":%u,\"permission_rc\":%u,\"permitted\":%s,\"hos_version\":%u,\"interface\":%u,\"interface_rc\":%u}",
        (unsigned)id, style, activate, permission, permitted ? "true" : "false", hosversionGet(), interface, interface_rc);
    if (R_FAILED(activate)) goto cleanup;
    activated = true;
    for (unsigned i = 0; i < 2; ++i) {
        HidVibrationDeviceInfo info = {0};
        bool mounted = false;
        Result info_rc = hidGetVibrationDeviceInfo(handles[i], &info);
        Result mount_rc = hidIsVibrationDeviceMounted(handles[i], &mounted);
        log_line("{\"stage\":\"actuator\",\"motor\":%u,\"handle\":%u,\"info_rc\":%u,\"type\":%u,\"position\":%u,\"mount_rc\":%u,\"mounted\":%s}",
            i, handles[i].type_value, info_rc, info.type, info.position, mount_rc, mounted ? "true" : "false");
    }
    if (R_FAILED(permission) || !permitted) goto cleanup;
    const uint32_t zero[2] = {0x40400100, 0x40400100};
    /* Absolute equivalents of the pulse's middle sample and the existing
     * two-band fixture. Long holds isolate amplitude/latency from short pulses. */
    const uint32_t trials[5][2] = {
        {0x46c00100, 0x40400100}, {0x40400100, 0x46c00100},
        {0x60208180, 0x60208180},
        {0x46c00100, 0x40400100}, {0x40400100, 0x46c00100}
    };
    HidVibrationValue stopped[2] = {{0,160,0,320},{0,160,0,320}};
    Result initial = hidSendVibrationValues(handles, stopped, 2);
    log_line("{\"stage\":\"initial_zero\",\"rc\":%u}", initial);
    observe(handles, 99, 99, 99, 200);
    for (unsigned route = 0; route < 3 && !interrupted && !overflowed; ++route) {
        for (unsigned trial = 0; trial < 5 && !interrupted && !overflowed; ++trial) {
            issue(handles, instance, route, trial, 0, zero);
            observe(handles, route, trial, 0, 20);
            issue(handles, instance, route, trial, 1, trials[trial]);
            observe(handles, route, trial, 1, trial < 3 ? 60 : 5);
            issue(handles, instance, route, trial, 2, zero);
            observe(handles, route, trial, 2, trial < 3 ? 30 : 40);
        }
    }
    ok = !R_FAILED(initial) && !interrupted && !overflowed && event_count == 45;
cleanup:
    sl_rumble_tick(false);
    sl_rumble_shutdown();
    sl_rumble_snapshot snapshot;
    sl_rumble_get_snapshot(&snapshot);
    size_t trace_count = sl_rumble_copy_trace(trace, 512);
    Result final_send = UINT32_MAX, final_rc[2] = {UINT32_MAX,UINT32_MAX};
    HidVibrationValue final[2] = {0};
    if (activated) {
        HidVibrationValue stop[2] = {{0,160,0,320},{0,160,0,320}};
        final_send = hidSendVibrationValues(handles, stop, 2);
        /* Let the system readback settle, without changing measured trials. */
        observe(handles, 98, 98, 98, 30);
        for (unsigned i = 0; i < 2; ++i)
            final_rc[i] = hidGetActualVibrationValue(handles[i], &final[i]);
    }
    log_line("{\"stage\":\"joined\",\"starts\":%llu,\"joins\":%llu,\"running\":%s,\"failures\":%llu,\"io_rc\":%u,\"trace_lost\":%llu,\"emitted\":%llu,\"rejected\":%llu,\"dropped\":%llu,\"final_send_rc\":%u}",
        (unsigned long long)snapshot.worker_starts, (unsigned long long)snapshot.worker_joins,
        snapshot.worker_running ? "true" : "false", (unsigned long long)snapshot.send_failures,
        snapshot.io_result, (unsigned long long)snapshot.trace_overwritten,
        (unsigned long long)snapshot.stream[0].emitted_samples,
        (unsigned long long)snapshot.stream[0].rejected,
        (unsigned long long)snapshot.stream[0].dropped_samples, final_send);
    log_values("stopped", 98, 98, 98, 0, 0, final_rc, final);
    for (unsigned i = 0; i < event_count; ++i) {
        event *e = &events[i];
        log_line("{\"stage\":\"event\",\"route\":%u,\"trial\":%u,\"step\":%u,\"word\":[%u,%u],\"seq\":%llu,\"begin_ns\":%llu,\"end_ns\":%llu,\"rc\":[%u,%u]}",
            e->route, e->trial, e->step, e->words[0], e->words[1],
            (unsigned long long)e->sequence, (unsigned long long)e->begin, (unsigned long long)e->end,
            e->rc[0], e->rc[1]);
        log_values("requested", e->route, e->trial, e->step, e->begin, e->end, e->rc, e->values);
    }
    for (size_t i = 0; i < trace_count; ++i) {
        sl_rumble_trace *t = &trace[i];
        if (t->kind == 1) continue;
        Result rc[2] = {t->result,t->result};
        log_line("{\"stage\":\"worker\",\"kind\":%u,\"seq\":%llu,\"begin_ns\":%llu,\"end_ns\":%llu,\"rc\":%u}",
            t->kind, (unsigned long long)t->sequence, (unsigned long long)t->submitted_ns,
            (unsigned long long)t->completed_ns, t->result);
        log_values("worker_values", 2, 99, (unsigned)t->sequence,
                   t->submitted_ns, t->completed_ns, rc, t->values);
    }
    for (unsigned i = 0; i < reading_count; ++i) {
        reading *r = &readings[i];
        log_values("readback", r->route, r->trial, r->step, r->begin, r->end, r->rc, r->values);
    }
    if (controller) SDL_GameControllerClose(controller);
    if (sdl) SDL_Quit();
    ok &= !snapshot.worker_running && snapshot.worker_starts == 1 && snapshot.worker_joins == 1 &&
          !snapshot.send_failures && !snapshot.io_result && !snapshot.trace_overwritten && !overflowed;
    log_line("{\"stage\":\"end\",\"complete\":%s,\"events\":%u,\"readings\":%u,\"interrupted\":%s,\"overflowed\":%s,\"physical_fidelity_verified\":false,\"loader_return_verified\":false}",
        ok ? "true" : "false", event_count, reading_count, interrupted ? "true" : "false", overflowed ? "true" : "false");
    if (log_fd >= 0) close(log_fd);
    socketExit();
    return ok ? 0 : 1;
}
