#include "platform/haptic_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct capture {
    sl_haptic_frame frames[8];
    unsigned count;
    uint64_t clock;
} capture;

static void pump(sl_haptic_stream *s, capture *out, uint64_t until) {
    for (;;) {
        uint64_t next = sl_haptic_deadline(s);
        if (!next || next > until)
            break;
        if (next < out->clock)
            next = out->clock;
        assert(next <= until);
        sl_haptic_frame f;
        assert(sl_haptic_advance(s, next, &f));
        if (f.native) {
            assert(out->count < 8);
            out->frames[out->count++] = f;
        }
        sl_haptic_output_complete(s, &f, next, next, true, true);
        out->clock = next;
    }
    out->clock = until;
}

static void wire(uint8_t b[69], uint32_t left, uint32_t right) {
    memset(b, 0, 69);
    b[0] = 13; b[1] = 10; b[5] = 16;
    for (unsigned i = 0; i < 4; ++i) {
        b[7+i] = (uint8_t)(left >> (8*i));
        b[11+i] = (uint8_t)(right >> (8*i));
    }
}

/* Keep the receive history identical while moving ONLY the owner-drain times.
 * The output clock can expire the ordinary value before older received data
 * is applied. Native values/order must remain invariant under that delay. */
static capture scenario(unsigned received_ms, unsigned delay_ms, bool paired,
                         uint32_t left, uint32_t right) {
    const uint64_t start = UINT64_C(1000000000);
    sl_haptic_stream s;
    sl_haptic_init(&s);
    capture out = {.clock = start};
    const uint8_t ordinary[9] = {1, 255, 255, 0, 128, 10};
    assert(sl_haptic_command(&s, ordinary, 9, start, start, paired));
    pump(&s, &out, start);
    uint8_t command[69];
    wire(command, left, right);
    uint64_t received = start + (uint64_t)received_ms * 1000000;
    uint64_t apply = received + (uint64_t)delay_ms * 1000000;
    pump(&s, &out, apply);
    assert(sl_haptic_command(&s, command, 69, received, apply, paired));
    pump(&s, &out, apply);
    /* A relative hold reveals state even when the preceding format emitted
     * no samples, or reset only one band/motor. */
    wire(command, 0x71800000, 0x71800000);
    received += 1000000;
    apply += 1000000;
    pump(&s, &out, apply);
    assert(sl_haptic_command(&s, command, 69, received, apply, paired));
    pump(&s, &out, start + 1000000000);
    assert(!s.count && !s.stats.rejected && !s.stats.dropped_samples);
    return out;
}

int main(void) {
    const unsigned gaps[] = {0, 1, 5, 9, 10, 11, 20, 50};
    const unsigned delays[] = {0, 1, 5, 10, 20, 100};
    /* All seven wire forms, paired independently across the two motors. */
    const uint32_t forms[] = {
        0,
        0x71800000,
        0x60208180,
        0x40000001 | 64u<<23 | 24u<<18 | 24u<<13 | 24u<<8 | 24u<<3,
        0x80000000 | 24u<<25 | 24u<<20 | 24u<<15 | 24u<<10,
        0x80000001 | 64u<<23 | 24u<<18 | 64u<<1 | 24u<<13 | 24u<<8,
        0xc0000000 | 24u<<25 | 24u<<20 | 24u<<15 | 24u<<10 | 24u<<5 | 24u
    };
    unsigned cases = 0;
    for (unsigned paired = 0; paired < 2; ++paired)
        for (unsigned g = 0; g < sizeof(gaps)/sizeof(gaps[0]); ++g)
            for (unsigned l = 0; l < 7; ++l)
                for (unsigned r = 0; r < 7; ++r) {
                    capture expected = scenario(gaps[g], 0, paired, forms[l], forms[r]);
                    for (unsigned d = 0; d < sizeof(delays)/sizeof(delays[0]); ++d) {
                        capture actual = scenario(gaps[g], delays[d], paired, forms[l], forms[r]);
                        assert(actual.count == expected.count);
                        for (unsigned i = 0; i < actual.count; ++i) {
                            assert(actual.frames[i].sequence == expected.frames[i].sequence);
                            assert(actual.frames[i].sample == expected.frames[i].sample);
                            if (memcmp(actual.frames[i].values, expected.frames[i].values,
                                       sizeof(actual.frames[i].values)) != 0) {
                                fprintf(stderr, "decode depends on playback: gap=%u delay=%u paired=%u forms=%u/%u sample=%u\n",
                                        gaps[g], delays[d], paired, l, r, i);
                                return 1;
                            }
                        }
                        ++cases;
                    }
                }
    printf("PASS %u source/owner-clock interleavings across all seven forms and both motors\n", cases);
}
