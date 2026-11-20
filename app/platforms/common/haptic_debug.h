#pragma once
/* Thread-safe rumble snapshots only: never borrow a runtime/session pointer.
 * The existing log worker can serve these while the session actor is waiting. */
#include "platform/rumble.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline bool sl_haptic_debug_read(const char *target, char *reply, size_t capacity) {
    if (!strcmp(target, "/haptics")) {
        sl_rumble_snapshot s;
        sl_rumble_get_snapshot(&s);
        sl_haptic_stats sum = {0};
        for (unsigned i = 0; i < 8; ++i) {
            sum.commands += s.stream[i].commands;
            sum.ordinary_commands += s.stream[i].ordinary_commands;
            sum.native_commands += s.stream[i].native_commands;
            sum.trigger_commands += s.stream[i].trigger_commands;
            sum.sony_commands += s.stream[i].sony_commands;
            sum.pcm_frames += s.stream[i].pcm_frames;
            sum.rejected += s.stream[i].rejected;
            sum.emitted_frames += s.stream[i].emitted_frames;
        }
        int n = snprintf(reply, capacity,
            "{\"stage\":\"haptics\",\"commands\":%llu,\"ordinary_commands\":%llu,"
            "\"native_commands\":%llu,\"trigger_commands\":%llu,\"sony_commands\":%llu,"
            "\"pcm_frames\":%llu,\"rejected\":%llu,\"emitted_frames\":%llu,"
            "\"sends\":%llu,\"send_failures\":%llu,\"trace_overwritten\":%llu,"
            "\"io_rc\":%u,\"session_actor_independent\":true}",
            (unsigned long long)sum.commands, (unsigned long long)sum.ordinary_commands,
            (unsigned long long)sum.native_commands, (unsigned long long)sum.trigger_commands,
            (unsigned long long)sum.sony_commands, (unsigned long long)sum.pcm_frames,
            (unsigned long long)sum.rejected, (unsigned long long)sum.emitted_frames,
            (unsigned long long)s.sends, (unsigned long long)s.send_failures,
            (unsigned long long)s.trace_overwritten, s.io_result);
        return n >= 0 && (size_t)n < capacity;
    }
    if (strncmp(target, "/haptic-trace?after=", 20)) return false;
    const char *digits = target + 20;
    if (*digits < '0' || *digits > '9') return false;
    char *end;
    errno = 0;
    uint64_t after = strtoull(digits, &end, 10);
    if (*end || errno) return false;
    sl_rumble_trace entries[16];
    size_t count = sl_rumble_read_trace(after, entries, 16), used = 0;
    for (size_t i = 0; i < count; ++i) {
        const sl_rumble_trace *t = &entries[i];
        char wire[139] = {0};
        if (t->size > 69) return false;
        for (unsigned j = 0; j < t->size; ++j)
            snprintf(wire + j * 2, 3, "%02x", t->command[j]);
        int n = snprintf(reply + used, capacity - used,
            "{\"serial\":%llu,\"kind\":%u,\"slot\":%u,\"seq\":%llu,\"sample\":%u,"
            "\"received_ns\":%llu,\"due_ns\":%llu,\"sent_ns\":%llu,\"completed_ns\":%llu,"
            "\"source\":%u,\"unsupported\":%u,\"rc\":%u,\"wire\":\"%s\","
            "\"values\":[[%.9g,%.9g,%.9g,%.9g],[%.9g,%.9g,%.9g,%.9g]]}\n",
            (unsigned long long)t->serial, t->kind, t->slot,
            (unsigned long long)t->sequence, t->sample, (unsigned long long)t->received_ns,
            (unsigned long long)t->due_ns, (unsigned long long)t->submitted_ns,
            (unsigned long long)t->completed_ns, t->source, t->unsupported, t->result, wire,
            t->values[0].amp_low, t->values[0].freq_low, t->values[0].amp_high, t->values[0].freq_high,
            t->values[1].amp_low, t->values[1].freq_low, t->values[1].amp_high, t->values[1].freq_high);
        if (n < 0 || (size_t)n >= capacity - used) return false;
        used += (size_t)n;
    }
    if (!used) {
        if (capacity < 3) return false;
        strcpy(reply, "{}");
    }
    return true;
}
