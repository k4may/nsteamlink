#include "platform/haptic_stream.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const uint8_t pulse[69] = {13, 10, 0, 0, 0, 0x10, 0,
    0, 1, 0x40, 0x40, 0, 0, 5, 0xc0};
static const uint8_t ordinary[9] = {1, 0xff, 0xff, 0, 0, 10, 0, 0, 0};

int main(void) {
    sl_haptic_stream s;
    sl_haptic_frame f;
    sl_haptic_init(&s);
    uint64_t start = 1000000000;
    assert(sl_haptic_command(&s, pulse, 69, start, start, true));
    assert(s.count == 3 && s.stats.decoded_samples == 3);
    assert(sl_haptic_advance(&s, start, &f));
    assert(f.values[1].amp_low == 0 && f.native && f.sample == 0);
    assert(!sl_haptic_advance(&s, start + 4999999, &f));
    assert(sl_haptic_advance(&s, start + 5000000, &f));
    assert(f.sample == 1 && fabsf(f.values[1].amp_low - .0441941738f) < 1e-7f);
    assert(f.values[0].amp_low == 0 && f.values[1].freq_low == 160);
    assert(sl_haptic_advance(&s, start + 10000000, &f));
    assert(f.sample == 2 && f.values[1].amp_low == 0);
    assert(!sl_haptic_advance(&s, start + 100000000, &f));

    /* Slow output must preserve the middle sample and report timing failure,
     * rather than draining three values at one instant. */
    sl_haptic_init(&s);
    assert(sl_haptic_command(&s, pulse, 69, start, start, false));
    assert(sl_haptic_advance(&s, start + 100000000, &f));
    sl_haptic_output_complete(&s, &f, start + 100000000, start + 100100000, true, true);
    assert(f.sample == 0 && s.stats.late_samples == 1);
    assert(!sl_haptic_advance(&s, start + 100000000, &f));
    assert(!sl_haptic_advance(&s, start + 105000000, &f));
    assert(sl_haptic_advance(&s, start + 105100000, &f));
    assert(f.sample == 1 && f.values[1].amp_low > 0);

    /* Invalid packets do not advance state. Playback overflow still consumes
     * valid wire state, so subsequent relative packets remain meaningful. */
    sl_haptic_init(&s);
    uint8_t bad[69];
    memcpy(bad, pulse, 69);
    bad[1] = 65;
    assert(!sl_haptic_command(&s, bad, 69, start, start, false));
    assert(s.count == 0 && s.stats.commands == 0);
    assert(!sl_haptic_command(&s, pulse, 68, start, start, false));
    for (unsigned i = 0; i < SL_HAPTIC_QUEUE / 3; ++i)
        assert(sl_haptic_command(&s, pulse, 69, start, start, false));
    uint8_t absolute[69];
    memcpy(absolute, pulse, sizeof(absolute));
    /* Absolute AM/FM values different from the queued silence. */
    uint32_t word = 0x40000000u | 64u << 23 | 32u << 16 | 64u << 9 | 96u << 2;
    for (unsigned i = 0; i < 4; ++i)
        absolute[7 + i] = absolute[11 + i] = (uint8_t)(word >> (i * 8));
    assert(!sl_haptic_command(&s, absolute, 69, start, start, false));
    assert(s.stats.overflows == 1 && s.stats.dropped_samples == SL_HAPTIC_QUEUE + 1);
    sl_hd_value held;
    sl_hd_current(&s.decoder[0], &held);
    assert(held.amp_low > .25f && held.freq_low == 80 && held.freq_high == 640);
    /* Free one queue entry, then a relative hold preserves the dropped state. */
    assert(sl_haptic_advance(&s, start, &f));
    uint32_t hold = 0x40000000u | 24u << 25 | 24u << 20;
    for (unsigned i = 0; i < 4; ++i)
        absolute[7 + i] = absolute[11 + i] = (uint8_t)(hold >> (i * 8));
    assert(sl_haptic_command(&s, absolute, 69, start, start, false));
    sl_hd_value after;
    sl_hd_current(&s.decoder[0], &after);
    assert(memcmp(&held, &after, sizeof(held)) == 0);

    /* An explicit ordinary stop/replace discards all queued native frames. */
    assert(sl_haptic_command(&s, ordinary, 9, start, start, true));
    assert(s.count == 1 && sl_haptic_advance(&s, start, &f));
    assert(fabsf(f.values[0].amp_low - .557193398f) < 1e-7f && f.values[1].amp_low == 0);
    assert(!sl_haptic_advance(&s, start + 9999999, &f));
    assert(sl_haptic_advance(&s, start + 10000000, &f));
    assert(f.values[0].amp_low == 0);

    /* Source packet gaps remain gaps even when receiver drain batches them. */
    sl_haptic_init(&s);
    assert(sl_haptic_command(&s, pulse, 69, start, start, false));
    assert(sl_haptic_command(&s, pulse, 69, start + 50000000, start, false));
    for (unsigned i = 0; i < 3; ++i)
        assert(sl_haptic_advance(&s, start + i * 5000000, &f));
    assert(!sl_haptic_advance(&s, start + 49999999, &f));
    assert(sl_haptic_advance(&s, start + 50000000, &f) && f.sample == 0);
    sl_haptic_stop(&s, start + 51000000);
    assert(s.count == 0 && sl_haptic_advance(&s, start + 51000000, &f));
    assert(f.values[0].amp_low == 0 && f.values[1].amp_low == 0);

    /* A little wake jitter must be absorbed by the idle gap, not accumulate
     * into a growing delay on every subsequent packet. */
    sl_haptic_init(&s);
    for (unsigned packet = 0; packet < 100; ++packet) {
        uint64_t source = start + packet * UINT64_C(50000000);
        assert(sl_haptic_command(&s, pulse, 69, source, source, false));
        assert(sl_haptic_deadline(&s) == source);
        for (unsigned sample = 0; sample < 3; ++sample) {
            assert(sl_haptic_advance(&s, sl_haptic_deadline(&s) + 100000, &f));
            uint64_t submitted = f.due_ns + 100000;
            sl_haptic_output_complete(&s, &f, submitted, submitted + 200000, true, true);
        }
    }

    /* Already queued packets retain their source gap after a late first
     * packet, as long as the gap can absorb the delay. */
    sl_haptic_init(&s);
    assert(sl_haptic_command(&s, pulse, 69, start, start, false));
    assert(sl_haptic_command(&s, pulse, 69, start + 50000000, start, false));
    for (unsigned sample = 0; sample < 3; ++sample)
        assert(sl_haptic_advance(&s, start + 2000000 + sample * 5000000, &f));
    assert(sl_haptic_deadline(&s) == start + 50000000);

    /* Selected is not emitted. A slow/failed IPC contributes real lateness
     * and only a successful retry counts as an emitted native sample. */
    sl_haptic_init(&s);
    assert(sl_haptic_command(&s, pulse, 69, start, start, false));
    assert(sl_haptic_advance(&s, start, &f));
    assert(s.stats.scheduled_samples == 1 && s.stats.emitted_samples == 0);
    sl_haptic_output_complete(&s, &f, start + 2000000, start + 3000000, false, true);
    assert(s.stats.emitted_samples == 0 && s.stats.max_lateness_ns == 2000000);
    assert(sl_haptic_advance(&s, start + 4000000, &f));
    sl_haptic_output_complete(&s, &f, start + 4000000, start + 5000000, true, true);
    assert(s.stats.emitted_samples == 1 && s.stats.scheduled_samples == 1);
    assert(sl_haptic_deadline(&s) == start + 10000000);

    /* A batched ordinary -> native transition retains its received gap.
     * The ordinary 10ms expiry still occurs before the native packet at 50ms,
     * without clearing its already decoded samples. */
    sl_haptic_init(&s);
    assert(sl_haptic_command(&s, ordinary, 9, start, start, true));
    assert(sl_haptic_command(&s, pulse, 69, start + 50000000, start, true));
    assert(sl_haptic_advance(&s, start, &f) && !f.native && fabsf(f.values[0].amp_low - .557193398f) < 1e-7f);
    assert(!sl_haptic_advance(&s, start + 9999999, &f));
    assert(sl_haptic_advance(&s, start + 10000000, &f) && !f.native && f.values[0].amp_low == 0);
    assert(s.count == 3);
    assert(!sl_haptic_advance(&s, start + 49999999, &f));
    assert(sl_haptic_advance(&s, start + 50000000, &f) && f.native && f.sample == 0);

    /* An early native packet supersedes the old ordinary expiry; its middle
     * sample must not get erased when that old timer would have elapsed. */
    sl_haptic_init(&s);
    assert(sl_haptic_command(&s, ordinary, 9, start, start, true));
    assert(sl_haptic_command(&s, pulse, 69, start + 5000000, start, true));
    assert(sl_haptic_advance(&s, start, &f) && !f.native);
    assert(sl_haptic_advance(&s, start + 5000000, &f) && f.native && f.sample == 0);
    assert(sl_haptic_advance(&s, start + 10000000, &f) && f.native && f.sample == 1);

    /* A zero-sample motor repeats the wire state after source-time expiry,
     * not the old ordinary strength that is still awaiting playback. */
    uint8_t repeat[69] = {13, 10, 0, 0, 0, 16};
    for (unsigned i = 0; i < 4; ++i)
        repeat[11 + i] = (uint8_t)(hold >> (i * 8));
    for (unsigned expired = 0; expired < 2; ++expired) {
        sl_haptic_init(&s);
        uint64_t gap = expired ? 50000000 : 5000000;
        assert(sl_haptic_command(&s, ordinary, 9, start, start, true));
        assert(sl_haptic_command(&s, repeat, 69, start + gap, start, true));
        assert(sl_haptic_advance(&s, start, &f) && !f.native);
        if (expired)
            assert(sl_haptic_advance(&s, start + 10000000, &f) && !f.native);
        assert(sl_haptic_advance(&s, start + gap, &f) && f.native);
        assert(fabsf(f.values[0].amp_low - (expired ? 0 : .557193398f)) < 1e-7f);
    }

    /* Playback expiry can happen while a pre-expiry wire packet is waiting
     * in IHS's owner queue. Its decoder history must survive that expiry. */
    sl_haptic_init(&s);
    assert(sl_haptic_command(&s, ordinary, 9, start, start, true));
    assert(sl_haptic_advance(&s, start, &f) && !f.native);
    assert(sl_haptic_advance(&s, start + 10000000, &f) && !f.native);
    assert(sl_haptic_command(&s, repeat, 69, start + 5000000, start + 20000000, true));
    assert(sl_haptic_advance(&s, start + 20000000, &f) && f.native);
    assert(fabsf(f.values[0].amp_low - .557193398f) < 1e-7f);
    puts("PASS native pulse preservation, absolute deadlines, late scheduling, gaps, overflow and stop");
}
