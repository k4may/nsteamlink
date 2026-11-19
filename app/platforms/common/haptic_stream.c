#include "platform/haptic_stream.h"
#include <string.h>
#include <math.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16; }
static uint64_t add(uint64_t a, uint64_t b) { return UINT64_MAX - a < b ? UINT64_MAX : a + b; }
static sl_hd_value silence(void) { return (sl_hd_value){0, 160, 0, 320}; }
static bool sampled(const sl_haptic_frame *f) { return f->native || f->source == SL_HAPTIC_PCM; }
static sl_haptic_frame *at(sl_haptic_stream *s, unsigned n) {
    return &s->queue[(s->head + n) % SL_HAPTIC_QUEUE];
}
static void compose(sl_haptic_stream *s, sl_haptic_frame *f) {
    for (unsigned i = 0; i < 2; ++i) {
        f->values[i] = s->body[i];
        /* Switch has no trigger motor. Its explicitly labelled conversion
         * uses the corresponding hand's high band, never fictitious force. */
        if (s->triggers[i].amp_high > f->values[i].amp_high) {
            f->values[i].amp_high = s->triggers[i].amp_high;
            f->values[i].freq_high = s->triggers[i].freq_high;
        }
    }
}
bool sl_haptic_valid_command(const uint8_t *b, size_t size) {
    return b && size && (((b[0] == 1 || b[0] == 6) && (size == 9 || size == 21)) ||
        (b[0] == 13 && size == 69 && ((le32(b + 1) == 10 && b[5] == 0x10) ||
                                      le32(b + 1) == SL_SONY_EFFECT_SIZE)));
}
void sl_haptic_init(sl_haptic_stream *s) {
    memset(s, 0, sizeof(*s));
    for (unsigned i = 0; i < 2; ++i) {
        sl_hd_reset(&s->decoder[i]);
        s->body[i] = s->triggers[i] = s->current.values[i] = silence();
    }
}
void sl_haptic_stop(sl_haptic_stream *s, uint64_t now) {
    sl_haptic_stats stats = s->stats;
    for (unsigned i = 0; i < s->count; ++i)
        stats.superseded_samples += at(s, i)->native;
    sl_haptic_init(s);
    s->stats = stats;
    s->current.due_ns = now;
    s->current.sequence = stats.commands;
    s->current.source = SL_HAPTIC_RESET;
    s->dirty = true;
}
void sl_haptic_discontinuity(sl_haptic_stream *s, uint64_t now) {
    sl_haptic_stop(s, now);
    ++s->stats.discontinuities;
    s->desynchronized = true;
}
/* Preserve source gaps for ordinary envelopes. Native samples retain their
 * 5ms minimum, with idle gaps absorbing worker jitter as before. */
