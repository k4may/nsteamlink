/* Self-contained DeviceWrite -> native worker -> HID IPC validation. All
 * stimuli are compiled into the NRO. No Steam host, SD fixture or screen report.
 * Logging is deferred until output has stopped and the worker has joined. */
#include "platform/rumble.h"
#include <switch.h>
#include <SDL.h>
#include "ihslib.h"
#include "ihslib/hid/sdl.h"
#include "hid/device.h"
#include "hid/manager.h"
#include "session/session_pri.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int log_fd = -1;
static SDL_GameController *controller;
static SDL_JoystickID joystick_id;
static sl_rumble_trace trace[512];
typedef struct readback {
    uint64_t ns, end_ns;
    Result rc[2];
    HidVibrationValue values[2];
} readback;
static readback readings[768];
static unsigned reading_count;
static bool permitted;
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void log_line(const char *format, ...) {
    if (log_fd < 0)
        return;
    char line[640];
    memcpy(line, "HAPTIC ", 7);
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line + 7, sizeof(line) - 9, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof(line) - 9)
        return;
    size_t total = (size_t)n + 7;
    line[total++] = '\n';
    for (size_t offset = 0; offset < total;) {
        ssize_t sent = send(log_fd, line + offset, total - offset, 0);
        if (sent <= 0) {
            close(log_fd);
            log_fd = -1;
            break;
        }
        offset += (size_t)sent;
    }
}
static int count(void *p) { (void)p; return controller ? 1 : 0; }
static int index_of(SDL_JoystickID id, void *p) { (void)p; return id == joystick_id ? 0 : -1; }
static SDL_JoystickID id_at(int i, void *p) { (void)i; (void)p; return joystick_id; }
static SDL_GameController *controller_at(int i, void *p) { (void)p; return i == 0 ? controller : NULL; }
static const IHS_HIDProviderSDLDeviceList list = {.count = count, .index = index_of,
    .instanceId = id_at, .controller = controller_at, .output = sl_rumble_command};

