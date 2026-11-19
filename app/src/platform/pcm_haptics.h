#pragma once
#include "platform/hd_rumble.h"

#define SL_PCM_MAX_WINDOW 1920u
#define SL_PCM_MAX_FFT 2048u
#define SL_PCM_MAX_OUTPUTS 32u
#define SL_PCM_WINDOW_NS UINT64_C(40000000)
#define SL_PCM_ANALYSIS_DELAY_NS UINT64_C(20000000)
#define SL_PCM_HOP_NS UINT64_C(5000000)

typedef enum sl_pcm_format { SL_PCM_S16LE = 1, SL_PCM_F32LE = 2 } sl_pcm_format;
typedef enum sl_pcm_layout { SL_PCM_LEFT_GRIP_RIGHT_GRIP = 1 } sl_pcm_layout;
typedef enum sl_pcm_source { SL_PCM_DEDICATED_HAPTICS = 1 } sl_pcm_source;
enum sl_pcm_flags { SL_PCM_DISCONTINUITY = 1u, SL_PCM_EOS = 2u };
enum sl_pcm_frame_flags { SL_PCM_FRAME_RESET = 1u, SL_PCM_FRAME_EOS = 2u };
typedef enum sl_pcm_result {
    SL_PCM_OK, SL_PCM_BAD_ARGUMENT, SL_PCM_BAD_FORMAT, SL_PCM_BAD_LENGTH,
    SL_PCM_BAD_SAMPLE, SL_PCM_BAD_SEQUENCE, SL_PCM_BAD_TIMESTAMP,
    SL_PCM_QUEUE_FULL, SL_PCM_STREAM_ENDED
} sl_pcm_result;

typedef struct sl_pcm_config {
    uint32_t sample_rate; /* Exactly 8000, 16000 or 48000 Hz; no guessed rate. */
    sl_pcm_format format;
    sl_pcm_layout layout;
    float gain; /* Fixed linear multiplier, 0..4; never per-window AGC. */
    float max_amplitude; /* Explicit target numerical ceiling, 0..1. */
    unsigned max_block_frames; /* 1..sample_rate/10 (100 ms maximum block). */
    unsigned queue_frames; /* 8..SL_PCM_MAX_OUTPUTS; no unbounded buffering. */
} sl_pcm_config;

typedef struct sl_pcm_block {
    sl_pcm_source source; /* Dedicated authored haptics; NOT game speaker audio. */
    sl_pcm_layout layout;
    sl_pcm_format format;
    uint32_t sample_rate, frames, flags;
    const uint8_t *data;
    size_t bytes;
    uint64_t epoch, sequence, source_frame, pts_ns; /* PTS of first PCM frame. */
} sl_pcm_block;

typedef struct sl_pcm_metrics {
    /* Per-side numerical measures, after the fixed gain. Spectral quantities
     * are Hann-weighted RMS; input_rms is unweighted; DC is separate. These
     * are source/representation measures, NOT acceleration or felt fidelity. */
    float input_rms[2], hop_rms[2], dc_rms[2], in_band_rms[2], out_of_band_rms[2];
    /* residual_rms is spectral energy outside selected peak neighborhoods;
     * fit_residual_rms is the optimistic two-sinusoid model's weighted error,
     * fitting phase internally. Switch output CANNOT preserve that phase.
     * output_rms is the two constant carriers' parametric RMS after limiting;
     * rms_error is an energy-level difference, not waveform error. */
    float residual_rms[2], fit_residual_rms[2], output_rms[2], rms_error[2], hop_rms_error[2];
    float requested_amplitude[2][2]; /* Before target amplitude limiting. */
    unsigned limited_bands, fit_failures;
} sl_pcm_metrics;

typedef struct sl_pcm_frame {
    sl_hd_value values[2]; /* Source left/right stay separate; each has two bands. */
    /* Both timestamps are in the source sample clock. available_pts is the
     * earliest window-end position, not wall-clock computation completion. */
    uint64_t epoch, sequence, source_pts_ns, available_pts_ns;
    uint64_t analysis_delay_ns;
    unsigned flags;
    sl_pcm_metrics metrics;
} sl_pcm_frame;

/* About 60 KiB, fixed storage. Keep in persistent/heap-owned device context,
 * not a small Switch thread stack. Caller serializes access. No I/O/threads. */
typedef struct sl_pcm_haptics {
    sl_pcm_config config;
    bool initialized, started, ended;
    uint64_t epoch, next_sequence, next_source_frame, base_pts_ns;
    uint64_t received_frames, analysis_frames, output_frames, reset_dropped;
    unsigned window, fft_size, hop, write_index, head, count;
    float window_power;
    float history[2][SL_PCM_MAX_WINDOW], taper[SL_PCM_MAX_WINDOW];
    float real[SL_PCM_MAX_FFT], imag[SL_PCM_MAX_FFT];
    float twiddle_real[SL_PCM_MAX_FFT / 2], twiddle_imag[SL_PCM_MAX_FFT / 2];
    float power[SL_PCM_MAX_FFT / 2 + 1], prefix[SL_PCM_MAX_FFT / 2 + 2];
    sl_pcm_frame queue[SL_PCM_MAX_OUTPUTS];
} sl_pcm_haptics;

bool sl_pcm_config_valid(const sl_pcm_config *config);
bool sl_pcm_init(sl_pcm_haptics *s, const sl_pcm_config *config);
/* Exact stereo size, finite normalized float samples (-1..1), sequence and
 * sample-clock PTS are validated BEFORE state changes. Non-OK is atomic.
 * Initial/discontinuous blocks emit RESET silence and discard old analysis /
 * queued output; each new epoch requires an explicit DISCONTINUITY.
 * EOS zero-pads half a window and emits final silence; no later data without
 * DISCONTINUITY. A full queue rejects the whole block, with no silent loss.
 *
 * Analysis uses an actual 40 ms Hann window and 5 ms hop. A stream starts with
 * 20 ms known silence on its left boundary; first analysis needs 20 ms input.
 * EOS right-padding also preserves streams shorter than a window.
 * source_pts labels the window CENTER, available_pts its END:
 * 20 ms analysis lag, plus network/queue/physical delay outside this module.
 * At most two spectral peaks per side, separated by >=50 Hz. Amplitudes are
 * constant-tone fits over the 40 ms window: short attacks can spread/smooth
 * across that window; hop_rms_error exposes the 5 ms energy discrepancy.
 * Arbitrary PCM, phase and broadband/transient detail cannot be reproduced
 * by two carriers. No automatic normalization or invented gain correction.
 * Input format/rate support here is NOT a Steam PCM wire/source capability.
 * Run bounded analysis off rendering/input and actuator deadline hot paths. */
sl_pcm_result sl_pcm_push(sl_pcm_haptics *s, const sl_pcm_block *block);
bool sl_pcm_pop(sl_pcm_haptics *s, sl_pcm_frame *out);
