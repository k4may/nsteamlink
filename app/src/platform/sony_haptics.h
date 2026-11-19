#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SL_SONY_EFFECT_SIZE 47u
#define SL_SONY_TRIGGER_SIZE 11u

enum sl_sony_category {
    SL_SONY_RUMBLE = 1u << 0,
    SL_SONY_TRIGGER_RIGHT = 1u << 1,
    SL_SONY_TRIGGER_LEFT = 1u << 2,
    SL_SONY_AUDIO_CONTROL = 1u << 3,
    SL_SONY_LIGHT_CONTROL = 1u << 4,
    SL_SONY_POWER_CONTROL = 1u << 5,
    SL_SONY_UNKNOWN_CONTROL = 1u << 6
};

enum sl_sony_unsupported {
    SL_SONY_UNSUPPORTED_TRIGGER = 1u << 0,
    SL_SONY_UNSUPPORTED_AUDIO = 1u << 1,
    SL_SONY_UNSUPPORTED_LIGHT = 1u << 2,
    SL_SONY_UNSUPPORTED_POWER = 1u << 3,
    SL_SONY_UNSUPPORTED_ROUTING = 1u << 4,
    SL_SONY_UNSUPPORTED_UNKNOWN = 1u << 5
};

enum sl_sony_rumble_mode {
    SL_SONY_RUMBLE_NONE,
    SL_SONY_RUMBLE_LEGACY,
    SL_SONY_RUMBLE_ENHANCED,
    SL_SONY_RUMBLE_AUDIO, /* Audio-haptics handoff; no PCM in this packet. */
    SL_SONY_RUMBLE_DISABLED
};

enum sl_sony_trigger_kind {
    SL_SONY_TRIGGER_UNKNOWN,
    SL_SONY_TRIGGER_OFF,
    SL_SONY_TRIGGER_FEEDBACK,
    SL_SONY_TRIGGER_WEAPON,
    SL_SONY_TRIGGER_VIBRATION
};

typedef struct sl_sony_trigger {
    uint8_t bytes[SL_SONY_TRIGGER_SIZE];
    enum sl_sony_trigger_kind kind; /* Mode identification, not force emulation. */
    bool known;
} sl_sony_trigger;

/* Per-device source state, not Nintendo relative-decoder history. All control
 * bytes are retained even when they cannot be rendered by Switch actuators. */
typedef struct sl_sony_state {
    uint8_t raw[SL_SONY_EFFECT_SIZE];
    uint8_t motor_left, motor_right;
    uint8_t power_level, haptic_filter, mute_control;
    enum sl_sony_rumble_mode rumble_mode;
    bool rumble_known, routing_unknown, unknown_control;
    sl_sony_trigger trigger[2]; /* 0 left, 1 right */
} sl_sony_state;

typedef struct sl_sony_effect {
    uint8_t raw[SL_SONY_EFFECT_SIZE];
    uint8_t flags[3];
    uint32_t categories, unsupported;
    enum sl_sony_rumble_mode rumble_mode;
    bool rumble_update, rumble_playable, rumble_stop;
    uint8_t motor_left, motor_right;
    /* Literal 8-bit source levels expanded to 16-bit. These are ordinary
     * emulated-rumble levels, not independent Sony waveform samples or a
     * physical-amplitude calibration. No speculative gain compensation. */
    uint16_t low, high;
    uint8_t power_level, haptic_filter, mute_control;
    sl_sony_trigger trigger[2];
} sl_sony_effect;

void sl_sony_reset(sl_sony_state *state);

/* Exact DS5EffectsState_t payload, with no USB report ID or Bluetooth framing.
 * False leaves both state and out unchanged. No allocation, clocks or I/O.
 * True means structurally accepted, not that every requested feature can play.
 * Haptics selection is a packet-level mode, NOT an omitted-field keepalive.
 * Missing rumble mode bits returns to audio haptics and ends an earlier Sony
 * body effect, including otherwise trigger-only or LED-only effect packets.
 * A handoff without an earlier Sony body effect has rumble_update=false;
 * the caller must additionally guard current output ownership so an old Sony
 * effect cannot stop a newer ordinary/Nintendo effect on the same device.
 * Apply a body-rumble update only if BOTH rumble_update and rumble_playable;
 * rumble_stop explicitly identifies a supported silent handoff/disable.
 * A non-playable update requires the caller to stop/reject that body effect,
 * never to reinterpret trigger/audio/unknown bytes as ordinary vibration.
 * There are NO dedicated PCM samples in this 47-byte state/control report. */
bool sl_sony_effect_parse(sl_sony_state *state, const uint8_t *payload, size_t size,
                          sl_sony_effect *out);

/* Steam Generic DeviceWrite 69-byte envelope: 0x0d, LE32(47), payload, zero pad.
 * Does not accept Nintendo len=10, raw USB or Bluetooth packets. */
bool sl_sony_command_parse(sl_sony_state *state, const uint8_t *command, size_t size,
                           sl_sony_effect *out);
