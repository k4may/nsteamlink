#include "platform/sony_haptics.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void checked(sl_sony_state *s, uint8_t p[47], sl_sony_effect *out) {
    assert(sl_sony_effect_parse(s, p, 47, out));
    assert(memcmp(out->raw, p, 47) == 0);
}

int main(void) {
    sl_sony_state s;
    sl_sony_effect e;
    uint8_t p[47] = {0};
    sl_sony_reset(&s);

    /* A trigger-only payload from SetDualSenseTriggerEffect contains no
     * body waveform. Distinguish both 11-byte sides and preserve all bytes. */
    p[0] = 0x0c;
    p[10] = 0x26;
    p[21] = 0x21;
    p[20] = 0xa5;
    p[31] = 0x5a;
    checked(&s, p, &e);
    assert(!e.rumble_update && e.rumble_stop && !e.low && !e.high);
    assert(e.rumble_mode == SL_SONY_RUMBLE_AUDIO);
    assert(e.categories == (SL_SONY_TRIGGER_LEFT | SL_SONY_TRIGGER_RIGHT | SL_SONY_AUDIO_CONTROL));
    assert(e.unsupported == (SL_SONY_UNSUPPORTED_TRIGGER | SL_SONY_UNSUPPORTED_AUDIO));
    assert(e.trigger[0].kind == SL_SONY_TRIGGER_FEEDBACK && e.trigger[0].bytes[10] == 0x5a);
    assert(e.trigger[1].kind == SL_SONY_TRIGGER_VIBRATION && e.trigger[1].bytes[10] == 0xa5);

    /* Fields are validity-gated. Dirty trigger/motor/power bytes in a light
     * update cannot start motors or overwrite retained trigger states. */
    memset(p, 0xff, sizeof(p));
    p[0] = 0;
    p[1] = 0x04;
    p[38] = 0;
    p[40] = 0;
    checked(&s, p, &e);
    assert(!e.rumble_update && e.rumble_stop);
    assert(e.power_level == 0 && e.trigger[1].bytes[10] == 0xa5);
    assert(e.categories == (SL_SONY_LIGHT_CONTROL | SL_SONY_AUDIO_CONTROL));

    /* Old and enhanced emulated-rumble encodings are separate modes. The
     * 8-bit values are source intent; never double legacy levels or infer PCM. */
    memset(p, 0, sizeof(p));
    p[0] = 0x03;
    p[2] = 0x21;
    p[3] = 0x81;
    checked(&s, p, &e);
    assert(e.rumble_update && e.rumble_playable && e.rumble_mode == SL_SONY_RUMBLE_LEGACY);
    assert(e.low == 0x8181 && e.high == 0x2121);
    p[0] = 0x02;
    p[38] = 0x04;
    p[2] = 0xff;
    p[3] = 0;
    checked(&s, p, &e);
    assert(e.rumble_mode == SL_SONY_RUMBLE_ENHANCED && e.rumble_playable);
    assert(e.low == 0 && e.high == 65535);

    /* Explicit zero motor commands stop. SDL also deliberately restores audio
     * haptics when mode bits are absent. An otherwise trigger-only packet
     * therefore ends earlier Sony body playback without inventing PCM. */
    p[2] = 0;
    checked(&s, p, &e);
    assert(e.rumble_update && e.rumble_playable && !e.low && !e.high);
    memset(p, 0, sizeof(p));
    p[0] = 0x04;
    p[10] = 0x05;
    checked(&s, p, &e);
    assert(e.rumble_update && e.rumble_stop && !e.low && !e.high);
    assert(e.trigger[1].kind == SL_SONY_TRIGGER_OFF);
    checked(&s, p, &e);
    assert(!e.rumble_update); /* No earlier Sony body effect remains. */

    /* Unknown trigger opcodes are retained and reported, never fed into a
     * body-rumble fallback. Missing the trigger-enable flag preserves state. */
    p[10] = 0xfe;
    checked(&s, p, &e);
    assert(!e.rumble_update && e.trigger[1].kind == SL_SONY_TRIGGER_UNKNOWN);
    assert(e.trigger[1].bytes[0] == 0xfe && (e.unsupported & SL_SONY_UNSUPPORTED_TRIGGER));
    p[0] = 0;
    p[10] = 0x25;
    checked(&s, p, &e);
    assert(e.trigger[1].bytes[0] == 0xfe);

    /* Packet-level HAPTICS_SELECT decides the route. Missing selection hands
     * off to audio; competing emulation flags are not safe positive rumble. */
    memset(p, 0, sizeof(p));
    p[0] = 1;
    p[2] = 200;
    checked(&s, p, &e);
    assert(!e.rumble_update && e.rumble_stop && !e.low && !e.high);
    assert(e.unsupported & SL_SONY_UNSUPPORTED_ROUTING);
    p[0] = 3;
    p[38] = 4;
    checked(&s, p, &e);
    assert(!e.rumble_playable);
    p[38] = 0;
    checked(&s, p, &e);
    assert(e.rumble_playable);

    /* Nonzero, unverified power reduction is not silently ignored or treated
     * as dB. Its valid flag controls persistent state, not arbitrary byte36. */
    p[1] = 0x40;
    p[36] = 0x27;
    checked(&s, p, &e);
    assert(!e.rumble_playable && e.power_level == 0x27);
    assert(e.unsupported & SL_SONY_UNSUPPORTED_POWER);
    p[1] = 0;
    p[36] = 0;
    checked(&s, p, &e);
    assert(!e.rumble_playable && e.power_level == 0x27);
    p[1] = 0x40;
    checked(&s, p, &e);
    assert(e.rumble_playable && e.high == 200u * 257u);

    /* Mode-less gain updates also return to audio, rather than restarting old
     * motors. Power state stays validity-gated for later explicit rumble. */
    p[0] = 0;
    p[36] = 0x71;
    checked(&s, p, &e);
    assert(e.rumble_update && e.rumble_stop && !e.low && !e.high);
    assert(e.unsupported & SL_SONY_UNSUPPORTED_POWER);
    p[36] = 0;
    checked(&s, p, &e);
    assert(!e.rumble_update && e.rumble_stop && !e.high);
    checked(&s, p, &e);
    assert(!e.rumble_update); /* No prior Sony body effect to stop/retrigger. */

    /* Audio routing/volume controls are not samples. An audio-only report
     * never revives old rumble. Unknown nonzero routing can block subsequent
     * emulated-rumble adaptation until explicitly reset. */
    p[1] = 0x82;
    p[9] = 0x10;
    p[37] = 0x7f;
    checked(&s, p, &e);
    assert(e.rumble_stop && (e.categories & SL_SONY_AUDIO_CONTROL));
    assert(!e.rumble_update);
    assert(e.unsupported & SL_SONY_UNSUPPORTED_AUDIO);
    p[9] = 0x80;
    checked(&s, p, &e);
    assert(e.rumble_stop && (e.unsupported & SL_SONY_UNSUPPORTED_ROUTING));
    p[9] = 0;
    checked(&s, p, &e);
    assert(e.rumble_stop && !e.rumble_update);

    /* Reference SDL UpdateEffects patterns: nonzero ctx rumble is re-added
     * even for LED updates; zero ctx produces LED-only flags and restores
     * audio. Repeat the actual transition after a NONZERO Sony body command,
     * so a stale motor hold is observable (a prior zero alone would hide it). */
    memset(p, 0, sizeof(p));
    p[0] = 3;
    p[1] = 4;
    p[2] = 7;
    p[3] = 63;
    checked(&s, p, &e);
    assert(e.rumble_update && !e.rumble_stop && e.low == 63u * 257u);
    p[0] = 0;
    p[2] = p[3] = 0;
    checked(&s, p, &e);
    assert(e.rumble_update && e.rumble_stop && !e.low && !e.high);
    assert(e.rumble_mode == SL_SONY_RUMBLE_AUDIO && !s.rumble_known);
    checked(&s, p, &e);
    assert(!e.rumble_update && e.rumble_stop);

    /* RumbleStart in SDL can set only HAPTICS_SELECT. It is not permission
     * to keep cached motors active while disabling the audio route. */
    p[0] = 3;
    p[3] = 80;
    checked(&s, p, &e);
    p[0] = 2;
    p[3] = 0;
    checked(&s, p, &e);
    assert(e.rumble_update && e.rumble_stop && !e.low && !e.high);
    assert(e.rumble_mode == SL_SONY_RUMBLE_DISABLED);

    /* Unknown future bits cannot silently become known vibration. The raw
     * packet remains available; a new device binding resets its state. */
    p[0] = 3;
    p[38] = 0x08;
    checked(&s, p, &e);
    assert(!e.rumble_playable && (e.unsupported & SL_SONY_UNSUPPORTED_UNKNOWN));
    sl_sony_reset(&s);
    p[0] = 3;
    p[38] = 0;
    checked(&s, p, &e);
    assert(e.rumble_playable);

    /* Every short/long length, null argument and malformed Steam envelope
     * must leave state AND caller output unchanged. No OOB reads permitted. */
    sl_sony_state before = s;
    sl_sony_effect old = e;
    for (size_t n = 0; n < 70; ++n) {
        if (n == 47) continue;
        assert(!sl_sony_effect_parse(&s, p, n, &e));
        assert(memcmp(&s, &before, sizeof(s)) == 0 && memcmp(&e, &old, sizeof(e)) == 0);
    }
    assert(!sl_sony_effect_parse(NULL, p, 47, &e));
    assert(!sl_sony_effect_parse(&s, NULL, 47, &e));
    assert(!sl_sony_effect_parse(&s, p, 47, NULL));
    uint8_t cmd[69] = {0x0d, 47};
    memcpy(cmd + 5, p, 47);
    assert(sl_sony_command_parse(&s, cmd, sizeof(cmd), &e));
    before = s;
    old = e;
    for (unsigned i = 0; i < sizeof(cmd); ++i) {
        uint8_t bad[69];
        memcpy(bad, cmd, sizeof(bad));
        if (i >= 5 && i < 52) continue;
        bad[i] ^= 1;
        assert(!sl_sony_command_parse(&s, bad, sizeof(bad), &e));
        assert(memcmp(&s, &before, sizeof(s)) == 0 && memcmp(&e, &old, sizeof(e)) == 0);
    }
    assert(!sl_sony_command_parse(&s, cmd, 68, &e));
    assert(!sl_sony_command_parse(&s, cmd, 70, &e));
    puts("Sony effect flags, state isolation, unsupported effects and bounds passed");
    return 0;
}
