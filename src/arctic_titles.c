/* arctic_titles.c -- which win32 title is running, and its generated tables.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The ETS titles have profile G, chosen once by select_title(); the win32
 * titles have this. It is deliberately the whole of the selection: a file that
 * named a title's generated symbol directly would compile against one title
 * for good, and every address and size in this project is per-title data.
 *
 * No windows.h, no host state and no logging: config.c and the unit suites
 * link this without the Arctic host.
 */
#include "arctic.h"
#include <string.h>

static const arctic_title_t *s_active;

const arctic_title_t *arctic_title_by_id(const char *id)
{
    unsigned i;

    if (!id || !*id)
        return 0;
    for (i = 0; g_arctic_titles[i]; i++)
        if (!_stricmp(g_arctic_titles[i]->ids->id, id))
            return g_arctic_titles[i];
    return 0;
}

const arctic_title_t *arctic_title(void)
{
    return s_active;
}

int arctic_title_select(const char *id)
{
    const arctic_title_t *t = arctic_title_by_id(id);

    if (!t)
        return 0;
    s_active = t;
    return 1;
}
