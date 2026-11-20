#include "platform/rumble_render.h"
#include <math.h>

/* Protocol/default-frequency reference (facts only, no driver code copied):
 * https://github.com/torvalds/linux/blob/master/drivers/hid/hid-nintendo.c
 * Inspected source blob: eb3274db8ccbac75b109962fb84f8aac75f6c240.
 * JC_RUMBLE_DFLT_LOW_FREQ/HIGH_FREQ and JOYCON_MIN/MAX_RUMBLE_*_FREQ.
 * libnx HidVibrationValue supplies per-band amplitude (max 1.0) and Hz:
 * https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/hid.h
 * The reference peak is the existing fixed SDL amplitude code 100 decoded as
 * exp2((100-127)/32); see OFFICIAL_INPUT_RE sections 22.8 and 22.20. */

sl_rumble_render_config sl_rumble_render_default_config(void) {
    return (sl_rumble_render_config){
        .mode = SL_RUMBLE_RENDER_NATIVE_CONTINUOUS,
        .low_hz = 160.0f, .high_hz = 320.0f,
        .low_max_amplitude = 0.5571934f, .high_max_amplitude = 0.5571934f};
}

bool sl_rumble_render_config_valid(const sl_rumble_render_config *c) {
    return c &&
        (c->mode == SL_RUMBLE_RENDER_NATIVE_CONTINUOUS ||
         c->mode == SL_RUMBLE_RENDER_SDL_REFERENCE) &&
        isfinite(c->low_hz) && c->low_hz >= 41.0f && c->low_hz <= 626.0f &&
        isfinite(c->high_hz) && c->high_hz >= 82.0f && c->high_hz <= 1253.0f &&
        isfinite(c->low_max_amplitude) && c->low_max_amplitude >= 0.0f &&
        c->low_max_amplitude <= 1.0f &&
        isfinite(c->high_max_amplitude) && c->high_max_amplitude >= 0.0f &&
        c->high_max_amplitude <= 1.0f;
}

static sl_hd_value quiet(void) {
    return (sl_hd_value){0, 160, 0, 320};
}

static sl_hd_value reference(uint16_t low, uint16_t high) {
    sl_hd_state local;
    sl_hd_samples result;
    sl_hd_reset(&local);
    sl_hd_decode(&local, sl_hd_ordinary_word(low, high), &result);
    return result.values[0];
}

bool sl_rumble_render(const sl_rumble_render_config *config, sl_rumble_layout layout,
                      uint16_t low, uint16_t high, sl_hd_value out[2]) {
    if (!out)
        return false;
    out[0] = out[1] = quiet();
    sl_rumble_render_config defaults;
    if (!config) {
        defaults = sl_rumble_render_default_config();
        config = &defaults;
    }
    if (!sl_rumble_render_config_valid(config) || layout < SL_RUMBLE_LAYOUT_PRO ||
        layout > SL_RUMBLE_LAYOUT_SINGLE_RIGHT)
        return false;

    sl_hd_value both, left, right;
    if (config->mode == SL_RUMBLE_RENDER_SDL_REFERENCE) {
        both = reference(low, high);
        left = reference(low, 0);
        right = reference(0, high);
    } else {
        both = (sl_hd_value){
            ((float)low / 65535.0f) * config->low_max_amplitude, config->low_hz,
            ((float)high / 65535.0f) * config->high_max_amplitude, config->high_hz};
        left = right = both;
        left.amp_high = 0;
        right.amp_low = 0;
    }
    switch (layout) {
        case SL_RUMBLE_LAYOUT_PRO:
            out[0] = out[1] = both;
            break;
        case SL_RUMBLE_LAYOUT_PAIRED:
            out[0] = left;
            out[1] = right;
            break;
        case SL_RUMBLE_LAYOUT_SINGLE_LEFT:
            out[0] = both;
            break;
        case SL_RUMBLE_LAYOUT_SINGLE_RIGHT:
            out[1] = both;
            break;
    }
    return true;
}
