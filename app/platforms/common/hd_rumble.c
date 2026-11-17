#include "platform/hd_rumble.h"
#include <math.h>

/* Ordinary rumble thresholds adapted from SDL's Switch HIDAPI encoder,
 * Copyright (C) 1997-2025 Sam Lantinga; Valve contributed Switch driver logic.
 * zlib license: third_party/sdl-rumble-LICENSE.txt. This is an altered lookup
 * and packing adaptation, verified against fixed Valve SDL original code.
 * Source reference: SDL a962f40bbba175e9716557a25d5d7965f134a3d3.
 * The independent native decoder below does not incorporate MissionControl. */
static unsigned ordinary_index(uint16_t value) {
    static const uint16_t thresholds[101] = {
        0, 514, 775, 921, 1096, 1303, 1550, 1843, 2192, 2606, 3100, 3686,
        4383, 5213, 6199, 7372, 7698, 8039, 8395, 8767, 9155, 9560, 9984,
        10426, 10887, 11369, 11873, 12398, 12947, 13520, 14119, 14744,
        15067, 15397, 15734, 16079, 16431, 16790, 17158, 17534, 17918,
        18310, 18711, 19121, 19540, 19967, 20405, 20851, 21308, 21775,
        22251, 22739, 23236, 23745, 24265, 24797, 25340, 25894, 26462,
        27041, 27633, 28238, 28856, 29488, 30134, 30794, 31468, 32157,
        32861, 33581, 34316, 35068, 35836, 36620, 37422, 38242, 39079,
        39935, 40809, 41703, 42616, 43549, 44503, 45477, 46473, 47491,
        48531, 49593, 50679, 51789, 52923, 54082, 55266, 56476, 57713,
        58977, 60268, 61588, 62936, 64315, 65535
    };
    unsigned lo = 0, hi = 100;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (value <= thresholds[mid]) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

uint32_t sl_hd_ordinary_word(uint16_t low, uint16_t high) {
    if (!low && !high)
        return 0x40400100;
    unsigned a = ordinary_index(low), b = ordinary_index(high);
    return 0x403d0074u | b << 9 | (a & 1u) << 23 | (a >> 1) << 24;
}

/* Protocol facts checked against MissionControl 71a837f's independent decoder.
 * Integer coordinates and explicit masks avoid ABI-dependent C bitfields.
 * This implementation does not link or incorporate that GPL-2.0-only decoder. */
static int clamp(int x, int low, int high) {
    return x < low ? low : x > high ? high : x;
}

void sl_hd_reset(sl_hd_state *s) {
    *s = (sl_hd_state){{-256, -256}, {0, 0}};
}

void sl_hd_current(const sl_hd_state *s, sl_hd_value *out) {
    *out = (sl_hd_value){
        s->amplitude[0] < -254 ? 0.0f : exp2f(s->amplitude[0] / 32.0f),
        160.0f * exp2f(s->frequency[0] / 32.0f),
        s->amplitude[1] < -254 ? 0.0f : exp2f(s->amplitude[1] / 32.0f),
        320.0f * exp2f(s->frequency[1] / 32.0f)};
}

static int amplitude(unsigned code) {
    return code == 0 ? -256 : code < 16 ? (int)code * 8 - 248 :
           code < 32 ? (int)code * 2 - 158 : (int)code - 127;
}

static void absolute(sl_hd_state *s, unsigned band, unsigned am, unsigned fm) {
    s->amplitude[band] = amplitude(am);
    s->frequency[band] = (int)fm - 64;
}

static void command(sl_hd_state *s, unsigned band, unsigned code) {
    if (code == 0) {
        s->amplitude[band] = -256;
        s->frequency[band] = 0;
    } else if (code < 12) {
        s->amplitude[band] = -16 * ((int)code - 1);
    } else if (code < 17) {
        s->frequency[band] = 6 * ((int)code - 14);
    } else {
        static const int delta[] = {4, 1, 0, -1, -4};
        unsigned group = (code - 17) / 3;
        s->amplitude[band] = clamp(s->amplitude[band] + delta[group], -256, 0);
        s->frequency[band] = clamp(s->frequency[band] + 1 - (int)((code - 17) % 3),
                                   -64, 64);
    }
}

static void pair(sl_hd_state *s, uint32_t word, unsigned high_shift, unsigned low_shift) {
    command(s, 0, (word >> low_shift) & 31);
    command(s, 1, (word >> high_shift) & 31);
}

void sl_hd_decode(sl_hd_state *s, uint32_t word, sl_hd_samples *out) {
    unsigned kind = word >> 30;
    out->count = 0;
    if (kind == 0)
        return; /* repeat existing actuator state, with no new samples */
    if (kind == 1 && (word & 0xfffff) == 0) {
        pair(s, word, 20, 25);
        out->count = 1;
    } else if (kind == 1 && (word & 3) == 0) {
        absolute(s, 0, (word >> 23) & 127, (word >> 16) & 127);
        absolute(s, 1, (word >> 9) & 127, (word >> 2) & 127);
        out->count = 1;
    } else if (kind == 1) {
        unsigned band = word & 1;
        unsigned value = (word >> 23) & 127;
        if (word & 4)
            s->frequency[band] = (int)value - 64;
        else
            s->amplitude[band] = amplitude(value);
        out->count = 3;
    } else if (kind == 2 && (word & 0x3ff) == 0) {
        pair(s, word, 20, 25);
        out->count = 2;
    } else if (kind == 2) {
        unsigned band = word & 1;
        absolute(s, band, (word >> 23) & 127, (word >> 1) & 127);
        command(s, 1 - band, (word >> 18) & 31);
        out->count = 2;
    } else {
        pair(s, word, 20, 25);
        out->count = 3;
    }
    sl_hd_current(s, &out->values[0]);
    if (out->count > 1) {
        unsigned shift = kind == 1 ? 13 : kind == 2 && (word & 0x3ff) != 0 ? 8 : 10;
        pair(s, word, shift, shift + 5);
        sl_hd_current(s, &out->values[1]);
    }
    if (out->count > 2) {
        pair(s, word, kind == 1 ? 3 : 0, kind == 1 ? 8 : 5);
        sl_hd_current(s, &out->values[2]);
    }
}

bool sl_hd_report(sl_hd_state state[2], const void *data, size_t size, sl_hd_samples out[2]) {
    if (!data || size != 10 || ((const uint8_t *)data)[0] != 0x10)
        return false;
    const uint8_t *b = data;
    for (unsigned i = 0; i < 2; ++i) {
        const uint8_t *p = b + 2 + i * 4;
        uint32_t word = (uint32_t)p[0] | (uint32_t)p[1] << 8 |
                        (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        sl_hd_decode(&state[i], word, &out[i]);
    }
    return true;
}