static void space_pending(sl_haptic_stream *s, uint64_t after) {
    uint64_t previous_source = s->current.source_ns;
    bool native = sampled(&s->current);
    for (unsigned i = 0; i < s->count; ++i) {
        sl_haptic_frame *f = at(s, i);
        uint64_t gap = native && f->source != SL_HAPTIC_TRIGGERS ? SL_HAPTIC_SAMPLE_NS :
            (f->source_ns > previous_source ? f->source_ns - previous_source : 0);
        uint64_t next = add(after, gap);
        if (f->due_ns < next) f->due_ns = next;
        after = f->due_ns;
        previous_source = f->source_ns;
        native = sampled(f);
    }
}
static uint64_t map_time(sl_haptic_stream *s, uint64_t received, uint64_t now) {
    /* A fresh batch gets its own bounded playback anchor. Once queued, all
     * events in that batch keep their original receive-time differences. */
    /* Queue emptiness is not a batch boundary: the output worker may take
     * the first frame while the owner is still draining older arrivals.
     * Only an arrival newer than that drain's initial local cutoff starts
     * a fresh idle anchor. This also lets genuine idle gaps absorb jitter. */
    if (!s->count && (!s->source_epoch_valid || received > s->batch_cutoff_ns)) {
        s->source_epoch = received;
        s->playback_epoch = now;
        s->batch_cutoff_ns = now;
        s->source_epoch_valid = true;
    }
    uint64_t due = add(s->playback_epoch,
        received > s->source_epoch ? received - s->source_epoch : 0);
    return due < now ? now : due;
}
static bool enqueue(sl_haptic_stream *s, const sl_haptic_frame *frame) {
    if (s->count == SL_HAPTIC_QUEUE) return false;
    unsigned n = s->count++;
    while (n && at(s, n - 1)->due_ns > frame->due_ns) {
        *at(s, n) = *at(s, n - 1);
        --n;
    }
    *at(s, n) = *frame;
    return true;
}
static void overflow_stop(sl_haptic_stream *s, uint64_t now, unsigned dropped) {
    /* Wire history advanced even if playback cannot keep up. Retain that
     * history, count the discontinuity, and stop instead of losing a stop
     * command behind a full queue or leaving the last intensity running. */
    sl_hd_state decoder[2];
    memcpy(decoder, s->decoder, sizeof(decoder));
    sl_sony_state sony = s->sony;
    uint64_t expiry = s->wire_expires_ns;
    bool desynchronized = s->desynchronized;
    ++s->stats.overflows;
    s->stats.dropped_frames += s->count + dropped;
    for (unsigned i = 0; i < s->count; ++i) s->stats.dropped_samples += at(s, i)->native;
    s->count = 0; /* Loss and intentional replacement are separate counters. */
    sl_haptic_stop(s, now);
    memcpy(s->decoder, decoder, sizeof(decoder));
    s->sony = sony;
    s->wire_expires_ns = expiry;
    s->desynchronized = desynchronized;
}
/* A new body state supersedes future samples of an older Nintendo packet,
 * while retaining already received earlier events. Trigger updates do not. */
