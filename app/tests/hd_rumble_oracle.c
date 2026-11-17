#include "platform/hd_rumble.h"
#include <stdio.h>
int main(void) {
    sl_hd_state state;
    sl_hd_reset(&state);
    uint8_t raw[4];
    while (fread(raw, 1, 4, stdin) == 4) {
        uint32_t word = (uint32_t)raw[0] | (uint32_t)raw[1] << 8 |
                        (uint32_t)raw[2] << 16 | (uint32_t)raw[3] << 24;
        sl_hd_samples samples;
        sl_hd_decode(&state, word, &samples);
        printf("%u", samples.count);
        for (unsigned i = 0; i < samples.count; ++i) {
            sl_hd_value *v = &samples.values[i];
            printf(" %.9g %.9g %.9g %.9g", v->amp_low, v->freq_low,
                   v->amp_high, v->freq_high);
        }
        putchar('\n');
    }
    return ferror(stdin) ? 1 : 0;
}
