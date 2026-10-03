/* xi_axes.h -- an XInput pad's six axes and ten buttons.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * One copy for the host (dinput.c, io_host.c) and the launcher's preview.
 */
#ifndef VCT_XI_AXES_H
#define VCT_XI_AXES_H

/* Requires <xinput.h>, which each caller includes. */

/* lx, ly, rx, ry, lt, rt. A stick is -1..+1; a trigger is -1 released .. +1
 * pressed, so a pedal role reads it as (v + 1) / 2, like a DirectInput pedal. */
static inline float xi_axis_unit(const XINPUT_GAMEPAD *g, unsigned index)
{
    switch (index) {
    case 0: return (float)g->sThumbLX / (g->sThumbLX < 0 ? 32768.0f : 32767.0f);
    case 1: return (float)g->sThumbLY / (g->sThumbLY < 0 ? 32768.0f : 32767.0f);
    case 2: return (float)g->sThumbRX / (g->sThumbRX < 0 ? 32768.0f : 32767.0f);
    case 3: return (float)g->sThumbRY / (g->sThumbRY < 0 ? 32768.0f : 32767.0f);
    case 4: return (float)g->bLeftTrigger / 255.0f * 2.0f - 1.0f;
    default: return (float)g->bRightTrigger / 255.0f * 2.0f - 1.0f;
    }
}

/* Bit 0 is button 1: A, B, X, Y, LB, RB, Back, Start, LS, RS. */
static inline unsigned xi_button_bits(WORD buttons)
{
    static const WORD native[] = {
        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_LEFT_SHOULDER,
        XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_LEFT_THUMB,
        XINPUT_GAMEPAD_RIGHT_THUMB
    };
    unsigned out = 0, i;

    for (i = 0; i < sizeof native / sizeof native[0]; i++)
        if (buttons & native[i])
            out |= 1u << i;
    return out;
}

#endif /* VCT_XI_AXES_H */
