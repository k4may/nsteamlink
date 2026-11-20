/* Included by the matrix fixture after its independent trace has been saved.
 * Each scene owns a fresh joinable backend lifetime, with no logs while active.
 * The matrix remains separately verifiable; demo records use demo-* stages. */
#include "haptic_experience.h"
static bool experience_demo_active;

static unsigned experience_buttons(void) {
#ifdef NSL_HAPTICS_PROBE_HOST
    return 0;
#else
    return (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START) ? 1u : 0u) |
        (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP) ? 2u : 0u) |
        (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT) ? 4u : 0u) |
        (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ? 8u : 0u) |
        (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN) ? 16u : 0u);
#endif
}
static void experience_screen(unsigned scene, bool playing) {
#ifndef NSL_HAPTICS_PROBE_HOST
    consoleClear();
    printf("NSteamLink - LOCAL HAPTIC EXPERIENCE\n\n");
    printf("%u / %u : %s\n\n%s\n\n", scene + 1, EXPERIENCE_CASES,
        experience_names[scene], experience_hints[scene]);
    printf("%s\n\n", playing ? "Playing for four seconds..." : "Stopped. Choose a scene or replay.");
    printf("D-pad up: replay             D-pad down: play all\n");
    printf("D-pad left/right: select      + : stop and exit\n\n");
    printf("First run plays all seven scenes automatically.\n");
    printf("Auto-exit after five minutes. No Steam or game needed.\n\n");
    printf("These are authored local effects, not game feedback.\n");
    printf("Frequency and amplitude are independent; physical feel may differ.\n");
    consoleUpdate(NULL);
#else
    (void)scene; (void)playing;
#endif
}
static bool run_haptic_experience(IHS_HIDManagedDevice *device, IHS_Session *session,
                                  HidVibrationDeviceHandle handles[2], unsigned devices) {
    uint64_t deadline = now_ns() + UINT64_C(300000000000);
    unsigned scene = 0, previous = 0, completed = 0;
    bool autoplay = true, play = true, exiting = false, ok = permitted;
    experience_demo_active = true;
    while (ok && !exiting && now_ns() < deadline && appletMainLoop()) {
        experience_screen(scene, play);
        if (play) {
            if (!sl_rumble_init()) { ok = false; break; }
            uint64_t begin = now_ns();
            unsigned next_ms = 0, commands = 0, skipped = 0, action = 0;
            bool write_ok = true;
            while (appletMainLoop() && now_ns() < deadline) {
                SDL_PumpEvents();
                unsigned buttons = experience_buttons(), down = buttons & ~previous;
                previous = buttons;
                if (down) { action = down; break; }
                unsigned ms = (unsigned)((now_ns() - begin) / UINT64_C(1000000));
                if (ms >= EXPERIENCE_MS) break;
                if (ms >= next_ms) {
                    unsigned step = ms / EXPERIENCE_STEP_MS * EXPERIENCE_STEP_MS;
                    skipped += (step - next_ms) / EXPERIENCE_STEP_MS;
                    uint8_t command[69];
                    size_t size = experience_command(scene, step, command);
                    write_ok &= matrix_apply(device, session, command, size);
                    ++commands;
                    next_ms = step + EXPERIENCE_STEP_MS;
                    if (!write_ok) break;
                }
                struct timespec pause = {.tv_nsec = 2000000};
                nanosleep(&pause, NULL);
            }
            const uint8_t stop[9] = {1};
            write_ok &= matrix_apply(device, session, stop, sizeof(stop));
            sl_rumble_tick(false);
            sl_rumble_shutdown(); /* stop -> join before diagnostics or UI */
            sl_rumble_snapshot state;
            sl_rumble_get_snapshot(&state);
            HidVibrationValue stopped[2] = {0};
            Result stop_rc[2] = {0};
            readback stop_reads[32];
            unsigned stop_count = 0;
            uint64_t joined_ns = now_ns();
            bool settled = false;
            do {
                readback *r = &stop_reads[stop_count++];
                *r = (readback){.ns = now_ns()};
                settled = true;
                for (unsigned i = 0; i < devices; ++i) {
                    stop_rc[i] = r->rc[i] = hidGetActualVibrationValue(handles[i], &r->values[i]);
                    stopped[i] = r->values[i];
                    settled &= R_SUCCEEDED(stop_rc[i]) && stopped[i].amp_low == 0 && stopped[i].amp_high == 0;
                }
                r->end_ns = now_ns();
                if (settled || r->end_ns - joined_ns >= UINT64_C(100000000)) break;
                struct timespec pause = {.tv_nsec = 5000000};
                nanosleep(&pause, NULL);
            } while (stop_count < 32);
            ok &= settled;
            size_t count = sl_rumble_copy_trace(trace, 512);
            ok &= write_ok && !state.worker_running && state.worker_starts == 1 &&
                state.worker_joins == 1 && !state.send_failures && !state.stop_failures &&
                !state.trace_overwritten && !state.stream[0].rejected && !state.stream[0].dropped_samples;
            log_line("{\"stage\":\"demo-scene\",\"scene\":%u,\"commands\":%u,\"skipped_steps\":%u,\"interrupted\":%s,\"sends\":%llu,\"joined\":%s,\"trace_lost\":%llu,\"ok\":%s}",
                scene + 1, commands, skipped, action ? "true" : "false",
                (unsigned long long)state.sends, !state.worker_running && state.worker_joins == 1 ? "true" : "false",
                (unsigned long long)state.trace_overwritten, ok ? "true" : "false");
            log_line("{\"stage\":\"demo-stopped\",\"scene\":%u,\"rc\":[%u,%u],\"amplitudes\":[%.9g,%.9g,%.9g,%.9g],\"settled\":%s,\"samples\":%u,\"after_join_ns\":%llu}",
                scene + 1, stop_rc[0], stop_rc[1], stopped[0].amp_low, stopped[0].amp_high,
                stopped[1].amp_low, stopped[1].amp_high, settled ? "true" : "false", stop_count,
                (unsigned long long)(stop_reads[stop_count - 1].end_ns - joined_ns));
            for (unsigned i = 0; i < stop_count; ++i) {
                readback *r = &stop_reads[i];
                log_line("{\"stage\":\"demo-stop-readback\",\"scene\":%u,\"ns\":%llu,\"rc\":[%u,%u],\"amplitudes\":[%.9g,%.9g,%.9g,%.9g]}",
                    scene + 1, (unsigned long long)r->ns, r->rc[0], r->rc[1],
                    r->values[0].amp_low, r->values[0].amp_high, r->values[1].amp_low, r->values[1].amp_high);
            }
            /* All logging is after the worker has stopped. Exact submitted
             * values remain available without asking the user to copy logs. */
            for (size_t i = 0; i < count; ++i) {
                sl_rumble_trace *t = &trace[i];
                if (t->kind == 1) continue;
                log_line("{\"stage\":\"demo-send\",\"scene\":%u,\"kind\":%u,\"sent_ns\":%llu,\"rc\":%u,\"values\":[[%.9g,%.9g,%.9g,%.9g],[%.9g,%.9g,%.9g,%.9g]]}",
                    scene + 1, t->kind, (unsigned long long)t->submitted_ns, t->result,
                    t->values[0].amp_low, t->values[0].freq_low, t->values[0].amp_high, t->values[0].freq_high,
                    t->values[1].amp_low, t->values[1].freq_low, t->values[1].amp_high, t->values[1].freq_high);
            }
            if (!action) ++completed;
            exiting = action & 1u;
            if (action & 4u) { scene = (scene + EXPERIENCE_CASES - 1) % EXPERIENCE_CASES; autoplay = false; }
            else if (action & 8u) { scene = (scene + 1) % EXPERIENCE_CASES; autoplay = false; }
            else if (action & 16u) { scene = 0; autoplay = true; }
            else if (!(action & 2u)) {
                if (autoplay && scene + 1 < EXPERIENCE_CASES) ++scene;
                else play = autoplay = false;
            }
#ifdef NSL_HAPTICS_PROBE_HOST
            if (!play) exiting = true;
#endif
        } else {
            while (appletMainLoop() && now_ns() < deadline) {
                SDL_PumpEvents();
                unsigned buttons = experience_buttons(), down = buttons & ~previous;
                previous = buttons;
                if (down & 1u) { exiting = true; break; }
                if (down & 4u) { scene = (scene + EXPERIENCE_CASES - 1) % EXPERIENCE_CASES; play = true; break; }
                if (down & 8u) { scene = (scene + 1) % EXPERIENCE_CASES; play = true; break; }
                if (down & 2u) { play = true; break; }
                if (down & 16u) { scene = 0; play = autoplay = true; break; }
                struct timespec pause = {.tv_nsec = 5000000};
                nanosleep(&pause, NULL);
            }
        }
    }
    log_line("{\"stage\":\"demo-end\",\"completed_scenes\":%u,\"ok\":%s,\"game_source_verified\":false,\"physical_fidelity_verified\":false}",
        completed, ok ? "true" : "false");
    experience_demo_active = false;
    return ok;
}
