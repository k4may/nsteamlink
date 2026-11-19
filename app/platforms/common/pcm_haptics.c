#include "platform/pcm_haptics.h"
#include <float.h>
#include <math.h>
#include <string.h>

/* Target value shape: libnx HidVibrationValue; numerical frequency limits
 * follow this project's rumble_render policy / Linux hid-nintendo encoder,
 * not a measured actuator passband. This module exposes a local dedicated
 * PCM adapter only. There is no PCM payload in Sony's 47-byte effects state. */
#define PI_F 3.14159265358979323846f
#define PEAKS 16u
_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24,
               "PCM float32 requires IEEE binary32");

static sl_hd_value silence(void) { return (sl_hd_value){0, 160, 0, 320}; }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static float sample(const sl_pcm_block *b, size_t i) {
    if (b->format == SL_PCM_S16LE) {
        uint16_t bits = (uint16_t)b->data[2 * i] | (uint16_t)b->data[2 * i + 1] << 8;
        int32_t v = bits < 32768u ? (int32_t)bits : (int32_t)bits - 65536;
        return (float)v / 32768.0f;
    }
    uint32_t bits = le32(b->data + 4 * i);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
bool sl_pcm_config_valid(const sl_pcm_config *c) {
    return c && (c->sample_rate == 8000 || c->sample_rate == 16000 || c->sample_rate == 48000) &&
        (c->format == SL_PCM_S16LE || c->format == SL_PCM_F32LE) &&
        c->layout == SL_PCM_LEFT_GRIP_RIGHT_GRIP &&
        isfinite(c->gain) && c->gain >= 0 && c->gain <= 4 &&
        isfinite(c->max_amplitude) && c->max_amplitude >= 0 && c->max_amplitude <= 1 &&
        c->max_block_frames && c->max_block_frames <= c->sample_rate / 10 &&
        c->queue_frames >= 8 && c->queue_frames <= SL_PCM_MAX_OUTPUTS;
}
bool sl_pcm_init(sl_pcm_haptics *s, const sl_pcm_config *c) {
    if (!s || !sl_pcm_config_valid(c)) return false;
    sl_pcm_config config = *c;
    memset(s, 0, sizeof(*s));
    s->config = config;
    s->window = config.sample_rate / 25; /* 40 ms */
    s->hop = config.sample_rate / 200;
    s->fft_size = 1;
    while (s->fft_size < s->window) s->fft_size *= 2;
    for (unsigned i = 0; i < s->window; ++i) {
        float w = 0.5f - 0.5f * cosf(2 * PI_F * (float)i / (float)(s->window - 1));
        s->taper[i] = w;
        s->window_power += w * w;
    }
    for (unsigned i = 0; i < s->fft_size / 2; ++i) {
        float angle = -2 * PI_F * (float)i / (float)s->fft_size;
        s->twiddle_real[i] = cosf(angle);
        s->twiddle_imag[i] = sinf(angle);
    }
    s->initialized = true;
    return true;
}
static bool pts_at(uint64_t base, uint64_t frames, uint32_t rate, uint64_t *pts) {
    uint64_t seconds = frames / rate, remainder = frames % rate;
    uint64_t fraction = remainder * UINT64_C(1000000000) / rate;
    if (seconds > (UINT64_MAX - base) / UINT64_C(1000000000)) return false;
    uint64_t value = base + seconds * UINT64_C(1000000000);
    if (fraction > UINT64_MAX - value) return false;
    *pts = value + fraction;
    return true;
}
static uint64_t due(const sl_pcm_haptics *s, uint64_t n) {
    return n < s->window ? 0 : (n - s->window) / s->hop + 1;
}
static void enqueue(sl_pcm_haptics *s, const sl_pcm_frame *f) {
    s->queue[(s->head + s->count) % s->config.queue_frames] = *f;
    ++s->count;
    ++s->output_frames;
}
static void quiet_frame(sl_pcm_haptics *s, uint64_t sequence, uint64_t pts,
                        uint64_t available, unsigned flags) {
    sl_pcm_frame f = {.epoch = s->epoch, .sequence = sequence, .source_pts_ns = pts,
                      .available_pts_ns = available, .flags = flags};
    f.values[0] = f.values[1] = silence();
    f.analysis_delay_ns = available - pts;
    enqueue(s, &f);
}
static void fft(sl_pcm_haptics *s) {
    unsigned n = s->fft_size;
    for (unsigned i = 1, j = 0; i < n; ++i) {
        unsigned bit = n >> 1;
        while (j & bit) { j ^= bit; bit >>= 1; }
        j ^= bit;
        if (i < j) {
            float t = s->real[i]; s->real[i] = s->real[j]; s->real[j] = t;
            t = s->imag[i]; s->imag[i] = s->imag[j]; s->imag[j] = t;
        }
    }
    for (unsigned width = 2; width <= n; width *= 2) {
        for (unsigned start = 0; start < n; start += width) {
            for (unsigned j = 0; j < width / 2; ++j) {
                unsigned a = start + j, b = a + width / 2;
                unsigned t = j * (n / width);
                float r = s->twiddle_real[t] * s->real[b] - s->twiddle_imag[t] * s->imag[b];
                float im = s->twiddle_real[t] * s->imag[b] + s->twiddle_imag[t] * s->real[b];
                s->real[b] = s->real[a] - r; s->imag[b] = s->imag[a] - im;
                s->real[a] += r; s->imag[a] += im;
            }
        }
    }
}
typedef struct peak { unsigned bin, first, last; float hz, energy; } peak;
static float energy(const sl_pcm_haptics *s, unsigned first, unsigned last) {
    return first > last ? 0 : fmaxf(0, s->prefix[last + 1] - s->prefix[first]);
}
static bool low_valid(float hz) { return hz >= 41 && hz <= 626; }
static bool high_valid(float hz) { return hz >= 82 && hz <= 1253; }

/* FFT chooses the two frequencies. A bounded 2/4-basis least-squares fit
 * estimates amplitudes without treating Hann sidelobes as extra motor energy.
 * Oscillator recurrence avoids a per-Hz bank or per-sample trig calls. Phase
 * is used ONLY in this error model; HidVibrationValue has no phase field. */
static bool fit(sl_pcm_haptics *s, unsigned side, float mean, const float hz[2],
                const bool active[2], float amplitude[2], float *residual) {
    double gram[4][4] = {{0}}, rhs[4] = {0}, augmented[4][5] = {{0}};
    double cs[2] = {1, 1}, sn[2] = {0}, dc[2] = {0}, ds[2] = {0};
    unsigned dimensions = 0, map[2] = {0, 0};
    for (unsigned band = 0; band < 2; ++band) {
        if (!active[band]) continue;
        map[band] = dimensions;
        dimensions += 2;
        double angle = 2.0 * (double)PI_F * hz[band] / s->config.sample_rate;
        dc[band] = cos(angle);
        ds[band] = sin(angle);
    }
    double power = 0;
    for (unsigned i = 0; i < s->window; ++i) {
        unsigned at = (s->write_index + i) % s->window;
        double x = s->history[side][at] * s->config.gain - mean;
        double weight = (double)s->taper[i] * s->taper[i], basis[4] = {0};
        power += weight * x * x;
        for (unsigned band = 0; band < 2; ++band) {
            if (!active[band]) continue;
            basis[map[band]] = cs[band];
            basis[map[band] + 1] = sn[band];
            double next = cs[band] * dc[band] - sn[band] * ds[band];
            sn[band] = sn[band] * dc[band] + cs[band] * ds[band];
            cs[band] = next;
        }
        for (unsigned j = 0; j < dimensions; ++j) {
            rhs[j] += weight * x * basis[j];
            for (unsigned k = 0; k < dimensions; ++k) gram[j][k] += weight * basis[j] * basis[k];
        }
    }
    for (unsigned j = 0; j < dimensions; ++j) {
        for (unsigned k = 0; k < dimensions; ++k) augmented[j][k] = gram[j][k];
        augmented[j][dimensions] = rhs[j];
    }
    for (unsigned column = 0; column < dimensions; ++column) {
        unsigned pivot = column;
        for (unsigned row = column + 1; row < dimensions; ++row)
            if (fabs(augmented[row][column]) > fabs(augmented[pivot][column])) pivot = row;
        if (fabs(augmented[pivot][column]) < 1e-8 * s->window_power) return false;
        for (unsigned k = 0; k <= dimensions; ++k) {
            double tmp = augmented[column][k];
            augmented[column][k] = augmented[pivot][k];
            augmented[pivot][k] = tmp;
        }
        double divisor = augmented[column][column];
        for (unsigned k = column; k <= dimensions; ++k) augmented[column][k] /= divisor;
        for (unsigned row = 0; row < dimensions; ++row) {
            if (row == column) continue;
            double scale = augmented[row][column];
            for (unsigned k = column; k <= dimensions; ++k) augmented[row][k] -= scale * augmented[column][k];
        }
    }
    for (unsigned band = 0; band < 2; ++band) {
        if (!active[band]) continue;
        unsigned j = map[band];
        amplitude[band] = (float)hypot(augmented[j][dimensions], augmented[j + 1][dimensions]);
    }
    for (unsigned j = 0; j < dimensions; ++j) power -= augmented[j][dimensions] * rhs[j];
    *residual = (float)sqrt(fmax(0, power) / s->window_power);
    return true;
}

static void analyze_side(sl_pcm_haptics *s, unsigned side, sl_pcm_frame *frame) {
    const unsigned n = s->fft_size, half = n / 2;
    const float step = (float)s->config.sample_rate / (float)n;
    float mean = 0, square = 0;
    for (unsigned i = 0; i < s->window; ++i) {
        float x = s->history[side][i] * s->config.gain;
        mean += x;
        square += x * x;
    }
    mean /= (float)s->window;
    frame->metrics.input_rms[side] = sqrtf(square / (float)s->window);
    frame->metrics.dc_rms[side] = fabsf(mean);
    float hop_power = 0;
    for (unsigned i = 0; i < s->hop; ++i) {
        unsigned at = (s->write_index + s->window / 2 + i) % s->window;
        float value = s->history[side][at] * s->config.gain;
        hop_power += value * value;
    }
    frame->metrics.hop_rms[side] = sqrtf(hop_power / (float)s->hop);
    memset(s->real, 0, n * sizeof(float));
    memset(s->imag, 0, n * sizeof(float));
    for (unsigned i = 0; i < s->window; ++i) {
        unsigned at = (s->write_index + i) % s->window;
        s->real[i] = (s->history[side][at] * s->config.gain - mean) * s->taper[i];
    }
    fft(s);
    float total = 0, inside = 0;
    s->prefix[0] = 0;
    for (unsigned i = 0; i <= half; ++i) {
        float p = (s->real[i] * s->real[i] + s->imag[i] * s->imag[i]) /
                  ((float)n * s->window_power);
        if (i && i != half) p *= 2;
        s->power[i] = p;
        total += p;
        float legal = (float)i * step >= 41 && (float)i * step <= 1253 ? p : 0;
        inside += legal;
        s->prefix[i + 1] = s->prefix[i] + legal;
    }
    frame->metrics.in_band_rms[side] = sqrtf(inside);
    frame->metrics.out_of_band_rms[side] = sqrtf(fmaxf(0, total - inside));

    peak peaks[PEAKS];
    unsigned count = 0;
    unsigned radius = (2 * n + s->window - 1) / s->window;
    for (unsigned i = 1; i < half; ++i) {
        if ((float)i * step < 41 - step / 2 || (float)i * step > 1253 + step / 2) continue;
        if (s->power[i] <= 1e-12f || s->power[i] < s->power[i - 1] ||
            s->power[i] <= s->power[i + 1]) continue;
        float a = logf(fmaxf(s->power[i - 1], 1e-30f));
        float b = logf(s->power[i]);
        float c = logf(fmaxf(s->power[i + 1], 1e-30f));
        float denominator = a - 2 * b + c;
        float delta = denominator != 0 ? 0.5f * (a - c) / denominator : 0;
        delta = fmaxf(-0.5f, fminf(0.5f, delta));
        peak p = {.bin = i, .first = i > radius ? i - radius : 0,
                  .last = i + radius < half ? i + radius : half,
                  .hz = ((float)i + delta) * step};
        if (!low_valid(p.hz) && !high_valid(p.hz)) continue;
        p.energy = energy(s, p.first, p.last);
        unsigned at = count;
        if (at == PEAKS) {
            if (p.energy <= peaks[at - 1].energy) continue;
            --at;
        } else ++count;
        while (at && p.energy > peaks[at - 1].energy) { peaks[at] = peaks[at - 1]; --at; }
        peaks[at] = p;
    }
    int best_low = -1, best_high = -1;
    float best = 0;
    for (int low = -1; low < (int)count; ++low) {
        if (low >= 0 && !low_valid(peaks[low].hz)) continue;
        for (int high = -1; high < (int)count; ++high) {
            if (high >= 0 && !high_valid(peaks[high].hz)) continue;
            if (low >= 0 && high >= 0 && peaks[high].hz - peaks[low].hz < 50) continue;
            float score = low < 0 ? 0 : peaks[low].energy;
            if (high >= 0) score += peaks[high].energy;
            if (low >= 0 && high >= 0) {
                unsigned first = peaks[low].first > peaks[high].first ? peaks[low].first : peaks[high].first;
                unsigned last = peaks[low].last < peaks[high].last ? peaks[low].last : peaks[high].last;
                score -= energy(s, first, last);
            }
            if (score > best) { best = score; best_low = low; best_high = high; }
        }
    }
    float selected[2] = {0, 0};
    int indices[2] = {best_low, best_high};
    float frequencies[2] = {160, 320}, amplitudes[2] = {0, 0};
    bool active[2] = {best_low >= 0, best_high >= 0};
    unsigned midpoint = 0;
    if (best_low >= 0 && best_high >= 0)
        midpoint = (unsigned)((peaks[best_low].hz + peaks[best_high].hz) / (2 * step));
    for (unsigned band = 0; band < 2; ++band) {
        int index = indices[band];
        if (index < 0) continue;
        peak p = peaks[index];
        if (best_low >= 0 && best_high >= 0) {
            if (!band && p.last > midpoint) p.last = midpoint;
            if (band && p.first <= midpoint) p.first = midpoint + 1;
        }
        selected[band] = energy(s, p.first, p.last);
        frequencies[band] = p.hz;
        amplitudes[band] = sqrtf(2 * selected[band]);
    }
    if (!fit(s, side, mean, frequencies, active, amplitudes, &frame->metrics.fit_residual_rms[side])) {
        ++frame->metrics.fit_failures;
        frame->metrics.fit_residual_rms[side] = frame->metrics.in_band_rms[side];
    }
    for (unsigned band = 0; band < 2; ++band) {
        if (!active[band]) continue;
        float requested = amplitudes[band];
        frame->metrics.requested_amplitude[side][band] = requested;
        float amplitude = fminf(requested, s->config.max_amplitude);
        if (requested > s->config.max_amplitude) ++frame->metrics.limited_bands;
        if (band) { frame->values[side].amp_high = amplitude; frame->values[side].freq_high = frequencies[band]; }
        else { frame->values[side].amp_low = amplitude; frame->values[side].freq_low = frequencies[band]; }
    }
    frame->metrics.residual_rms[side] = sqrtf(fmaxf(0, inside - selected[0] - selected[1]));
    sl_hd_value value = frame->values[side];
    frame->metrics.output_rms[side] = sqrtf((value.amp_low * value.amp_low + value.amp_high * value.amp_high) / 2);
    frame->metrics.rms_error[side] = fabsf(frame->metrics.in_band_rms[side] - frame->metrics.output_rms[side]);
    frame->metrics.hop_rms_error[side] = fabsf(frame->metrics.hop_rms[side] - frame->metrics.output_rms[side]);
}
static void feed(sl_pcm_haptics *s, float left, float right, uint64_t sequence) {
    s->history[0][s->write_index] = left;
    s->history[1][s->write_index] = right;
    s->write_index = (s->write_index + 1) % s->window;
    ++s->analysis_frames;
    if (s->analysis_frames < s->window || (s->analysis_frames - s->window) % s->hop) return;
    sl_pcm_frame frame = {.epoch = s->epoch, .sequence = sequence,
                          .analysis_delay_ns = SL_PCM_ANALYSIS_DELAY_NS};
    frame.values[0] = frame.values[1] = silence();
    (void)pts_at(s->base_pts_ns, s->analysis_frames - s->window, s->config.sample_rate, &frame.source_pts_ns);
    (void)pts_at(s->base_pts_ns, s->analysis_frames - s->window / 2, s->config.sample_rate, &frame.available_pts_ns);
    analyze_side(s, 0, &frame);
    analyze_side(s, 1, &frame);
    enqueue(s, &frame);
}

sl_pcm_result sl_pcm_push(sl_pcm_haptics *s, const sl_pcm_block *b) {
    if (!s || !s->initialized || !b) return SL_PCM_BAD_ARGUMENT;
    if (b->source != SL_PCM_DEDICATED_HAPTICS || b->layout != SL_PCM_LEFT_GRIP_RIGHT_GRIP ||
        b->sample_rate != s->config.sample_rate || b->format != s->config.format ||
        (b->flags & ~(uint32_t)(SL_PCM_DISCONTINUITY | SL_PCM_EOS))) return SL_PCM_BAD_FORMAT;
    unsigned width = b->format == SL_PCM_S16LE ? 4 : 8;
    if (b->frames > s->config.max_block_frames || b->bytes != (size_t)b->frames * width ||
        (b->bytes && !b->data) || (!b->frames && !(b->flags & (SL_PCM_EOS | SL_PCM_DISCONTINUITY))))
        return SL_PCM_BAD_LENGTH;
    if (b->format == SL_PCM_F32LE) {
        for (size_t i = 0; i < (size_t)b->frames * 2; ++i) {
            float x = sample(b, i);
            if (!isfinite(x) || x < -1 || x > 1) return SL_PCM_BAD_SAMPLE;
        }
    }
    bool reset = !s->started || (b->flags & SL_PCM_DISCONTINUITY);
    if (!reset) {
        if (s->ended) return SL_PCM_STREAM_ENDED;
        if (b->epoch != s->epoch || b->sequence != s->next_sequence ||
            b->source_frame != s->next_source_frame) return SL_PCM_BAD_SEQUENCE;
        uint64_t expected;
        if (!pts_at(s->base_pts_ns, s->received_frames, b->sample_rate, &expected) ||
            (b->pts_ns > expected ? b->pts_ns - expected : expected - b->pts_ns) > 1)
            return SL_PCM_BAD_TIMESTAMP;
    }
    if (b->sequence == UINT64_MAX || b->frames > UINT64_MAX - b->source_frame) return SL_PCM_BAD_SEQUENCE;
    uint64_t before = reset ? s->window / 2 : s->analysis_frames;
    unsigned pad = b->flags & SL_PCM_EOS ? s->window / 2 - 1 : 0;
    if ((uint64_t)b->frames + pad > UINT64_MAX - before) return SL_PCM_BAD_TIMESTAMP;
    uint64_t after = before + b->frames + pad;
    uint64_t actual = reset ? b->frames : s->received_frames + b->frames;
    uint64_t base = reset ? b->pts_ns : s->base_pts_ns, latest;
    if (actual > UINT64_MAX - s->window / 2 ||
        !pts_at(base, actual + s->window / 2, b->sample_rate, &latest)) return SL_PCM_BAD_TIMESTAMP;
    uint64_t outputs = due(s, after) - due(s, before) + (reset ? 1u : 0u) +
                       ((b->flags & SL_PCM_EOS) ? 1u : 0u);
    if (outputs + (reset ? 0 : s->count) > s->config.queue_frames) return SL_PCM_QUEUE_FULL;
    if (reset) {
        s->reset_dropped += s->count;
        memset(s->history, 0, sizeof(s->history));
        s->head = s->count = s->write_index = 0;
        s->received_frames = 0;
        s->analysis_frames = s->window / 2;
        s->write_index = s->window / 2;
        s->base_pts_ns = b->pts_ns;
        s->epoch = b->epoch;
        s->started = true;
        s->ended = false;
        quiet_frame(s, b->sequence, b->pts_ns, b->pts_ns, SL_PCM_FRAME_RESET);
    }
    for (unsigned i = 0; i < b->frames; ++i) feed(s, sample(b, (size_t)i * 2), sample(b, (size_t)i * 2 + 1), b->sequence);
    s->received_frames = actual;
    s->next_source_frame = b->source_frame + b->frames;
    s->next_sequence = b->sequence + 1;
    if (b->flags & SL_PCM_EOS) {
        for (unsigned i = 0; i < pad; ++i) feed(s, 0, 0, b->sequence);
        uint64_t end_pts, stop_available;
        (void)pts_at(s->base_pts_ns, actual, b->sample_rate, &end_pts);
        (void)pts_at(s->base_pts_ns, actual + s->window / 2, b->sample_rate, &stop_available);
        quiet_frame(s, b->sequence, end_pts, stop_available, SL_PCM_FRAME_EOS);
        s->ended = true;
    }
    return SL_PCM_OK;
}
bool sl_pcm_pop(sl_pcm_haptics *s, sl_pcm_frame *out) {
    if (!s || !s->initialized || !out || !s->count) return false;
    *out = s->queue[s->head];
    s->head = (s->head + 1) % s->config.queue_frames;
    --s->count;
    return true;
}
