/* xinput_hid.c -- which DirectInput controller is an XInput pad.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * See xinput_hid.h.
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "xinput_hid.h"

static int holds_ig(const char *path)
{
    for (; *path; path++)
        if ((path[0] == 'I' || path[0] == 'i') &&
            (path[1] == 'G' || path[1] == 'g') && path[2] == '_')
            return 1;
    return 0;
}

unsigned xinput_hid_list(xinput_hid_t *out, unsigned max)
{
    RAWINPUTDEVICELIST *list = NULL;
    UINT count = 0, k;
    unsigned n = 0;

    /* Sized by the API, and asked again if a device arrives between the
     * two calls: a short buffer fails the whole call. */
    for (;;) {
        if (GetRawInputDeviceList(NULL, &count, sizeof *list) == (UINT)-1 ||
            !count || !(list = malloc(count * sizeof *list)))
            return 0;
        if (GetRawInputDeviceList(list, &count, sizeof *list) != (UINT)-1)
            break;
        free(list);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return 0;
    }
    for (k = 0; k < count && n < max; k++) {
        RID_DEVICE_INFO info;
        char path[512];
        UINT size;

        if (list[k].dwType != RIM_TYPEHID)
            continue;
        memset(&info, 0, sizeof info);
        info.cbSize = sizeof info;
        size = sizeof info;
        if (GetRawInputDeviceInfoA(list[k].hDevice, RIDI_DEVICEINFO, &info,
                                   &size) == (UINT)-1)
            continue;
        size = sizeof path;
        path[0] = '\0';
        if (GetRawInputDeviceInfoA(list[k].hDevice, RIDI_DEVICENAME, path,
                                   &size) == (UINT)-1)
            continue;
        path[sizeof path - 1] = '\0';
        out[n].vid = info.hid.dwVendorId;
        out[n].pid = info.hid.dwProductId;
        out[n].ig = holds_ig(path);
        n++;
    }
    free(list);
    return n;
}

int xinput_hid_is_xinput(const xinput_hid_t *list, unsigned count,
                         unsigned long product_data1)
{
    unsigned k;

    for (k = 0; k < count; k++)
        if (list[k].ig &&
            ((unsigned long)list[k].vid | ((unsigned long)list[k].pid << 16))
                == product_data1)
            return 1;
    return 0;
}

int xinput_hid_hides(const xinput_hid_t *list, unsigned count,
                     unsigned long product_data1, int force_feedback)
{
    return !force_feedback &&
           xinput_hid_is_xinput(list, count, product_data1);
}
