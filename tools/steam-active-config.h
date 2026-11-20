#ifndef NSL_STEAM_ACTIVE_CONFIG_H
#define NSL_STEAM_ACTIVE_CONFIG_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* This is the latest queued activation in Steam's log, not an atomic worker
 * readback. It must be reported as such. The client input cache is per-process
 * and can stay at 769 while the actual desktop device changes to 413080. */
static bool nsl_steam_last_activation(const char *text, unsigned index, uint32_t *appid) {
    const char *p = text;
    bool found = false;
    while ((p = strstr(p, "Queueing activation for controller: "))) {
        unsigned controller, app;
        if (sscanf(p, "Queueing activation for controller: %u app: %u", &controller, &app) == 2 &&
            controller == index) {
            *appid = app;
            found = true;
        }
        ++p;
    }
    return found;
}
#endif
