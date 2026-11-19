/* Included after observe/log_line/now_ns by switch_haptics_probe.c. The complete
 * first-stage stimulus is in the NRO; no SD file or host-side helper is used.
 * Stage notes are deferred until the final stop to keep logging out of the
 * timing windows. The independent checker pins all 14 commands and outputs. */
typedef struct feedback_matrix_note {
    const char *name;
    unsigned first_seq, last_seq;
    uint64_t begin_ns, end_ns;
    bool write_ok;
} feedback_matrix_note;

static bool matrix_apply(IHS_HIDManagedDevice *device, IHS_Session *session,
                          const uint8_t *command, size_t size) {
    bool ok = IHS_HIDDeviceWrite(device->device, command, size) == 0;
    IHS_HIDSDLApplyPendingWrites(session);
    return ok;
}

static bool run_feedback_matrix(IHS_HIDManagedDevice *device, IHS_Session *session,
                                 HidVibrationDeviceHandle handles[2], unsigned devices) {
    feedback_matrix_note notes[13] = {0};
    unsigned used = 0, sequence = 0;
    bool ok = true;
    static const uint8_t batch_on[9] = {1, 0, 128, 0, 64};
    static const uint8_t stop[21] = {1};
    static const uint8_t body[9] = {1, 255, 255};
    static const uint8_t trigger_right[21] = {6, 0, 0, 0, 128};
    static const uint8_t trigger_left[9] = {6, 0, 64};
    static const uint8_t trigger_stop[21] = {6};
    feedback_matrix_note *note = &notes[used++];
    *note = (feedback_matrix_note){.name = "ordinary-batch", .first_seq = 1,
        .last_seq = 2, .begin_ns = now_ns(), .write_ok = true};
    note->write_ok &= IHS_HIDDeviceWrite(device->device, batch_on, sizeof(batch_on)) == 0;
    ++sequence;
    /* Neither ordinary command is applied until both have arrived. */
    observe(handles, devices, UINT64_C(30000000));
    note->write_ok &= IHS_HIDDeviceWrite(device->device, stop, sizeof(stop)) == 0;
    ++sequence;
    matrix_interleave_output = true;
    matrix_callbacks = 0;
    IHS_HIDSDLApplyPendingWrites(session);
    matrix_interleave_output = false;
    observe(handles, devices, UINT64_C(45000000));
    note->end_ns = now_ns();
    ok &= note->write_ok;

    const struct {
        const char *name;
        const uint8_t *command;
        size_t size;
    } ordinary[] = {
        {"body-start", body, sizeof(body)},
        {"trigger-start", trigger_right, sizeof(trigger_right)},
        {"trigger-stop", trigger_stop, sizeof(trigger_stop)},
        {"trigger-restart", trigger_left, sizeof(trigger_left)},
        {"body-stop", stop, sizeof(stop)},
        {"trigger-final-stop", trigger_stop, sizeof(trigger_stop)}
    };
    for (unsigned i = 0; i < sizeof(ordinary) / sizeof(ordinary[0]); ++i) {
        note = &notes[used++];
        ++sequence;
        *note = (feedback_matrix_note){.name = ordinary[i].name,
            .first_seq = sequence, .last_seq = sequence, .begin_ns = now_ns()};
        note->write_ok = matrix_apply(device, session, ordinary[i].command, ordinary[i].size);
        observe(handles, devices, UINT64_C(12000000));
        note->end_ns = now_ns();
        ok &= note->write_ok;
    }

    uint8_t sony[3][69] = {{13, 47, 0, 0, 0}, {13, 47, 0, 0, 0}, {13, 47, 0, 0, 0}};
    sony[0][5] = 0x03; /* ordinary emulation mode + haptics select */
    sony[0][7] = 64;   /* right emulated motor */
    sony[0][8] = 128;  /* left emulated motor */
    memcpy(sony[1], sony[0], sizeof(sony[1]));
    sony[1][6] = 0x04; /* SDL's LED update retains active mode and motor bytes */
    sony[1][49] = 0xaa;
    sony[1][50] = 0x2a;
    sony[1][51] = 0x55;
    /* sony[2] has no emulation mode: hand back to audio; no PCM in this report. */
    static const char *const sony_names[] = {"sony-legacy", "sony-led", "sony-audio-handoff"};
    for (unsigned i = 0; i < 3; ++i) {
        note = &notes[used++];
        ++sequence;
        *note = (feedback_matrix_note){.name = sony_names[i], .first_seq = sequence,
            .last_seq = sequence, .begin_ns = now_ns()};
        note->write_ok = matrix_apply(device, session, sony[i], sizeof(sony[i]));
        observe(handles, devices, UINT64_C(12000000));
        note->end_ns = now_ns();
        ok &= note->write_ok;
    }

    static const uint8_t pulse[69] = {13, 10, 0, 0, 0, 0x10, 0,
        0, 1, 0x40, 0x40, 0, 0, 5, 0xc0};
    uint8_t bands[69] = {13, 10, 0, 0, 0, 0x10};
    const uint32_t word = UINT32_C(0x60208180); /* 80/640 Hz, AM code 64 */
    for (unsigned motor = 0; motor < 2; ++motor)
        for (unsigned byte = 0; byte < 4; ++byte)
            bands[7 + motor * 4 + byte] = (uint8_t)(word >> (byte * 8));
    const struct {
        const char *name;
        const uint8_t *command;
        size_t size;
        uint64_t observe_ns;
    } final[] = {
        {"nintendo-pulse", pulse, sizeof(pulse), UINT64_C(25000000)},
        {"nintendo-bands", bands, sizeof(bands), UINT64_C(15000000)},
        {"final-stop", stop, sizeof(stop), UINT64_C(15000000)}
    };
    for (unsigned i = 0; i < sizeof(final) / sizeof(final[0]); ++i) {
        note = &notes[used++];
        ++sequence;
        *note = (feedback_matrix_note){.name = final[i].name, .first_seq = sequence,
            .last_seq = sequence, .begin_ns = now_ns()};
        note->write_ok = matrix_apply(device, session, final[i].command, final[i].size);
        observe(handles, devices, final[i].observe_ns);
        note->end_ns = now_ns();
        ok &= note->write_ok;
    }
    ok &= used == 13 && sequence == 14;
    for (unsigned i = 0; i < used; ++i) {
        note = &notes[i];
        log_line("{\"stage\":\"matrix-case\",\"name\":\"%s\",\"first_seq\":%u,\"last_seq\":%u,\"begin_ns\":%llu,\"end_ns\":%llu,\"write_ok\":%s}",
            note->name, note->first_seq, note->last_seq,
            (unsigned long long)note->begin_ns, (unsigned long long)note->end_ns,
            note->write_ok ? "true" : "false");
    }
    log_line("{\"stage\":\"matrix-end\",\"cases\":%u,\"commands\":%u,\"ok\":%s}",
        used, sequence, ok ? "true" : "false");
    return ok;
}
