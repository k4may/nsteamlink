/* Controlled Steam Input source, Windows x64. Uses the host's own Steam DLL.
 * ABI slots: Valve Proton steamworks_sdk_163 isteamclient.h / isteaminput.h,
 * Output uses SteamClient023 and SteamInput006. Empty public enumeration can
 * use a hash/layout-pinned read-only controller-context query; no patching.
 * API dispatch is evidence of a request, never evidence of delivered haptics.
 * Build: x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror -municode
 *        tools/steam-haptics-probe-win.c -o steam-haptics-probe.exe -ladvapi32 -lbcrypt
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

#ifndef NSL_PROBE_HOST_SHA256
#define NSL_PROBE_HOST_SHA256 "669fa08e259277fbf5a1dfe15ce88afe734e89151194304b44d92f418dcc985b"
#endif

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
typedef int (*context_controllers_fn)(void *, uint32_t, uint64_t *);
#define METHOD(object, slot, type) ((type)((*(void ***)(object))[(slot) / 8]))

static void *input;
static frame_fn frame;
static LARGE_INTEGER frequency, started;
static bool file_sha256(const wchar_t *path, char hex[65]) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, 0, NULL);
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR object = NULL;
    DWORD object_size = 0, bytes = 0;
    unsigned char digest[32], buffer[4096];
    bool good = false;
    if (file == INVALID_HANDLE_VALUE) return false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_size,
                          sizeof(object_size), &bytes, 0) < 0) goto done;
    object = malloc(object_size);
    if (!object || BCryptCreateHash(algorithm, &hash, object, object_size, NULL, 0, 0) < 0) goto done;
    for (;;) {
        if (!ReadFile(file, buffer, sizeof(buffer), &bytes, NULL)) goto done;
        if (!bytes) break;
        if (BCryptHashData(hash, buffer, bytes, 0) < 0) goto done;
    }
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) goto done;
    for (unsigned i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    good = true;
done:
    if (hash) BCryptDestroyHash(hash);
    free(object);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return good;
}
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
static int context_controllers(HMODULE module, const wchar_t *dll, uint32_t appid,
                                uint64_t handles[16]) {
    char sha[65];
    if (!file_sha256(dll, sha) || strcmp(sha, NSL_PROBE_HOST_SHA256)) {
        stage("context_query_unknown_host_binary");
        return -1;
    }
    uintptr_t base = (uintptr_t)module;
    if ((uintptr_t)*(void ***)input != base + 0x133f1e0) {
        stage("context_query_unexpected_input_layout");
        return -1;
    }
    void *controller = *(void **)((unsigned char *)input + 8);
    if (controller) {
        printf("{\"stage\":\"context_query_layout\",\"controller_vtable_rva\":\"%llx\","
               "\"query_function_rva\":\"%llx\"}\n",
               (unsigned long long)((uintptr_t)*(void ***)controller - base),
               (unsigned long long)((uintptr_t)METHOD(controller, 0x60, context_controllers_fn) - base));
    }
    if (!controller || (uintptr_t)*(void ***)controller != base + 0x1304dc0 ||
        (uintptr_t)METHOD(controller, 0x60, context_controllers_fn) != base + 0x61a9a0) {
        stage("context_query_unexpected_controller_layout");
        return -1;
    }
    const uint32_t contexts[] = {0, 769, 413080, appid};
    int count = 0;
    for (unsigned i = 0; i < sizeof(contexts) / sizeof(contexts[0]); ++i) {
        uint64_t found[16] = {0};
        int n = METHOD(controller, 0x60, context_controllers_fn)(controller, contexts[i], found);
        printf("{\"stage\":\"context_controller_count\",\"context_appid\":%lu,\"count\":%d}\n",
               (unsigned long)contexts[i], n);
        if (n < 0 || n > 16) {
            stage("context_query_invalid_count");
            return -1;
        }
        for (int j = 0; j < n; ++j) {
            bool duplicate = !found[j];
            for (int k = 0; k < count; ++k) duplicate |= handles[k] == found[j];
            if (!duplicate) {
                if (count == 16) {
                    stage("context_query_invalid_count");
                    return -1;
                }
                handles[count++] = found[j];
            }
        }
    }
    stage("context_query_complete");
    return count;
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
    printf("{\"stage\":\"public_controller_count\",\"count\":%d}\n", count);
    if (!count) {
        count = context_controllers(module, dll, (uint32_t)wcstoul(appid, NULL, 10), handles);
        if (count < 0) goto cleanup;
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
