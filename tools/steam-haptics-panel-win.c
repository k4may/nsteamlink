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
#include <math.h>
#include "haptics-panel-state.h"

#define METHOD(o, slot, type) ((type)((*(void ***)(o))[(slot) / 8]))
#define UPDATE_CAPTURE (WM_APP + 1)
#define UPDATE_PROFILE (WM_APP + 2)
enum { ID_ROUTE=100, ID_TARGET, ID_LOW, ID_HIGH, ID_LOW_EDIT, ID_HIGH_EDIT,
       ID_MODE, ID_ON, ID_OFF, ID_LIMIT, ID_PLAY, ID_STOP, ID_ADDRESS, ID_CONNECT,
       ID_LOGS, ID_ZERO, ID_WEAK, ID_MID, ID_FULL, ID_LOW_HZ, ID_HIGH_HZ,
       ID_PEAK, ID_ROUTING, ID_APPLY, ID_320, ID_640 };
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
typedef struct profile { float low, high, peak; unsigned routing; } profile;
static HWND window, low_slider, high_slider, low_edit, high_edit, route_box, target_box,
    mode_box, on_edit, off_edit, limit_edit, address_edit, status_label, receive_label, output_label,
    low_label, high_label, low_hz_edit, high_hz_edit, peak_edit, routing_box, profile_label;
