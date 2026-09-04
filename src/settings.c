/* settings.c -- settings.def, expanded twice.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Two arrays from one table. They are separate rather than one array with a
 * filter because save_settings() writes every entry of the launcher's array
 * back to vcthunder.ini: a developer switch in that array would be injected
 * into the user's file the first time they opened the launcher.
 */
#include "vcthunder.h"
#include "settings.h"

#include <string.h>

const setting_desc_t vct_setting[VCT_SETTING_COUNT] = {
#define SETTING(section, key, store, show)  { section, key, store, show },
#define SETTING_DEV(section, key, store)    { section, key, store, SHOW_NEVER },
#include "settings.def"
#undef SETTING
#undef SETTING_DEV
};

const setting_desc_t vct_ui_setting[VCT_UI_SETTING_COUNT] = {
#define SETTING(section, key, store, show)  { section, key, store, show },
#define SETTING_DEV(section, key, store)
#include "settings.def"
#undef SETTING
#undef SETTING_DEV
};

int vct_choice_contains(const char *choices, const char *value)
{
    const char *p = choices;
    size_t n;

    if (!choices || !value)
        return 0;
    n = strlen(value);
    while (*p) {
        const char *end = strchr(p, '|');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (len == n && _strnicmp(p, value, n) == 0)
            return 1;
        if (!end)
            break;
        p = end + 1;
    }
    return 0;
}
