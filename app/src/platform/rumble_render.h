#pragma once
#include "platform/hd_rumble.h"

typedef enum sl_rumble_render_mode {
    SL_RUMBLE_RENDER_NATIVE_CONTINUOUS,
    SL_RUMBLE_RENDER_SDL_REFERENCE
} sl_rumble_render_mode;

typedef enum sl_rumble_layout {
    SL_RUMBLE_LAYOUT_PRO,
    SL_RUMBLE_LAYOUT_PAIRED, /* Handheld or JoyDual: low left, high right. */
    SL_RUMBLE_LAYOUT_SINGLE_LEFT,
    SL_RUMBLE_LAYOUT_SINGLE_RIGHT
} sl_rumble_layout;

typedef struct sl_rumble_render_config {
    sl_rumble_render_mode mode;
    float low_hz, high_hz;
    float low_max_amplitude, high_max_amplitude;
} sl_rumble_render_config;

/* Native policy starts at 160/320 Hz, the Linux hid-nintendo default bands.
 * Each band retains the fixed SDL reference's ~0.55719 peak as a conservative
 * numerical starting point, rather than increasing it to 1.0. These are not
 * perceptual calibration or a claim of optimal mechanical output. */
sl_rumble_render_config sl_rumble_render_default_config(void);

/* Policy bounds: low 41..626 Hz, high 82..1253 Hz, amplitudes
 * 0..1, all finite. The frequency bounds follow Linux hid-nintendo's supported
 * encoder range; they are not a newly inferred HOS validation contract.
 * Bands are independent: equal/overlapping carriers are valid. Reference mode
 * uses its fixed encoder; custom values do not modify it. */
bool sl_rumble_render_config_valid(const sl_rumble_render_config *config);

/* Pure ordinary-rumble rendering. NULL config selects the native default.
 * Source uint16 values enter the native mapping directly, with no Nintendo
 * wire quantization. PRO duplicates both bands to both actuators; PAIRED
 * separates sources; SINGLE merges both sources into its one actuator.
 * The unused single-controller output is silent. Invalid arguments return
 * false and leave both outputs silent (unless out itself is NULL).
 *
 * No caller-owned wire decoder state is accepted or modified. The stream must
 * still seed its independent native decoder using sl_hd_ordinary_word for
 * subsequent relative Nintendo reports, regardless of this playback policy. */
bool sl_rumble_render(const sl_rumble_render_config *config, sl_rumble_layout layout,
                      uint16_t low, uint16_t high, sl_hd_value out[2]);
