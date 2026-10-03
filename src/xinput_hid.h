/* xinput_hid.h -- which DirectInput controller is an XInput pad.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * MAME's winhybrid rule: a game controller whose HID path holds "IG_" is an
 * XInput device (Microsoft's documented convention) and is read through
 * XInput only. It is matched to its DirectInput entry by VID/PID, which
 * DirectInput packs into guidProduct.Data1. RawInput lists the paths without
 * COM.
 */
#ifndef VCT_XINPUT_HID_H
#define VCT_XINPUT_HID_H

typedef struct {
    unsigned vid, pid;
    int      ig;                     /* the path holds IG_ */
} xinput_hid_t;

#define XINPUT_HID_MAX 64

/* The attached HID devices, from RawInput. */
unsigned xinput_hid_list(xinput_hid_t *out, unsigned max);

/* Does `product_data1` (a DirectInput guidProduct.Data1) name an XInput
 * device in `list`? */
int xinput_hid_is_xinput(const xinput_hid_t *list, unsigned count,
                         unsigned long product_data1);

/* Whether DirectInput sets this device aside, for the host and the launcher
 * alike: an XInput device, unless it publishes force feedback, because XInput
 * has no constant force and an Xbox-edition wheel would lose its motor. */
int xinput_hid_hides(const xinput_hid_t *list, unsigned count,
                     unsigned long product_data1, int force_feedback);

#endif /* VCT_XINPUT_HID_H */
