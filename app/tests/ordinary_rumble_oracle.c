#include "platform/hd_rumble.h"
#include <stdio.h>
int main(void) {
    uint8_t bytes[4];
    while (fread(bytes, 1, 4, stdin) == 4) {
        uint16_t low = (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
        uint16_t high = (uint16_t)bytes[2] | (uint16_t)bytes[3] << 8;
        uint32_t word = sl_hd_ordinary_word(low, high);
        for (unsigned i = 0; i < 4; ++i)
            bytes[i] = (uint8_t)(word >> (8*i));
        if (fwrite(bytes, 1, 4, stdout) != 4)
            return 1;
    }
    return ferror(stdin) ? 1 : 0;
}