static void replace_future_native(sl_haptic_stream *s, uint64_t received) {
    unsigned kept = 0;
    for (unsigned i = 0; i < s->count; ++i) {
        sl_haptic_frame f = *at(s, i);
        if (f.native && f.source_ns >= received) {
            ++s->stats.superseded_samples;
        } else {
            *at(s, kept++) = f;
        }
    }
    s->count = kept;
}
static bool absolute_report(const uint8_t *p) {
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t w = le32(p + 2 + i * 4);
        if (w >> 30 != 1 || (w & 3) || (w & 0xfffff) == 0) return false;
    }
    return true;
}
static void seed_ordinary(sl_haptic_stream *s, uint16_t low, uint16_t high, sl_rumble_layout layout) {
    for (unsigned i = 0; i < 2; ++i) {
        sl_hd_samples mapped;
        bool paired = layout == SL_RUMBLE_LAYOUT_PAIRED;
        sl_hd_decode(&s->decoder[i], sl_hd_ordinary_word(paired && i == 1 ? 0 : low,
                     paired && i == 0 ? 0 : high), &mapped);
    }
    s->desynchronized = false;
}
bool sl_haptic_command_layout(sl_haptic_stream *s, const uint8_t *b, size_t size,
                              uint64_t received, uint64_t now, sl_rumble_layout layout) {
    s->last_unsupported = 0;
    if (layout < SL_RUMBLE_LAYOUT_PRO || layout > SL_RUMBLE_LAYOUT_SINGLE_RIGHT ||
        !sl_haptic_valid_command(b, size)) goto reject;
    if (b[0] == 13 && le32(b + 1) == SL_SONY_EFFECT_SIZE) {
        sl_sony_effect effect;
        if (!sl_sony_command_parse(&s->sony, b, size, &effect)) goto reject;
        ++s->stats.commands;
        ++s->stats.sony_commands;
        s->last_unsupported = effect.unsupported;
        if (effect.unsupported) ++s->stats.unsupported_effects;
        if (!effect.rumble_update ||
            (effect.rumble_stop && s->last_body_source != SL_HAPTIC_SONY))
            return true; /* A Sony audio handoff cannot stop another source. */
        sl_haptic_frame f = {.received_ns = received, .source_ns = received,
            .sequence = s->stats.commands, .source = SL_HAPTIC_SONY};
        if (!sl_rumble_render(NULL, layout, effect.rumble_playable ? effect.low : 0,
                              effect.rumble_playable ? effect.high : 0, f.values)) goto reject;
        if (effect.rumble_playable) ++s->stats.adapted_effects;
        s->last_body_source = SL_HAPTIC_SONY;
        replace_future_native(s, received);
        f.due_ns = map_time(s, received, now);
        if (!enqueue(s, &f)) { overflow_stop(s, now, 1); goto reject; }
        return true;
    }
    if (b[0] == 1 || b[0] == 6) {
        uint16_t low = le16(b + 1), high = le16(b + 3);
        uint64_t duration = (uint64_t)le32(b + 5) * 1000000;
        bool trigger = b[0] == 6;
        sl_haptic_frame f = {.received_ns = received, .source_ns = received,
            .sequence = s->stats.commands + 1, .duration_ns = duration,
            .source = trigger ? SL_HAPTIC_TRIGGERS : SL_HAPTIC_BODY};
        if (!trigger) {
            if (!sl_rumble_render(NULL, layout, low, high, f.values)) goto reject;
            seed_ordinary(s, low, high, layout);
            s->last_body_source = SL_HAPTIC_BODY;
            s->wire_expires_ns = duration && (low || high) ? add(received, duration) : 0;
            replace_future_native(s, received);
        } else {
            f.values[0] = f.values[1] = silence();
            float peak = sl_rumble_render_default_config().high_max_amplitude;
            f.values[0].amp_high = (float)low / 65535.0f * peak;
            f.values[1].amp_high = (float)high / 65535.0f * peak;
            if (layout == SL_RUMBLE_LAYOUT_SINGLE_LEFT || layout == SL_RUMBLE_LAYOUT_SINGLE_RIGHT) {
                unsigned i = layout == SL_RUMBLE_LAYOUT_SINGLE_RIGHT;
                f.values[i].amp_high = (float)(low > high ? low : high) / 65535.0f * peak;
                f.values[1-i].amp_high = 0;
            }
        }
        f.due_ns = map_time(s, received, now);
        ++s->stats.commands;
        if (trigger) { ++s->stats.trigger_commands; ++s->stats.adapted_effects; }
        else ++s->stats.ordinary_commands;
        if (!enqueue(s, &f)) { overflow_stop(s, now, 1); goto reject; }
        return true;
    }
    if (s->desynchronized && !absolute_report(b + 5)) {
        ++s->stats.desync_rejected;
        goto reject;
    }
    sl_hd_state decoded[2] = {s->decoder[0], s->decoder[1]};
    if (s->wire_expires_ns && received >= s->wire_expires_ns)
        for (unsigned i = 0; i < 2; ++i) sl_hd_reset(&decoded[i]);
    sl_hd_samples samples[2];
    if (!sl_hd_report(decoded, b + 5, 10, samples)) goto reject;
    s->desynchronized = false;
    unsigned count = samples[0].count > samples[1].count ? samples[0].count : samples[1].count;
    if (count || (s->wire_expires_ns && received >= s->wire_expires_ns)) s->wire_expires_ns = 0;
    ++s->stats.commands;
    ++s->stats.native_commands;
    s->stats.decoded_samples += count;
    if (count) s->last_body_source = SL_HAPTIC_NINTENDO;
    memcpy(s->decoder, decoded, sizeof(decoded));
    if (s->count + count > SL_HAPTIC_QUEUE) {
        s->stats.dropped_samples += count;
        overflow_stop(s, now, count);
        goto reject;
    }
    uint64_t due = map_time(s, received, now);
    if (s->last_sample_ns && due < add(s->last_sample_ns, SL_HAPTIC_SAMPLE_NS))
        due = add(s->last_sample_ns, SL_HAPTIC_SAMPLE_NS);
    if (s->count && due < add(at(s, s->count - 1)->due_ns, SL_HAPTIC_SAMPLE_NS))
        due = add(at(s, s->count - 1)->due_ns, SL_HAPTIC_SAMPLE_NS);
    for (unsigned i = 0; i < count; ++i) {
        sl_haptic_frame f = {.due_ns = add(due, i * SL_HAPTIC_SAMPLE_NS),
            .received_ns = received, .source_ns = add(received, i * SL_HAPTIC_SAMPLE_NS),
            .sequence = s->stats.commands, .sample = i, .native = true,
            .source = SL_HAPTIC_NINTENDO};
        for (unsigned motor = 0; motor < 2; ++motor) {
            unsigned n = samples[motor].count;
            if (n) f.values[motor] = samples[motor].values[i < n ? i : n-1];
            else sl_hd_current(&decoded[motor], &f.values[motor]);
        }
        enqueue(s, &f);
    }
    return true;
reject:
    ++s->stats.rejected;
    return false;
}
bool sl_haptic_command(sl_haptic_stream *s, const uint8_t *b, size_t size,
                       uint64_t received, uint64_t now, bool paired) {
    return sl_haptic_command_layout(s, b, size, received, now,
        paired ? SL_RUMBLE_LAYOUT_PAIRED : SL_RUMBLE_LAYOUT_PRO);
}
bool sl_haptic_pcm_frame(sl_haptic_stream *s, const sl_pcm_frame *p,
                         uint64_t source_ns, uint64_t now) {
    if (!p || p->flags & ~(unsigned)(SL_PCM_FRAME_RESET | SL_PCM_FRAME_EOS) ||
        p->flags == (SL_PCM_FRAME_RESET | SL_PCM_FRAME_EOS)) goto reject;
    bool reset = (p->flags & SL_PCM_FRAME_RESET) != 0;
    if (!reset && (!s->pcm_started || s->pcm_ended || p->epoch != s->pcm_epoch ||
                   p->available_pts_ns <= s->pcm_available_pts)) goto reject;
    for (unsigned i = 0; i < 2; ++i) {
        sl_hd_value v = p->values[i];
        if (!isfinite(v.amp_low) || v.amp_low < 0 || v.amp_low > 1 ||
            !isfinite(v.amp_high) || v.amp_high < 0 || v.amp_high > 1 ||
            !isfinite(v.freq_low) || v.freq_low < 41 || v.freq_low > 626 ||
            !isfinite(v.freq_high) || v.freq_high < 82 || v.freq_high > 1253 ||
            (p->flags && (v.amp_low || v.amp_high))) goto reject;
    }
    if (reset) {
        unsigned kept = 0;
        for (unsigned i = 0; i < s->count; ++i) {
            sl_haptic_frame f = *at(s, i);
            if (f.source == SL_HAPTIC_PCM) ++s->stats.dropped_frames;
            else *at(s, kept++) = f;
        }
        s->count = kept;
    }
    s->pcm_started = true; s->pcm_epoch = p->epoch;
    s->pcm_available_pts = p->available_pts_ns;
    s->pcm_ended = (p->flags & SL_PCM_FRAME_EOS) != 0;
    replace_future_native(s, source_ns);
    sl_haptic_frame f = {.received_ns = source_ns, .source_ns = source_ns,
        .due_ns = map_time(s, source_ns, now), .source = SL_HAPTIC_PCM,
        .sequence = ++s->stats.commands};
    if (!reset && s->last_sample_ns && f.due_ns < add(s->last_sample_ns, SL_HAPTIC_SAMPLE_NS))
        f.due_ns = add(s->last_sample_ns, SL_HAPTIC_SAMPLE_NS);
    memcpy(f.values, p->values, sizeof(f.values));
    ++s->stats.pcm_frames; ++s->stats.adapted_effects;
    s->last_body_source = SL_HAPTIC_PCM;
    if (!enqueue(s, &f)) { overflow_stop(s, now, 1); goto reject; }
    return true;
reject:
    ++s->stats.rejected; return false;
}
void sl_haptic_output_complete(sl_haptic_stream *s, const sl_haptic_frame *f,
                               uint64_t submitted, uint64_t completed, bool success,
                               bool current_binding) {
    uint64_t late = submitted > f->due_ns ? submitted - f->due_ns : 0;
    if (late > s->stats.max_lateness_ns) s->stats.max_lateness_ns = late;
    if (success) ++s->stats.emitted_frames;
    if (f->native && success) {
        ++s->stats.emitted_samples;
        if (late >= 1000000) ++s->stats.late_samples;
    }
    if (!current_binding || s->current.sequence != f->sequence || s->current.sample != f->sample ||
        s->current.native != f->native || s->current.due_ns != f->due_ns || s->current.source != f->source)
        return;
    if (!success) s->dirty = true;
    else {
        /* A successful ordinary/trigger/Sony frame can itself be selected
         * late. Preserve its source gap for backlog callbacks admitted after
         * the worker emptied the queue, just as for already queued frames.
         * Do not let completion of an older in-flight batch re-anchor a new
         * live batch, or add IPC duration to the source-to-submit intervals. */
        if (!sampled(f) && f->source != SL_HAPTIC_RESET && f->source != SL_HAPTIC_EXPIRY &&
            s->source_epoch_valid && f->source_ns >= s->source_epoch) {
            s->source_epoch = f->source_ns;
            s->playback_epoch = submitted;
            if (submitted > s->batch_cutoff_ns) s->batch_cutoff_ns = submitted;
        }
        if (sampled(f) && completed > s->last_sample_ns) s->last_sample_ns = completed;
        if (f->source != SL_HAPTIC_RESET && f->source != SL_HAPTIC_TRIGGERS)
            space_pending(s, sampled(f) ? completed : submitted);
    }
}
static bool is_silent(const sl_haptic_frame *f) {
    return !f->values[0].amp_low && !f->values[0].amp_high &&
           !f->values[1].amp_low && !f->values[1].amp_high;
}
static bool stops_component(const sl_hd_value old[2], const sl_haptic_frame *f) {
    for (unsigned i = 0; i < 2; ++i)
        if ((old[i].amp_low && !f->values[i].amp_low) ||
            (old[i].amp_high && !f->values[i].amp_high)) return true;
    return false;
}
/* A failed IPC contains the composition of BOTH channels. A due stop on
 * either channel must remove that channel before retrying. For the other
 * channel's stop, keep the failed frame's identity/deadline: it is still a
 * retry, including the native sample's completion-based minimum hold. */
