/* Live Windows game-source panel. Launch as a Steam non-Steam game.
 * XInput is the ordinary game's path; public SteamInput006 is selectable.
 * No private RPC, fake AppID, driver, injected packet, or Valve DLL is shipped.
 * Native ABI order: Valve Proton steamworks_sdk_163 isteamclient/isteaminput.h.
 * Build: x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror
 * -municode -mwindows tools/steam-haptics-panel-win.c -o steam-haptics-panel.exe
 * -lcomctl32 -lshell32 -ladvapi32 -lws2_32 -lgdi32
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <xinput.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
#include <string.h>
#include "haptics-panel-state.h"

#define METHOD(o, slot, type) ((type)((*(void ***)(o))[(slot) / 8]))
#define UPDATE_CAPTURE (WM_APP + 1)
enum { ID_ROUTE=100, ID_TARGET, ID_LOW, ID_HIGH, ID_LOW_EDIT, ID_HIGH_EDIT,
       ID_MODE, ID_ON, ID_OFF, ID_LIMIT, ID_PLAY, ID_STOP, ID_ADDRESS, ID_CONNECT,
       ID_LOGS, ID_ZERO, ID_WEAK, ID_MID, ID_FULL };
typedef DWORD (WINAPI *get_state_fn)(DWORD, XINPUT_STATE *);
typedef DWORD (WINAPI *set_state_fn)(DWORD, XINPUT_VIBRATION *);
typedef void *(*create_fn)(const char *, int *);
typedef int (*pipe_fn)(void *);
typedef int (*user_fn)(void *, int);
typedef bool (*release_pipe_fn)(void *, int);
typedef void (*release_user_fn)(void *, int, int);
typedef void *(*interface_fn)(void *, int, int, const char *);
typedef bool (*init_fn)(void *, bool);
typedef bool (*shutdown_fn)(void *);
typedef void (*frame_fn)(void *, bool);
typedef int (*controllers_fn)(void *, uint64_t *);
typedef void (*rumble_fn)(void *, uint64_t, uint16_t, uint16_t);
typedef int (*type_fn)(void *, uint64_t);
typedef uint32_t (*remote_fn)(void *, uint64_t);
typedef uint64_t (*index_handle_fn)(void *, int);
typedef struct target { uint64_t id; unsigned route; } target;
static HWND window, low_slider, high_slider, low_edit, high_edit, route_box, target_box,
    mode_box, on_edit, off_edit, limit_edit, address_edit, status_label, receive_label, output_label,
    low_label, high_label;
static HFONT font;
static HINSTANCE instance;
static HMODULE xmodule, smodule;
static get_state_fn xget;
static set_state_fn xset;
static void *client, *input;
static int steam_pipe, steam_user;
static bool input_initialized, controls_changing, winsock_ready, foreground = true;
static nsl_panel_state state = {.on_ms=500, .off_ms=500, .limit_ms=60000};
static target targets[16], chosen;
static unsigned target_count;
static uint64_t request_sequence, last_scan, origin_ms;
static wchar_t root_override[MAX_PATH], log_folder[MAX_PATH], log_path[MAX_PATH], settings_path[MAX_PATH];
static FILE *log_file;
static CRITICAL_SECTION data_lock;
static HANDLE capture_thread, capture_stop;
static char capture_ip[64];
static wchar_t capture_status[512], capture_output[512];
static uint16_t capture_low, capture_high;
static uint64_t capture_packets;
static bool capture_have_levels;
static wchar_t steam_context[96];

static uint64_t now_ms(void) { return GetTickCount64(); }
static void log_event(const char *format, ...) {
    EnterCriticalSection(&data_lock);
    if (log_file) {
        fprintf(log_file, "{\"host_ms\":%llu,", (unsigned long long)(now_ms() - origin_ms));
        va_list args; va_start(args, format); vfprintf(log_file, format, args); va_end(args);
        fputs("}\n", log_file);
    }
    LeaveCriticalSection(&data_lock);
}
static HWND control(const wchar_t *kind, const wchar_t *text, DWORD style,
                      int x, int y, int width, int height, int id) {
    HWND h = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, window, (HMENU)(INT_PTR)id, instance, NULL);
    if (h) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}
static void label(const wchar_t *text, int x, int y, int width) {
    control(L"STATIC", text, 0, x, y, width, 25, 0);
}
static void set_status(const wchar_t *format, ...) {
    wchar_t message[512]; va_list args; va_start(args, format);
    vswprintf(message, 512, format, args); va_end(args);
    SetWindowTextW(status_label, message);
}
static bool number(HWND edit, unsigned min, unsigned max, unsigned *out) {
    wchar_t text[48], *end;
    GetWindowTextW(edit, text, 48);
    if (text[0] < L'0' || text[0] > L'9') return false;
    unsigned long n = wcstoul(text, &end, 10);
    if (*end || n < min || n > max) return false;
    *out = (unsigned)n; return true;
}
static void put_number(HWND edit, unsigned value) {
    wchar_t text[32]; swprintf(text, 32, L"%u", value); SetWindowTextW(edit, text);
}
static void levels(uint16_t low, uint16_t high) {
    controls_changing = true;
    state.low = low; state.high = high;
    SendMessageW(low_slider, TBM_SETPOS, TRUE, low);
    SendMessageW(high_slider, TBM_SETPOS, TRUE, high);
    put_number(low_edit, low); put_number(high_edit, high);
    wchar_t text[48];
    swprintf(text,48,L"低频 %.1f%%",100.0*low/65535); SetWindowTextW(low_label,text);
    swprintf(text,48,L"高频 %.1f%%",100.0*high/65535); SetWindowTextW(high_label,text);
    controls_changing = false;
}
static bool send_levels(target t, uint16_t low, uint16_t high, const char *reason) {
    DWORD rc = ERROR_DEVICE_NOT_CONNECTED;
    if (!t.route && xset && t.id < 4) {
        XINPUT_VIBRATION vibration = {low, high}; rc = xset((DWORD)t.id, &vibration);
    } else if (t.route == 1 && input_initialized && t.id) {
        METHOD(input, 0xf0, rumble_fn)(input, t.id, low, high); rc = 0;
    }
    log_event("\"stage\":\"request\",\"sequence\":%llu,\"route\":\"%s\",\"target\":%llu,"
        "\"low\":%u,\"high\":%u,\"rc\":%lu,\"reason\":\"%s\",\"delivery_verified\":false",
        (unsigned long long)++request_sequence, t.route ? "steaminput" : "xinput",
        (unsigned long long)t.id, low, high, (unsigned long)rc, reason);
    return rc == ERROR_SUCCESS;
}
static void stop_output(const char *reason) {
    bool was_active = state.active;
    nsl_panel_stop(&state);
    if (target_count && (was_active || state.sent)) (void)send_levels(chosen, 0, 0, reason);
    state.sent_low = state.sent_high = 0; state.sent = true; state.last_send_ms = now_ms();
}
static void steam_open(void) {
    wchar_t root[MAX_PATH], path[MAX_PATH];
    if (root_override[0]) wcscpy(root, root_override);
    else {
        DWORD size = sizeof(root);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                        RRF_RT_REG_SZ, NULL, root, &size) != ERROR_SUCCESS) return;
    }
    if (wcslen(root) > MAX_PATH - 32) return;
    swprintf(path, MAX_PATH, L"%ls\\steamclient64.dll", root);
    smodule = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!smodule) return;
    create_fn create = (create_fn)(uintptr_t)GetProcAddress(smodule, "CreateInterface");
    if (!create || !(client = create("SteamClient023", NULL))) return;
    steam_pipe = METHOD(client, 0, pipe_fn)(client);
    if (!steam_pipe || !(steam_user = METHOD(client, 0x10, user_fn)(client, steam_pipe))) return;
    input = METHOD(client, 0x60, interface_fn)(client, steam_user, steam_pipe, "SteamInput006");
    if (input) input_initialized = METHOD(input, 0, init_fn)(input, true);
    log_event("\"stage\":\"steam_input_init\",\"ready\":%s,\"inherited_context\":true",
              input_initialized ? "true" : "false");
}
static void scan_targets(bool force) {
    target found[16]; unsigned count = 0;
    unsigned route = (unsigned)SendMessageW(route_box, CB_GETCURSEL, 0, 0);
    if (!route && xget) {
        for (DWORD i = 0; i < 4; ++i) {
            XINPUT_STATE value;
            if (xget(i, &value) == ERROR_SUCCESS) found[count++] = (target){i, 0};
        }
    } else if (route == 1 && input_initialized) {
        uint64_t handles[16] = {0};
        int n = METHOD(input, 0x30, controllers_fn)(input, handles);
        if (n >= 0 && n <= 16)
            for (int i = 0; i < n; ++i) if (handles[i]) found[count++] = (target){handles[i], 1};
    }
    bool same = count == target_count;
    for (unsigned i = 0; same && i < count; ++i)
        same = found[i].route == targets[i].route && found[i].id == targets[i].id;
    if (same && !force) return;
    target old = chosen;
    bool still_connected = false;
    for (unsigned i = 0; target_count && i < count; ++i)
        if (found[i].route == old.route && found[i].id == old.id) still_connected = true;
    if (!still_connected || force) stop_output("target_change");
    target_count = count;
    memcpy(targets, found, count * sizeof(found[0]));
    SendMessageW(target_box, CB_RESETCONTENT, 0, 0);
    unsigned select = 0;
    for (unsigned i = 0; i < count; ++i) {
        wchar_t text[128];
        if (!found[i].route) {
            uint64_t mapped=input_initialized ? METHOD(input,0x130,index_handle_fn)(input,(int)found[i].id) : 0;
            uint32_t remote=mapped ? METHOD(input,0x168,remote_fn)(input,mapped) : 0;
            swprintf(text,128,L"游戏手柄 %llu (XInput)%ls",(unsigned long long)found[i].id+1,remote?L" / 远程":L"");
            if(!still_connected && remote) select=i;
        }
        else {
            int type = METHOD(input, 0x128, type_fn)(input, found[i].id);
            uint32_t remote = METHOD(input, 0x168, remote_fn)(input, found[i].id);
            swprintf(text, 128, L"%ls / 类型 %d / %016llx", remote ? L"远程手柄" : L"本地手柄",
                type, (unsigned long long)found[i].id);
            if (!still_connected && remote) select = i;
        }
        if (found[i].id == old.id && found[i].route == old.route && still_connected) select = i;
        SendMessageW(target_box, CB_ADDSTRING, 0, (LPARAM)text);
    }
    if (count) { chosen = found[select]; SendMessageW(target_box, CB_SETCURSEL, select, 0); }
    else set_status(L"未发现手柄：请连接 Switch，再从 Steam 库启动本工具。可切换发送方式检查。");
    log_event("\"stage\":\"controllers\",\"route\":\"%s\",\"count\":%u", route ? "steaminput" : "xinput", count);
}
static void start_output(void) {
    unsigned on, off, limit, low, high;
    if (!number(low_edit,0,65535,&low) || !number(high_edit,0,65535,&high)) {
        set_status(L"力度需为 0–65535 的整数。"); return;
    }
    if (!number(on_edit, 20, 60000, &on) || !number(off_edit, 20, 60000, &off) ||
        !number(limit_edit, 100, 600000, &limit)) {
        set_status(L"时间需为整数：开/关 20–60000ms，播放上限 100–600000ms。"); return;
    }
    if (!target_count) { set_status(L"没有可用手柄。请先连接 Switch，从 Steam 库启动本工具。"); return; }
    stop_output("restart");
    state.low=(uint16_t)low; state.high=(uint16_t)high;
    state.on_ms = on; state.off_ms = off; state.limit_ms = limit;
    state.mode = (nsl_panel_mode)SendMessageW(mode_box, CB_GETCURSEL, 0, 0);
    if (nsl_panel_start(&state, now_ms()))
        set_status(L"播放中：直接拖动力度即可实时调整。停止 / 空格键 / 切到其他窗口会归零。");
}
static void update_output(void) {
    uint16_t values[2];
    bool active = state.active;
    if (target_count && nsl_panel_next(&state, now_ms(), values)) {
        if (!send_levels(chosen, values[0], values[1], "update")) {
            nsl_panel_stop(&state);
            set_status(L"发送失败或手柄已断开；已停止。重新连接后再点击播放。");
        }
    }
    if (active && !state.active) set_status(L"已停止。可以修改参数再播放。");
}
static uint64_t json_uint(const char *text, const char *key, bool *exists) {
    char pattern[80]; snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(text, pattern);
    if (!p) { if (exists) *exists = false; return 0; }
    p += strlen(pattern);
    if (*p < '0' || *p > '9') { if (exists) *exists = false; return 0; }
    if (exists) *exists = true;
    return strtoull(p, NULL, 10);
}
static int hex_nibble(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static void received_line(const char *line, uint64_t *after, bool *changed) {
    bool has_serial;
    uint64_t serial = json_uint(line, "serial", &has_serial);
    if (!has_serial || serial <= *after) return;
    if (*after && serial != *after + 1)
        log_event("\"stage\":\"trace_gap\",\"previous\":%llu,\"next\":%llu",
            (unsigned long long)*after, (unsigned long long)serial);
    *after = serial;
    unsigned kind = (unsigned)json_uint(line, "kind", NULL);
    EnterCriticalSection(&data_lock);
    if (kind == 1) {
        const char *wire = strstr(line, "\"wire\":\"");
        if (wire) {
            wire += 8;
            if (strlen(wire) < 10) { LeaveCriticalSection(&data_lock); return; }
            uint8_t b[5]; bool valid = true;
            for (unsigned i = 0; i < 5; ++i) {
                int hi = hex_nibble(wire[i * 2]), lo = hex_nibble(wire[i * 2 + 1]);
                if (hi < 0 || lo < 0) { valid = false; break; }
                b[i] = (uint8_t)(hi * 16 + lo);
            }
            if (valid && b[0] == 1) {
                capture_low = (uint16_t)(b[1] | b[2] << 8);
                capture_high = (uint16_t)(b[3] | b[4] << 8);
                capture_have_levels = true;
            } else capture_have_levels = false;
            ++capture_packets;
            if(capture_have_levels)
                swprintf(capture_status,512,L"Switch 已读到 %llu 条命令；最近普通双路值：%u / %u",
                    (unsigned long long)capture_packets,capture_low,capture_high);
            else
                swprintf(capture_status,512,L"Switch 已读到 %llu 条命令；最近反馈：%ls",
                    (unsigned long long)capture_packets,valid && b[0]==13 && b[1]==10 ? L"Nintendo 原生效果" :
                    valid && b[0]==13 && b[1]==47 ? L"Sony 效果" : valid && b[0]==6 ? L"扳机震动" : L"其他反馈");
        }
    } else if (kind == 2 || kind == 3) {
        const char *v = strstr(line, "\"values\":[[");
        float a,b,c,d,e,f,g,h;
        if (v && sscanf(v + 11, "%f,%f,%f,%f],[%f,%f,%f,%f", &a,&b,&c,&d,&e,&f,&g,&h) == 8)
            swprintf(capture_output, 512, L"Switch 输出：左 %.3f@%.0fHz + %.3f@%.0fHz\n右 %.3f@%.0fHz + %.3f@%.0fHz  / IPC=%llu",
                a,b,c,d,e,f,g,h,(unsigned long long)json_uint(line,"rc",NULL));
    }
    LeaveCriticalSection(&data_lock);
    *changed = true;
}
static DWORD WINAPI collect(LPVOID unused) {
    (void)unused;
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in peer = {0}; peer.sin_family = AF_INET; peer.sin_port = htons(28772);
    DWORD timeout = 150;
    uint64_t after = 0, next_stats = 0;
    bool connected = sock != INVALID_SOCKET && InetPtonA(AF_INET, capture_ip, &peer.sin_addr) == 1 &&
        !setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) &&
        !setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout)) &&
        !connect(sock, (const struct sockaddr *)&peer, sizeof(peer));
    if (connected) while (WaitForSingleObject(capture_stop, 20) == WAIT_TIMEOUT) {
        char command[80], reply[8192];
        bool stats = now_ms() >= next_stats;
        if (stats) strcpy(command, "haptics");
        else snprintf(command, sizeof(command), "haptic-trace %llu", (unsigned long long)after);
        uint64_t sent_ms = now_ms() - origin_ms;
        if (send(sock, command, (int)strlen(command), 0) != (int)strlen(command)) continue;
        int n = recv(sock, reply, sizeof(reply) - 1, 0);
        if (n <= 0) {
            EnterCriticalSection(&data_lock);
            wcscpy(capture_status, L"Switch 采集暂无回应：请运行正常客户端并连接 Steam。发送控制仍可使用。");
            LeaveCriticalSection(&data_lock);
            PostMessageW(window, UPDATE_CAPTURE, 0, 0); continue;
        }
        reply[n] = 0;
        bool changed = false;
        char *context = NULL;
        for (char *line = strtok_r(reply, "\n\r", &context); line; line = strtok_r(NULL, "\n\r", &context)) {
            size_t length = strlen(line);
            if (length < 2 || line[0] != '{' || line[length - 1] != '}') continue;
            log_event("\"stage\":\"device_reply\",\"query\":\"%s\",\"query_sent_ms\":%llu,\"record\":%s",
                command, (unsigned long long)sent_ms, line);
            received_line(line, &after, &changed);
        }
        if (stats) next_stats = now_ms() + 1000;
        if (changed) PostMessageW(window, UPDATE_CAPTURE, 0, 0);
    }
    if (sock != INVALID_SOCKET) closesocket(sock);
    return 0;
}
static void stop_capture(void) {
    if (capture_thread) {
        SetEvent(capture_stop); WaitForSingleObject(capture_thread, INFINITE);
        CloseHandle(capture_thread); capture_thread = NULL;
    }
    if (capture_stop) { CloseHandle(capture_stop); capture_stop = NULL; }
}
static void start_capture(void) {
    stop_capture();
    wchar_t ip[64]; GetWindowTextW(address_edit, ip, 64);
    if (!WideCharToMultiByte(CP_UTF8, 0, ip, -1, capture_ip, sizeof(capture_ip), NULL, NULL)) return;
    struct in_addr addr;
    if (!winsock_ready || InetPtonA(AF_INET, capture_ip, &addr) != 1) {
        SetWindowTextW(receive_label, L"请输入 Switch 的 IPv4 地址。"); return;
    }
    EnterCriticalSection(&data_lock);
    capture_packets = 0; capture_have_levels = false;
    wcscpy(capture_status, L"正在连接 Switch 的自动采集…"); capture_output[0] = 0;
    LeaveCriticalSection(&data_lock);
    capture_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (capture_stop) capture_thread = CreateThread(NULL, 0, collect, NULL, 0, NULL);
    if (!capture_thread) { stop_capture(); SetWindowTextW(receive_label, L"自动采集启动失败。"); }
}
static void save_settings(void) {
    wchar_t value[64];
    swprintf(value,64,L"%u",state.low); WritePrivateProfileStringW(L"Panel",L"Low",value,settings_path);
    swprintf(value,64,L"%u",state.high); WritePrivateProfileStringW(L"Panel",L"High",value,settings_path);
    GetWindowTextW(address_edit,value,64); WritePrivateProfileStringW(L"Panel",L"SwitchIP",value,settings_path);
    const struct { unsigned number; const wchar_t *key; } times[]={{state.on_ms,L"OnMs"},{state.off_ms,L"OffMs"},{state.limit_ms,L"LimitMs"}};
    for (unsigned i=0;i<3;++i) { swprintf(value,64,L"%u",times[i].number); WritePrivateProfileStringW(L"Panel",times[i].key,value,settings_path); }
    swprintf(value,64,L"%ld",(long)SendMessageW(mode_box,CB_GETCURSEL,0,0));
    WritePrivateProfileStringW(L"Panel",L"Mode",value,settings_path);
}
static void create_controls(void) {
    label(L"从 Steam 库启动本工具，Switch 正常连接主机；拖动力度可实时试听。",20,15,700);
    label(L"发送方式",20,52,90);
    route_box=control(WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_TABSTOP,110,48,230,130,ID_ROUTE);
    SendMessageW(route_box,CB_ADDSTRING,0,(LPARAM)L"普通游戏路径 (XInput)");
    SendMessageW(route_box,CB_ADDSTRING,0,(LPARAM)L"Steam Input 双路震动");
    SendMessageW(route_box,CB_SETCURSEL,0,0);
    target_box=control(WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_TABSTOP,350,48,355,160,ID_TARGET);
    low_label=control(L"STATIC",L"低频通道",0,20,100,105,25,0);
    low_slider=control(TRACKBAR_CLASSW,L"",TBS_AUTOTICKS|WS_TABSTOP,115,88,445,46,ID_LOW);
    high_label=control(L"STATIC",L"高频通道",0,20,155,105,25,0);
    high_slider=control(TRACKBAR_CLASSW,L"",TBS_AUTOTICKS|WS_TABSTOP,115,143,445,46,ID_HIGH);
    low_edit=control(L"EDIT",L"",WS_BORDER|ES_NUMBER|WS_TABSTOP,575,94,120,28,ID_LOW_EDIT);
    high_edit=control(L"EDIT",L"",WS_BORDER|ES_NUMBER|WS_TABSTOP,575,149,120,28,ID_HIGH_EDIT);
    for(unsigned i=0;i<2;++i) {
        HWND h=i?high_slider:low_slider;
        SendMessageW(h,TBM_SETRANGEMAX,FALSE,65535); SendMessageW(h,TBM_SETTICFREQ,8192,0);
        SendMessageW(h,TBM_SETLINESIZE,0,1); SendMessageW(h,TBM_SETPAGESIZE,0,655);
    }
    label(L"输入范围 0–65535；0 为停止，65535 为满量程。数字框可精调弱震阈值。",20,198,690);
    control(L"BUTTON",L"归零",WS_TABSTOP,20,230,110,30,ID_ZERO);
    control(L"BUTTON",L"弱震 10%",WS_TABSTOP,145,230,120,30,ID_WEAK);
    control(L"BUTTON",L"中等 50%",WS_TABSTOP,280,230,120,30,ID_MID);
    control(L"BUTTON",L"满量程",WS_TABSTOP,415,230,110,30,ID_FULL);
    label(L"播放方式",20,280,90);
    mode_box=control(WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_TABSTOP,110,275,185,130,ID_MODE);
    SendMessageW(mode_box,CB_ADDSTRING,0,(LPARAM)L"持续（到上限停止）");
    SendMessageW(mode_box,CB_ADDSTRING,0,(LPARAM)L"单次脉冲");
    SendMessageW(mode_box,CB_ADDSTRING,0,(LPARAM)L"重复脉冲");
    unsigned mode=GetPrivateProfileIntW(L"Panel",L"Mode",0,settings_path);
    SendMessageW(mode_box,CB_SETCURSEL,mode<=2?mode:0,0);
    label(L"震动 ms",310,280,80); on_edit=control(L"EDIT",L"500",WS_BORDER|ES_NUMBER|WS_TABSTOP,385,275,80,28,ID_ON);
    label(L"间隔 ms",480,280,80); off_edit=control(L"EDIT",L"500",WS_BORDER|ES_NUMBER|WS_TABSTOP,555,275,80,28,ID_OFF);
    label(L"播放上限 ms",20,325,120); limit_edit=control(L"EDIT",L"60000",WS_BORDER|ES_NUMBER|WS_TABSTOP,145,320,110,28,ID_LIMIT);
    control(L"BUTTON",L"播放（实时调节）",WS_TABSTOP,280,316,190,38,ID_PLAY);
    control(L"BUTTON",L"停止 / 空格",WS_TABSTOP,490,316,190,38,ID_STOP);
    status_label=control(L"STATIC",L"等待手柄连接。",0,20,370,685,45,0);
    label(L"Switch 地址",20,425,110); address_edit=control(L"EDIT",L"10.10.10.111",WS_BORDER|WS_TABSTOP,135,420,190,28,ID_ADDRESS);
    control(L"BUTTON",L"连接自动采集",WS_TABSTOP,345,418,180,32,ID_CONNECT);
    control(L"BUTTON",L"打开记录文件夹",WS_TABSTOP,540,418,165,32,ID_LOGS);
    receive_label=control(L"STATIC",L"自动记录 Switch 实际收到的命令。",0,20,465,685,40,0);
    output_label=control(L"STATIC",L"本机输出参数将在这里显示。",0,20,510,685,65,0);
    label(L"力度与节奏通过 Steam 发出；记录自动保存。切换窗口或关闭程序即停止。",20,590,700);
    uint16_t low=(uint16_t)GetPrivateProfileIntW(L"Panel",L"Low",6554,settings_path);
    uint16_t high=(uint16_t)GetPrivateProfileIntW(L"Panel",L"High",6554,settings_path);
    levels(low,high);
    wchar_t text[64]; GetPrivateProfileStringW(L"Panel",L"SwitchIP",L"10.10.10.111",text,64,settings_path); SetWindowTextW(address_edit,text);
    const struct { HWND edit; const wchar_t *key,*value; } edits[]={{on_edit,L"OnMs",L"500"},{off_edit,L"OffMs",L"500"},{limit_edit,L"LimitMs",L"60000"}};
    for(unsigned i=0;i<3;++i) { GetPrivateProfileStringW(L"Panel",edits[i].key,edits[i].value,text,64,settings_path); SetWindowTextW(edits[i].edit,text); }
}
static LRESULT CALLBACK procedure(HWND h,UINT msg,WPARAM w,LPARAM l) {
    switch(msg) {
    case WM_ACTIVATE:
        foreground=LOWORD(w)!=WA_INACTIVE;
        if(!foreground) { stop_output("focus_lost"); if(status_label) set_status(L"窗口失去焦点，已停止。点击播放可继续。"); }
        break;
    case WM_HSCROLL:
        if((HWND)l==low_slider || (HWND)l==high_slider)
            levels((uint16_t)SendMessageW(low_slider,TBM_GETPOS,0,0),(uint16_t)SendMessageW(high_slider,TBM_GETPOS,0,0));
        return 0;
    case WM_COMMAND: {
        unsigned id=LOWORD(w),notification=HIWORD(w);
        if((id==ID_LOW_EDIT || id==ID_HIGH_EDIT) && notification==EN_CHANGE && !controls_changing) {
            unsigned n;
            if(number(id==ID_LOW_EDIT?low_edit:high_edit,0,65535,&n)) {
                if(id==ID_LOW_EDIT) { state.low=(uint16_t)n; SendMessageW(low_slider,TBM_SETPOS,TRUE,n); }
                else { state.high=(uint16_t)n; SendMessageW(high_slider,TBM_SETPOS,TRUE,n); }
                wchar_t text[48]; swprintf(text,48,L"%ls %.1f%%",id==ID_LOW_EDIT?L"低频":L"高频",100.0*n/65535);
                SetWindowTextW(id==ID_LOW_EDIT?low_label:high_label,text);
            }
        } else if((id==ID_ON || id==ID_OFF || id==ID_LIMIT) && notification==EN_CHANGE && !controls_changing) {
            unsigned n;
            if(number(id==ID_ON?on_edit:id==ID_OFF?off_edit:limit_edit,id==ID_LIMIT?100:20,id==ID_LIMIT?600000:60000,&n)) {
                if(id==ID_ON) state.on_ms=n;
                else if(id==ID_OFF) state.off_ms=n;
                else state.limit_ms=n;
            }
        } else if(id==ID_ROUTE && notification==CBN_SELCHANGE) scan_targets(true);
        else if(id==ID_TARGET && notification==CBN_SELCHANGE) {
            stop_output("target_selection"); LRESULT i=SendMessageW(target_box,CB_GETCURSEL,0,0);
            if(i>=0 && (unsigned)i<target_count) chosen=targets[i];
        } else if(id==ID_MODE && notification==CBN_SELCHANGE) stop_output("mode_change");
        else if(notification==BN_CLICKED) {
            if(id==ID_PLAY) start_output();
            else if(id==ID_STOP) { stop_output("stop_button"); set_status(L"已停止。"); }
            else if(id==ID_ZERO) { levels(0,0); stop_output("zero_button"); }
            else if(id==ID_WEAK) levels(6554,6554);
            else if(id==ID_MID) levels(32768,32768);
            else if(id==ID_FULL) levels(65535,65535);
            else if(id==ID_CONNECT) start_capture();
            else if(id==ID_LOGS) ShellExecuteW(h,L"open",log_folder,NULL,NULL,SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_TIMER:
        if(input_initialized) METHOD(input,0x18,frame_fn)(input,true);
        if(now_ms()-last_scan>=500) { scan_targets(false); last_scan=now_ms(); }
        if(foreground) update_output();
        if(log_file) { EnterCriticalSection(&data_lock); fflush(log_file); LeaveCriticalSection(&data_lock); }
        return 0;
    case UPDATE_CAPTURE:
        EnterCriticalSection(&data_lock);
        SetWindowTextW(receive_label,capture_status); SetWindowTextW(output_label,capture_output);
        LeaveCriticalSection(&data_lock); return 0;
    case WM_CLOSE: stop_output("window_close"); save_settings(); DestroyWindow(h); return 0;
    case WM_DESTROY: KillTimer(h,1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h,msg,w,l);
}
static bool files_open(void) {
    wchar_t local[MAX_PATH];
    if(!GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH) || wcslen(local)>MAX_PATH-90) return false;
    swprintf(log_folder,MAX_PATH,L"%ls\\NSteamLink",local); CreateDirectoryW(log_folder,NULL);
    swprintf(log_folder,MAX_PATH,L"%ls\\NSteamLink\\HapticsPanel",local); CreateDirectoryW(log_folder,NULL);
    swprintf(settings_path,MAX_PATH,L"%ls\\settings.ini",log_folder);
    SYSTEMTIME time; GetLocalTime(&time);
    swprintf(log_path,MAX_PATH,L"%ls\\session-%04u%02u%02u-%02u%02u%02u-%lu.jsonl",log_folder,
        time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,(unsigned long)GetCurrentProcessId());
    log_file=_wfopen(log_path,L"wb");
    return log_file!=NULL;
}
#ifdef NSL_PANEL_TEST
static void panel_self_test(void);
#endif
int WINAPI wWinMain(HINSTANCE inst,HINSTANCE previous,PWSTR cmd,int show) {
    (void)previous; (void)cmd;
    instance=inst; origin_ms=now_ms(); InitializeCriticalSection(&data_lock);
    int argc; LPWSTR *argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    for(int i=1;argv && i<argc;++i) {
        if(!wcscmp(argv[i],L"--steam-root") && i+1<argc && wcslen(argv[i+1])<MAX_PATH) wcscpy(root_override,argv[++i]);
    }
    if(argv) LocalFree(argv);
    files_open();
    /* Preserve Steam's actual non-Steam-game context; never invent an AppID. */
    GetEnvironmentVariableW(L"SteamGameId",steam_context,96);
    log_event("\"stage\":\"begin\",\"launched_with_steam_context\":%s,\"physical_fidelity_verified\":false",steam_context[0]?"true":"false");
    steam_open();
