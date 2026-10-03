/* di_canon.h -- the canonical DirectInput offset an axis object's guidType names.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * AN AXIS'S DATA LANDS AT THE OFFSET ITS `guidType` NAMES, not at the slot
 * `EnumObjects` reports it in. SDL's haptic backend and Supermodel both address
 * an object this way, and on a G29, a G923 and a Pro DD the object's real
 * offset is REFUSED while this one is accepted and felt. One copy, for the host
 * and the launcher's live view both: the one answer they cannot disagree about.
 */
#ifndef VCT_DI_CANON_H
#define VCT_DI_CANON_H

/* Requires <dinput.h>, which each caller includes with its own
 * DIRECTINPUT_VERSION already set. */

/* No axis: the GUID names none, or there is no slot left for it. */
#define DI_CANON_NONE 0xFFFFFFFFu

/* CALL THIS EXACTLY ONCE PER AXIS OBJECT, IN ENUMERATION ORDER, and before any
 * early return that skips an object; reset the counter when the enumeration
 * restarts. Every slider carries GUID_Slider, so the GUID alone cannot say
 * which slider an object is and only its position in the enumeration can,
 * which is why the counter is a parameter rather than a static: the signature
 * states the contract. DIJOYSTATE2 has two slider slots, so a third slider has
 * no canonical offset and gets DI_CANON_NONE; answering DIJOFS_SLIDER(1) would
 * alias it onto the second slider's data and hand a force effect an axis it is
 * not. */
static DWORD di_canonical_offset(const GUID *g, unsigned *slider_seen)
{
    if (IsEqualGUID(g, &GUID_XAxis))  return DIJOFS_X;
    if (IsEqualGUID(g, &GUID_YAxis))  return DIJOFS_Y;
    if (IsEqualGUID(g, &GUID_ZAxis))  return DIJOFS_Z;
    if (IsEqualGUID(g, &GUID_RxAxis)) return DIJOFS_RX;
    if (IsEqualGUID(g, &GUID_RyAxis)) return DIJOFS_RY;
    if (IsEqualGUID(g, &GUID_RzAxis)) return DIJOFS_RZ;
    if (IsEqualGUID(g, &GUID_Slider))
        switch ((*slider_seen)++) {
        case 0:  return (DWORD)DIJOFS_SLIDER(0);
        case 1:  return (DWORD)DIJOFS_SLIDER(1);
        default: return DI_CANON_NONE;
        }
    return DI_CANON_NONE;
}

#endif /* VCT_DI_CANON_H */
