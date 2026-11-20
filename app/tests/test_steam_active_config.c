#include "../../tools/steam-active-config.h"
#include <assert.h>

int main(void) {
    /* Sequence taken from the 17:17:34 host archive: the client input cache
     * remained 769 after this later device activation. */
    const char *log =
        "[2026-11-20 17:14:01] Queueing activation for controller: 0 app: 769\n"
        "[2026-11-20 17:14:08] Queueing activation for controller: 0 app: 413080\n"
        "[2026-11-20 17:14:09] Queueing activation for controller: 1 app: 4115450\n";
    uint32_t appid = 0;
    assert(nsl_steam_last_activation(log, 0, &appid) && appid == 413080);
    assert(nsl_steam_last_activation(log, 1, &appid) && appid == 4115450);
    assert(!nsl_steam_last_activation(log, 2, &appid));
    assert(!nsl_steam_last_activation("Queueing activation for controller: ? app: ?", 0, &appid));
    return 0;
}
