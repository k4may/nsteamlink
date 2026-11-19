#include "platform/pcm_haptics.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI_D 3.14159265358979323846

static sl_pcm_config config(uint32_t rate, sl_pcm_format format) {
    return (sl_pcm_config){.sample_rate = rate, .format = format,
        .layout = SL_PCM_LEFT_GRIP_RIGHT_GRIP, .gain = 1, .max_amplitude = 1,
        .max_block_frames = rate / 10, .queue_frames = 32};
}
static void store(uint8_t *data, size_t at, sl_pcm_format format, float value) {
    if (format == SL_PCM_S16LE) {
        int32_t v = (int32_t)lrintf(fmaxf(-1, fminf(32767.0f / 32768, value)) * 32768);
        uint16_t bits = (uint16_t)v;
        data[2 * at] = (uint8_t)bits;
        data[2 * at + 1] = (uint8_t)(bits >> 8);
    } else {
        uint32_t bits;
        memcpy(&bits, &value, sizeof(bits));
        for (unsigned j = 0; j < 4; ++j) data[4 * at + j] = (uint8_t)(bits >> (j * 8));
    }
}
static sl_pcm_block block(uint32_t rate, sl_pcm_format format, const uint8_t *data,
                          uint32_t frames, uint64_t start, uint64_t sequence) {
    return (sl_pcm_block){.source = SL_PCM_DEDICATED_HAPTICS,
        .layout = SL_PCM_LEFT_GRIP_RIGHT_GRIP, .format = format,
        .sample_rate = rate, .frames = frames, .data = data,
        .bytes = (size_t)frames * (format == SL_PCM_S16LE ? 4u : 8u),
        .epoch = 17, .sequence = sequence, .source_frame = start,
        .pts_ns = UINT64_C(1000000000) + start * UINT64_C(1000000000) / rate};
}
static void tones(uint8_t *data, uint32_t frames, uint32_t rate, sl_pcm_format format,
                  uint64_t start, unsigned kind) {
    for (uint32_t i = 0; i < frames; ++i) {
        double t = (double)(start + i) / rate;
        float left = 0, right = 0;
        switch (kind) {
            case 0:
                left = (float)(0.4 * sin(2 * PI_D * 160 * t) + 0.2 * sin(2 * PI_D * 320 * t));
                right = (float)(0.35 * sin(2 * PI_D * 500 * t));
                break;
            case 1: left = 0.5f; break;
            case 2: left = (float)(0.5 * sin(2 * PI_D * 3000 * t)); break;
            case 3: left = (float)(0.8 * sin(2 * PI_D * 160 * t)); break;
            case 4: left = (float)(0.3 * sin(2 * PI_D * 160 * t) +
                                   0.3 * sin(2 * PI_D * 320 * t) +
                                   0.3 * sin(2 * PI_D * 900 * t)); break;
            default: break;
        }
        store(data, (size_t)i * 2, format, left);
        store(data, (size_t)i * 2 + 1, format, right);
    }
}
static void finite_frame(const sl_pcm_frame *f) {
    for (unsigned side = 0; side < 2; ++side) {
        sl_hd_value v = f->values[side];
        assert(isfinite(v.amp_low) && v.amp_low >= 0 && v.amp_low <= 1);
        assert(isfinite(v.amp_high) && v.amp_high >= 0 && v.amp_high <= 1);
        assert(isfinite(v.freq_low) && v.freq_low >= 41 && v.freq_low <= 626);
        assert(isfinite(v.freq_high) && v.freq_high >= 82 && v.freq_high <= 1253);
        if (v.amp_low && v.amp_high) assert(v.freq_high - v.freq_low >= 49.99f);
        assert(isfinite(f->metrics.residual_rms[side]) && f->metrics.residual_rms[side] >= 0);
        assert(isfinite(f->metrics.fit_residual_rms[side]) && f->metrics.fit_residual_rms[side] >= 0);
        assert(isfinite(f->metrics.hop_rms_error[side]) && f->metrics.hop_rms_error[side] >= 0);
        assert(isfinite(f->metrics.out_of_band_rms[side]) && f->metrics.out_of_band_rms[side] >= 0);
    }
}
static void rejected(sl_pcm_haptics *s, const sl_pcm_block *b, sl_pcm_result expected) {
    sl_pcm_haptics *before = malloc(sizeof(*before));
    assert(before);
    memcpy(before, s, sizeof(*s));
    assert(sl_pcm_push(s, b) == expected);
    assert(memcmp(before, s, sizeof(*s)) == 0);
    free(before);
}
static sl_pcm_frame exercise(sl_pcm_config c, unsigned kind, bool eos) {
    sl_pcm_haptics *s = malloc(sizeof(*s));
    uint32_t frames = c.sample_rate * 80 / 1000;
    uint8_t *data = malloc((size_t)frames * 8);
    assert(s && data && sl_pcm_init(s, &c));
    tones(data, frames, c.sample_rate, c.format, 0, kind);
    sl_pcm_block b = block(c.sample_rate, c.format, data, frames, 0, 0);
    if (eos) b.flags = SL_PCM_EOS;
    assert(sl_pcm_push(s, &b) == SL_PCM_OK);
    sl_pcm_frame f, middle = {0};
    unsigned analyses = 0;
    uint64_t previous = 0;
    bool stopped = false;
    while (sl_pcm_pop(s, &f)) {
        finite_frame(&f);
        if (f.flags == SL_PCM_FRAME_RESET) {
            assert(f.source_pts_ns == b.pts_ns && !f.values[0].amp_low);
            continue;
        }
        assert(f.analysis_delay_ns == SL_PCM_ANALYSIS_DELAY_NS);
        assert(f.available_pts_ns - f.source_pts_ns == SL_PCM_ANALYSIS_DELAY_NS);
        if (f.flags == SL_PCM_FRAME_EOS) {
            assert(f.source_pts_ns == b.pts_ns + UINT64_C(80000000));
            assert(!f.values[0].amp_low && !f.values[0].amp_high);
            stopped = true;
        } else {
            if (analyses) assert(f.source_pts_ns - previous == SL_PCM_HOP_NS);
            else assert(f.source_pts_ns == b.pts_ns);
            previous = f.source_pts_ns;
            ++analyses;
            if (f.source_pts_ns == b.pts_ns + UINT64_C(40000000)) middle = f;
        }
    }
    assert(analyses == (eos ? 16u : 13u));
    assert(stopped == eos && middle.source_pts_ns);
    if (eos) {
        b = block(c.sample_rate, c.format, data, c.sample_rate / 200, frames, 1);
        rejected(s, &b, SL_PCM_STREAM_ENDED);
    }
    free(data);
    free(s);
    return middle;
}
static void spectrum_tests(void) {
    const uint32_t rates[] = {8000, 16000, 48000};
    for (unsigned i = 0; i < 3; ++i) {
        for (unsigned fmt = SL_PCM_S16LE; fmt <= SL_PCM_F32LE; ++fmt) {
            sl_pcm_frame f = exercise(config(rates[i], (sl_pcm_format)fmt), 0, true);
            assert(fabsf(f.values[0].freq_low - 160) < 3);
            assert(fabsf(f.values[0].freq_high - 320) < 3);
            assert(fabsf(f.values[0].amp_low - 0.4f) < 0.025f);
            assert(fabsf(f.values[0].amp_high - 0.2f) < 0.025f);
            sl_hd_value r = f.values[1];
            float amp = r.amp_low > r.amp_high ? r.amp_low : r.amp_high;
            float hz = r.amp_low > r.amp_high ? r.freq_low : r.freq_high;
            assert(fabsf(hz - 500) < 3 && fabsf(amp - 0.35f) < 0.025f);
            assert(f.metrics.rms_error[0] < 0.025f);
            assert(f.metrics.fit_residual_rms[0] < 0.025f && !f.metrics.fit_failures);
        }
    }
    sl_pcm_config c = config(8000, SL_PCM_F32LE);
    sl_pcm_frame dc = exercise(c, 1, false);
    assert(dc.metrics.dc_rms[0] == 0.5f && dc.metrics.input_rms[0] == 0.5f);
    assert(!dc.values[0].amp_low && !dc.values[0].amp_high);
    sl_pcm_frame outside = exercise(c, 2, false);
    assert(outside.metrics.out_of_band_rms[0] > 0.34f);
    assert(outside.values[0].amp_low < 0.001f && outside.values[0].amp_high < 0.001f);
    sl_pcm_frame strong = exercise(c, 3, false);
    assert(!strong.values[1].amp_low && !strong.values[1].amp_high);
    c.gain = 0.5f;
    sl_pcm_frame weak = exercise(c, 3, false);
    assert(fabsf(weak.metrics.output_rms[0] * 2 - strong.metrics.output_rms[0]) < 0.001f);
    c.gain = 1;
    c.max_amplitude = 0.2f;
    sl_pcm_frame limited = exercise(c, 3, false);
    assert(limited.metrics.limited_bands && limited.metrics.rms_error[0] > 0.3f);
    assert(limited.values[0].amp_low <= 0.2f && limited.values[0].amp_high <= 0.2f);
    c.max_amplitude = 1;
    sl_pcm_frame three = exercise(c, 4, false);
    assert(three.metrics.residual_rms[0] > 0.15f); /* Third carrier cannot disappear from metrics. */
    assert(three.metrics.fit_residual_rms[0] > 0.15f);
}
static void frequency_boundaries(void) {
    const uint32_t rates[]={8000,16000,48000};
    const double frequencies[]={41,42,50,81,82,83,625,626,627,1250,1252,1253};
    unsigned tested=0, projections=0;
    for(unsigned rate=0;rate<3;++rate)
        for(unsigned fmt=SL_PCM_S16LE;fmt<=SL_PCM_F32LE;++fmt)
            for(unsigned h=0;h<12;++h)
                for(unsigned phase=0;phase<8;++phase) {
                    sl_pcm_config c=config(rates[rate],(sl_pcm_format)fmt);
                    sl_pcm_haptics *s=malloc(sizeof(*s));
                    unsigned n=rates[rate]*80/1000;
                    uint8_t *data=calloc(n,8);
                    assert(s && data && sl_pcm_init(s,&c));
                    for(unsigned i=0;i<n;++i) {
                        double angle=2*PI_D*(frequencies[h]*i/rates[rate]+phase/8.0);
                        store(data,(size_t)i*2,c.format,(float)(.4*sin(angle)));
                    }
                    sl_pcm_block b=block(rates[rate],c.format,data,n,0,0);
                    assert(sl_pcm_push(s,&b)==SL_PCM_OK);
                    sl_pcm_frame f; bool checked=false;
                    while(sl_pcm_pop(s,&f)) {
                        finite_frame(&f);
                        if(f.flags || f.source_pts_ns!=b.pts_ns+40000000) continue;
                        sl_hd_value v=f.values[0];
                        float amplitude=fmaxf(v.amp_low,v.amp_high);
                        float hz=v.amp_low>v.amp_high?v.freq_low:v.freq_high;
                        /* Independent sine source amplitude and frequency;
                         * a sidelobe or near-silent output cannot pass. */
                        if(fabsf(amplitude-.4f)>=.03f || fabs(hz-frequencies[h])>=6)
                            fprintf(stderr,"boundary rate=%u fmt=%u hz=%g phase=%u: amplitude=%g carrier=%g\n",
                                    rates[rate],fmt,frequencies[h],phase,amplitude,hz);
                        assert(fabsf(amplitude-.4f)<.03f);
                        assert(fabs(hz-frequencies[h])<6);
                        assert(!f.values[1].amp_low && !f.values[1].amp_high);
                        projections+=f.metrics.frequency_projections;
                        checked=true; ++tested;
                    }
                    assert(checked);
                    free(s); free(data);
                }
    assert(tested==576 && projections);
}
static void validation_tests(void) {
    sl_pcm_config c = config(8000, SL_PCM_F32LE);
    sl_pcm_haptics *s = malloc(sizeof(*s));
    uint8_t data[800 * 8];
    assert(s && sl_pcm_init(s, &c));
    tones(data, 800, 8000, SL_PCM_F32LE, 0, 0);
    sl_pcm_block b = block(8000, SL_PCM_F32LE, data, 160, 0, 9), bad;
    bad = b; bad.bytes--;
    rejected(s, &bad, SL_PCM_BAD_LENGTH);
    bad = b; bad.frames = UINT32_MAX;
    rejected(s, &bad, SL_PCM_BAD_LENGTH);
    bad = b; bad.data = NULL;
    rejected(s, &bad, SL_PCM_BAD_LENGTH);
    bad = b; bad.source = (sl_pcm_source)0;
    rejected(s, &bad, SL_PCM_BAD_FORMAT);
    bad = b; bad.layout = (sl_pcm_layout)0;
    rejected(s, &bad, SL_PCM_BAD_FORMAT);
    bad = b; bad.sample_rate = 44100;
    rejected(s, &bad, SL_PCM_BAD_FORMAT);
    bad = b; bad.format = SL_PCM_S16LE;
    rejected(s, &bad, SL_PCM_BAD_FORMAT);
    bad = b; bad.flags = 4;
    rejected(s, &bad, SL_PCM_BAD_FORMAT);
    bad = b; bad.pts_ns = UINT64_MAX;
    rejected(s, &bad, SL_PCM_BAD_TIMESTAMP);
    for (unsigned i = 0; i < 4; ++i) {
        const float invalid[] = {NAN, INFINITY, -INFINITY, 1.001f};
        store(data, 17, SL_PCM_F32LE, invalid[i]);
        rejected(s, &b, SL_PCM_BAD_SAMPLE);
    }
    tones(data, 800, 8000, SL_PCM_F32LE, 0, 0);
    assert(sl_pcm_push(s, &b) == SL_PCM_OK);
    b = block(8000, SL_PCM_F32LE, data, 160, 160, 10);
    bad = b; bad.epoch++;
    rejected(s, &bad, SL_PCM_BAD_SEQUENCE);
    bad = b; bad.sequence++;
    rejected(s, &bad, SL_PCM_BAD_SEQUENCE);
    bad = b; bad.source_frame++;
    rejected(s, &bad, SL_PCM_BAD_SEQUENCE);
    bad = b; bad.pts_ns += 2;
    rejected(s, &bad, SL_PCM_BAD_TIMESTAMP);
    assert(sl_pcm_push(s, &b) == SL_PCM_OK);
    bad = b; bad.sequence = UINT64_MAX; bad.flags = SL_PCM_DISCONTINUITY;
    rejected(s, &bad, SL_PCM_BAD_SEQUENCE);
    assert(!sl_pcm_pop(s, NULL));
    free(s);
    c.sample_rate = 44100; assert(!sl_pcm_config_valid(&c));
    c = config(8000, SL_PCM_F32LE); c.gain = NAN; assert(!sl_pcm_config_valid(&c));
    c.gain = -1; assert(!sl_pcm_config_valid(&c));
    c.gain = 1; c.max_amplitude = 1.01f; assert(!sl_pcm_config_valid(&c));
    c.max_amplitude = 1; c.queue_frames = 33; assert(!sl_pcm_config_valid(&c));
}
static void stream_tests(void) {
    sl_pcm_config c = config(8000, SL_PCM_S16LE);
    c.queue_frames = 8;
    sl_pcm_haptics *s = malloc(sizeof(*s));
    uint8_t data[800 * 4];
    assert(s && sl_pcm_init(s, &c));
    tones(data, 800, 8000, SL_PCM_S16LE, 0, 3);
    sl_pcm_block b = block(8000, SL_PCM_S16LE, data, 160, 0, 0);
    assert(sl_pcm_push(s, &b) == SL_PCM_OK);
    b = block(8000, SL_PCM_S16LE, data, 160, 160, 1);
    assert(sl_pcm_push(s, &b) == SL_PCM_OK);
    b = block(8000, SL_PCM_S16LE, data, 160, 320, 2);
    rejected(s, &b, SL_PCM_QUEUE_FULL);
    sl_pcm_frame f;
    while (sl_pcm_pop(s, &f)) {}
    assert(sl_pcm_push(s, &b) == SL_PCM_OK); /* Same rejected source block can retry. */
    uint64_t dropped = s->count;
    memset(data, 0, sizeof(data));
    b = block(8000, SL_PCM_S16LE, data, 160, 900, 22);
    b.flags = SL_PCM_DISCONTINUITY;
    b.epoch = 23;
    assert(sl_pcm_push(s, &b) == SL_PCM_OK && s->reset_dropped == dropped);
    assert(sl_pcm_pop(s, &f) && f.flags == SL_PCM_FRAME_RESET && f.epoch == 23);
    while (sl_pcm_pop(s, &f)) assert(!f.values[0].amp_low && !f.values[0].amp_high);

    /* A 5 ms authored pulse must not vanish just because it is shorter than
     * the 40 ms analysis window: explicit silent boundaries let EOS flush it. */
    c.queue_frames = 32;
    assert(sl_pcm_init(s, &c));
    for (unsigned i = 0; i < 40; ++i) {
        store(data, (size_t)i * 2, SL_PCM_S16LE, (float)(0.8 * sin(2 * PI_D * 320 * i / 8000)));
        store(data, (size_t)i * 2 + 1, SL_PCM_S16LE, 0);
    }
    b = block(8000, SL_PCM_S16LE, data, 40, 0, 0);
    b.flags = SL_PCM_EOS;
    assert(sl_pcm_push(s, &b) == SL_PCM_OK);
    bool pulse = false, ended = false;
    while (sl_pcm_pop(s, &f)) {
        finite_frame(&f);
        if (!f.flags) pulse |= f.values[0].amp_low > 0 || f.values[0].amp_high > 0;
        if (f.flags == SL_PCM_FRAME_EOS) {
            ended = true;
            assert(f.source_pts_ns == b.pts_ns + SL_PCM_HOP_NS);
        }
    }
    assert(pulse && ended);

    /* EOS may be a distinct empty typed control block, not a guessed zero
     * rumble value. Its timestamp remains on the existing source clock. */
    assert(sl_pcm_init(s, &c));
    b = block(8000, SL_PCM_S16LE, data, 40, 0, 0);
    assert(sl_pcm_push(s, &b) == SL_PCM_OK);
    b = block(8000, SL_PCM_S16LE, NULL, 0, 40, 1);
    b.flags = SL_PCM_EOS;
    assert(sl_pcm_push(s, &b) == SL_PCM_OK && s->ended);
    pulse = ended = false;
    while (sl_pcm_pop(s, &f)) {
        if (!f.flags) pulse |= f.values[0].amp_low > 0 || f.values[0].amp_high > 0;
        if (f.flags == SL_PCM_FRAME_EOS) ended = true;
    }
    assert(pulse && ended);
    free(s);
}
static void chunk_invariance(void) {
    sl_pcm_config c = config(48000, SL_PCM_F32LE);
    sl_pcm_haptics *whole = malloc(sizeof(*whole)), *split = malloc(sizeof(*split));
    uint8_t *data = malloc(3840 * 8);
    assert(whole && split && data && sl_pcm_init(whole, &c) && sl_pcm_init(split, &c));
    tones(data, 3840, 48000, SL_PCM_F32LE, 0, 0);
    sl_pcm_block b = block(48000, SL_PCM_F32LE, data, 3840, 0, 0);
    b.flags = SL_PCM_EOS;
    assert(sl_pcm_push(whole, &b) == SL_PCM_OK);
    for (unsigned i = 0; i < 16; ++i) {
        b = block(48000, SL_PCM_F32LE, data + (size_t)i * 240 * 8, 240, i * 240u, i);
        if (i == 15) b.flags = SL_PCM_EOS;
        assert(sl_pcm_push(split, &b) == SL_PCM_OK);
    }
    sl_pcm_frame a, z;
    while (sl_pcm_pop(whole, &a)) {
        assert(sl_pcm_pop(split, &z));
        assert(a.flags == z.flags && a.source_pts_ns == z.source_pts_ns && a.available_pts_ns == z.available_pts_ns);
        for (unsigned side = 0; side < 2; ++side) {
            assert(a.values[side].amp_low == z.values[side].amp_low);
            assert(a.values[side].amp_high == z.values[side].amp_high);
            assert(a.values[side].freq_low == z.values[side].freq_low);
            assert(a.values[side].freq_high == z.values[side].freq_high);
            assert(a.metrics.fit_residual_rms[side] == z.metrics.fit_residual_rms[side]);
        }
    }
    assert(!sl_pcm_pop(split, &z));
    free(data); free(whole); free(split);
}
int main(void) {
    spectrum_tests();
    frequency_boundaries();
    validation_tests();
    stream_tests();
    chunk_invariance();
    puts("Dedicated PCM spectral approximation, typed bounds and sample-clock lifecycle passed");
    return 0;
}
