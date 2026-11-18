/* Read-only, hash-pinned November Steam controller-state layout.
 * Reader callback bounds each access. No pointer invocation or memory writes.
 * Values are unlocked observations, never an atomic snapshot or device ACK.
 * Source addresses and limits: OFFICIAL_INPUT_RE.md section 22.16.
 */
#ifndef NSL_STEAM_HAPTICS_HOST_STATE_H
#define NSL_STEAM_HAPTICS_HOST_STATE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*nsl_host_read)(void *, uint64_t, void *, size_t);
typedef struct nsl_host_haptics_state {
    unsigned slot;
    uint64_t device_caps, config_caps;
    uint32_t appid, config_type;
    uint16_t ordinary_left, ordinary_right;
    uint8_t pulse_gate, rumble_gate, config_pulse, config_rumble;
    uint8_t preference_valid, haptics_preference, enhanced;
} nsl_host_haptics_state;

/* 1=unique matching local controller, 0=no match, -1=read/layout/inconsistent.
 * Report nothing on an ambiguous match or invalid required field.
 */
static int nsl_host_haptics_state_read(nsl_host_read read, void *context,
                                      uint64_t base, uint64_t handle,
                                      nsl_host_haptics_state *out) {
    uint64_t engine = base + 0x1854550, vt, nodes;
    int32_t root;
#define READ(addr, dest) do { if (!read(context, (addr), &(dest), sizeof(dest))) return -1; } while (0)
    READ(engine, vt);
    if (vt != base + 0x138f580) return -1;
    READ(engine + 0x108 + 0x18, root);
    READ(engine + 0x108 + 0x30, nodes);
    int32_t stack[64], visited[64];
    unsigned pending = 0, count = 0, matches = 0;
    if (root != -1) stack[pending++] = root;
    while (pending) {
        int32_t index = stack[--pending];
        if (index < 0 || index >= 256 || count == 64) return -1;
        for (unsigned i = 0; i < count; ++i) if (visited[i] == index) return -1;
        visited[count++] = index;
        uint64_t node = nodes + (uint64_t)index * 128, parent;
        int32_t children[2];
        READ(node, children);
        for (unsigned i = 0; i < 2; ++i) {
            if (children[i] != -1) {
                if (pending == 64) return -1;
                stack[pending++] = children[i];
            }
        }
        READ(node + 0x78, parent);
        if (!parent) continue;
        READ(parent, vt);
        if (vt != base + 0x1304dc0) return -1;
        uint8_t local;
        READ(parent + 0x10922d, local);
        if (local != 1) continue;
        uint64_t io;
        READ(parent + 0x180, io);
        if (!io) continue;
        READ(io, vt);
        if (vt != base + 0x1304c68) return -1;
        for (unsigned slot = 0; slot < 16; ++slot) {
            uint64_t state = io + slot * 0x2a0, abstraction, id, config;
            READ(state + 0x1c0, abstraction);
            if (!abstraction) continue;
            READ(abstraction, vt);
            if (vt != base + 0x1318fd0) continue;
            uint32_t type;
            READ(abstraction + 0x98, id);
            READ(abstraction + 8, type);
            if (id != handle || type != 38) continue;
            uint8_t valid;
            READ(abstraction + 0xb8, valid);
            if (!valid) continue;
            READ(state + 0x2a0, config);
            if (!config) return -1;
            nsl_host_haptics_state value = {0};
            value.slot = slot;
            READ(abstraction + 0x80, value.device_caps);
            READ(abstraction + 0x588, value.enhanced);
            READ(abstraction + 0x552, value.ordinary_left);
            READ(abstraction + 0x554, value.ordinary_right);
            READ(state + 0x1d0, value.rumble_gate);
            READ(state + 0x1d1, value.pulse_gate);
            READ(config + 0x240, value.config_caps);
            READ(config + 0x230, value.config_type);
            READ(config + 0x258, value.appid);
            READ(config + 0x660, value.config_rumble);
            READ(config + 0x661, value.config_pulse);
            READ(parent + slot * 0xe0 + 0x1f0, value.preference_valid);
            READ(parent + slot * 0xe0 + 0x230 + 0x8c, value.haptics_preference);
            uint64_t check;
            READ(abstraction + 0x98, check);
            if (check != handle) return -1;
            READ(state + 0x2a0, check);
            if (check != config) return -1;
            READ(state + 0x1c0, check);
            if (check != abstraction) return -1;
            if (++matches != 1) return -1;
            *out = value;
        }
    }
    return matches ? 1 : 0;
#undef READ
}
#endif
