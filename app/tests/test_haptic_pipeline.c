#include "platform/rumble.h"
#include "ihslib.h"
#include "ihslib/hid/sdl.h"
#include "hid/device.h"
#include "hid/manager.h"
#include "hid/provider.h"
#include "ihs_enumeration.h"
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
static pthread_mutex_t hid_lock = PTHREAD_MUTEX_INITIALIZER;
#define HID_OUTPUT_CAPACITY 128u
static HidVibrationValue hid_outputs[HID_OUTPUT_CAPACITY][2];
static unsigned hid_output_count;
static unsigned queued_commands;
static bool interleave_output;
static unsigned interleave_callbacks;
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
    (void)h;
    assert(!pthread_equal(owner, pthread_self()) && n == 2);
    pthread_mutex_lock(&hid_lock);
    assert(hid_output_count < HID_OUTPUT_CAPACITY);
    memcpy(hid_outputs[hid_output_count++], v, sizeof(hid_outputs[0]));
    pthread_mutex_unlock(&hid_lock);
    return 0;
}
static void pause_ms(unsigned ms) {
    struct timespec pause = {.tv_nsec = (long)ms * 1000000};
    nanosleep(&pause, NULL);
}
static unsigned output_count(void) {
    pthread_mutex_lock(&hid_lock);
    unsigned result = hid_output_count;
    pthread_mutex_unlock(&hid_lock);
    return result;
}
static void await_output(unsigned before) {
    for (unsigned tries = 0; tries < 500; ++tries) {
        if (output_count() > before) return;
        pause_ms(1);
    }
    assert(!"worker did not submit the queued effect");
}
/* Delegate to the production frontend, but hold the owner's first callback
 * until the actual output worker has run. This makes the SDK batch race
 * deterministic rather than relying on favorable OS thread scheduling. */
