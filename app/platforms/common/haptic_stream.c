#include "platform/haptic_stream.h"
#include <math.h>
#include <string.h>

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16;
}
static sl_hd_value silence(void) {
    return (sl_hd_value){0, 160, 0, 320};
}
bool sl_haptic_valid_command(const uint8_t *b, size_t size) {
    return b && size && ((b[0] == 1 && (size == 9 || size == 21)) ||
        (b[0] == 13 && size == 69 && le32(b + 1) == 10 && b[5] == 0x10));
}
void sl_haptic_init(sl_haptic_stream *s) {
    memset(s, 0, sizeof(*s));
    sl_hd_reset(&s->decoder[0]);
    sl_hd_reset(&s->decoder[1]);
    s->current.values[0] = s->current.values[1] = silence();
}
void sl_haptic_stop(sl_haptic_stream *s, uint64_t now) {
    sl_haptic_stats stats = s->stats;
    sl_haptic_init(s);
    s->stats = stats;
    s->current.due_ns = now;
    s->current.sequence = stats.commands;
    s->dirty = true;
}
static void seed(sl_hd_state *s, const sl_hd_value *v) {
    const float amps[] = {v->amp_low, v->amp_high};
    for (unsigned i = 0; i < 2; ++i) {
        int a = amps[i] > 0 ? (int)lroundf(log2f(amps[i]) * 32) : -256;
        s->amplitude[i] = a < -256 ? -256 : a > 0 ? 0 : a;
        s->frequency[i] = 0; /* ordinary path uses 160/320 Hz */
    }
}
bool sl_haptic_command(sl_haptic_stream *s, const uint8_t *b, size_t size,
                       uint64_t received, uint64_t now, bool paired) {
    if (!sl_haptic_valid_command(b, size))
        goto reject;
    if (b[0] == 1 && (size == 9 || size == 21)) {
        float low = le16(b + 1) / 65535.0f, high = le16(b + 3) / 65535.0f;
        uint32_t ms = le32(b + 5);
        s->count = s->head = 0;
        s->native_epoch = false;
        ++s->stats.commands;
        s->current = (sl_haptic_frame){.values = {{low, 160, high, 320},
                                                  {low, 160, high, 320}},
            .due_ns = now, .received_ns = received, .sequence = s->stats.commands};
        if (paired) {
            s->current.values[0].amp_high = 0;
            s->current.values[1].amp_low = 0;
        }
        seed(&s->decoder[0], &s->current.values[0]);
        seed(&s->decoder[1], &s->current.values[1]);
        s->expires_ns = ms && (low || high) ? now + (uint64_t)ms * 1000000 : 0;
        s->dirty = true;
        return true;
    }
    if (b[0] != 13 || size != 69 || le32(b + 1) != 10)
        goto reject;
    sl_hd_state decoded[2] = {s->decoder[0], s->decoder[1]};
    sl_hd_samples samples[2];
    if (!sl_hd_report(decoded, b + 5, 10, samples))
        goto reject;
    unsigned count = samples[0].count > samples[1].count ? samples[0].count : samples[1].count;
    if (s->count + count > SL_HAPTIC_QUEUE) {
        ++s->stats.overflows;
        s->stats.dropped_samples += count;
        /* The wire state advanced even though playback could not. Retain that
         * state so later relative packets decode correctly. Outputs are always
         * absolute, and this discontinuity remains an explicit verification
         * failure rather than poisoning every subsequent packet. */
        memcpy(s->decoder, decoded, sizeof(decoded));
        ++s->stats.commands;
        ++s->stats.native_commands;
        s->stats.decoded_samples += count;
        goto reject;
    }
    ++s->stats.commands;
    ++s->stats.native_commands;
    if (!s->native_epoch) {
        s->source_epoch = received;
        s->playback_epoch = now;
        s->native_epoch = true;
    }
    uint64_t due = s->playback_epoch + (received > s->source_epoch ? received - s->source_epoch : 0);
    if (due < now)
        due = now;
    if (s->last_sample_ns && due < s->last_sample_ns + SL_HAPTIC_SAMPLE_NS)
        due = s->last_sample_ns + SL_HAPTIC_SAMPLE_NS;
    if (s->count) {
        uint64_t end = s->queue[(s->head + s->count - 1) % SL_HAPTIC_QUEUE].due_ns;
        if (due < end + SL_HAPTIC_SAMPLE_NS)
            due = end + SL_HAPTIC_SAMPLE_NS;
    }
    sl_hd_value hold[2];
    sl_hd_current(&s->decoder[0], &hold[0]);
    sl_hd_current(&s->decoder[1], &hold[1]);
    for (unsigned i = 0; i < count; ++i) {
        sl_haptic_frame f = {.due_ns = due + i * SL_HAPTIC_SAMPLE_NS,
            .received_ns = received, .sequence = s->stats.commands, .sample = i, .native = true};
        for (unsigned motor = 0; motor < 2; ++motor) {
            unsigned n = samples[motor].count;
            f.values[motor] = n ? samples[motor].values[i < n ? i : n - 1] : hold[motor];
        }
        s->queue[(s->head + s->count++) % SL_HAPTIC_QUEUE] = f;
    }
    memcpy(s->decoder, decoded, sizeof(decoded));
    s->stats.decoded_samples += count;
    s->expires_ns = 0;
    return true;
reject:
    ++s->stats.rejected;
    return false;
}

bool sl_haptic_advance(sl_haptic_stream *s, uint64_t now, sl_haptic_frame *out) {
    if (s->expires_ns && now >= s->expires_ns)
        sl_haptic_stop(s, now);
    if (s->dirty) {
        s->dirty = false;
        *out = s->current;
        return true;
    }
    if (!s->count || s->queue[s->head].due_ns > now)
        return false;
    s->current = s->queue[s->head];
    s->head = (s->head + 1) % SL_HAPTIC_QUEUE;
    --s->count;
    uint64_t late = now - s->current.due_ns;
    if (late > s->stats.max_lateness_ns)
        s->stats.max_lateness_ns = late;
    if (late >= 1000000)
        ++s->stats.late_samples;
    if (late) {
        for (unsigned i = 0; i < s->count; ++i)
            s->queue[(s->head + i) % SL_HAPTIC_QUEUE].due_ns += late;
        s->playback_epoch += late;
    }
    ++s->stats.emitted_samples;
    s->last_sample_ns = now;
    *out = s->current;
    return true;
}

uint64_t sl_haptic_deadline(const sl_haptic_stream *s) {
    if (s->dirty)
        return 1;
    uint64_t next = s->count ? s->queue[s->head].due_ns : 0;
    if (s->expires_ns && (!next || s->expires_ns < next))
        next = s->expires_ns;
    return next;
}
