/* Passive game observer: no Steam API calls, IPC requests, writes or injection.
 * Pinning and offsets are documented in OFFICIAL_INPUT_RE.md 22.16/22.33. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "steam-haptics-host-inspect-win.h"

static volatile LONG stopping;
static BOOL WINAPI interrupt(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    InterlockedExchange(&stopping, 1); return TRUE;
}
static bool file_sha256(const wchar_t *path, char hex[65]) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING, 0, NULL);
    BCRYPT_ALG_HANDLE algorithm = NULL; BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR object = NULL; DWORD size = 0, bytes = 0;
    unsigned char digest[32], buffer[4096]; bool good = false;
    if (file == INVALID_HANDLE_VALUE) return false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&size, sizeof(size), &bytes, 0) < 0) goto done;
    object = malloc(size);
    if (!object || BCryptCreateHash(algorithm, &hash, object, size, NULL, 0, 0) < 0) goto done;
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
    CloseHandle(file); return good;
}

static HANDLE find_steam(uint64_t *base) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0), found = NULL;
    if (snapshot == INVALID_HANDLE_VALUE) return NULL;
    PROCESSENTRY32W p = {.dwSize=sizeof(p)};
    if (Process32FirstW(snapshot, &p)) do {
        if (_wcsicmp(p.szExeFile, L"steam.exe")) continue;
        HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, p.th32ProcessID);
        if (modules == INVALID_HANDLE_VALUE) continue;
        MODULEENTRY32W m = {.dwSize=sizeof(m)};
        if (Module32FirstW(modules, &m)) do {
            if (_wcsicmp(m.szModule, L"steamclient64.dll")) continue;
            char sha[65] = "unreadable";
            bool supported = file_sha256(m.szExePath, sha) && !strcmp(sha,
                "669fa08e259277fbf5a1dfe15ce88afe734e89151194304b44d92f418dcc985b");
            printf("{\"stage\":\"steam_module\",\"pid\":%lu,\"sha256\":\"%s\",\"supported\":%s}\n",
                   (unsigned long)p.th32ProcessID, sha, supported ? "true" : "false");
            if (supported) {
                found = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, p.th32ProcessID);
                *base = (uintptr_t)m.modBaseAddr;
            }
            break;
        } while (Module32NextW(modules, &m));
        CloseHandle(modules);
        if (found) break;
    } while (Process32NextW(snapshot, &p));
    CloseHandle(snapshot); return found;
}

/* Baselines, identity/config switches and unavailable intervals are not activity.
 * Persistent old words/shared timestamps must never be called native events. */
static bool same_target(const nsl_host_haptics_state *a, const nsl_host_haptics_state *b) {
    return a->handle == b->handle && a->appid == b->appid &&
           a->io_address == b->io_address && a->slot == b->slot &&
           a->controller_type == b->controller_type;
}

