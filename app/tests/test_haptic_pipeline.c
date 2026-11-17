#include "platform/rumble.h"
#include "ihslib.h"
#include "ihslib/hid/sdl.h"
#include "hid/device.h"
#include "hid/manager.h"
#include "session/session_pri.h"
#include <SDL.h>
#include <switch.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pthread_t owner;
static SDL_GameController *controller;
static SDL_JoystickID instance;
static unsigned activations;
static int device_count(void *p) { (void)p; return 1; }
static int device_index(SDL_JoystickID id, void *p) { (void)p; return id == instance ? 0 : -1; }
static SDL_JoystickID device_id(int i, void *p) { (void)i; (void)p; return instance; }
static SDL_GameController *device_controller(int i, void *p) { (void)p; return i ? NULL : controller; }
u32 hidGetNpadStyleSet(HidNpadIdType id) { return id == HidNpadIdType_No1 ? HidNpadStyleTag_NpadFullKey : 0; }
Result hidInitializeVibrationDevices(HidVibrationDeviceHandle *handles, int n, HidNpadIdType id,
                                     HidNpadStyleTag style) {
    assert(pthread_equal(owner, pthread_self()));
    assert(id == HidNpadIdType_No1 && style == HidNpadStyleTag_NpadFullKey && n == 2);
    memset(handles, 0, (size_t)n * sizeof(*handles));
    ++activations;
    return 0;
}
Result hidSendVibrationValues(const HidVibrationDeviceHandle *h, const HidVibrationValue *v, int n) {
    (void)h; (void)v;
    assert(!pthread_equal(owner, pthread_self()) && n == 2);
    return 0;
}
int main(void) {
    owner = pthread_self();
    IHS_Init();
    assert(SDL_Init(SDL_INIT_GAMECONTROLLER) == 0);
    int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 16, 0);
    assert(index >= 0);
    controller = SDL_GameControllerOpen(index);
    assert(controller);
    instance = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
    assert(instance == 0);
    assert(sl_rumble_init());
    const uint8_t key[32] = {0};
    IHS_ClientConfig config = {.deviceId = 1, .secretKey = key, .deviceName = "Haptics pipeline"};
    IHS_SessionInfo info = {.sessionKeyLen = 32};
    IHS_Session *session = IHS_SessionCreate(&config, &info);
    assert(session);
    IHS_HIDProviderSDLDeviceList list = {.count = device_count, .index = device_index,
        .instanceId = device_id, .controller = device_controller, .output = sl_rumble_command};
    IHS_HIDProvider *provider = IHS_HIDProviderSDLCreateUnmanaged(&list, NULL);
    assert(provider);
    IHS_SessionHIDAddProvider(session, provider);
    IHS_HIDManagedDevice *device = IHS_HIDManagerOpenDevice(session->hidManager, "sdl://0");
    assert(device);
    const uint8_t original[69] = {13, 10, 0, 0, 0, 0x10, 0,
        0, 1, 0x40, 0x40, 0, 0, 5, 0xc0};
    uint8_t wire[69];
    memcpy(wire, original, sizeof(wire));
    assert(IHS_HIDDeviceWrite(device->device, wire, sizeof(wire)) == 0);
    memset(wire, 0xaa, sizeof(wire));
    assert(activations == 0); /* Network receive cannot touch SDL/HID. */
    IHS_HIDSDLApplyPendingWrites(session);
    assert(activations == 1);
    sl_rumble_trace trace[16];
    size_t count = 0;
    unsigned samples = 0;
    for (unsigned tries = 0; tries < 1000; ++tries) {
        count = sl_rumble_copy_trace(trace, 16);
        samples = 0;
        for (size_t i = 0; i < count; ++i)
            samples += trace[i].kind == 2;
        if (samples == 3)
            break;
        struct timespec pause = {.tv_nsec = 1000000};
        nanosleep(&pause, NULL);
    }
    assert(samples == 3 && trace[0].kind == 1);
    for (size_t i = 0; i < count; ++i)
        assert(trace[i].serial == i + 1);
    sl_rumble_trace page[2];
    assert(sl_rumble_read_trace(1, page, 2) == 2 && page[0].serial == 2 && page[1].serial == 3);
    assert(sl_rumble_read_trace(trace[count-1].serial, page, 2) == 0);
    assert(trace[0].received_ns && memcmp(trace[0].command, original, 69) == 0);
    samples = 0;
    uint64_t previous = 0;
    for (size_t i = 0; i < count; ++i) {
        if (trace[i].kind != 2)
            continue;
        assert(trace[i].sample == samples && trace[i].received_ns == trace[0].received_ns);
        assert(trace[i].result == 0 && trace[i].values[0].amp_low == 0);
        assert(fabsf(trace[i].values[1].amp_low - (samples == 1 ? .0441941738f : 0)) < 1e-7f);
        if (samples)
            assert(trace[i].submitted_ns - previous >= SL_HAPTIC_SAMPLE_NS);
        previous = trace[i].completed_ns;
        ++samples;
    }
    sl_rumble_shutdown();
    sl_rumble_snapshot snapshot;
    sl_rumble_get_snapshot(&snapshot);
    assert(snapshot.worker_starts == 1 && snapshot.worker_joins == 1 && !snapshot.worker_running);
    assert(!snapshot.send_failures && !snapshot.stop_failures);
    IHS_HIDSDLOutputStats output;
    IHS_HIDSDLGetOutputStats(session, &output);
    assert(output.queued == 1 && output.applied == 1 && !output.rejected && !output.queueDropped);
    IHS_SessionDestroy(session);
    IHS_HIDProviderSDLDestroy(provider);
    SDL_GameControllerClose(controller);
    SDL_JoystickDetachVirtual(index);
    SDL_Quit();
    IHS_Quit();
    puts("PASS actual DeviceWrite -> pending drain -> native decoder/worker -> mocked HID, all pulse samples");
}
