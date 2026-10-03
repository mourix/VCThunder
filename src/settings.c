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
#include "ini.h"

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

/* XInput's six axis names, from settings.def's xinput_throttle_axis row. */
static const char *xinput_axes(void)
{
    unsigned i;

    for (i = 0; i < VCT_SETTING_COUNT; i++)
        if (!strcmp(vct_setting[i].key, "xinput_throttle_axis"))
            return vct_setting[i].choices;
    return NULL;
}

int vct_axis_choice_ok(const char *choices, const char *value)
{
    char label[INI_LABEL_CAP], axis[32];

    if (!ini_parse_axis_ref(value, label, sizeof label, axis, sizeof axis))
        return 0;
    /* After a label, XInput's names too: `pad:rt` is a role on an XInput
     * pad slot. The host says when the axis is not that controller's. */
    return vct_choice_contains(choices, axis) ||
           (label[0] && vct_choice_contains(xinput_axes(), axis));
}
