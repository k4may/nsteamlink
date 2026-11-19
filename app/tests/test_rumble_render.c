#include "platform/rumble_render.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void silent(const sl_hd_value *v) {
    assert(v->amp_low == 0 && v->amp_high == 0);
    assert(v->freq_low == 160 && v->freq_high == 320);
}

static void close_to(float a, float b) {
    assert(fabsf(a - b) < 0.000001f);
}

static void endpoints_and_topology(void) {
    sl_hd_value out[2];
    const sl_rumble_render_config c = sl_rumble_render_default_config();
    assert(sl_rumble_render_config_valid(&c));
    for (int layout = SL_RUMBLE_LAYOUT_PRO; layout <= SL_RUMBLE_LAYOUT_SINGLE_RIGHT; ++layout) {
        assert(sl_rumble_render(&c, (sl_rumble_layout)layout, 0, 0, out));
        silent(&out[0]);
        silent(&out[1]);
    }
    assert(sl_rumble_render(NULL, SL_RUMBLE_LAYOUT_PRO, 65535, 65535, out));
    assert(out[0].amp_low == c.low_max_amplitude && out[0].amp_high == c.high_max_amplitude);
    assert(out[0].freq_low == 160 && out[0].freq_high == 320);
    assert(memcmp(&out[0], &out[1], sizeof(out[0])) == 0);
    assert(c.low_max_amplitude < 1 && c.high_max_amplitude < 1);

    assert(sl_rumble_render(&c, SL_RUMBLE_LAYOUT_PAIRED, 65535, 32768, out));
    assert(out[0].amp_low == c.low_max_amplitude && out[0].amp_high == 0);
    assert(out[1].amp_low == 0 && out[1].amp_high > 0);
    for (int layout = SL_RUMBLE_LAYOUT_SINGLE_LEFT; layout <= SL_RUMBLE_LAYOUT_SINGLE_RIGHT; ++layout) {
        unsigned active = layout == SL_RUMBLE_LAYOUT_SINGLE_RIGHT;
        assert(sl_rumble_render(&c, (sl_rumble_layout)layout, 65535, 32768, out));
        assert(out[active].amp_low == c.low_max_amplitude && out[active].amp_high > 0);
        assert(out[active].freq_low < out[active].freq_high);
        silent(&out[1 - active]);
        /* Neither source disappears when it is the only active one. */
        assert(sl_rumble_render(&c, (sl_rumble_layout)layout, 0, 1, out));
        assert(out[active].amp_low == 0 && out[active].amp_high > 0);
        assert(sl_rumble_render(&c, (sl_rumble_layout)layout, 1, 0, out));
        assert(out[active].amp_low > 0 && out[active].amp_high == 0);
    }
}

static void source_resolution_and_independence(void) {
    sl_hd_value out[2];
    float previous_low = -1, previous_high = -1;
    const sl_rumble_render_config c = sl_rumble_render_default_config();
    /* Exhaustive one-dimensional sweeps prove all 16-bit source codes remain
     * distinct in software, bounds hold, and the other source is independent.
     * They do not claim the hardware/perception resolves 65,536 amplitudes. */
    for (unsigned raw = 0; raw <= 65535; ++raw) {
        assert(sl_rumble_render(&c, SL_RUMBLE_LAYOUT_PRO, (uint16_t)raw, 12345, out));
        assert(isfinite(out[0].amp_low) && out[0].amp_low > previous_low);
        assert(out[0].amp_low >= 0 && out[0].amp_low <= c.low_max_amplitude);
        close_to(out[0].amp_high, (12345.0f / 65535.0f) * c.high_max_amplitude);
        unsigned recovered = (unsigned)lroundf(out[0].amp_low / c.low_max_amplitude * 65535.0f);
        assert(recovered == raw);
        previous_low = out[0].amp_low;

        assert(sl_rumble_render(&c, SL_RUMBLE_LAYOUT_PRO, 23456, (uint16_t)raw, out));
        assert(isfinite(out[0].amp_high) && out[0].amp_high > previous_high);
        assert(out[0].amp_high >= 0 && out[0].amp_high <= c.high_max_amplitude);
        close_to(out[0].amp_low, (23456.0f / 65535.0f) * c.low_max_amplitude);
        recovered = (unsigned)lroundf(out[0].amp_high / c.high_max_amplitude * 65535.0f);
        assert(recovered == raw);
        previous_high = out[0].amp_high;
    }
}