#ifdef NSL_PANEL_TEST
    wchar_t xfile[MAX_PATH]; swprintf(xfile,MAX_PATH,L"%ls\\nsl-xinput-mock.dll",root_override);
    xmodule=LoadLibraryExW(xfile,NULL,LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    xmodule=LoadLibraryExW(L"xinput1_4.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!xmodule) xmodule=LoadLibraryExW(L"xinput9_1_0.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
#endif
    if(xmodule) { xget=(get_state_fn)(uintptr_t)GetProcAddress(xmodule,"XInputGetState"); xset=(set_state_fn)(uintptr_t)GetProcAddress(xmodule,"XInputSetState"); }
    WSADATA ws; winsock_ready=WSAStartup(MAKEWORD(2,2),&ws)==0;
    INITCOMMONCONTROLSEX common={sizeof(common),ICC_BAR_CLASSES|ICC_STANDARD_CLASSES}; InitCommonControlsEx(&common);
    font=CreateFontW(-18,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    WNDCLASSW cls={0}; cls.lpfnWndProc=procedure; cls.hInstance=instance; cls.lpszClassName=L"NSteamLinkHapticsPanel";
    cls.hCursor=LoadCursorW(NULL,IDC_ARROW); cls.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); RegisterClassW(&cls);
    window=CreateWindowExW(0,cls.lpszClassName,L"NSteamLink · Steam 实时震动控制面板",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,750,670,NULL,NULL,instance,NULL);
    if(window) {
        create_controls(); scan_targets(true); ShowWindow(window,show); UpdateWindow(window);
        SetTimer(window,1,16,NULL); start_capture();
#ifdef NSL_PANEL_TEST
        panel_self_test();
#endif
        MSG message;
        while(GetMessageW(&message,NULL,0,0)>0) {
            if(message.message==WM_KEYDOWN && (message.wParam==VK_SPACE || message.wParam==VK_ESCAPE)) {
                stop_output("keyboard_stop"); set_status(L"已停止。"); continue;
            }
            if(!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
    }
    stop_output("exit"); stop_capture();
    if(input_initialized) METHOD(input,8,shutdown_fn)(input);
    if(steam_user) METHOD(client,0x20,release_user_fn)(client,steam_pipe,steam_user);
    if(steam_pipe) METHOD(client,8,release_pipe_fn)(client,steam_pipe);
    if(smodule) FreeLibrary(smodule);
    if(xmodule) FreeLibrary(xmodule);
    if(font) DeleteObject(font);
    if(winsock_ready) WSACleanup();
    log_event("\"stage\":\"end\",\"capture_joined\":true,\"stopped\":true");
    if(log_file) fclose(log_file);
    DeleteCriticalSection(&data_lock);
    return 0;
}
#ifdef NSL_PANEL_TEST
#include "../app/tests/haptics_panel_ui_test.h"
#endif
