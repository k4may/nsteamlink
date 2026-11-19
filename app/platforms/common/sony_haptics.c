#include "platform/sony_haptics.h"
#include <string.h>

/* Evidence: SDL3 SDL_hidapi_ps5.c DS5EffectsState_t and UpdateEffects /
 * InternalSendJoystickEffect; Linux hid-playstation.c's Sony-authored common
 * 47-byte output report; Microsoft GameInput DualSenseHelper.h's 11-byte
 * trigger modes. Steam Generic slot 23 (November SHA 669fa08e..., entry
 * 0x138693e70) wraps exactly this 47-byte state in DeviceWrite 0x0d/69B.
 * SDL deliberately calls offsets 32..37 unknown. In particular, third-party
 * descriptions disagree about byte 36 power nibbles: retain that byte and
 * its flag, but DO NOT invent a calibrated gain/dB mapping for it.
 * References (protocol facts only, no external implementation copied):
 * github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_ps5.c
 * github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c
 * github.com/microsoftconnect/GameInput/blob/main/companion/DualSense/DualSenseHelper.h
 * Power-field conflict: awalol/DS5Dongle 6c0ed6ec49bf6fe09e1e4cc803f56ebd9f310478
 * src/utils.h versus tools/wireshark_dualsense_setstate.lua. */

static enum sl_sony_trigger_kind trigger_kind(uint8_t mode) {
    switch (mode) {
        case 0x05: return SL_SONY_TRIGGER_OFF;
        case 0x21: return SL_SONY_TRIGGER_FEEDBACK;
        case 0x25: return SL_SONY_TRIGGER_WEAPON;
        case 0x26: return SL_SONY_TRIGGER_VIBRATION;
        default: return SL_SONY_TRIGGER_UNKNOWN;
    }
}

void sl_sony_reset(sl_sony_state *state) {
    if (state) memset(state, 0, sizeof(*state));
}