static void observe(HidVibrationDeviceHandle handles[2], unsigned devices, uint64_t duration_ns) {
    uint64_t until = now_ns() + duration_ns;
    while (appletMainLoop() && now_ns() < until) {
        SDL_PumpEvents();
        if (reading_count < sizeof(readings) / sizeof(readings[0])) {
            readback *r = &readings[reading_count++];
            r->ns = now_ns();
            for (unsigned i = 0; i < devices; ++i)
                r->rc[i] = hidGetActualVibrationValue(handles[i], &r->values[i]);
            r->end_ns = now_ns();
        }
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
}
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    bool sdl = false, ihs = false, ok = false, activated = false;
    HidVibrationDeviceHandle handles[2];
    unsigned devices = 0;
    IHS_Session *session = NULL;
    IHS_HIDProvider *provider = NULL;
    if (R_FAILED(socketInitializeDefault()))
        return 1;
    log_fd = nxlinkConnectToHost(false, false);
    if (log_fd < 0) {
        socketExit();
        return 1;
    }
    struct timeval timeout = {.tv_usec = 100000};
    setsockopt(log_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef NSL_HAPTICS_PROBE_HOST
    log_line("{\"stage\":\"begin\",\"platform\":\"host-simulation\",\"physical_fidelity_verified\":false}");
#else
    log_line("{\"stage\":\"begin\",\"platform\":\"switch\",\"physical_fidelity_verified\":false}");
#endif
    IHS_Init();
    ihs = true;
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0)
        goto cleanup;
    sdl = true;
    controller = SDL_GameControllerOpen(0);
    if (!controller)
        goto cleanup;
    joystick_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
    if (!sl_rumble_init())
        goto cleanup;
    HidNpadIdType id = hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld
        ? HidNpadIdType_Handheld : HidNpadIdType_No1;
    u32 style = hidGetNpadStyleSet(id);
    devices = style & (HidNpadStyleTag_NpadHandheld | HidNpadStyleTag_NpadJoyDual |
                                HidNpadStyleTag_NpadFullKey) ? 2 : 1;
    Result activate = hidInitializeVibrationDevices(handles, (int)devices, id, (HidNpadStyleTag)style);
    Result permission = hidIsVibrationPermitted(&permitted);
    log_line("{\"stage\":\"device\",\"id\":%u,\"style\":%u,\"devices\":%u,\"activate\":%u,\"permission_rc\":%u,\"permitted\":%s}",
        (unsigned)id, style, devices, activate, permission, permitted ? "true" : "false");
    if (R_FAILED(activate))
        goto cleanup;
    activated = true;
    static const uint8_t key[32] = {0};
    IHS_ClientConfig config = {.deviceId = 1, .secretKey = key, .deviceName = "Haptics fixture"};
    IHS_SessionInfo info = {.sessionKeyLen = 32};
    session = IHS_SessionCreate(&config, &info);
    if (!session)
        goto cleanup;
    provider = IHS_HIDProviderSDLCreateUnmanaged(&list, NULL);
    if (!provider)
        goto cleanup;
    IHS_SessionHIDAddProvider(session, provider);
    char path[48];
    snprintf(path, sizeof(path), "sdl://%d", joystick_id);
    IHS_HIDManagedDevice *device = IHS_HIDManagerOpenDevice(session->hidManager, path);
    if (!device)
        goto cleanup;
    ok = true;
    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned repeat = 0; repeat < 6; ++repeat) {
            uint8_t command[69] = {13, 10, 0, 0, 0, 0x10, 0,
                0, 1, 0x40, 0x40, 0, 1, 0x40, 0x40};
            unsigned offset = 7 + side * 4;
            command[offset] = command[offset + 1] = 0;
            command[offset + 2] = 5;
            command[offset + 3] = 0xc0;
            ok &= IHS_HIDDeviceWrite(device->device, command, sizeof(command)) == 0;
            IHS_HIDSDLApplyPendingWrites(session);
            observe(handles, devices, 45000000);
        }
    }
    /* Both bands, different frequencies, then explicit ordinary stop. This is
     * an absolute wire stimulus; no attack boost, smoothing or fabricated tail. */
    uint32_t word = 0x40000000u | 64u << 23 | 32u << 16 | 64u << 9 | 96u << 2;
    uint8_t absolute[69] = {13, 10, 0, 0, 0, 0x10};
    for (unsigned motor = 0; motor < 2; ++motor)
        for (unsigned byte = 0; byte < 4; ++byte)
            absolute[7 + motor * 4 + byte] = (uint8_t)(word >> (byte * 8));
    ok &= IHS_HIDDeviceWrite(device->device, absolute, sizeof(absolute)) == 0;
    IHS_HIDSDLApplyPendingWrites(session);
    observe(handles, devices, 30000000);
    const uint8_t stop[21] = {1};
    ok &= IHS_HIDDeviceWrite(device->device, stop, sizeof(stop)) == 0;
    IHS_HIDSDLApplyPendingWrites(session);
    observe(handles, devices, 15000000);

