#include "input_settings.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool paths(const char *dir, char *path, char *tmp, char *backup) {
    return snprintf(path, 512, "%s/input.conf", dir) < 512 &&
           snprintf(tmp, 512, "%s/input.tmp", dir) < 512 &&
           snprintf(backup, 512, "%s/input.bak", dir) < 512;
}
bool sl_input_settings_load(const char *dir) {
    char path[512], tmp[512], backup[512], text[64] = {0};
    if (!paths(dir, path, tmp, backup))
        return false;
    FILE *file = fopen(path, "rb");
    if (!file && errno == ENOENT)
        file = fopen(backup, "rb");
    if (!file)
        return false;
    size_t size = fread(text, 1, sizeof(text) - 1, file);
    bool ok = !ferror(file) && fgetc(file) == EOF;
    if (fclose(file))
        ok = false;
    /* Exact schema only: unknown versions or values keep the default mapping. */
    const char *swapped = "version=1\nswap_buttons=1\n";
    return ok && size == strlen(swapped) && !memcmp(text, swapped, size);
}
bool sl_input_settings_save(const char *dir, bool swap_buttons) {
    char path[512], tmp[512], backup[512];
    if (!paths(dir, path, tmp, backup) || (mkdir(dir, 0700) && errno != EEXIST))
        return false;
    FILE *file = fopen(tmp, "wb");
    if (!file)
        return false;
    bool ok = fprintf(file, "version=1\nswap_buttons=%d\n", swap_buttons ? 1 : 0) > 0;
    if (fflush(file))
        ok = false;
    if (ok && fsync(fileno(file)) && errno != ENOSYS && errno != EINVAL)
        ok = false;
    if (fclose(file))
        ok = false;
    if (ok && rename(tmp, path)) {
        /* Horizon does not replace existing files. Keep a recoverable backup. */
        ok = false;
        if (errno == EEXIST || errno == ENOTEMPTY) {
            if ((remove(backup) == 0 || errno == ENOENT) && rename(path, backup) == 0) {
                ok = rename(tmp, path) == 0;
                if (!ok)
                    rename(backup, path);
            }
        }
    }
    if (ok)
        remove(backup);
    else
        remove(tmp);
    return ok;
}
