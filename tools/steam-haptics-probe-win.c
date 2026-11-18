/* Controlled Steam Input source, Windows x64. Uses the host's own Steam DLL.
 * ABI slots: Valve Proton steamworks_sdk_163 isteamclient.h / isteaminput.h,
 * Default output uses SteamClient023 and SteamInput006. Empty public enumeration can
 * use a hash/layout-pinned read-only controller-context query; no patching.
 * Explicit RPC comparison pins private ABI and brackets requests via device UDP.
 * API dispatch is evidence of a request, never evidence of delivered haptics.
 * Build: x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror -municode
 *        tools/steam-haptics-probe-win.c -o steam-haptics-probe.exe -ladvapi32 -lbcrypt -lws2_32
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
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
typedef void *(*map_factory_fn)(int, int);
typedef void *(*map_destroy_fn)(void *, unsigned);
typedef bool (*details_fn)(void *, unsigned, unsigned char *);
typedef void (*index_pulse_fn)(void *, unsigned, int, uint16_t, uint16_t, uint16_t, unsigned);
#define METHOD(object, slot, type) ((type)((*(void ***)(object))[(slot) / 8]))

static void *input;
static frame_fn frame;
static LARGE_INTEGER frequency, started;
static SOCKET diagnostic = INVALID_SOCKET;
static void *host_map;
static int host_index = -1;
static uint64_t trace_serial;
static const char *route = "public";
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
static void quoted(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 32 || *p > 126) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}
/* One connected UDP socket, no netloader traffic. Each fresh socket excludes
 * late replies to a timed-out prior request from a later request's bracket. */
