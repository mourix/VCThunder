/* settings.h -- the shape of one configuration setting.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * There is ONE table of settings, in settings.def, and it is the only place a
 * key's section, default, type, range or choices is written down. NOTHING ELSE
 * MAY CARRY A SECOND COPY OF ANY COLUMN. A range or a choice set spelled in
 * more than one place agrees by inspection, which is to say it does not have to
 * agree at all: nothing holds the copies to each other, and each is plausible
 * where it stands.
 *
 * The table has two consumers and they read different columns:
 *
 *   config.c    the STORE half; where the value lands in shim_config_t, how
 *               to parse it, what range or choice set it must satisfy. It also
 *               builds config_defaults() by feeding each entry's `fallback`
 *               through the same parser the ini goes through, so a default
 *               that cannot be spelled in the file cannot exist.
 *   launcher.c  the SHOW half: page, label, control type, help, warning.
 *
 * tools/check-launcher-settings.py reads settings.def directly and holds the
 * table to the shipped ini: every shipped key is one visible entry and every
 * visible entry is a shipped key.
 */
#ifndef VCT_SETTINGS_H
#define VCT_SETTINGS_H

#include <stddef.h>

#include "vcthunder.h"

/* Launcher pages. Here rather than in launcher.c because settings.def names
 * one per entry; NAV_* stays with the navigation control that owns it. */
enum {
    PAGE_HOME,
    PAGE_GRAPHICS,
    PAGE_AUDIO,
    PAGE_BINDINGS,
    PAGE_DEVICES,
    PAGE_FFB,
    PAGE_SYSTEM,
    PAGE_LINK,
    PAGE_FILES,
    PAGE_TROUBLE,
    PAGE_COUNT
};

/* What the launcher puts on screen for this key. */
typedef enum {
    ST_TEXT,
    ST_BOOL,
    ST_ENUM,
    ST_UINT,
    ST_AUTO_UINT,
    ST_MHZ_UINT
} setting_type_t;

typedef enum {
    WARNING_NONE,
    WARNING_CAUTION,
    WARNING_DANGER
} warning_level_t;

enum {
    SF_NONE      = 0,
    SF_REQUIRED  = 1 << 0,
    SF_DIR       = 1 << 1,
    SF_OPEN_FILE = 1 << 2,
    SF_SAVE_FILE = 1 << 3,
    SF_AXIS      = 1 << 4,
    SF_HEX       = 1 << 5,
    SF_HIDDEN    = 1 << 6       /* parsed, never shown, never shipped */
};

/* How config.c stores this key, which is a different question from how the
 * launcher shows it: `aspect`, `log_level` and `video_mode` are all one combo
 * box and land in a char[], an enum and a signed int respectively. */
typedef enum {
    /* Not config.c's key at all. diego.c, dinput.c and the launcher read the
     * [analog], [ffb] and binding sections straight from the file through
     * their own precedence rule (ini_read_scoped), so the loader must leave them
     * alone, but the launcher still edits them and the ini still ships them,
     * which is why they are in this table. */
    VCT_STORE_NONE = 0,
    VCT_STORE_STR,          /* char[field_size]                               */
    VCT_STORE_ENUM,         /* char[field_size], and it must be in `choices`   */
    VCT_STORE_BOOL,         /* int; true/false, yes/no, on/off, 1/0           */
    VCT_STORE_UINT,         /* unsigned in [minimum, maximum]                 */
    VCT_STORE_UINT_AUTO,    /* as UINT, and the word "auto" means 0           */
    VCT_STORE_LEVEL,        /* log_level_t, named in `choices`                */
    VCT_STORE_MODE          /* int; "auto"/"cabinet"/empty is -1              */
} vct_store_t;

typedef struct {
    const char *section, *key;

    /* ---- the store half, read by config.c ---- */
    vct_store_t store;
    unsigned    field_off, field_size;  /* into shim_config_t; 0/0 if NONE   */
    const char *fallback;               /* the default, spelled as ini text  */
    const char *choices;                /* '|'-separated, or NULL            */
    unsigned long minimum, maximum;

    /* ---- the show half, read by launcher.c ---- */
    int             page;               /* -1 on a hidden entry              */
    const char     *label;
    setting_type_t  type;
    unsigned        flags;
    warning_level_t warning;
    const char     *help;
} setting_desc_t;

/* The two halves of a settings.def row. Both expand identically for every
 * consumer (only SETTING and SETTING_DEV differ), so they live here. */
#define STORE(store_kind, field, dflt, choice_list, lo, hi)                  \
    store_kind,                                                              \
    (unsigned)offsetof(shim_config_t, field),                                \
    (unsigned)sizeof(((shim_config_t *)0)->field),                           \
    dflt, choice_list, (unsigned long)(lo), (unsigned long)(hi)

/* A key config.c does not own. It still has a default and a choice set,
 * because the launcher validates it and the shipped ini carries it. */
#define STORE_ELSEWHERE(dflt, choice_list, lo, hi)                           \
    VCT_STORE_NONE, 0u, 0u, dflt, choice_list,                               \
    (unsigned long)(lo), (unsigned long)(hi)

#define SHOW(page_id, label, ctrl, flags, warning, help)                     \
    page_id, label, ctrl, flags, warning, help

/* A developer switch: parsed wherever it is found, shown nowhere, and absent
 * from the shipped ini on purpose (docs/configuration.md §6). */
#define SHOW_NEVER                                                           \
    -1, NULL, ST_TEXT, SF_HIDDEN, WARNING_NONE, NULL

/* Every entry, then every entry the launcher shows. Counted here so the
 * launcher's parallel value array can be a fixed-size static. */
enum {
    VCT_SETTING_COUNT = 0
#define SETTING(section, key, store, show)     + 1
#define SETTING_DEV(section, key, store)       + 1
#include "settings.def"
#undef SETTING
#undef SETTING_DEV
};

enum {
    VCT_UI_SETTING_COUNT = 0
#define SETTING(section, key, store, show)     + 1
#define SETTING_DEV(section, key, store)
#include "settings.def"
#undef SETTING
#undef SETTING_DEV
};

/* Every setting, in table order. */
extern const setting_desc_t vct_setting[VCT_SETTING_COUNT];
/* Only the ones the launcher shows and the ini ships, in table order. The
 * launcher writes every entry of ITS array back to the file on save, so a
 * developer switch must not be in it or opening the launcher would inject one. */
extern const setting_desc_t vct_ui_setting[VCT_UI_SETTING_COUNT];

/* Case-insensitive membership in a '|'-separated list. Shared because the
 * launcher validates a choice before writing it and config.c validates the
 * same choice after reading it, and one spelling rule serves both. */
int vct_choice_contains(const char *choices, const char *value);

#endif /* VCT_SETTINGS_H */
