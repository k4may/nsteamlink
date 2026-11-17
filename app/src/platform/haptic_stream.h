#pragma once
#include "platform/hd_rumble.h"

#define SL_HAPTIC_SAMPLE_NS UINT64_C(5000000)
#define SL_HAPTIC_QUEUE 96u

typedef struct sl_haptic_frame {
    sl_hd_value values[2];
    uint64_t due_ns, received_ns, sequence;
    unsigned sample;
    bool native;
} sl_haptic_frame;

typedef struct sl_haptic_stats {
    uint64_t commands, native_commands, decoded_samples, scheduled_samples, emitted_samples;
    uint64_t rejected, overflows, dropped_samples, late_samples, max_lateness_ns;
} sl_haptic_stats;

/* Caller serializes this deterministic state machine. No hardware or threads. */
typedef struct sl_haptic_stream {
    sl_hd_state decoder[2];
    sl_haptic_frame current, queue[SL_HAPTIC_QUEUE];
    unsigned head, count;
    uint64_t expires_ns, wire_expires_ns, source_epoch, playback_epoch, last_sample_ns;
    bool dirty, source_epoch_valid;
    sl_haptic_stats stats;
} sl_haptic_stream;

void sl_haptic_init(sl_haptic_stream *s);
bool sl_haptic_valid_command(const uint8_t *command, size_t size);
void sl_haptic_stop(sl_haptic_stream *s, uint64_t now);
bool sl_haptic_command(sl_haptic_stream *s, const uint8_t *command, size_t size,
                       uint64_t received_ns, uint64_t now, bool paired_joycon);
/* At most one frame per call. Overdue samples are rescheduled, never collapsed
 * or emitted back-to-back as a burst. Lateness is retained as failed timing
 * evidence in stats rather than disguised as an on-time output. */
bool sl_haptic_advance(sl_haptic_stream *s, uint64_t now, sl_haptic_frame *out);
/* Account actual IPC attempt timing/results, then enforce spacing from a
 * successful completion. Stale bindings still contribute actual output stats
 * but cannot mutate the replacement binding's playback state. */
void sl_haptic_output_complete(sl_haptic_stream *s, const sl_haptic_frame *frame,
                               uint64_t submitted_ns, uint64_t completed_ns, bool success,
                               bool current_binding);
uint64_t sl_haptic_deadline(const sl_haptic_stream *s);