static void configuration_and_rejection(void) {
    sl_rumble_render_config c = sl_rumble_render_default_config();
    sl_hd_value out[2];
    c.low_hz = 100;
    c.high_hz = 500;
    c.low_max_amplitude = 0.2f;
    c.high_max_amplitude = 0.4f;
    assert(sl_rumble_render(&c, SL_RUMBLE_LAYOUT_PRO, 65535, 65535, out));
    assert(out[0].freq_low == 100 && out[0].freq_high == 500);
    assert(out[0].amp_low == 0.2f && out[0].amp_high == 0.4f);
    c.low_max_amplitude = 0;
    assert(sl_rumble_render(&c, SL_RUMBLE_LAYOUT_PRO, 65535, 65535, out));
    assert(out[0].amp_low == 0 && out[0].amp_high == 0.4f);

    sl_rumble_render_config bad[10];
    for (unsigned i = 0; i < 10; ++i)
        bad[i] = sl_rumble_render_default_config();
    bad[0].mode = (sl_rumble_render_mode)99;
    bad[1].low_hz = NAN;
    bad[2].high_hz = INFINITY;
    bad[3].low_hz = 40;
    bad[4].high_hz = 1254;
    bad[5].low_hz = bad[5].high_hz;
    bad[6].low_max_amplitude = -0.1f;
    bad[7].high_max_amplitude = 1.01f;
    bad[8].low_max_amplitude = NAN;
    bad[9].high_max_amplitude = INFINITY;
    for (unsigned i = 0; i < 10; ++i) {
        assert(!sl_rumble_render_config_valid(&bad[i]));
        out[0] = out[1] = (sl_hd_value){1, 1, 1, 1};
        assert(!sl_rumble_render(&bad[i], SL_RUMBLE_LAYOUT_PRO, 65535, 65535, out));
        silent(&out[0]);
        silent(&out[1]);
    }
    assert(!sl_rumble_render_config_valid(NULL));
    assert(!sl_rumble_render(NULL, (sl_rumble_layout)-1, 1, 2, out));
    silent(&out[0]);
    silent(&out[1]);
    assert(!sl_rumble_render(NULL, SL_RUMBLE_LAYOUT_PRO, 1, 2, NULL));
}

static void fixed_reference_and_decoder_isolation(void) {
    sl_rumble_render_config c = sl_rumble_render_default_config();
    c.mode = SL_RUMBLE_RENDER_SDL_REFERENCE;
    sl_hd_value out[2];
    assert(sl_rumble_render(&c, SL_RUMBLE_LAYOUT_PRO, 65535, 65535, out));
    close_to(out[0].amp_low, 0.557193398f);
    close_to(out[0].amp_high, 0.557193398f);
    close_to(out[0].freq_low, 149.9334106f);
    close_to(out[0].freq_high, 149.9334106f);
    assert(memcmp(&out[0], &out[1], sizeof(out[0])) == 0);
    assert(sl_rumble_render(&c, SL_RUMBLE_LAYOUT_PAIRED, 65535, 0, out));
    assert(out[0].amp_low > 0 && out[0].amp_high == 0);
    silent(&out[1]);
    assert(sl_rumble_render(&c, SL_RUMBLE_LAYOUT_SINGLE_RIGHT, 0, 65535, out));
    silent(&out[0]);
    assert(out[1].amp_low == 0 && out[1].amp_high > 0);

    /* Strongly different policies must never replace the native wire history.
     * A caller-owned decoder follows a relative native packet identically. */
    sl_hd_state wire, copy;
    sl_hd_samples seeded, a, b;
    sl_hd_reset(&wire);
    sl_hd_decode(&wire, sl_hd_ordinary_word(12345, 54321), &seeded);
    copy = wire;
    for (unsigned i = 0; i < 2; ++i) {
        c.mode = i ? SL_RUMBLE_RENDER_NATIVE_CONTINUOUS : SL_RUMBLE_RENDER_SDL_REFERENCE;
        c.low_max_amplitude = 0.01f;
        c.high_max_amplitude = 0.02f;
        assert(sl_rumble_render(&c, SL_RUMBLE_LAYOUT_PRO, 1, 65535, out));
        assert(memcmp(&wire, &copy, sizeof(wire)) == 0);
    }
    const uint32_t relative = UINT32_C(0x40000000) | 18u << 25 | 28u << 20;
    sl_hd_decode(&wire, relative, &a);
    sl_hd_decode(&copy, relative, &b);
    assert(a.count == b.count && a.count == 1);
    assert(memcmp(&a.values[0], &b.values[0], sizeof(a.values[0])) == 0);
    assert(memcmp(&wire, &copy, sizeof(wire)) == 0);
}

int main(void) {
    endpoints_and_topology();
    source_resolution_and_independence();
    configuration_and_rejection();
    fixed_reference_and_decoder_isolation();
    puts("PASS continuous source resolution, topology, policy bounds, reference and decoder isolation");
}
