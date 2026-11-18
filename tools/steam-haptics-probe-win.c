/* Controlled Steam Input source, Windows x64. Uses the host's own Steam DLL.
 * ABI slots: Valve Proton steamworks_sdk_163 isteamclient.h / isteaminput.h,
 * SteamClient023 and SteamInput006 only. No private DLL addresses/patching.
 * API dispatch is evidence of a request, never evidence of delivered haptics.
 * Build: x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror -municode
 *        tools/steam-haptics-probe-win.c -o steam-haptics-probe.exe -ladvapi32
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef void *(*create_interface_fn)(const char *, int *);
typedef int (*pipe_fn)(void *);
typedef bool (*release_pipe_fn)(void *, int);
typedef int (*user_fn)(void *, int);
typedef void (*release_user_fn)(void *, int, int);
typedef void *(*interface_fn)(void *, int, int, const char *);
typedef bool (*init_fn)(void *, bool);
typedef bool (*shutdown_fn)(void *);
typedef void (*frame_fn)(void *, bool);
typedef int (*controllers_fn)(void *, uint64_t *);
typedef int (*type_fn)(void *, uint64_t);
typedef uint32_t (*remote_fn)(void *, uint64_t);
typedef void (*pulse_fn)(void *, uint64_t, int, uint16_t);
typedef void (*rumble_fn)(void *, uint64_t, uint16_t, uint16_t);
#define METHOD(object, slot, type) ((type)((*(void ***)(object))[(slot) / 8]))

static void *input;
static frame_fn frame;
static LARGE_INTEGER frequency, started;
static uint64_t elapsed_us(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart - started.QuadPart) * 1000000 / frequency.QuadPart);
}
static void stage(const char *name) {
    printf("{\"stage\":\"%s\",\"elapsed_us\":%llu}\n", name,
           (unsigned long long)elapsed_us());
    fflush(stdout);
}
static void pump(unsigned ms) {
    ULONGLONG end = GetTickCount64() + ms;
    do {
        frame(input, true);
        Sleep(8);
    } while (GetTickCount64() < end);
}
static void stop(uint64_t handle) {
    METHOD(input, 0xf0, rumble_fn)(input, handle, 0, 0);
    METHOD(input, 0x110, pulse_fn)(input, handle, 0, 0);
    METHOD(input, 0x110, pulse_fn)(input, handle, 1, 0);
}
int wmain(int argc, wchar_t **argv) {
    wchar_t root[32768] = {0}, dll[32768], appid[16] = L"1049320";
    bool probe = false, initialized = false;
    int result = 1, pipe = 0, user = 0;
    uint64_t selected = 0;
    void *client = NULL;
    HMODULE module = NULL;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--probe")) probe = true;
        else if (!wcscmp(argv[i], L"--list")) probe = false;
        else if (!wcscmp(argv[i], L"--steam-root") && i + 1 < argc) {
            if (wcslen(argv[++i]) >= 32000) goto cleanup;
            wcscpy(root, argv[i]);
        } else if (!wcscmp(argv[i], L"--appid") && i + 1 < argc) {
            wchar_t *end;
            errno = 0;
            unsigned long value = wcstoul(argv[++i], &end, 10);
            if (argv[i][0] < L'0' || argv[i][0] > L'9' || errno || *end || !value ||
                value > UINT32_MAX) goto cleanup;
            swprintf(appid, 16, L"%lu", value);
        } else {
            stage("invalid_arguments");
            goto cleanup;
        }
    }
    if (!root[0]) {
        DWORD size = sizeof(root);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                         RRF_RT_REG_SZ, NULL, root, &size) != ERROR_SUCCESS) {
            stage("steam_path_missing");
            goto cleanup;
        }
    }
    if (wcslen(root) >= 32000) goto cleanup;
    swprintf(dll, 32768, L"%ls\\steamclient64.dll", root);
    /* Child process environment only. No Steam configuration or layout edits. */
    if (!SetEnvironmentVariableW(L"SteamAppId", appid) ||
        !SetEnvironmentVariableW(L"SteamGameId", appid)) goto cleanup;
    SetDllDirectoryW(root);
    module = LoadLibraryExW(dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        printf("{\"stage\":\"dll_load_failed\",\"win32_error\":%lu}\n", GetLastError());
        goto cleanup;
    }
    create_interface_fn create = (create_interface_fn)(uintptr_t)GetProcAddress(module, "CreateInterface");
    if (!create || !(client = create("SteamClient023", NULL))) {
        stage("steamclient023_missing");
        goto cleanup;
    }
    pipe = METHOD(client, 0, pipe_fn)(client);
    if (!pipe || !(user = METHOD(client, 0x10, user_fn)(client, pipe))) {
        stage("steam_user_connection_failed");
        goto cleanup;
    }
    input = METHOD(client, 0x60, interface_fn)(client, user, pipe, "SteamInput006");
    if (!input || !(initialized = METHOD(input, 0, init_fn)(input, true))) {
        stage("steaminput006_init_failed");
        goto cleanup;
    }
    frame = METHOD(input, 0x18, frame_fn);
    stage("steaminput_ready");
    uint64_t handles[16] = {0};
    int count = 0, switches = 0;
    for (unsigned i = 0; i < 250; ++i) {
        pump(8);
        count = METHOD(input, 0x30, controllers_fn)(input, handles);
        if (count > 0) break;
    }
    if (count < 0 || count > 16) {
        stage("invalid_controller_count");
        goto cleanup;
    }
    for (int i = 0; i < count; ++i) {
        int type = METHOD(input, 0x128, type_fn)(input, handles[i]);
        uint32_t remote = METHOD(input, 0x168, remote_fn)(input, handles[i]);
        printf("{\"stage\":\"controller\",\"handle\":\"%016llx\",\"steaminput_type\":%d,"
               "\"remote_session_id\":%lu}\n", (unsigned long long)handles[i], type, (unsigned long)remote);
        if (type == 10) { selected = handles[i]; ++switches; }
    }
    if (!probe) { result = 0; stage("list_complete"); goto cleanup; }
    if (switches != 1) {
        selected = 0;
        stage("probe_requires_exactly_one_switch_pro");
        goto cleanup;
    }
    stop(selected);
    pump(800);
    stage("ordinary_left_request");
    METHOD(input, 0xf0, rumble_fn)(input, selected, 16384, 0);
    pump(150);
    stop(selected);
    pump(800); /* Separate from the host's ordinary/pulse conflict window. */
    for (int side = 0; side < 2; ++side) {
        for (int n = 0; n < 3; ++n) {
            stage(side ? "native_right_request" : "native_left_request");
            /* Ordinary legacy API: no internal bypass flags, no preference edit. */
            METHOD(input, 0x110, pulse_fn)(input, selected, side, 1000);
            pump(120);
        }
    }
    stop(selected);
    pump(250);
    stage("requests_complete_delivery_unverified");
    result = 0;
cleanup:
    if (initialized) {
        if (probe && selected) { stop(selected); pump(150); }
        METHOD(input, 8, shutdown_fn)(input);
    }
    if (user) METHOD(client, 0x20, release_user_fn)(client, pipe, user);
    if (pipe) METHOD(client, 8, release_pipe_fn)(client, pipe);
    if (module) FreeLibrary(module);
    stage(result ? "probe_failed" : "cleanup_complete");
    return result;
}
