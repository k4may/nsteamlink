#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "platform/haptic_stream.h"

typedef struct sl_rumble_snapshot {
    sl_haptic_stats stream[8];
    uint64_t sends, send_failures, stop_failures, worker_starts, worker_joins;
    uint64_t trace_overwritten;
    uint64_t canceled_outputs;
    uint32_t io_result;
    uint32_t native_capability_mask; /* Local support snapshot, not a host ACK. */
    bool worker_running;
} sl_rumble_snapshot;

typedef struct sl_rumble_trace {
    uint64_t serial, received_ns, due_ns, submitted_ns, completed_ns, sequence;
    uint32_t result;
    unsigned slot, sample, kind, size, count, source;
    uint32_t unsupported;
    uint8_t command[69];
    sl_hd_value values[2];
} sl_rumble_trace;

/* Init/shutdown and command/tick belong to the SDL/media owner. The joinable
 * worker uses private HID IPC and never calls SDL or application log callbacks. */
#if defined(__SWITCH__) || defined(NSL_RUMBLE_TEST)
bool sl_rumble_init(void);
void sl_rumble_shutdown(void);
int sl_rumble_command(int32_t joystick_id, const uint8_t *command, size_t size,
                      uint64_t received_ns, void *context);
void sl_rumble_discontinuity(int32_t joystick_id, void *context);
/* Owner-thread submission of already analyzed dedicated PCM. source_ns is
 * explicitly mapped local available-time; this is not a Remote Play packet.
 * Dedicated left/right PCM requires two actuators; single Joy-Con is rejected
 * and counted, rather than silently losing the other authored channel. */
int sl_rumble_pcm_frame(int32_t joystick_id, const sl_pcm_frame *frame, uint64_t source_ns);
void sl_rumble_tick(bool session_active);
/* Owner refreshes hardware/style availability; query only reads a locked
 * snapshot and is safe on the SDK enumeration thread. No motor ACK implied. */
void sl_rumble_refresh_capabilities(void);
bool sl_rumble_supports_native_haptics(int32_t joystick_id, void *context);
void sl_rumble_get_snapshot(sl_rumble_snapshot *out);
size_t sl_rumble_copy_trace(sl_rumble_trace *out, size_t capacity);
size_t sl_rumble_read_trace(uint64_t after_serial, sl_rumble_trace *out, size_t capacity);
#else
static inline bool sl_rumble_init(void) { return true; }
static inline void sl_rumble_shutdown(void) {}
static inline void sl_rumble_tick(bool session_active) { (void)session_active; }
#endif