bool sl_sony_effect_parse(sl_sony_state *state, const uint8_t *p, size_t size,
                          sl_sony_effect *out) {
    if (!state || !p || !out || size != SL_SONY_EFFECT_SIZE) return false;

    sl_sony_state next = *state;
    sl_sony_effect result = {0};
    memcpy(next.raw, p, sizeof(next.raw));
    memcpy(result.raw, p, sizeof(result.raw));
    result.flags[0] = p[0];
    result.flags[1] = p[1];
    result.flags[2] = p[38];

    const bool legacy = (p[0] & 0x01) != 0;
    const bool enhanced = (p[38] & 0x04) != 0;
    const bool select_rumble = (p[0] & 0x02) != 0;
    /* SDL zeroes the full effect state for every UpdateEffects call, then
     * re-adds mode/motor bytes if ctx->rumble_left/right is nonzero, even for
     * a LED update. Its explicit comment says leaving emulation bits off
     * restores audio haptics. Therefore this selection must NOT be treated
     * like the trigger/LED validity flags below: a mode-less packet ends
     * the previous emulated body effect, rather than retaining it forever.
     * Linux's independently constructed LED packets don't prove persistent
     * body selection. This implements the Steam/SDL effect contract, not a
     * claim of experimentally verified physical firmware behavior. */
    const bool had_body = state->rumble_known;
    next.routing_unknown = (legacy && enhanced) || (!select_rumble && (legacy || enhanced));
    if (select_rumble && (legacy || enhanced)) {
        result.categories |= SL_SONY_RUMBLE;
        result.rumble_update = true;
        next.rumble_known = true;
        /* Both the mode enable and HAPTICS_SELECT are required by the Sony
         * Linux producer and SDL producer. Conflicting/missing mode bits are
         * not a usable body-rumble command. Preserve the literal bytes. */
        next.motor_left = p[3];
        next.motor_right = p[2];
        next.rumble_mode = enhanced ? SL_SONY_RUMBLE_ENHANCED : SL_SONY_RUMBLE_LEGACY;
    } else {
        /* SDL uses this during rumble-mode setup, without motor enable bits.
         * A handoff to audio never supplies new PCM samples in this report. */
        next.motor_left = next.motor_right = 0;
        next.rumble_known = false;
        next.rumble_mode = select_rumble ? SL_SONY_RUMBLE_DISABLED : SL_SONY_RUMBLE_AUDIO;
        result.rumble_stop = true;
        result.rumble_update = had_body;
        result.categories |= SL_SONY_AUDIO_CONTROL;
        result.unsupported |= SL_SONY_UNSUPPORTED_AUDIO;
    }

    for (unsigned side = 0; side < 2; ++side) {
        const uint8_t flag = side == 0 ? 0x08 : 0x04;
        if (!(p[0] & flag)) continue;
        sl_sony_trigger *t = &next.trigger[side];
        memcpy(t->bytes, p + (side == 0 ? 21 : 10), SL_SONY_TRIGGER_SIZE);
        t->kind = trigger_kind(t->bytes[0]);
        t->known = true;
        result.categories |= side == 0 ? SL_SONY_TRIGGER_LEFT : SL_SONY_TRIGGER_RIGHT;
        /* Even a recognized vibration-mode trigger is a different actuator
         * contract. Switch body LRAs cannot reproduce trigger resistance. */
        result.unsupported |= SL_SONY_UNSUPPORTED_TRIGGER;
    }

    if ((p[0] & 0xf0) || (p[1] & 0x82)) {
        result.categories |= SL_SONY_AUDIO_CONTROL;
        result.unsupported |= SL_SONY_UNSUPPORTED_AUDIO;
    }
    if ((p[1] & 0x1d) || (p[38] & 0x03)) {
        result.categories |= SL_SONY_LIGHT_CONTROL;
        result.unsupported |= SL_SONY_UNSUPPORTED_LIGHT;
    }
    if (p[1] & 0x02) {
        next.mute_control = p[9];
    }
    if (p[1] & 0x40) {
        next.power_level = p[36];
        result.categories |= SL_SONY_POWER_CONTROL;
        /* Zero is the only unambiguous no-reduction value across the
         * conflicting references. Persistent nonzero power state blocks the
         * body adapter until explicitly reset; absent validity flag MUST NOT
         * apply arbitrary bytes or silently reset an earlier control. */
    }
    if (p[1] & 0x20) {
        next.haptic_filter = p[39];
        result.categories |= SL_SONY_UNKNOWN_CONTROL;
    }
    if (p[38] & 0xf8 || p[40]) {
        next.unknown_control = true;
        result.categories |= SL_SONY_UNKNOWN_CONTROL;
        result.unsupported |= SL_SONY_UNSUPPORTED_UNKNOWN;
    }

    result.motor_left = next.motor_left;
    result.motor_right = next.motor_right;
    result.rumble_mode = next.rumble_mode;
    result.power_level = next.power_level;
    result.haptic_filter = next.haptic_filter;
    result.mute_control = next.mute_control;
    memcpy(result.trigger, next.trigger, sizeof(result.trigger));
    if (next.power_level) result.unsupported |= SL_SONY_UNSUPPORTED_POWER;
    if (next.haptic_filter || next.unknown_control)
        result.unsupported |= SL_SONY_UNSUPPORTED_UNKNOWN;
    /* The non-microphone mute/power-save bits are preserved as audio/device
     * controls, not re-labelled as motor amplitudes. They need independent
     * device validation, so don't play an approximation while they are set. */
    if (next.mute_control & (uint8_t)~0x10u)
        result.unsupported |= SL_SONY_UNSUPPORTED_ROUTING;
    if (next.routing_unknown) result.unsupported |= SL_SONY_UNSUPPORTED_ROUTING;
    result.rumble_playable = result.rumble_stop || (next.rumble_known &&
        !(result.unsupported & (SL_SONY_UNSUPPORTED_POWER | SL_SONY_UNSUPPORTED_ROUTING |
                                SL_SONY_UNSUPPORTED_UNKNOWN)));
    if (result.rumble_playable && !result.rumble_stop) {
        result.low = (uint16_t)(next.motor_left * 257u);
        result.high = (uint16_t)(next.motor_right * 257u);
    }
    *state = next;
    *out = result;
    return true;
}

bool sl_sony_command_parse(sl_sony_state *state, const uint8_t *p, size_t size,
                           sl_sony_effect *out) {
    if (!p || size != 69 || p[0] != 0x0d || p[1] != SL_SONY_EFFECT_SIZE ||
        p[2] || p[3] || p[4]) return false;
    for (size_t i = 5 + SL_SONY_EFFECT_SIZE; i < size; ++i)
        if (p[i]) return false;
    return sl_sony_effect_parse(state, p + 5, SL_SONY_EFFECT_SIZE, out);
}
