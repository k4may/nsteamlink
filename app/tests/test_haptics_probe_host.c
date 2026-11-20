/* Run the actual NRO fixture and logger without a device. Only the HID,
 * applet and nxlink terminals are simulated. The begin record explicitly
 * identifies this as host simulation, never as real Switch IPC evidence. */
#define main sl_haptics_fixture_main
#ifdef NSL_HAPTICS_CALIBRATION_HOST
#include "switch_haptics_calibration.c"
#else
#include "switch_haptics_probe.c"
#endif
#undef main
#include <assert.h>
#include <pthread.h>

static pthread_t host_reader;
static bool host_reader_started;
static int host_reader_fd;
static pthread_mutex_t host_hid_lock = PTHREAD_MUTEX_INITIALIZER;
static HidVibrationValue host_values[2] = {{0, 160, 0, 320}, {0, 160, 0, 320}};
#ifdef NSL_HAPTICS_DEMO
static HidVibrationValue host_before_stop[2];
static uint64_t host_stop_visible_ns[2];
#endif

static void *host_read_logs(void *unused) {
    (void)unused;
    char bytes[4096];
    ssize_t n;
    while ((n = read(host_reader_fd, bytes, sizeof(bytes))) > 0)
        assert(fwrite(bytes, 1, (size_t)n, stdout) == (size_t)n);
    close(host_reader_fd);
    return NULL;
}
Result socketInitializeDefault(void) { return 0; }
int nxlinkConnectToHost(bool redirect_stdout, bool redirect_stderr) {
    assert(!redirect_stdout && !redirect_stderr);
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0)
        return -1;
    host_reader_fd = fds[1];
    if (pthread_create(&host_reader, NULL, host_read_logs, NULL)) {
        close(fds[0]); close(fds[1]);
        return -1;
    }
    host_reader_started = true;
    return fds[0];
}
void socketExit(void) {
    if (host_reader_started) {
        pthread_join(host_reader, NULL);
        host_reader_started = false;
    }
}
bool appletMainLoop(void) { return true; }
u32 hidGetNpadStyleSet(HidNpadIdType id) {
    return id == HidNpadIdType_No1 ? HidNpadStyleTag_NpadFullKey : 0;
}
Result hidInitializeVibrationDevices(HidVibrationDeviceHandle *h, int n, HidNpadIdType id,
                                     HidNpadStyleTag style) {
    assert(n == 2 && id == HidNpadIdType_No1 && style == HidNpadStyleTag_NpadFullKey);
    h[0].type_value = 0; h[1].type_value = 1;
    return 0;
}
Result hidIsVibrationPermitted(bool *allowed) { *allowed = true; return 0; }
Result hidSendVibrationValues(const HidVibrationDeviceHandle *h, const HidVibrationValue *v, int n) {
    assert(n == 2);
    pthread_mutex_lock(&host_hid_lock);
    for (int i = 0; i < n; ++i) {
        assert(h[i].type_value < 2);
#ifdef NSL_HAPTICS_DEMO
        unsigned index = h[i].type_value;
        if (experience_demo_active && !v[i].amp_low && !v[i].amp_high &&
            (host_values[index].amp_low || host_values[index].amp_high)) {
            host_before_stop[index] = host_values[index];
            host_stop_visible_ns[index] = now_ns() + UINT64_C(40000000);
        } else if (v[i].amp_low || v[i].amp_high) host_stop_visible_ns[index] = 0;
#endif
        host_values[h[i].type_value] = v[i];
    }
    pthread_mutex_unlock(&host_hid_lock);
    return 0;
}
Result hidGetActualVibrationValue(HidVibrationDeviceHandle h, HidVibrationValue *v) {
    assert(h.type_value < 2);
    pthread_mutex_lock(&host_hid_lock);
    *v = host_values[h.type_value];
#ifdef NSL_HAPTICS_DEMO
    if (now_ns() < host_stop_visible_ns[h.type_value])
        *v = host_before_stop[h.type_value];
#endif
    pthread_mutex_unlock(&host_hid_lock);
    return 0;
}
#ifdef NSL_HAPTICS_CALIBRATION_HOST
u32 hosversionGet(void) { return 0; }
Result hidGetNpadInterfaceType(HidNpadIdType id, u8 *interface) {
    (void)id; *interface = 2; return 0;
}
Result hidGetVibrationDeviceInfo(HidVibrationDeviceHandle h, HidVibrationDeviceInfo *info) {
    *info = (HidVibrationDeviceInfo){.type = 1, .position = h.type_value + 1};
    return 0;
}
Result hidIsVibrationDeviceMounted(HidVibrationDeviceHandle h, bool *mounted) {
    (void)h; *mounted = true; return 0;
}
Result hidSendVibrationValue(HidVibrationDeviceHandle h, const HidVibrationValue *v) {
    assert(h.type_value < 2);
    pthread_mutex_lock(&host_hid_lock);
    host_values[h.type_value] = *v;
    pthread_mutex_unlock(&host_hid_lock);
    return 0;
}
#endif
int main(int argc, char **argv) {
    assert(SDL_Init(SDL_INIT_GAMECONTROLLER) == 0);
    assert(SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 16, 0) == 0);
    int rc = sl_haptics_fixture_main(argc, argv);
    assert(!host_reader_started);
    SDL_Quit();
    return rc;
}