static bool diagnostic_query(const char *command, char reply[8192]) {
    struct sockaddr_in peer;
    int length = sizeof(peer);
    if (getpeername(diagnostic, (struct sockaddr *)&peer, &length)) return false;
    SOCKET request = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (request == INVALID_SOCKET) return false;
    DWORD timeout = 300;
    if (setsockopt(request, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) ||
        setsockopt(request, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout))) {
        closesocket(request); stage("device_socket_timeout_setup_failed"); return false;
    }
    uint64_t sent = elapsed_us();
    int size = -1;
    if (!connect(request, (struct sockaddr *)&peer, sizeof(peer)) &&
        send(request, command, (int)strlen(command), 0) == (int)strlen(command))
        size = recv(request, reply, 8191, 0);
    int error = size < 0 ? WSAGetLastError() : 0;
    closesocket(request);
    reply[size < 0 ? 0 : size] = 0;
    printf("{\"stage\":\"device_reply\",\"route\":\"%s\",\"sent_us\":%llu,\"received_us\":%llu,"
           "\"error\":%d,\"command\":", route, (unsigned long long)sent,
           (unsigned long long)elapsed_us(), error);
    quoted(command);
    printf(",\"reply\":"); quoted(reply); puts("}"); fflush(stdout);
    return size > 0;
}
static bool snapshot(const char *label) {
    if (diagnostic == INVALID_SOCKET) return true;
    printf("{\"stage\":\"snapshot_begin\",\"route\":\"%s\",\"label\":\"%s\",\"elapsed_us\":%llu}\n",
           route, label, (unsigned long long)elapsed_us());
    char command[80], reply[8192];
    if (!diagnostic_query("haptics", reply)) return false;
    /* Read-only drain, bounded even if the device produces continuous output. */
    for (unsigned page = 0; page < 64; ++page) {
        snprintf(command, sizeof(command), "haptic-trace %llu", (unsigned long long)trace_serial);
        if (!diagnostic_query(command, reply)) return false;
        uint64_t previous = trace_serial;
        const char *p = reply;
        while ((p = strstr(p, "\"serial\":"))) {
            p += 9;
            unsigned long long value = strtoull(p, NULL, 10);
            if (value > trace_serial) trace_serial = value;
        }
        if (trace_serial == previous) {
            printf("{\"stage\":\"snapshot_end\",\"route\":\"%s\",\"label\":\"%s\",\"after_serial\":%llu,"
                   "\"elapsed_us\":%llu}\n", route, label, (unsigned long long)trace_serial,
                   (unsigned long long)elapsed_us());
            fflush(stdout);
            return true;
        }
    }
    stage("device_trace_drain_limit");
    return false;
}
static bool open_host_map(HMODULE module, const wchar_t *dll, int user, uint64_t handle) {
    char sha[65];
    uintptr_t base = (uintptr_t)module;
    if (!file_sha256(dll, sha) || strcmp(sha, NSL_PROBE_HOST_SHA256)) {
        stage("host_rpc_unknown_host_binary"); return false;
    }
    /* The engine's original call uses pipe=0. This is an owned, separate map. */
    host_map = ((map_factory_fn)(base + 0x76d560))(0, user);
    if (!host_map) { stage("host_rpc_map_allocation_failed"); return false; }
    if ((uintptr_t)*(void ***)host_map != base + 0x1369980 ||
        (uintptr_t)METHOD(host_map, 0, map_destroy_fn) != base + 0x7483d0 ||
        (uintptr_t)METHOD(host_map, 0x2a8, index_pulse_fn) != base + 0x8b6b60 ||
        (uintptr_t)METHOD(host_map, 0x338, details_fn) != base + 0x77e0b0) {
        stage("host_rpc_unexpected_map_layout"); return false;
    }
    int matches = 0;
    for (unsigned i = 0; i < 16; ++i) {
        unsigned char info[88] = {0};
        if (!METHOD(host_map, 0x338, details_fn)(host_map, i, info)) continue;
        uint64_t id, caps;
        uint32_t type;
        memcpy(&id, info + 0x20, 8); memcpy(&caps, info + 8, 8); memcpy(&type, info + 0x34, 4);
        printf("{\"stage\":\"host_controller_details\",\"index\":%u,\"handle\":\"%016llx\","
               "\"type\":%lu,\"caps\":\"%016llx\",\"valid\":%u}\n", i,
               (unsigned long long)id, (unsigned long)type, (unsigned long long)caps, info[0x40]);
        if (id == handle && type == 38 && info[0x40]) { host_index = (int)i; ++matches; }
    }
    if (matches != 1) { host_index = -1; stage("host_rpc_requires_unique_matching_handle"); return false; }
    stage("host_rpc_target_ready");
    return true;
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
static void request_pulse(uint64_t handle, int side, uint16_t duration) {
    if (!strcmp(route, "host_rpc"))
        METHOD(host_map, 0x2a8, index_pulse_fn)(host_map, (unsigned)host_index, side, duration, 0, 1, 0);
    else METHOD(input, 0x110, pulse_fn)(input, handle, side, duration);
}
static void stop_pulse(uint64_t handle) {
    request_pulse(handle, 0, 0); request_pulse(handle, 1, 0);
}
int wmain(int argc, wchar_t **argv) {
    wchar_t root[32768] = {0}, dll[32768], appid[16] = L"1049320";
    bool probe = false, initialized = false, compare_rpc = false, winsock = false;
    wchar_t switch_address[64] = {0};
    int result = 1, pipe = 0, user = 0;
    uint64_t selected = 0;
    void *client = NULL;
    HMODULE module = NULL;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--probe")) probe = true;
        else if (!wcscmp(argv[i], L"--list")) probe = false;
        else if (!wcscmp(argv[i], L"--compare-host-rpc")) compare_rpc = true;
        else if (!wcscmp(argv[i], L"--switch-address") && i + 1 < argc) {
            if (wcslen(argv[++i]) >= 64) goto cleanup;
            wcscpy(switch_address, argv[i]);
        }
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
    if (compare_rpc && (!probe || !switch_address[0])) {
        stage("host_rpc_requires_probe_and_device"); goto cleanup;
    }
    if (switch_address[0]) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data)) goto cleanup;
        winsock = true;
        struct sockaddr_in address = {0};
        address.sin_family = AF_INET; address.sin_port = htons(28772);
        if (InetPtonW(AF_INET, switch_address, &address.sin_addr) != 1 ||
            (diagnostic = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET ||
            connect(diagnostic, (struct sockaddr *)&address, sizeof(address))) {
            stage("device_address_invalid"); goto cleanup;
        }
        if (!snapshot("initial")) { stage("device_unavailable_before_requests"); goto cleanup; }
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
    uint32_t selected_remote = 0;
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
        if (type == 10) { selected = handles[i]; selected_remote = remote; ++switches; }
    }
    if (!probe) { result = 0; stage("list_complete"); goto cleanup; }
    if (switches != 1) {
        selected = 0;
        stage("probe_requires_exactly_one_switch_pro");
        goto cleanup;
    }
    if (compare_rpc && !selected_remote) { stage("host_rpc_requires_remote_controller"); goto cleanup; }
    if (compare_rpc && !open_host_map(module, dll, user, selected)) goto cleanup;
    stop(selected);
    pump(800);
    stage("ordinary_left_request");
    METHOD(input, 0xf0, rumble_fn)(input, selected, 16384, 0);
    pump(150);
    stop(selected);
    pump(800); /* Separate from the host's ordinary/pulse conflict window. */
    for (unsigned pass = 0; pass < (compare_rpc ? 2u : 1u); ++pass) {
        route = pass ? "host_rpc" : "public";
        for (int side = 0; side < 2; ++side) {
            for (int n = 0; n < 3; ++n) {
                if (compare_rpc) {
                    stop_pulse(selected); pump(150);
                    if (!snapshot("before_pulse")) { stage("device_snapshot_failed"); goto cleanup; }
                }
                printf("{\"stage\":\"pulse_request\",\"route\":\"%s\",\"side\":%d,\"ordinal\":%d,"
                       "\"duration_us\":1000,\"flags\":0,\"elapsed_us\":%llu}\n",
                       route, side, n, (unsigned long long)elapsed_us());
                stage(side ? "native_right_request" : "native_left_request");
                /* Ordinary legacy API: no internal bypass flags, no preference edit. */
                request_pulse(selected, side, 1000);
                pump(compare_rpc ? 350 : 120);
                if (compare_rpc && !snapshot("after_pulse")) { stage("device_snapshot_failed"); goto cleanup; }
            }
        }
        stop_pulse(selected); pump(150);
        if (!snapshot("route_complete")) { stage("device_snapshot_failed"); goto cleanup; }
    }
    stop(selected);
    pump(250);
    stage("requests_complete_delivery_unverified");
    result = 0;
cleanup:
    if (host_map && host_index >= 0) {
        route = "host_rpc"; stop_pulse(selected);
        if (initialized) pump(150);
    }
    if (host_map) {
        /* Hash-pinned deleting destructor, including layout rejection cleanup. */
        ((map_destroy_fn)((uintptr_t)module + 0x7483d0))(host_map, 1);
        host_map = NULL;
        stage("host_rpc_map_destroyed");
    }
    if (initialized) {
        if (probe && selected) { stop(selected); pump(150); }
        METHOD(input, 8, shutdown_fn)(input);
    }
    if (user) METHOD(client, 0x20, release_user_fn)(client, pipe, user);
    if (pipe) METHOD(client, 8, release_pipe_fn)(client, pipe);
    if (module) FreeLibrary(module);
    if (diagnostic != INVALID_SOCKET) closesocket(diagnostic);
    if (winsock) WSACleanup();
    stage(result ? "probe_failed" : "cleanup_complete");
    return result;
}