cleanup:
    sl_rumble_tick(false);
    sl_rumble_shutdown();
    if (activated) {
        HidVibrationValue v[2] = {0};
        Result rc[2] = {0};
        for (unsigned i = 0; i < devices; ++i)
            rc[i] = hidGetActualVibrationValue(handles[i], &v[i]);
        log_line("{\"stage\":\"stopped\",\"rc\":[%u,%u],\"amplitudes\":[%.9g,%.9g,%.9g,%.9g]}",
            rc[0], rc[1], v[0].amp_low, v[0].amp_high, v[1].amp_low, v[1].amp_high);
    }
    sl_rumble_snapshot snapshot;
    sl_rumble_get_snapshot(&snapshot);
    size_t n = sl_rumble_copy_trace(trace, 512);
    IHS_HIDSDLOutputStats output = {0};
    if (session)
        IHS_HIDSDLGetOutputStats(session, &output);
    log_line("{\"stage\":\"joined\",\"running\":%s,\"starts\":%llu,\"joins\":%llu,\"sends\":%llu,\"failures\":%llu,\"io_rc\":%u,\"queued\":%llu,\"applied\":%llu,\"rejected\":%llu,\"dropped\":%llu,\"trace_lost\":%llu,\"decoded\":%llu,\"scheduled\":%llu,\"emitted\":%llu,\"stream_rejected\":%llu,\"stream_dropped\":%llu}",
        snapshot.worker_running ? "true" : "false", (unsigned long long)snapshot.worker_starts,
        (unsigned long long)snapshot.worker_joins, (unsigned long long)snapshot.sends,
        (unsigned long long)snapshot.send_failures, snapshot.io_result,
        (unsigned long long)output.queued, (unsigned long long)output.applied,
        (unsigned long long)output.rejected, (unsigned long long)output.queueDropped,
        (unsigned long long)snapshot.trace_overwritten,
        (unsigned long long)snapshot.stream[0].decoded_samples,
        (unsigned long long)snapshot.stream[0].scheduled_samples,
        (unsigned long long)snapshot.stream[0].emitted_samples,
        (unsigned long long)snapshot.stream[0].rejected,
        (unsigned long long)snapshot.stream[0].dropped_samples);
    for (size_t i = 0; i < n; ++i) {
        sl_rumble_trace *t = &trace[i];
        if (t->kind == 1) {
            char bytes[139];
            for (unsigned j = 0; j < t->size; ++j)
                snprintf(bytes + j * 2, 3, "%02x", t->command[j]);
            bytes[t->size * 2] = 0;
            log_line("{\"stage\":\"command\",\"slot\":%u,\"seq\":%llu,\"received_ns\":%llu,\"apply_ns\":%llu,\"rc\":%u,\"wire\":\"%s\"}",
                t->slot, (unsigned long long)t->sequence, (unsigned long long)t->received_ns,
                (unsigned long long)t->submitted_ns, t->result, bytes);
        } else {
            log_line("{\"stage\":\"send\",\"kind\":%u,\"slot\":%u,\"seq\":%llu,\"sample\":%u,\"due_ns\":%llu,\"sent_ns\":%llu,\"completed_ns\":%llu,\"rc\":%u,\"values\":[[%.9g,%.9g,%.9g,%.9g],[%.9g,%.9g,%.9g,%.9g]]}",
                t->kind, t->slot, (unsigned long long)t->sequence, t->sample,
                (unsigned long long)t->due_ns, (unsigned long long)t->submitted_ns,
                (unsigned long long)t->completed_ns, t->result,
                t->values[0].amp_low, t->values[0].freq_low, t->values[0].amp_high, t->values[0].freq_high,
                t->values[1].amp_low, t->values[1].freq_low, t->values[1].amp_high, t->values[1].freq_high);
        }
    }
    for (unsigned i = 0; i < reading_count; ++i) {
        readback *r = &readings[i];
        log_line("{\"stage\":\"readback\",\"ns\":%llu,\"end_ns\":%llu,\"rc\":[%u,%u],\"values\":[[%.9g,%.9g,%.9g,%.9g],[%.9g,%.9g,%.9g,%.9g]]}",
            (unsigned long long)r->ns, (unsigned long long)r->end_ns, r->rc[0], r->rc[1],
            r->values[0].amp_low, r->values[0].freq_low, r->values[0].amp_high, r->values[0].freq_high,
            r->values[1].amp_low, r->values[1].freq_low, r->values[1].amp_high, r->values[1].freq_high);
    }
    if (session)
        IHS_SessionDestroy(session);
    if (provider)
        IHS_HIDProviderSDLDestroy(provider);
    if (controller)
        SDL_GameControllerClose(controller);
    if (sdl)
        SDL_Quit();
    if (ihs)
        IHS_Quit();
    ok &= snapshot.worker_starts == 1 && snapshot.worker_joins == 1 &&
          !snapshot.worker_running && !snapshot.send_failures && !output.rejected &&
          !output.queueDropped && !snapshot.trace_overwritten;
    log_line("{\"stage\":\"end\",\"software_ok\":%s,\"physical_fidelity_verified\":false,\"loader_return_verified\":false}",
        ok ? "true" : "false");
    if (log_fd >= 0)
        close(log_fd);
    socketExit();
    return ok ? 0 : 1;
}