static int output_with_interleave(int32_t id, const uint8_t *data, size_t size,
                                  uint64_t received, void *context) {
    unsigned before=output_count();
    int result=sl_rumble_command(id,data,size,received,context);
    if(interleave_output && !interleave_callbacks++) await_output(before);
    return result;
}
static void assert_output(unsigned index, float left_low, float left_high,
                           float right_low, float right_high) {
    pthread_mutex_lock(&hid_lock);
    assert(index < hid_output_count);
    const float amplitudes[2][2] = {{left_low, left_high}, {right_low, right_high}};
    for (unsigned i = 0; i < 2; ++i) {
        HidVibrationValue v = hid_outputs[index][i];
        assert(fabsf(v.amp_low - amplitudes[i][0]) < 1e-7f);
        assert(fabsf(v.amp_high - amplitudes[i][1]) < 1e-7f);
        assert(v.freq_low == 160 && v.freq_high == 320);
    }
    pthread_mutex_unlock(&hid_lock);
}
static void queue(IHS_HIDManagedDevice *device, const uint8_t *data, size_t size) {
    assert(IHS_HIDDeviceWrite(device->device, data, size) == 0);
    ++queued_commands;
}
static unsigned apply(IHS_Session *session, IHS_HIDManagedDevice *device,
                       const uint8_t *data, size_t size) {
    unsigned before = output_count();
    queue(device, data, size);
    IHS_HIDSDLApplyPendingWrites(session);
    await_output(before);
    return before;
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
    sl_rumble_refresh_capabilities();
    const uint8_t key[32] = {0};
    IHS_ClientConfig config = {.deviceId = 1, .secretKey = key, .deviceName = "Haptics pipeline"};
    IHS_SessionInfo info = {.sessionKeyLen = 32};
    IHS_Session *session = IHS_SessionCreate(&config, &info);
    assert(session);
    IHS_HIDProviderSDLDeviceList list = {.count = device_count, .index = device_index,
        .instanceId = device_id, .controller = device_controller, .output = output_with_interleave,
        .outputDiscontinuity = sl_rumble_discontinuity,
        .supportsNativeHaptics = sl_rumble_supports_native_haptics};
    IHS_HIDProvider *provider = IHS_HIDProviderSDLCreateUnmanaged(&list, NULL);
    assert(provider);
    IHS_SessionHIDAddProvider(session, provider);
    IHS_Enumeration *enumeration = IHS_HIDProviderEnumerateDevices(provider);
    IHS_HIDDeviceInfo descriptor = {0};
    IHS_HIDProviderDeviceInfo(provider, enumeration, &descriptor);
    assert(descriptor.native_haptic_pulse);
    IHS_EnumerationFree(enumeration);
    IHS_HIDManagedDevice *device = IHS_HIDManagerOpenDevice(session->hidManager, "sdl://0");
    assert(device);
    const uint8_t original[69] = {13, 10, 0, 0, 0, 0x10, 0,
        0, 1, 0x40, 0x40, 0, 0, 5, 0xc0};
    uint8_t wire[69];
    memcpy(wire, original, sizeof(wire));
    queue(device, wire, sizeof(wire));
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
    /* Both source events arrived before draining. The first callback waits
     * for the real worker, emptying the queue before the SDK applies stop. */
    const uint8_t batch_on[9]={1,255,255}, batch_off[9]={1};
    unsigned batch_before=output_count();
    sl_rumble_snapshot batch_stats; sl_rumble_get_snapshot(&batch_stats);
    uint64_t first_sequence=batch_stats.stream[0].commands+1;
    queue(device,batch_on,sizeof(batch_on));
    pause_ms(30);
    queue(device,batch_off,sizeof(batch_off));
    interleave_output=true;interleave_callbacks=0;
    IHS_HIDSDLApplyPendingWrites(session);
    interleave_output=false;
    for(unsigned tries=0;tries<500 && output_count()<batch_before+2;++tries)pause_ms(1);
    assert(output_count()==batch_before+2 && interleave_callbacks==2);
    sl_rumble_trace batch_trace[32];size_t batch_count=sl_rumble_copy_trace(batch_trace,32);
    uint64_t received_times[2]={0},sent_times[2]={0};
    for(size_t i=0;i<batch_count;++i) {
        const sl_rumble_trace *t=&batch_trace[i];
        if(t->sequence<first_sequence || t->sequence>first_sequence+1)continue;
        unsigned n=(unsigned)(t->sequence-first_sequence);
        if(t->kind==1)received_times[n]=t->received_ns;
        if(t->kind==2)sent_times[n]=t->submitted_ns;
    }
    assert(received_times[0] && sent_times[0] && received_times[1] && sent_times[1]);
    assert(sent_times[1]-sent_times[0]>=received_times[1]-received_times[0]);

    /* Independent continuous numerical goldens: source / 65535 times the
     * conservative ~0.5571934 peak, with separated 160/320-Hz bands. */
    const uint8_t ordinary[21] = {1, 0x34, 0x12, 0xcd, 0xab, 0xe8, 0x03};
    const uint8_t stop[21] = {1};
    unsigned before = apply(session, device, ordinary, sizeof(ordinary));
    assert_output(before, .039620375f, .373936415f, .039620375f, .373936415f);
    before = apply(session, device, stop, sizeof(stop));
    assert_output(before, 0, 0, 0, 0);

    /* Generic trigger rumble reaches the callback independently of body
     * rumble. Stopping either source leaves the other source running. */
    const uint8_t body[21] = {1, 255, 255};
    const uint8_t trigger_right[21] = {6, 0, 0, 255, 255};
    const uint8_t trigger_left[21] = {6, 255, 255};
    const uint8_t trigger_stop[21] = {6};
    before = apply(session, device, body, sizeof(body));
    assert_output(before, .5571934f, 0, .5571934f, 0);
    before = apply(session, device, trigger_right, sizeof(trigger_right));
    assert_output(before, .5571934f, 0, .5571934f, .5571934f);
    before = apply(session, device, trigger_stop, sizeof(trigger_stop));
    assert_output(before, .5571934f, 0, .5571934f, 0);
    before = apply(session, device, trigger_left, sizeof(trigger_left));
    assert_output(before, .5571934f, .5571934f, .5571934f, 0);
    before = apply(session, device, stop, sizeof(stop));
    assert_output(before, 0, .5571934f, 0, 0);
    before = apply(session, device, trigger_stop, sizeof(trigger_stop));
    assert_output(before, 0, 0, 0, 0);

    /* Sony's actual 47-byte state is framed by the existing SDK envelope.
     * These motor fields are ordinary emulated rumble, not authored PCM. */
    uint8_t sony[69] = {13, 47, 0, 0, 0};
    sony[5] = 0x03; /* legacy emulated-rumble enable + haptics select */
    sony[7] = 64;  /* right motor */
    sony[8] = 128; /* left motor */
    before = apply(session, device, sony, sizeof(sony));
    assert_output(before, .279689252f, .139844626f, .279689252f, .139844626f);
    uint8_t partial[69];
    memcpy(partial, sony, sizeof(partial));
    /* SDL adds the active rumble mode and cached motor bytes to its full
     * state even when updating a light. Do not invent persistent mode bits. */
    partial[6] = 0x04; /* additional valid light-control field */
    before = apply(session, device, partial, sizeof(partial));
    assert_output(before, .279689252f, .139844626f, .279689252f, .139844626f);

    memcpy(partial, sony, sizeof(partial));
    partial[5] |= 0x04; /* additional right adaptive-trigger field */
    partial[15] = 0x26; /* vibration-mode trigger: unsupported body conversion */
    before = apply(session, device, partial, sizeof(partial));
    assert_output(before, .279689252f, .139844626f, .279689252f, .139844626f);

    /* A report without emulated-rumble selection returns to audio mode.
     * There are no PCM samples in it, so continuing the old body is wrong. */
    memset(partial, 0, sizeof(partial));
    partial[0] = 13;
    partial[1] = 47;
    partial[6] = 0x04;
    partial[8] = 255; /* not a motor update in audio mode */
    before = apply(session, device, partial, sizeof(partial));
    assert_output(before, 0, 0, 0, 0);
    before = apply(session, device, sony, sizeof(sony));
    assert_output(before, .279689252f, .139844626f, .279689252f, .139844626f);

    /* Invalid Sony routing shuts down that body effect instead of playing
     * arbitrary motor bytes. Other packet categories are never motor data. */
    sony[5] = 0x01; /* missing haptics select */
    before = apply(session, device, sony, sizeof(sony));
    assert_output(before, 0, 0, 0, 0);
    sony[5] = 0x03;
    before = apply(session, device, sony, sizeof(sony));
    assert_output(before, .279689252f, .139844626f, .279689252f, .139844626f);

    /* Overflow the SDK queue with harmless report-version control writes.
     * Its discontinuity callback must stop an already active body effect.
     * No empty TCP/real-device/network fixture is involved. */
    const uint8_t version[2] = {9, 3};
    unsigned accepted = 0;
    while (IHS_HIDDeviceWrite(device->device, version, sizeof(version)) == 0) {
        ++queued_commands;
        assert(++accepted < 2000);
    }
    assert(accepted > 0);
    /* Even a smaller write is rejected until the lost-order boundary drains. */
    assert(IHS_HIDDeviceWrite(device->device, version, sizeof(version)) == -1);
    before = output_count();
    IHS_HIDSDLApplyPendingWrites(session);
    await_output(before);
    assert_output(before, 0, 0, 0, 0);
    sl_rumble_snapshot snapshot;
    sl_rumble_get_snapshot(&snapshot);
    assert(snapshot.stream[0].discontinuities == 1);

    /* A relative Nintendo report cannot decode across that lost history. */
    before = output_count();
    queue(device, original, sizeof(original));
    IHS_HIDSDLApplyPendingWrites(session);
    pause_ms(10);
    assert(output_count() == before);
    sl_rumble_get_snapshot(&snapshot);
    assert(snapshot.stream[0].desync_rejected == 1);
    before = apply(session, device, body, sizeof(body));
    assert_output(before, .5571934f, 0, .5571934f, 0);
    before = apply(session, device, stop, sizeof(stop));
    assert_output(before, 0, 0, 0, 0);

    sl_rumble_shutdown();
    sl_rumble_get_snapshot(&snapshot);
    assert(snapshot.worker_starts == 1 && snapshot.worker_joins == 1 && !snapshot.worker_running);
    assert(!snapshot.send_failures && !snapshot.stop_failures);
    assert(snapshot.stream[0].trigger_commands == 4 && snapshot.stream[0].sony_commands == 7);
    /* Explicit Sony body stops are playable adaptations too: four generic
     * trigger commands plus seven Sony body updates, including two stops. */
    assert(snapshot.stream[0].adapted_effects == 11 && snapshot.stream[0].unsupported_effects == 4);
    IHS_HIDSDLOutputStats output;
    IHS_HIDSDLGetOutputStats(session, &output);
    assert(output.queued == queued_commands && output.applied == queued_commands);
    assert(output.rejected == 1 && output.queueDropped == 2);
    IHS_SessionDestroy(session);
    IHS_HIDProviderSDLDestroy(provider);
    SDL_GameControllerClose(controller);
    SDL_JoystickDetachVirtual(index);
    SDL_Quit();
    IHS_Quit();
    puts("PASS DeviceWrite -> owner -> worker: native samples, continuous body/trigger, Sony partial controls and gap stop");
}