static HFONT font;
static HINSTANCE instance;
static HMODULE xmodule, smodule;
static get_state_fn xget;
static set_state_fn xset;
static void *client, *input;
static int steam_pipe, steam_user;
static unsigned steam_init_step;
static bool input_initialized, controls_changing, winsock_ready;
static nsl_panel_state state = {.on_ms=500, .off_ms=500};
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
static profile profile_pending, profile_received;
static bool profile_queued, profile_resume, profile_success;
static unsigned profile_generation, profile_reply_generation;

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
    swprintf(text,48,L"A %.1f%%",100.0*low/65535); SetWindowTextW(low_label,text);
    swprintf(text,48,L"B %.1f%%",100.0*high/65535); SetWindowTextW(high_label,text);
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
    profile_resume = false;
    nsl_panel_stop(&state);
    if (target_count && (was_active || state.sent)) (void)send_levels(chosen, 0, 0, reason);
    state.sent_low = state.sent_high = 0; state.sent = true; state.last_send_ms = now_ms();
}
static void steam_open(void) {
    if(input_initialized) return;
    /* A launch-time connection failure is not permanent. Release any partial
     * pipe/user before an explicit retry; keep the loaded DLL reference. */
    if(steam_user && client) METHOD(client,0x20,release_user_fn)(client,steam_pipe,steam_user);
    if(steam_pipe && client) METHOD(client,8,release_pipe_fn)(client,steam_pipe);
    steam_user=steam_pipe=0;input=NULL;
    steam_init_step=1;
    wchar_t root[MAX_PATH], path[MAX_PATH];
    if (root_override[0]) wcscpy(root, root_override);
    else {
        DWORD size = sizeof(root);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                        RRF_RT_REG_SZ, NULL, root, &size) != ERROR_SUCCESS) return;
    }
    steam_init_step=2;
    if (wcslen(root) > MAX_PATH - 32) return;
    swprintf(path, MAX_PATH, L"%ls\\steamclient64.dll", root);
    steam_init_step=3;
    if(!smodule) smodule = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!smodule) return;
    steam_init_step=4;create_fn create = (create_fn)(uintptr_t)GetProcAddress(smodule, "CreateInterface");
    if (!create || !(client = create("SteamClient023", NULL))) return;
    steam_init_step=5;steam_pipe = METHOD(client, 0, pipe_fn)(client);
    if (!steam_pipe || !(steam_user = METHOD(client, 0x10, user_fn)(client, steam_pipe))) return;
    steam_init_step=6;input = METHOD(client, 0x60, interface_fn)(client, steam_user, steam_pipe, "SteamInput006");
    if (input) {steam_init_step=7;input_initialized = METHOD(input, 0, init_fn)(input, true);}
    if(input_initialized) steam_init_step=0;
}
static void steam_log_initialization(void) {
    wchar_t appid[32]={0};GetEnvironmentVariableW(L"SteamAppId",appid,32);
    log_event("\"stage\":\"steam_input_init\",\"ready\":%s,\"failure_step\":%u,\"pipe\":%d,\"user\":%d,\"appid\":%llu,\"gameid\":%llu,\"pid\":%lu",
        input_initialized?"true":"false",steam_init_step,steam_pipe,steam_user,
        (unsigned long long)wcstoull(appid,NULL,10),(unsigned long long)wcstoull(steam_context,NULL,10),
        (unsigned long)GetCurrentProcessId());
}
static void scan_targets(bool force) {
    target found[16]; unsigned count = 0;
    unsigned route = (unsigned)SendMessageW(route_box, CB_GETCURSEL, 0, 0);
    if(force && route==1 && !input_initialized) {steam_open();steam_log_initialization();}
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
        unsigned listed = count;
        /* Public bridge for legacy/gamepad configurations: an empty native
         * list is not proof that the emulated gamepad has no Steam handle. */
        for (DWORD i=0; xget && i<4 && count<16; ++i) {
            XINPUT_STATE value;
            if(xget(i,&value)!=ERROR_SUCCESS) continue;
            uint64_t handle=METHOD(input,0x130,index_handle_fn)(input,(int)i);
            bool duplicate=!handle;
            for(unsigned j=0;j<count;++j) duplicate|=found[j].id==handle;
            if(!duplicate) found[count++]=(target){handle,1};
        }
        if(force || count!=target_count)
            log_event("\"stage\":\"steam_enumeration\",\"public_count\":%d,\"mapped_extra\":%u",n,count-listed);
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
    else if(route==1 && !input_initialized) {
        const wchar_t *reasons[]={L"未就绪",L"未找到Steam安装路径",L"Steam路径无效",L"加载Steam客户端DLL失败",
            L"Steam客户端接口未返回",L"连接Steam用户失败",L"Steam Input接口未返回",L"Steam Input初始化失败"};
        set_status(L"%ls；具体状态已记录。",reasons[steam_init_step<=7?steam_init_step:0]);
    }
    else if(route==1) set_status(L"Steam Input公开列表及XInput关联查询都为空；已记录具体枚举结果。");
    else set_status(L"未发现手柄：请连接 Switch，再从 Steam 库启动本工具。可切换发送方式检查。");
    log_event("\"stage\":\"controllers\",\"route\":\"%s\",\"count\":%u", route ? "steaminput" : "xinput", count);
}
static void start_output(void) {
    unsigned on, off, limit, low, high;
    if (!number(low_edit,0,65535,&low) || !number(high_edit,0,65535,&high)) {
        set_status(L"力度需为 0–65535 的整数。"); return;
    }
    if (!number(on_edit, 1, UINT32_MAX, &on) || !number(off_edit, 0, UINT32_MAX, &off) ||
        !number(limit_edit, 0, UINT32_MAX, &limit)) {
        set_status(L"时间需为整数；震动时间至少1ms，间隔可为0，上限为0表示不限。"); return;
    }
    if(!input_initialized && SendMessageW(route_box,CB_GETCURSEL,0,0)==1) scan_targets(true);
    if (!target_count) { set_status(L"没有可用手柄。初始化和枚举结果已记录，可再次点击播放重试连接。"); return; }
    stop_output("restart");
    state.low=(uint16_t)low; state.high=(uint16_t)high;
    state.on_ms = on; state.off_ms = off; state.limit_ms = limit;
    state.mode = (nsl_panel_mode)SendMessageW(mode_box, CB_GETCURSEL, 0, 0);
    if (nsl_panel_start(&state, now_ms()))
        set_status(L"播放中：可实时调整力度和频率。停止 / 空格 / Esc / 关闭会归零。");
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
static bool json_float(const char *text,const char *key,float *out) {
    char pattern[48],*end;snprintf(pattern,sizeof(pattern),"\"%s\":",key);
    const char *p=strstr(text,pattern);if(!p) return false;p+=strlen(pattern);
    *out=strtof(p,&end);return end!=p && isfinite(*out);
}
static bool float_edit(HWND edit,float min,float max,float *out) {
    wchar_t text[48],*end;GetWindowTextW(edit,text,48);
    *out=wcstof(text,&end);return end!=text && !*end && isfinite(*out) && *out>=min && *out<=max;
}
static void profile_exchange(const struct sockaddr_in *peer,const profile *wanted,unsigned generation) {
    SOCKET request=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    DWORD timeout=300;char command[128],reply[1024];int n=-1;
    if(wanted) snprintf(command,sizeof(command),"rumble-config %.9g %.9g %.9g %u",wanted->low,wanted->high,wanted->peak,wanted->routing);
    else strcpy(command,"rumble-config");
    if(request!=INVALID_SOCKET) {
        setsockopt(request,SOL_SOCKET,SO_RCVTIMEO,(const char *)&timeout,sizeof(timeout));
        setsockopt(request,SOL_SOCKET,SO_SNDTIMEO,(const char *)&timeout,sizeof(timeout));
        if(!connect(request,(const struct sockaddr *)peer,sizeof(*peer)) &&
           send(request,command,(int)strlen(command),0)==(int)strlen(command))
            n=recv(request,reply,sizeof(reply)-1,0);
        closesocket(request);
    }
    reply[n>0?n:0]=0;
    profile received={0};bool has_routing=false;
    received.routing=(unsigned)json_uint(reply,"routing",&has_routing);
    bool good=n>0 && strstr(reply,"\"stage\":\"rumble_config\"") && strstr(reply,"\"ok\":true") &&
        json_float(reply,"low_hz",&received.low) && json_float(reply,"high_hz",&received.high) &&
        json_float(reply,"peak",&received.peak) && has_routing && received.routing<=3;
    if(good && wanted) good=fabsf(wanted->low-received.low)<.001f && fabsf(wanted->high-received.high)<.001f &&
        fabsf(wanted->peak-received.peak)<.000001f && wanted->routing==received.routing;
    log_event("\"stage\":\"profile_result\",\"generation\":%u,\"ok\":%s,\"low_hz\":%.9g,\"high_hz\":%.9g,\"peak\":%.9g,\"routing\":%u,\"frequency_transport\":\"diagnostic_mapping\"",
        generation,good?"true":"false",received.low,received.high,received.peak,received.routing);
    EnterCriticalSection(&data_lock);profile_received=received;profile_success=good;
    profile_reply_generation=generation;LeaveCriticalSection(&data_lock);
    PostMessageW(window,UPDATE_PROFILE,generation,good);
}
static void apply_profile(void) {
    profile wanted;
    if(!float_edit(low_hz_edit,41,626,&wanted.low) || !float_edit(high_hz_edit,82,1253,&wanted.high) ||
       !float_edit(peak_edit,0,1,&wanted.peak)) {
        set_status(L"频率范围：A 41–626Hz，B 82–1253Hz；幅度上限0–1。两频率可相同。");return;
    }
    wanted.routing=(unsigned)SendMessageW(routing_box,CB_GETCURSEL,0,0);
    if(!capture_thread || wanted.routing>3) { set_status(L"请先连接Switch自动采集，再应用输出频率。");return; }
    bool resume=state.active;stop_output("frequency_change");profile_resume=resume;
    EnterCriticalSection(&data_lock);profile_pending=wanted;profile_queued=true;++profile_generation;LeaveCriticalSection(&data_lock);
    set_status(L"正在应用Switch输出频率，力度保持不变…");
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
            swprintf(capture_output, 512, L"左侧：低 %.3f@%.0fHz / 高 %.3f@%.0fHz\n右侧：低 %.3f@%.0fHz / 高 %.3f@%.0fHz  / IPC=%llu",
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
    if(connected) profile_exchange(&peer,NULL,0);
    if (connected) while (WaitForSingleObject(capture_stop, 20) == WAIT_TIMEOUT) {
        EnterCriticalSection(&data_lock);
        bool configure=profile_queued;profile wanted=profile_pending;unsigned generation=profile_generation;
        profile_queued=false;LeaveCriticalSection(&data_lock);
        if(configure) { profile_exchange(&peer,&wanted,generation);continue; }
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
    EnterCriticalSection(&data_lock);profile_queued=false;++profile_generation;LeaveCriticalSection(&data_lock);
    profile_resume=false;
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
    const struct {HWND edit;const wchar_t *key;} edits[]={{low_hz_edit,L"LowHz"},{high_hz_edit,L"HighHz"},{peak_edit,L"Peak"}};
    for(unsigned i=0;i<3;++i) {GetWindowTextW(edits[i].edit,value,64);WritePrivateProfileStringW(L"Panel",edits[i].key,value,settings_path);}
    swprintf(value,64,L"%ld",(long)SendMessageW(routing_box,CB_GETCURSEL,0,0));WritePrivateProfileStringW(L"Panel",L"Routing",value,settings_path);
}
static void create_controls(void) {
    label(L"A/B力度经Steam发送；输出Hz单独配置Switch映射，可固定力度比较频率。",20,15,700);
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
    label(L"A/B是游戏两路力度，0–65535；不是左右震动器各自的频率限制。",20,198,690);
    control(L"BUTTON",L"归零",WS_TABSTOP,20,230,110,30,ID_ZERO);
    control(L"BUTTON",L"弱震 10%",WS_TABSTOP,145,230,120,30,ID_WEAK);
    control(L"BUTTON",L"中等 50%",WS_TABSTOP,280,230,120,30,ID_MID);
    control(L"BUTTON",L"满量程",WS_TABSTOP,415,230,110,30,ID_FULL);
    label(L"输出A Hz",20,278,90);low_hz_edit=control(L"EDIT",L"160",WS_BORDER|WS_TABSTOP,110,272,85,28,ID_LOW_HZ);
    label(L"输出B Hz",210,278,90);high_hz_edit=control(L"EDIT",L"320",WS_BORDER|WS_TABSTOP,300,272,85,28,ID_HIGH_HZ);
    label(L"幅度上限",395,278,85);peak_edit=control(L"EDIT",L"0.5571934",WS_BORDER|WS_TABSTOP,480,272,100,28,ID_PEAK);
    control(L"BUTTON",L"应用频率",WS_TABSTOP,590,270,115,32,ID_APPLY);
    label(L"输出位置",20,318,90);routing_box=control(WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_TABSTOP,110,312,280,150,ID_ROUTING);
    const wchar_t *routes[]={L"默认映射（由手柄形态决定）",L"左右都播放A+B",L"仅左侧播放A+B",L"仅右侧播放A+B"};
    for(unsigned i=0;i<4;++i) SendMessageW(routing_box,CB_ADDSTRING,0,(LPARAM)routes[i]);
    unsigned routing=GetPrivateProfileIntW(L"Panel",L"Routing",0,settings_path);
    SendMessageW(routing_box,CB_SETCURSEL,routing<=3?routing:0,0);
    control(L"BUTTON",L"B → 320Hz",WS_TABSTOP,405,310,135,32,ID_320);
    control(L"BUTTON",L"B → 640Hz",WS_TABSTOP,555,310,150,32,ID_640);
    profile_label=control(L"STATIC",L"Switch输出映射：连接后自动读回；应用频率时保持A/B力度不变。",0,20,350,685,25,0);
    label(L"播放方式",20,390,90);
    mode_box=control(WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_TABSTOP,110,384,185,130,ID_MODE);
    SendMessageW(mode_box,CB_ADDSTRING,0,(LPARAM)L"持续");
    SendMessageW(mode_box,CB_ADDSTRING,0,(LPARAM)L"单次脉冲");
    SendMessageW(mode_box,CB_ADDSTRING,0,(LPARAM)L"重复脉冲");
    unsigned mode=GetPrivateProfileIntW(L"Panel",L"Mode",0,settings_path);
    SendMessageW(mode_box,CB_SETCURSEL,mode<=2?mode:0,0);
    label(L"震动 ms",310,390,80); on_edit=control(L"EDIT",L"500",WS_BORDER|ES_NUMBER|WS_TABSTOP,385,384,80,28,ID_ON);
    label(L"间隔 ms",480,390,80); off_edit=control(L"EDIT",L"500",WS_BORDER|ES_NUMBER|WS_TABSTOP,555,384,80,28,ID_OFF);
    label(L"上限ms (0不限)",20,432,145); limit_edit=control(L"EDIT",L"0",WS_BORDER|ES_NUMBER|WS_TABSTOP,165,426,90,28,ID_LIMIT);
    control(L"BUTTON",L"播放（实时调节）",WS_TABSTOP,280,422,190,38,ID_PLAY);
    control(L"BUTTON",L"停止 / 空格",WS_TABSTOP,490,422,190,38,ID_STOP);
    status_label=control(L"STATIC",L"等待手柄连接。",0,20,475,685,35,0);
    label(L"Switch 地址",20,525,110); address_edit=control(L"EDIT",L"10.10.10.111",WS_BORDER|WS_TABSTOP,135,518,190,28,ID_ADDRESS);
    control(L"BUTTON",L"连接自动采集",WS_TABSTOP,345,516,180,32,ID_CONNECT);
    control(L"BUTTON",L"打开记录文件夹",WS_TABSTOP,540,516,165,32,ID_LOGS);
    receive_label=control(L"STATIC",L"自动记录 Switch 实际收到的命令。",0,20,560,685,32,0);
    output_label=control(L"STATIC",L"本机输出参数将在这里显示。",0,20,600,685,65,0);
    label(L"可切换窗口继续测试。停止 / 空格 / Esc / 关闭归零；参数和记录自动保存。",20,678,700);
    uint16_t low=(uint16_t)GetPrivateProfileIntW(L"Panel",L"Low",6554,settings_path);
    uint16_t high=(uint16_t)GetPrivateProfileIntW(L"Panel",L"High",6554,settings_path);
    levels(low,high);
    wchar_t text[64]; GetPrivateProfileStringW(L"Panel",L"SwitchIP",L"10.10.10.111",text,64,settings_path); SetWindowTextW(address_edit,text);
    const struct { HWND edit; const wchar_t *key,*value; } edits[]={{on_edit,L"OnMs",L"500"},{off_edit,L"OffMs",L"500"},{limit_edit,L"LimitMs",L"0"},
        {low_hz_edit,L"LowHz",L"160"},{high_hz_edit,L"HighHz",L"320"},{peak_edit,L"Peak",L"0.5571934"}};
    for(unsigned i=0;i<6;++i) { GetPrivateProfileStringW(L"Panel",edits[i].key,edits[i].value,text,64,settings_path); SetWindowTextW(edits[i].edit,text); }
}
static LRESULT CALLBACK procedure(HWND h,UINT msg,WPARAM w,LPARAM l) {
    switch(msg) {
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
                wchar_t text[48]; swprintf(text,48,L"%ls %.1f%%",id==ID_LOW_EDIT?L"A":L"B",100.0*n/65535);
                SetWindowTextW(id==ID_LOW_EDIT?low_label:high_label,text);
            }
        } else if((id==ID_ON || id==ID_OFF || id==ID_LIMIT) && notification==EN_CHANGE && !controls_changing) {
            unsigned n;
            if(number(id==ID_ON?on_edit:id==ID_OFF?off_edit:limit_edit,id==ID_ON?1:0,UINT32_MAX,&n)) {
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
            else if(id==ID_APPLY) apply_profile();
            else if(id==ID_320 || id==ID_640) {SetWindowTextW(high_hz_edit,id==ID_320?L"320":L"640");apply_profile();}
            else if(id==ID_CONNECT) start_capture();
            else if(id==ID_LOGS) ShellExecuteW(h,L"open",log_folder,NULL,NULL,SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_TIMER:
        if(input_initialized) METHOD(input,0x18,frame_fn)(input,true);
        if(now_ms()-last_scan>=500) { scan_targets(false); last_scan=now_ms(); }
        update_output();
        if(log_file) { EnterCriticalSection(&data_lock); fflush(log_file); LeaveCriticalSection(&data_lock); }
        return 0;
    case UPDATE_CAPTURE:
        EnterCriticalSection(&data_lock);
        SetWindowTextW(receive_label,capture_status); SetWindowTextW(output_label,capture_output);
        LeaveCriticalSection(&data_lock); return 0;
    case UPDATE_PROFILE: {
        EnterCriticalSection(&data_lock);
        bool current=profile_reply_generation==(unsigned)w && ((unsigned)w==0 || profile_generation==(unsigned)w);
        profile p=profile_received;bool good=profile_success;
        LeaveCriticalSection(&data_lock);
        if(!current) return 0;
        if(good) {
            const wchar_t *routes[]={L"默认映射",L"双侧A+B",L"仅左A+B",L"仅右A+B"};
            wchar_t text[256];swprintf(text,256,L"Switch普通映射：A %.1fHz / B %.1fHz，上限%.4f，%ls",p.low,p.high,p.peak,routes[p.routing]);
            SetWindowTextW(profile_label,text);
            if(w) {bool resume=profile_resume;profile_resume=false;if(resume) start_output();}
        } else {
            SetWindowTextW(profile_label,L"尚未取得频率设置回执；请使用支持实时配置的正常客户端。");
            if(w) {profile_resume=false;set_status(L"频率设置未确认，保持停止；重连采集后再应用。");}
        }
        return 0;
    }
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
    steam_log_initialization();
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
        CW_USEDEFAULT,CW_USEDEFAULT,750,755,NULL,NULL,instance,NULL);
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
