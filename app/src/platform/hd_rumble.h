#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Nintendo actuator state, in exact 1/32-octave/log2-amplitude units.
 * Decoder is independent of SDL/libnx, allocation, clocks and output hardware. */
typedef struct sl_hd_state {
    int16_t amplitude[2];
    int16_t frequency[2];
} sl_hd_state;

typedef struct sl_hd_value {
    float amp_low, freq_low, amp_high, freq_high;
} sl_hd_value;

typedef struct sl_hd_samples {
    unsigned count;
    sl_hd_value values[3];
} sl_hd_samples;

void sl_hd_reset(sl_hd_state *state);
void sl_hd_current(const sl_hd_state *state, sl_hd_value *out);
void sl_hd_decode(sl_hd_state *state, uint32_t word, sl_hd_samples *out);
/* Validate report ID and exact wire size; no state changes on invalid input. */
bool sl_hd_report(sl_hd_state state[2], const void *data, size_t size, sl_hd_samples out[2]);
