/* Included only by the test build of the actual Win32 program. Uses its real
 * controls, WndProc, timers, loader, sockets and cleanup; only vendor DLLs mock. */
#include <assert.h>
#undef assert
#define assert(condition) do { if (!(condition)) { \
    log_event("\"stage\":\"ui_test_failed\",\"line\":%d",__LINE__); \
    if(log_file) { fflush(log_file); } ExitProcess(3); \
} } while(0)
typedef uint64_t (*mock_read_fn)(unsigned);
typedef void (*mock_set_fn)(unsigned,uint64_t);
static void click(unsigned id) { SendMessageW(window,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),0); }
static void choose_route(unsigned id) {
    LRESULT count=SendMessageW(route_box,CB_GETCOUNT,0,0), selected=CB_ERR;
    for(LRESULT i=0;i<count;++i) if(SendMessageW(route_box,CB_GETITEMDATA,i,0)==id) selected=i;
    assert(selected!=CB_ERR);SendMessageW(route_box,CB_SETCURSEL,selected,0);
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(ID_ROUTE,CBN_SELCHANGE),0);
}
static void tick(void) { SendMessageW(window,WM_TIMER,1,0); }
static void panel_self_test(void) {
    wchar_t scenario[64]; GetEnvironmentVariableW(L"NSL_PANEL_CASE",scenario,64);
    mock_read_fn xr=(mock_read_fn)(uintptr_t)GetProcAddress(xmodule,"NslMockRead");
    mock_read_fn sr=(mock_read_fn)(uintptr_t)GetProcAddress(smodule,"NslMockRead");
    mock_set_fn xs=(mock_set_fn)(uintptr_t)GetProcAddress(xmodule,"NslMockSet");
    mock_set_fn ss=(mock_set_fn)(uintptr_t)GetProcAddress(smodule,"NslMockSet");
    if(!wcsncmp(scenario,L"desktop",7)) {
        assert(desktop_mode && desktop_ready && steam_user && target_count==1 && chosen.route==2);
        assert(SendMessageW(route_box,CB_GETCOUNT,0,0)==2);
        stop_capture();levels(789,23456);click(ID_PLAY);tick();assert(sr(0)==789 && sr(1)==23456);
        if(!wcscmp(scenario,L"desktop_native") || !wcscmp(scenario,L"desktop_native_close") || !wcscmp(scenario,L"desktop_native_train")) {
            unsigned ordinary_on=state.on_ms,ordinary_off=state.off_ms;
            choose_route(3);assert(chosen.route==3 && native_controls && !state.active && !sr(0));
            click(ID_HAPTICS_ON);assert(sr(10)==1);
            SetWindowTextW(on_edit,L"250");SetWindowTextW(off_edit,L"750");SetWindowTextW(limit_edit,L"4");
            bool train=wcscmp(scenario,L"desktop_native")!=0;
            click(train?ID_NATIVE_TRAIN:ID_PLAY);assert(native_active && sr(5)==2 && sr(6)==0 && sr(7)==250 && sr(8)==750 && sr(9)==4);
            if(!wcscmp(scenario,L"desktop_native_close")) {log_event("\"stage\":\"ui_test_passed\"");PostMessageW(window,WM_CLOSE,0,0);return;}
            if(train) {
                assert(native_train_until);native_train_next=1;tick();assert(sr(5)==4 && sr(6)==0);
                native_train_until=1;tick();assert(!native_active && !native_train_until && sr(5)==6 && sr(6)==2 && !sr(7));
            } else {click(ID_STOP);assert(!native_active && sr(5)==4 && sr(6)==2 && !sr(7));}
            click(ID_HAPTICS_OFF);assert(sr(10)==0);
            choose_route(2);assert(!native_controls && chosen.route==2 && state.on_ms==ordinary_on && state.off_ms==ordinary_off);
        } else if(!wcscmp(scenario,L"desktop_profile_log")) {
            wchar_t folder[MAX_PATH],path[MAX_PATH];swprintf(folder,MAX_PATH,L"%ls\\logs",steam_root);CreateDirectoryW(folder,NULL);
            swprintf(path,MAX_PATH,L"%ls\\controller.txt",folder);FILE *f=_wfopen(path,L"wb");assert(f);
            fputs("[17:14:01] Queueing activation for controller: 0 app: 769\n[17:14:08] Queueing activation for controller: 0 app: 413080\n",f);fclose(f);
            ss(5,413080);state.last_send_ms=0;tick();assert(sr(0)==789);
            click(ID_STOP);DeleteFileW(path);ss(5,769);
            click(ID_IDENTIFY);assert(sr(11)==1);
        } else if(!wcscmp(scenario,L"desktop_capture_timeout")) {
            SetWindowTextW(address_edit,L"127.0.0.1");start_capture();Sleep(1400);stop_capture();
            assert(desktop_ready && target_count==1 && !capture_packets);
        } else if(!wcscmp(scenario,L"desktop_export")) {
            click(ID_LOGS);
        } else if(!wcscmp(scenario,L"desktop_app_change")) {
            ss(4,413080);state.last_send_ms=0;tick();assert(sr(0)==789);
        } else if(!wcscmp(scenario,L"desktop_disconnect")) {
            ss(0,0);last_scan=0;tick();assert(!state.active && !target_count);
            ss(0,1);last_scan=0;tick();assert(target_count==1 && !state.active);
        } else if(!wcscmp(scenario,L"desktop_handle_change")) {
            ss(2,0x9999);last_scan=0;tick();assert(!state.active && target_count==1 && chosen.id==0x9999);
        } else assert(!wcscmp(scenario,L"desktop"));
        click(ID_STOP);assert(!sr(0) && !sr(1));
        log_event("\"stage\":\"ui_test_passed\"");PostMessageW(window,WM_CLOSE,0,0);return;
    }
    assert(xr && sr && xs && ss && target_count==1 && !state.active);
    stop_capture();
    SendMessageW(window,WM_ACTIVATE,WA_ACTIVE,0);
    SetWindowTextW(on_edit,L"100"); SetWindowTextW(off_edit,L"100"); SetWindowTextW(limit_edit,L"10000");
    SendMessageW(mode_box,CB_SETCURSEL,0,0);
    levels(123,45678);click(ID_PLAY);tick();
    assert(state.active && xr(0)==123 && xr(1)==45678);
    SetWindowTextW(low_edit,L"65535");SetWindowTextW(high_edit,L"1");tick();
    assert(xr(0)==65535 && xr(1)==1);
    if(!wcscmp(scenario,L"focus")) {
        SendMessageW(window,WM_ACTIVATE,WA_INACTIVE,0);
        tick();assert(state.active && xr(0)==65535 && xr(1)==1);
        SendMessageW(window,WM_ACTIVATE,WA_ACTIVE,0);click(ID_STOP);assert(!state.active && !xr(0));
    } else if(!wcscmp(scenario,L"disconnect")) {
        xs(0,0);last_scan=0;tick();assert(!state.active && !target_count && !xr(0));
        xs(0,1);last_scan=0;tick();assert(target_count==1 && !state.active && !xr(0));
    } else if(!wcscmp(scenario,L"invalid")) {
        click(ID_STOP);SetWindowTextW(low_edit,L"65536");click(ID_PLAY);assert(!state.active);
        levels(1,2);SetWindowTextW(on_edit,L"0");click(ID_PLAY);assert(!state.active);
        SetWindowTextW(on_edit,L"100");SetWindowTextW(limit_edit,L"bad");click(ID_PLAY);assert(!state.active);
    } else if(!wcscmp(scenario,L"init_retry") || !wcscmp(scenario,L"user_retry")) {
        assert(!input_initialized && steam_init_step==5);
        assert(steam_pipe==(!wcscmp(scenario,L"user_retry")?7:0) && !steam_user);
        choose_route(1);assert(input_initialized && target_count==1 && steam_pipe==7 && steam_user==11);
        levels(789,23456);click(ID_PLAY);tick();assert(sr(0)==789 && sr(1)==23456);
    } else if(!wcscmp(scenario,L"empty_public")) {
        ss(1,1);choose_route(1);assert(target_count==1);
        levels(789,23456);click(ID_PLAY);tick();assert(sr(0)==789 && sr(1)==23456);
    } else if(!wcscmp(scenario,L"steam")) {
        choose_route(1);assert(!xr(0) && !xr(1) && !state.active && target_count==1);
        levels(789,23456);click(ID_PLAY);tick();assert(sr(0)==789 && sr(1)==23456);
        ss(2,0x9999);last_scan=0;tick();assert(!state.active && !sr(0));
        click(ID_PLAY);tick();assert(state.active && sr(0)==789);
        ss(1,1);xs(0,0);last_scan=0;tick();assert(!state.active && !target_count && !sr(0));
        ss(1,0);xs(0,1);last_scan=0;tick();assert(!state.active && target_count==1);
    } else if(!wcscmp(scenario,L"pulse")) {
        click(ID_STOP);SendMessageW(mode_box,CB_SETCURSEL,2,0);click(ID_PLAY);tick();assert(xr(0));
        state.started_ms=now_ms()-100;tick();assert(!xr(0) && !xr(1));
        state.started_ms=now_ms()-200;tick();assert(xr(0)==65535);
        state.started_ms=now_ms()-10000;tick();assert(!state.active && !xr(0));
    } else if(!wcscmp(scenario,L"capture") || !wcscmp(scenario,L"frequency")) {
        SetWindowTextW(address_edit,L"127.0.0.1");start_capture();
        uint64_t deadline=now_ms()+3000;
        bool got=false;
        while(now_ms()<deadline) {
            EnterCriticalSection(&data_lock);got=capture_have_levels && capture_low==16384 && capture_high==32768 && capture_output[0];LeaveCriticalSection(&data_lock);
            if(got) break;
            Sleep(10);
        }
        assert(got);stop_capture();assert(!capture_thread && !capture_stop);
        if(!wcscmp(scenario,L"frequency")) {
            start_capture();
            SetWindowTextW(low_hz_edit,L"160");SetWindowTextW(high_hz_edit,L"640");SetWindowTextW(peak_edit,L"0.5571934");
            SendMessageW(routing_box,CB_SETCURSEL,3,0);
            click(ID_APPLY);assert(!state.active && !xr(0));
            deadline=now_ms()+3000;bool applied=false;
            while(now_ms()<deadline) {
                MSG m;
                while(PeekMessageW(&m,window,UPDATE_PROFILE,UPDATE_PROFILE,PM_REMOVE)) DispatchMessageW(&m);
                if(state.active) {applied=true;break;} Sleep(5);
            }
            assert(applied);tick();assert(xr(0)==65535 && xr(1)==1);
            assert(profile_received.high==640 && profile_received.routing==3);
            SetWindowTextW(low_hz_edit,L"320");click(ID_320);
            deadline=now_ms()+3000;applied=false;
            while(now_ms()<deadline) {
                MSG m;while(PeekMessageW(&m,window,UPDATE_PROFILE,UPDATE_PROFILE,PM_REMOVE)) DispatchMessageW(&m);
                if(state.active) {applied=true;break;} Sleep(5);
            }
            assert(applied && profile_received.low==320 && profile_received.high==320);
            tick();assert(xr(0)==65535 && xr(1)==1);
        }
    } else assert(!wcscmp(scenario,L"close"));
    log_event("\"stage\":\"ui_test_passed\"");
    PostMessageW(window,WM_CLOSE,0,0);
}
