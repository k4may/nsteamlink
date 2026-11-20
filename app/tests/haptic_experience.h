/* Authored local stimuli, never claimed to originate from a Steam game.
 * Every command goes through DeviceWrite -> SDK owner -> production backend.
 * 20ms stimulus steps are intentionally slower than native sample capacity. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#define EXPERIENCE_CASES 7u
#define EXPERIENCE_MS 4000u
#define EXPERIENCE_STEP_MS 20u
static const char *const experience_names[EXPERIENCE_CASES] = {
    "STEADY / ordinary two-channel rumble",
    "RAMP / gradually weak - strong - weak",
    "PITCH / 80 - 160 - 320 - 640 Hz",
    "POSITION / left - right alternating",
    "CLICKS / short taps versus sustained buzz",
    "ENGINE / rising pitch with two carriers",
    "TEXTURE / alternating pairs of frequencies"
};
static const char *const experience_hints[EXPERIENCE_CASES] = {
    "Reference: a steady buzz, followed by silence.",
    "Feel the strength rise and fall. Perceived strength is not linear.",
    "Same encoded amplitude; feel pitch/texture change, not equal loudness.",
    "Two actuators required for left/right separation.",
    "Three 40ms taps, then one 600ms hold; repeated twice.",
    "An authored effect: bass and its upper carrier rise together.",
    "80+640 Hz then 160+320 Hz. Both bands on each actuator."
};
static uint32_t experience_word(unsigned low_am, unsigned low_fm,
                                 unsigned high_am, unsigned high_fm) {
    return UINT32_C(0x40000000) | low_am << 23 | low_fm << 16 |
        high_am << 9 | high_fm << 2;
}
static size_t experience_command(unsigned scene, unsigned ms, uint8_t out[69]) {
    memset(out, 0, 69);
    out[0] = 1;
    if (scene >= EXPERIENCE_CASES || ms >= EXPERIENCE_MS) return 9;
    if (scene < 2) {
        float gain = scene == 0 ? (ms < 3000 ? 0.65f : 0.0f) :
            0.85f * (1.0f - fabsf((float)ms / 2000.0f - 1.0f));
        uint16_t level = (uint16_t)lroundf(gain * 65535.0f);
        out[1] = out[3] = (uint8_t)level;
        out[2] = out[4] = (uint8_t)(level >> 8);
        out[5] = 100; /* bounded duration, refreshed every 20ms */
        return 9;
    }
    uint32_t left = UINT32_C(0x40400100), right = left;
    if (scene == 2 && ms % 1000 < 800) {
        unsigned segment = ms / 1000;
        left = right = segment < 2 ? experience_word(64, 32 + 32 * segment, 0, 64) :
            experience_word(0, 64, 64, 64 + 32 * (segment - 2));
    } else if (scene == 3 && ms % 500 < 350) {
        if ((ms / 500) % 2) right = experience_word(80, 64, 0, 64);
        else left = experience_word(80, 64, 0, 64);
    } else if (scene == 4) {
        unsigned t = ms % 2000;
        if ((t < 600 && t % 200 < 40) || (t >= 1000 && t < 1600))
            left = right = experience_word(80, 64, 0, 64);
    } else if (scene == 5) {
        unsigned fm = 32 + (unsigned)(64.0f * (float)ms / EXPERIENCE_MS);
        left = right = experience_word(64, fm, 48, fm);
    } else if (scene == 6) {
        left = right = (ms / 100) % 2 ? experience_word(64, 64, 64, 64) :
            experience_word(64, 32, 64, 96);
    }
    out[0] = 13; out[1] = 10; out[5] = 0x10;
    for (unsigned byte = 0; byte < 4; ++byte) {
        out[7 + byte] = (uint8_t)(left >> (byte * 8));
        out[11 + byte] = (uint8_t)(right >> (byte * 8));
    }
    return 69;
}
