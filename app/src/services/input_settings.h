#pragma once
#include <stdbool.h>
/* Streamed game input preferences, independent from the local UI bindings.
 * swap_buttons exchanges A/B and X/Y before reports reach the host, like the
 * Nintendo face layout option of other Switch streaming clients. Missing or
 * unknown files keep the default letter-for-letter mapping. */
bool sl_input_settings_load(const char *directory);
bool sl_input_settings_save(const char *directory, bool swap_buttons);