int main(int argc, char **argv) {
    unsigned seconds = 180; uint64_t requested = 0;
    for (int i = 1; i < argc; ++i) {
        char *end = NULL;
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (*end || value < 1 || value > 3600) return 2;
            seconds = (unsigned)value;
        } else if (!strcmp(argv[i], "--handle") && i + 1 < argc) {
            requested = strtoull(argv[++i], &end, 0); if (*end || !requested) return 2;
        } else { fputs("Usage: steam-game-haptics-watch.exe [--seconds 180] [--handle 0x...]\n", stderr); return 2; }
    }
    SetConsoleCtrlHandler(interrupt, TRUE);
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("{\"stage\":\"capture_begin\",\"read_only\":true,\"host_sampling_ms\":100,"
         "\"game_api_attribution_verified\":false,\"packet_delivery_verified\":false}");
    uint64_t start = GetTickCount64(), base = 0, cached = 0, next_scan = 0, next_find = 0;
    uint64_t next_heartbeat = 0; unsigned snapshots = 0, native_changes = 0, ordinary_changes = 0;
    HANDLE process = NULL; bool previous_valid = false;
    nsl_host_haptics_state previous = {0};
    while (!InterlockedCompareExchange(&stopping, 0, 0) && GetTickCount64() - start < seconds * 1000ULL) {
        uint64_t now = GetTickCount64();
        if (process) {
            DWORD exit_code;
            if (!GetExitCodeProcess(process, &exit_code) || exit_code != STILL_ACTIVE) {
                CloseHandle(process); process = NULL; cached = 0; previous_valid = false;
            }
        }
        if (!process && now >= next_find) {
            process = find_steam(&base); next_find = now + 5000; next_scan = 0;
        }
        nsl_host_haptics_state state = {0}; nsl_host_inspection report = {0};
        bool allow_scan = now >= next_scan;
        int result = process ? nsl_host_inspect_process_for(process, base, requested, &cached,
                                                          allow_scan, true, &state, &report) : 0;
        if (allow_scan) next_scan = now + 10000;
        if (result != 1) {
            previous_valid = false;
            if (now >= next_heartbeat) {
                printf("{\"stage\":\"host_unavailable\",\"elapsed_ms\":%llu,\"process_open\":%s,"
                       "\"result\":%d,\"source\":\"%s\",\"scanned_bytes\":%llu,\"bounded\":%s}\n",
                       (unsigned long long)(now-start), process ? "true" : "false", result,
                       report.source ? report.source : "no_supported_process",
                       (unsigned long long)report.scanned_bytes, report.bounded ? "true" : "false");
                next_heartbeat = now + 1000;
            }
        } else {
            bool baseline = !previous_valid || !same_target(&previous, &state);
            bool sony = state.controller_type == 45 || state.controller_type == 48;
            bool native = !sony && !baseline && (previous.native_left_ticks != state.native_left_ticks ||
                                        previous.native_right_ticks != state.native_right_ticks);
            bool ordinary = !baseline && (previous.ordinary_left != state.ordinary_left ||
                                          previous.ordinary_right != state.ordinary_right);
            native_changes += native; ordinary_changes += ordinary;
            if (baseline || memcmp(&previous, &state, sizeof(state)) || now >= next_heartbeat) {
                if (sony) {
                    const bool changed = !baseline &&
                        memcmp(previous.sony_trigger_effects, state.sony_trigger_effects,
                               sizeof(state.sony_trigger_effects));
                    printf("{\"stage\":\"sony_host_state\",\"elapsed_ms\":%llu,\"handle\":\"0x%016llx\","
                           "\"slot\":%u,\"appid\":%u,\"controller_type\":%u,\"config_type\":%u,"
                           "\"device_caps\":\"%016llx\",\"config_caps\":\"%016llx\","
                           "\"baseline\":%s,\"ordinary_cache_changed\":%s,\"ordinary_left\":%u,"
                           "\"ordinary_right\":%u,\"sony_firmware_raw\":%u,\"sony_details_caps\":\"%016llx\","
                           "\"sony_connection_raw\":%u,\"sony_trigger_cache_changed\":%s,"
                           "\"sony_trigger_cache_hex\":\"",
                           (unsigned long long)(now-start), (unsigned long long)state.handle,
                           state.slot, state.appid, state.controller_type, state.config_type,
                           (unsigned long long)state.device_caps, (unsigned long long)state.config_caps,
                           baseline?"true":"false", ordinary?"true":"false", state.ordinary_left, state.ordinary_right,
                           state.sony_firmware, (unsigned long long)state.sony_details_caps, state.sony_connection,
                           changed?"true":"false");
                    for (size_t i = 0; i < sizeof(state.sony_trigger_effects); ++i) {
                        printf("%02x", state.sony_trigger_effects[i]);
                    }
                    printf("\",\"source\":\"%s\",\"packet_delivery_verified\":false}\n", report.source);
                } else {
                    printf("{\"stage\":\"host_state\",\"elapsed_ms\":%llu,\"handle\":\"0x%016llx\","
                           "\"slot\":%u,\"appid\":%u,\"config_type\":%u,\"baseline\":%s,"
                           "\"ordinary_cache_changed\":%s,\"native_producer_changed\":%s,"
                           "\"ordinary_left\":%u,\"ordinary_right\":%u,\"native_left_ticks\":%llu,"
                           "\"native_right_ticks\":%llu,\"shared_ticks\":%llu,\"native_left_word\":\"%08x\","
                           "\"native_right_word\":\"%08x\",\"pulse_gate\":%u,\"rumble_gate\":%u,"
                           "\"config_pulse\":%u,\"config_rumble\":%u,\"haptics_preference\":%u,"
                           "\"preference_valid\":%u,\"enhanced\":%u,\"source\":\"%s\","
                           "\"packet_delivery_verified\":false}\n",
                           (unsigned long long)(now-start), (unsigned long long)state.handle,
                           state.slot, state.appid, state.config_type, baseline?"true":"false",
                           ordinary?"true":"false", native?"true":"false", state.ordinary_left, state.ordinary_right,
                           (unsigned long long)state.native_left_ticks, (unsigned long long)state.native_right_ticks,
                           (unsigned long long)state.shared_ticks, state.native_left_word, state.native_right_word,
                           state.pulse_gate, state.rumble_gate, state.config_pulse, state.config_rumble,
                           state.haptics_preference, state.preference_valid, state.enhanced, report.source);
                }
                ++snapshots; next_heartbeat = now + 1000;
            }
            previous = state; previous_valid = true;
        }
        Sleep(100);
    }
    if (process) CloseHandle(process);
    printf("{\"stage\":\"capture_end\",\"host_snapshots\":%u,\"native_change_observations\":%u,"
           "\"ordinary_change_observations\":%u,\"exact_event_counts\":false,\"packet_delivery_verified\":false}\n",
           snapshots, native_changes, ordinary_changes);
    return 0;
}
