/* Platform-independent live game-source scheduling. All levels are original
 * uint16 motor values; this code invents no frequency or device gain mapping. */
#ifndef NSL_HAPTICS_PANEL_STATE_H
#define NSL_HAPTICS_PANEL_STATE_H
#include <stdbool.h>
#include <stdint.h>

typedef enum { NSL_PANEL_HOLD, NSL_PANEL_ONCE, NSL_PANEL_REPEAT } nsl_panel_mode;
typedef struct nsl_panel_state {
    uint16_t low, high, sent_low, sent_high;
    uint32_t on_ms, off_ms, limit_ms;
    uint64_t started_ms, last_send_ms;
    nsl_panel_mode mode;
    bool active, sent;
} nsl_panel_state;

static inline bool nsl_panel_valid(const nsl_panel_state *s) {
    return s && s->mode <= NSL_PANEL_REPEAT && s->mode >= NSL_PANEL_HOLD &&
        s->on_ms >= 20 && s->on_ms <= 60000 && s->off_ms >= 20 &&
        s->off_ms <= 60000 && s->limit_ms >= 100 && s->limit_ms <= 600000;
}
static inline bool nsl_panel_start(nsl_panel_state *s, uint64_t now) {
    if (!nsl_panel_valid(s)) return false;
    s->started_ms = now; s->active = true; s->sent = false;
    return true;
}
static inline void nsl_panel_stop(nsl_panel_state *s) { s->active = false; }
static inline bool nsl_panel_next(nsl_panel_state *s, uint64_t now, uint16_t out[2]) {
    bool on = false;
    if (s->active) {
        if (now < s->started_ms || now - s->started_ms >= s->limit_ms) s->active = false;
        else {
            uint64_t elapsed = now - s->started_ms;
            if (s->mode == NSL_PANEL_ONCE && elapsed >= s->on_ms) s->active = false;
            else on = s->mode != NSL_PANEL_REPEAT || elapsed % ((uint64_t)s->on_ms + s->off_ms) < s->on_ms;
        }
    }
    out[0] = on ? s->low : 0; out[1] = on ? s->high : 0;
    bool update = !s->sent || out[0] != s->sent_low || out[1] != s->sent_high ||
        ((out[0] || out[1]) && now - s->last_send_ms >= 100);
    if (update) {
        s->sent_low = out[0]; s->sent_high = out[1]; s->last_send_ms = now; s->sent = true;
    }
    return update;
}
#endif