static bool stop_failed_output(sl_haptic_stream *s, uint64_t now, sl_haptic_frame *out) {
    bool failed_trigger = s->current.source == SL_HAPTIC_TRIGGERS;
    for (unsigned n = 0; n < s->count; ++n) {
        sl_haptic_frame f = *at(s, n);
        if (f.due_ns > now) break;
        bool trigger = f.source == SL_HAPTIC_TRIGGERS;
        sl_hd_value *values = trigger ? s->triggers : s->body;
        if (!is_silent(&f) && (sampled(&f) || !stops_component(values, &f))) continue;
        unsigned kept = 0;
        for (unsigned j = 0; j < s->count; ++j) {
            sl_haptic_frame q = *at(s, j);
            if (j <= n && (q.source == SL_HAPTIC_TRIGGERS) == trigger) {
                if (j != n) ++s->stats.dropped_frames;
                if (j != n || trigger != failed_trigger)
                    s->stats.superseded_samples += q.native;
            } else *at(s, kept++) = q;
        }
        s->count = kept;
        memcpy(values, f.values, sizeof(f.values));
        uint64_t expires = !is_silent(&f) && f.duration_ns ? add(now, f.duration_ns) : 0;
        if (trigger) {
            s->trigger_expires_ns = expires;
            s->trigger_expiry_source_ns = add(f.source_ns, f.duration_ns);
        } else {
            s->expires_ns = expires;
            s->body_expiry_source_ns = add(f.source_ns, f.duration_ns);
        }
        if (trigger != failed_trigger) {
            compose(s, &s->current);
            *out = s->current;
            return true;
        }
        s->stats.superseded_samples += s->current.native;
        compose(s, &f); s->current = f; s->dirty = false;
        ++s->stats.scheduled_frames;
        if (f.native) { ++s->stats.scheduled_samples; s->last_sample_ns = now; }
        space_pending(s, now);
        *out = f; return true;
    }
    return false;
}
bool sl_haptic_advance(sl_haptic_stream *s, uint64_t now, sl_haptic_frame *out) {
    /* Expiry is evaluated against actual playback time before composing any
     * partial update, even if that update itself was delayed in the queue. */
    uint64_t expired_at = 0, expired_source = 0;
    if (s->expires_ns && s->expires_ns <= now) {
        expired_at = s->expires_ns; expired_source = s->body_expiry_source_ns;
        s->expires_ns = 0;
        s->body[0] = s->body[1] = silence();
    }
    if (s->trigger_expires_ns && s->trigger_expires_ns <= now) {
        if (!expired_at || s->trigger_expires_ns < expired_at) {
            expired_at = s->trigger_expires_ns; expired_source = s->trigger_expiry_source_ns;
        }
        s->trigger_expires_ns = 0;
        s->triggers[0] = s->triggers[1] = silence();
    }
    if (s->dirty) {
        s->dirty = false;
        bool expired = expired_at != 0;
        bool stale_retry = s->current.source != SL_HAPTIC_RESET &&
            now > add(s->current.source_ns, SL_HAPTIC_MAX_LATENCY_NS);
        if (stale_retry) {
            ++s->stats.stale_frames;
            for (unsigned i = 0; i < 2; ++i) s->body[i] = s->triggers[i] = silence();
            s->expires_ns = s->trigger_expires_ns = 0;
            s->current = (sl_haptic_frame){.due_ns = now, .source = SL_HAPTIC_RESET,
                                         .sequence = s->stats.commands};
            compose(s, &s->current); *out = s->current; return true;
        }
        if (s->current.source != SL_HAPTIC_RESET && stop_failed_output(s, now, out)) return true;
        if (!expired || s->current.source == SL_HAPTIC_RESET) { *out = s->current; return true; }
    }
    bool stale = false;
    while (s->count && now > add(at(s, 0)->source_ns, SL_HAPTIC_MAX_LATENCY_NS)) {
        ++s->stats.stale_frames;
        s->stats.dropped_samples += at(s, 0)->native;
        s->head = (s->head + 1) % SL_HAPTIC_QUEUE; --s->count;
        stale = true;
    }
    if (stale) {
        for (unsigned i = 0; i < 2; ++i) s->body[i] = s->triggers[i] = silence();
        s->expires_ns = s->trigger_expires_ns = 0;
        s->current = (sl_haptic_frame){.due_ns = now, .source = SL_HAPTIC_RESET,
                                     .sequence = s->stats.commands};
        compose(s, &s->current); *out = s->current; return true;
    }
    if (expired_at && (!s->count || at(s, 0)->due_ns > now)) {
        sl_haptic_frame f = {.due_ns = expired_at, .source_ns = expired_source,
            .sequence = s->current.sequence, .source = SL_HAPTIC_EXPIRY};
        compose(s, &f); s->current = f; *out = f; return true;
    }
    if (!s->count || at(s, 0)->due_ns > now) return false;
    sl_haptic_frame f = *at(s, 0);
    s->head = (s->head + 1) % SL_HAPTIC_QUEUE; --s->count;
    /* A partial update at an expiry boundary must see the other channel
     * already stopped, without an extra transient frame at the same time. */
    if (s->expires_ns && s->expires_ns <= f.due_ns) {
        for (unsigned i = 0; i < 2; ++i) s->body[i] = silence();
        s->expires_ns = 0;
    }
    if (s->trigger_expires_ns && s->trigger_expires_ns <= f.due_ns) {
        for (unsigned i = 0; i < 2; ++i) s->triggers[i] = silence();
        s->trigger_expires_ns = 0;
    }
    bool trigger = f.source == SL_HAPTIC_TRIGGERS;
    sl_hd_value *values = trigger ? s->triggers : s->body;
    memcpy(values, f.values, sizeof(f.values));
    bool active = values[0].amp_low || values[0].amp_high || values[1].amp_low || values[1].amp_high;
    uint64_t expires = active && f.duration_ns ? add(now, f.duration_ns) : 0;
    if (trigger) {
        s->trigger_expires_ns = expires;
        s->trigger_expiry_source_ns = add(f.source_ns, f.duration_ns);
    } else {
        s->expires_ns = expires;
        s->body_expiry_source_ns = add(f.source_ns, f.duration_ns);
    }
    compose(s, &f);
    s->current = f;
    ++s->stats.scheduled_frames;
    if (f.native) ++s->stats.scheduled_samples;
    if (sampled(&f)) s->last_sample_ns = now;
    space_pending(s, now);
    *out = f;
    return true;
}
uint64_t sl_haptic_deadline(const sl_haptic_stream *s) {
    if (s->dirty) return 1;
    uint64_t next = s->count ? s->queue[s->head].due_ns : 0;
    if (s->expires_ns && (!next || s->expires_ns < next)) next = s->expires_ns;
    if (s->trigger_expires_ns && (!next || s->trigger_expires_ns < next)) next = s->trigger_expires_ns;
    return next;
}
