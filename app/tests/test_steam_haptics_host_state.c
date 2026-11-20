#include "../../tools/steam-haptics-host-state.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static const uint64_t base = UINT64_C(0x138000000), id = UINT64_C(0x0057e2009f4e844f);
typedef struct region { uint64_t address; size_t size; unsigned char *bytes; } region;
static region regions[6];
static bool mutate;
static bool read_state(void *context, uint64_t address, void *out, size_t size) {
    (void)context;
    for (unsigned i = 0; i < 6; ++i) {
        region *r = regions + i;
        if (address >= r->address && size <= r->size && address - r->address <= r->size - size) {
            memcpy(out, r->bytes + address - r->address, size);
            if (mutate && address == 0x300000 + 0x98) {
                uint64_t changed = id + 1;
                memcpy(r->bytes + 0x98, &changed, 8);
            }
            return true;
        }
    }
    return false;
}
static void put(uint64_t address, uint64_t value, size_t size) {
    for (unsigned i = 0; i < 6; ++i) {
        region *r = regions + i;
        if (address >= r->address && address - r->address + size <= r->size) {
            memcpy(r->bytes + address - r->address, &value, size);
            return;
        }
    }
    abort();
}
static void fixture(void) {
    const uint64_t addresses[] = {base + 0x1854550, 0x100000, 0x180000, 0x2a0000, 0x300000, 0x310000};
    const size_t sizes[] = {4096, 256, 0x11c000, 0x3000, 4096, 4096};
    for (unsigned i = 0; i < 6; ++i) {
        free(regions[i].bytes);
        regions[i] = (region){addresses[i], sizes[i], calloc(1, sizes[i])};
        assert(regions[i].bytes);
    }
    mutate = false;
    put(addresses[0], base + 0x138f580, 8);
    put(addresses[0] + 0x120, 0, 4); put(addresses[0] + 0x138, 0x100000, 8);
    put(0x100000, UINT64_MAX, 8); put(0x100078, 0x180000, 8);
    put(0x180000, base + 0x1304dc0, 8); put(0x180000 + 0x10922d, 1, 1);
    put(0x180180, 0x2a0000, 8); put(0x2a0000, base + 0x1304c68, 8);
    put(0x2a01c0, 0x300000, 8); put(0x2a02a0, 0x310000, 8);
    put(0x300000, base + 0x1318fd0, 8); put(0x300008, 38, 4);
    put(0x300098, id, 8); put(0x3000b8, 1, 1); put(0x300080, 0x8b6bff, 8);
    put(0x300588, 1, 1); put(0x2a01d1, 1, 1); put(0x310661, 1, 1);
    put(0x310240, 0x8b6bff, 8); put(0x310230, 38, 4); put(0x310258, 769, 4);
    put(0x1801f0, 1, 1); put(0x1802bc, 1, 1);
}
int main(void) {
    nsl_host_haptics_state s;
    fixture();
    assert(nsl_host_haptics_state_read(read_state, NULL, base, id, &s) == 1);
    assert(s.slot == 0 && s.device_caps == 0x8b6bff && s.config_caps == 0x8b6bff);
    assert(s.appid == 769 && s.pulse_gate && s.config_pulse && s.haptics_preference && s.preference_valid);
    /* Actual forwarding selects +0x190 for flag=0. The old discovery skipped
     * every such parent before checking whether its IO is inspectable. */
    put(0x180000 + 0x10922d, 0, 1);put(0x180190, 0x2a0000, 8);
    assert(nsl_host_haptics_parent_read(read_state, NULL, base, 0x180000, id, &s) == 1);
    assert(s.io_address == 0x2a0000 && s.parent_address == 0x180000);
    put(0x310240, 0, 8); put(0x2a01d1, 0, 1); put(0x310661, 0, 1);
    assert(nsl_host_haptics_state_read(read_state, NULL, base, id, &s) == 1);
    assert(!s.config_caps && !s.pulse_gate && !s.config_pulse);
    fixture(); put(base + 0x1854550, 0, 8);
    assert(nsl_host_haptics_state_read(read_state, NULL, base, id, &s) == -1);
    fixture(); put(0x100000, 0, 4); /* cycle */
    assert(nsl_host_haptics_state_read(read_state, NULL, base, id, &s) == -1);
    fixture(); put(0x100000, 256, 4);
    assert(nsl_host_haptics_state_read(read_state, NULL, base, id, &s) == -1);
    fixture(); put(0x300008, 4, 4);
    assert(nsl_host_haptics_state_read(read_state, NULL, base, id, &s) == 0);
    fixture(); put(0x2a02a0, 0x900000, 8); /* unreadable */
    assert(nsl_host_haptics_state_read(read_state, NULL, base, id, &s) == -1);
    fixture(); mutate = true;
    assert(nsl_host_haptics_state_read(read_state, NULL, base, id, &s) == -1);
    for (unsigned i = 0; i < 6; ++i) free(regions[i].bytes);
    return 0;
}
