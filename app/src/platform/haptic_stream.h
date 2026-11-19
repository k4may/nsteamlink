#pragma once
#include "platform/hd_rumble.h"
#include "platform/rumble_render.h"
#include "platform/sony_haptics.h"
#include "platform/pcm_haptics.h"

#define SL_HAPTIC_SAMPLE_NS UINT64_C(5000000)
#define SL_HAPTIC_QUEUE 96u
/* Bound interactive feedback delay; expired data is stopped and counted,
 * never replayed seconds after the event. This is a software budget. */
#define SL_HAPTIC_MAX_LATENCY_NS UINT64_C(250000000)

typedef enum sl_haptic_source {
    SL_HAPTIC_BODY, SL_HAPTIC_NINTENDO, SL_HAPTIC_TRIGGERS,
    SL_HAPTIC_SONY, SL_HAPTIC_EXPIRY, SL_HAPTIC_RESET, SL_HAPTIC_PCM
} sl_haptic_source;

typedef struct sl_haptic_frame {
    sl_hd_value values[2];
    uint64_t due_ns, received_ns, sequence;
    unsigned sample;
    bool native;
    sl_haptic_source source;
    uint64_t source_ns, duration_ns;
} sl_haptic_frame;

typedef struct sl_haptic_stats {
    uint64_t commands, native_commands, decoded_samples, scheduled_samples, emitted_samples;
    uint64_t rejected, overflows, dropped_samples, late_samples, max_lateness_ns;
    uint64_t ordinary_commands, trigger_commands, sony_commands;
    uint64_t superseded_samples, stale_frames, discontinuities, desync_rejected;
    uint64_t unsupported_effects, adapted_effects, scheduled_frames, emitted_frames;
    uint64_t dropped_frames;
    uint64_t pcm_frames;
} sl_haptic_stats;

/* Caller serializes this deterministic state machine. No hardware or threads. */
typedef struct sl_haptic_stream {
    sl_hd_state decoder[2];
    sl_haptic_frame current, queue[SL_HAPTIC_QUEUE];
    unsigned head, count;
    uint64_t expires_ns, wire_expires_ns, source_epoch, playback_epoch, last_sample_ns;
    bool dirty, source_epoch_valid;
    sl_hd_value body[2], triggers[2];
    uint64_t trigger_expires_ns, body_expiry_source_ns, trigger_expiry_source_ns;
    bool desynchronized;
    sl_sony_state sony;
    uint32_t last_unsupported;
    sl_haptic_source last_body_source;
    uint64_t pcm_epoch, pcm_available_pts;
    bool pcm_started, pcm_ended;
    sl_haptic_stats stats;
} sl_haptic_stream;

void sl_haptic_init(sl_haptic_stream *s);
bool sl_haptic_valid_command(const uint8_t *command, size_t size);
void sl_haptic_stop(sl_haptic_stream *s, uint64_t now);
bool sl_haptic_command(sl_haptic_stream *s, const uint8_t *command, size_t size,
                       uint64_t received_ns, uint64_t now, bool paired_joycon);
bool sl_haptic_command_layout(sl_haptic_stream *s, const uint8_t *command, size_t size,
                              uint64_t received_ns, uint64_t now, sl_rumble_layout layout);
void sl_haptic_discontinuity(sl_haptic_stream *s, uint64_t now);
/* Local dedicated-PCM adapter output, NOT a Steam wire packet. The caller
 * explicitly maps available_pts to local monotonic source_ns. Spectrum
 * analysis belongs off the media/input/actuator deadline threads. */
bool sl_haptic_pcm_frame(sl_haptic_stream *s, const sl_pcm_frame *frame,
                         uint64_t source_ns, uint64_t now);
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
