/* arctic_dsound.c -- Arctic's DirectSound, routed by [audio] as the other titles are.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The cabinet's amplifier takes the PC's line out as two buses, the front
 * speakers and the woofers (seat woofer included), and the host treats them as
 * it treats Hydro's and Offroad's: left is the front bus, right the seat bus,
 * and [audio] output, balance and volume act per voice through
 * audio_route_buses()' rules.
 *
 * The game mixes through DirectSound, so a voice's two bus gains are its
 * buffer's volume and pan. DirectSoundCreate is a host boundary returning a
 * proxy that forwards everything; SetVolume and SetPan on a secondary buffer
 * are kept as the game's values and replaced by the routed pair. The primary
 * buffer is the real one.
 *
 * All per-call arithmetic is in hundredths of a dB, in integers: the game
 * calls from goose's fibers with the x87 at 24-bit precision. The log-sum table and the bus trims are computed on the host thread
 * before the game runs.
 */
#include "vcthunder.h"
#include "arctic.h"

#include <initguid.h>
#include <dsound.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CB_MIN DSBVOLUME_MIN          /* silence                              */
#define CB_SPAN 10000

typedef HRESULT (WINAPI *ds_create_fn)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);

static ds_create_fn s_create;
static int s_on;                      /* arctic_dsound_init() ran            */
static int s_mute;                    /* [audio] audio = false               */
static int s_cabinet;                 /* [audio] output = cabinet            */
static int s_front_cb, s_seat_cb;     /* the balance's bus trims             */
static int s_mix_cb;                  /* mix: back to the game's own level   */
static int s_user_cb;                 /* [audio] volume                      */
static short s_logsum[CB_SPAN + 1];   /* 20log10(1 + 10^(-d/20)), in cB      */
static volatile LONG s_voices, s_said;

static int cb_of(float g)
{
    double cb;

    if (g <= 0.0f)
        return CB_MIN;
    cb = 2000.0 * log10((double)g);
    return cb < CB_MIN ? CB_MIN : (int)lround(cb);
}

/* The routed (volume, pan) for the game's (volume, pan). DirectSound's pan
 * attenuates one channel and leaves the other at the buffer's volume; centre
 * is both at full (IDirectSoundBuffer::SetPan, SetVolume: hundredths of a dB,
 * -10000 is silence). */
static void route(LONG vol, LONG pan, LONG *ovol, LONG *opan)
{
    int a = (int)vol + (pan > 0 ? -(int)pan : 0) + s_front_cb;
    int b = (int)vol + (pan < 0 ? (int)pan : 0) + s_seat_cb;
    int l, r, top;

    if (s_mute || vol <= CB_MIN) {
        *ovol = CB_MIN;
        *opan = DSBPAN_CENTER;
        return;
    }
    if (a < CB_MIN) a = CB_MIN;
    if (b < CB_MIN) b = CB_MIN;
    if (s_cabinet) {
        l = a;
        r = b;
    } else {
        int d = abs(a - b);

        l = r = (a > b ? a : b) + s_logsum[d > CB_SPAN ? CB_SPAN : d] + s_mix_cb;
    }
    l += s_user_cb;
    r += s_user_cb;
    /* DirectSound has no gain above 0 dB: a boost keeps the pair's ratio and
     * stops at full scale. */
    top = l > r ? l : r;
    if (top > 0) {
        l -= top;
        r -= top;
        top = 0;
    }
    *ovol = top < CB_MIN ? CB_MIN : top;
    r -= l;
    *opan = r < DSBPAN_LEFT ? DSBPAN_LEFT : r > DSBPAN_RIGHT ? DSBPAN_RIGHT : r;
}

/* ---- the buffer proxy --------------------------------------------------- */

typedef struct {
    IDirectSoundBuffer iface;
    IDirectSoundBuffer *real;
    LONG refs;
    LONG vol, pan;                    /* the game's values                   */
} dsb_t;

static IDirectSoundBufferVtbl s_dsb_vtbl;

static dsb_t *dsb(IDirectSoundBuffer *b) { return (dsb_t *)b; }

static HRESULT apply(dsb_t *p)
{
    LONG v, pan;
    HRESULT hr;

    route(p->vol, p->pan, &v, &pan);
    hr = IDirectSoundBuffer_SetVolume(p->real, v);
    if (SUCCEEDED(hr))
        hr = IDirectSoundBuffer_SetPan(p->real, pan);
    return hr;
}

static HRESULT wrap(IDirectSoundBuffer *real, LONG vol, LONG pan,
                    IDirectSoundBuffer **out)
{
    dsb_t *p = calloc(1, sizeof *p);

    if (!p) {
        IDirectSoundBuffer_Release(real);
        *out = NULL;
        return DSERR_OUTOFMEMORY;
    }
    p->iface.lpVtbl = &s_dsb_vtbl;
    p->real = real;
    p->refs = 1;
    p->vol = vol;
    p->pan = pan;
    apply(p);
    InterlockedIncrement(&s_voices);
    *out = &p->iface;
    return DS_OK;
}

static HRESULT WINAPI b_QueryInterface(IDirectSoundBuffer *t, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDirectSoundBuffer)) {
        InterlockedIncrement(&dsb(t)->refs);
        *out = t;
        return S_OK;
    }
    return IDirectSoundBuffer_QueryInterface(dsb(t)->real, iid, out);
}

static ULONG WINAPI b_AddRef(IDirectSoundBuffer *t)
{
    return (ULONG)InterlockedIncrement(&dsb(t)->refs);
}

static ULONG WINAPI b_Release(IDirectSoundBuffer *t)
{
    dsb_t *p = dsb(t);
    LONG n = InterlockedDecrement(&p->refs);

    if (!n) {
        IDirectSoundBuffer_Release(p->real);
        free(p);
    }
    return (ULONG)n;
}

static HRESULT WINAPI b_GetCaps(IDirectSoundBuffer *t, LPDSBCAPS c)
{ return IDirectSoundBuffer_GetCaps(dsb(t)->real, c); }
static HRESULT WINAPI b_GetCurrentPosition(IDirectSoundBuffer *t, LPDWORD p, LPDWORD w)
{ return IDirectSoundBuffer_GetCurrentPosition(dsb(t)->real, p, w); }
static HRESULT WINAPI b_GetFormat(IDirectSoundBuffer *t, LPWAVEFORMATEX f, DWORD n, LPDWORD w)
{ return IDirectSoundBuffer_GetFormat(dsb(t)->real, f, n, w); }
static HRESULT WINAPI b_GetFrequency(IDirectSoundBuffer *t, LPDWORD f)
{ return IDirectSoundBuffer_GetFrequency(dsb(t)->real, f); }
static HRESULT WINAPI b_GetStatus(IDirectSoundBuffer *t, LPDWORD s)
{ return IDirectSoundBuffer_GetStatus(dsb(t)->real, s); }
static HRESULT WINAPI b_Initialize(IDirectSoundBuffer *t, LPDIRECTSOUND d, LPCDSBUFFERDESC c)
{ return IDirectSoundBuffer_Initialize(dsb(t)->real, d, c); }
static HRESULT WINAPI b_Lock(IDirectSoundBuffer *t, DWORD o, DWORD n, LPVOID *p1,
                             LPDWORD n1, LPVOID *p2, LPDWORD n2, DWORD f)
{ return IDirectSoundBuffer_Lock(dsb(t)->real, o, n, p1, n1, p2, n2, f); }
static HRESULT WINAPI b_Play(IDirectSoundBuffer *t, DWORD r1, DWORD r2, DWORD f)
{ return IDirectSoundBuffer_Play(dsb(t)->real, r1, r2, f); }
static HRESULT WINAPI b_SetCurrentPosition(IDirectSoundBuffer *t, DWORD p)
{ return IDirectSoundBuffer_SetCurrentPosition(dsb(t)->real, p); }
static HRESULT WINAPI b_SetFormat(IDirectSoundBuffer *t, LPCWAVEFORMATEX f)
{ return IDirectSoundBuffer_SetFormat(dsb(t)->real, f); }
static HRESULT WINAPI b_SetFrequency(IDirectSoundBuffer *t, DWORD f)
{ return IDirectSoundBuffer_SetFrequency(dsb(t)->real, f); }
static HRESULT WINAPI b_Stop(IDirectSoundBuffer *t)
{ return IDirectSoundBuffer_Stop(dsb(t)->real); }
static HRESULT WINAPI b_Unlock(IDirectSoundBuffer *t, LPVOID p1, DWORD n1, LPVOID p2, DWORD n2)
{ return IDirectSoundBuffer_Unlock(dsb(t)->real, p1, n1, p2, n2); }
static HRESULT WINAPI b_Restore(IDirectSoundBuffer *t)
{ return IDirectSoundBuffer_Restore(dsb(t)->real); }

static HRESULT WINAPI b_GetVolume(IDirectSoundBuffer *t, LPLONG v)
{
    if (!v)
        return DSERR_INVALIDPARAM;
    *v = dsb(t)->vol;
    return DS_OK;
}

static HRESULT WINAPI b_GetPan(IDirectSoundBuffer *t, LPLONG pan)
{
    if (!pan)
        return DSERR_INVALIDPARAM;
    *pan = dsb(t)->pan;
    return DS_OK;
}

static HRESULT WINAPI b_SetVolume(IDirectSoundBuffer *t, LONG v)
{
    if (v < DSBVOLUME_MIN || v > DSBVOLUME_MAX)
        return DSERR_INVALIDPARAM;
    dsb(t)->vol = v;
    return apply(dsb(t));
}

static HRESULT WINAPI b_SetPan(IDirectSoundBuffer *t, LONG pan)
{
    if (pan < DSBPAN_LEFT || pan > DSBPAN_RIGHT)
        return DSERR_INVALIDPARAM;
    dsb(t)->pan = pan;
    return apply(dsb(t));
}

static IDirectSoundBufferVtbl s_dsb_vtbl = {
    b_QueryInterface, b_AddRef, b_Release, b_GetCaps, b_GetCurrentPosition,
    b_GetFormat, b_GetVolume, b_GetPan, b_GetFrequency, b_GetStatus,
    b_Initialize, b_Lock, b_Play, b_SetCurrentPosition, b_SetFormat,
    b_SetVolume, b_SetPan, b_SetFrequency, b_Stop, b_Unlock, b_Restore,
};

/* ---- the device proxy --------------------------------------------------- */

typedef struct {
    IDirectSound iface;
    IDirectSound *real;
    LONG refs;
} ds_t;

static ds_t *ds(IDirectSound *d) { return (ds_t *)d; }

static HRESULT WINAPI d_QueryInterface(IDirectSound *t, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDirectSound)) {
        InterlockedIncrement(&ds(t)->refs);
        *out = t;
        return S_OK;
    }
    return IDirectSound_QueryInterface(ds(t)->real, iid, out);
}

static ULONG WINAPI d_AddRef(IDirectSound *t)
{
    return (ULONG)InterlockedIncrement(&ds(t)->refs);
}

static ULONG WINAPI d_Release(IDirectSound *t)
{
    ds_t *p = ds(t);
    LONG n = InterlockedDecrement(&p->refs);

    if (!n) {
        IDirectSound_Release(p->real);
        free(p);
    }
    return (ULONG)n;
}

/* A secondary buffer gets volume and pan control if it lacks them (a 3D
 * buffer cannot have pan and is left alone), so every voice can be routed. */
static HRESULT WINAPI d_CreateSoundBuffer(IDirectSound *t, LPCDSBUFFERDESC desc,
                                          LPLPDIRECTSOUNDBUFFER out, IUnknown *outer)
{
    DSBUFFERDESC d;
    IDirectSoundBuffer *real = NULL;
    HRESULT hr;

    if (!desc || !out || desc->dwSize < sizeof(DSBUFFERDESC1) ||
        desc->dwSize > sizeof d ||
        (desc->dwFlags & (DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D)))
        return IDirectSound_CreateSoundBuffer(ds(t)->real, desc, out, outer);
    memcpy(&d, desc, desc->dwSize);
    d.dwFlags |= DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN;
    hr = IDirectSound_CreateSoundBuffer(ds(t)->real, &d, &real, outer);
    if (FAILED(hr)) {
        *out = NULL;
        return hr;
    }
    if (!InterlockedExchange(&s_said, 1))
        LOGI("arctic: the game's DirectSound voices are routed by [audio]"
             " (first buffer: flags 0x%lx)", (unsigned long)desc->dwFlags);
    return wrap(real, DSBVOLUME_MAX, DSBPAN_CENTER, out);
}

static HRESULT WINAPI d_DuplicateSoundBuffer(IDirectSound *t, LPDIRECTSOUNDBUFFER src,
                                             LPLPDIRECTSOUNDBUFFER out)
{
    IDirectSoundBuffer *real = NULL;
    HRESULT hr;

    if (!src || src->lpVtbl != &s_dsb_vtbl)
        return IDirectSound_DuplicateSoundBuffer(ds(t)->real, src, out);
    hr = IDirectSound_DuplicateSoundBuffer(ds(t)->real, dsb(src)->real, &real);
    if (FAILED(hr)) {
        if (out)
            *out = NULL;
        return hr;
    }
    return wrap(real, dsb(src)->vol, dsb(src)->pan, out);
}

static HRESULT WINAPI d_GetCaps(IDirectSound *t, LPDSCAPS c)
{ return IDirectSound_GetCaps(ds(t)->real, c); }
static HRESULT WINAPI d_SetCooperativeLevel(IDirectSound *t, HWND w, DWORD l)
{ return IDirectSound_SetCooperativeLevel(ds(t)->real, w, l); }
static HRESULT WINAPI d_Compact(IDirectSound *t)
{ return IDirectSound_Compact(ds(t)->real); }
static HRESULT WINAPI d_GetSpeakerConfig(IDirectSound *t, LPDWORD c)
{ return IDirectSound_GetSpeakerConfig(ds(t)->real, c); }
static HRESULT WINAPI d_SetSpeakerConfig(IDirectSound *t, DWORD c)
{ return IDirectSound_SetSpeakerConfig(ds(t)->real, c); }
static HRESULT WINAPI d_Initialize(IDirectSound *t, LPCGUID g)
{ return IDirectSound_Initialize(ds(t)->real, g); }

static IDirectSoundVtbl s_ds_vtbl = {
    d_QueryInterface, d_AddRef, d_Release, d_CreateSoundBuffer, d_GetCaps,
    d_DuplicateSoundBuffer, d_SetCooperativeLevel, d_Compact,
    d_GetSpeakerConfig, d_SetSpeakerConfig, d_Initialize,
};

static HRESULT WINAPI b_DirectSoundCreate(LPCGUID guid, LPDIRECTSOUND *out,
                                          LPUNKNOWN outer)
{
    IDirectSound *real = NULL;
    ds_t *p;
    HRESULT hr = s_create(guid, &real, outer);

    if (FAILED(hr) || !out) {
        if (out)
            *out = NULL;
        return hr;
    }
    if (!(p = calloc(1, sizeof *p))) {
        IDirectSound_Release(real);
        *out = NULL;
        return DSERR_OUTOFMEMORY;
    }
    p->iface.lpVtbl = &s_ds_vtbl;
    p->real = real;
    p->refs = 1;
    *out = &p->iface;
    return hr;
}

/* ---- setup ---------------------------------------------------------------- */

void arctic_dsound_init(void)
{
    float front, seat, mix, user;
    int d;

    audio_route_config(g_cfg.audio_routing, g_cfg.audio_balance,
                       g_cfg.audio_volume);
    audio_route_gains(&s_cabinet, &front, &seat, &mix, &user);
    s_mute = !g_cfg.audio || user <= 0.0f;
    s_front_cb = cb_of(front);
    s_seat_cb = cb_of(seat);
    /* mix plays (front*Gl + seat*Gr) and is scaled so a centred voice, at
     * full on both channels under DirectSound's pan law, keeps its level:
     * the analogue of audio_route_buses()' constant-power mix gain. */
    s_mix_cb = -cb_of(front + seat);
    s_user_cb = cb_of(user);
    for (d = 0; d <= CB_SPAN; d++)
        s_logsum[d] = (short)lround(2000.0 * log10(1.0 + pow(10.0, -d / 2000.0)));
    s_on = 1;
    LOGI("arctic: audio %s, front bus %d cB, seat bus %d cB, volume %u%%%s",
         s_cabinet ? "cabinet (front bus left, seat woofer right)"
                   : "mix (both buses to both speakers)",
         s_front_cb, s_seat_cb, g_cfg.audio_volume,
         s_mute ? ", muted" : "");
}

/* DSOUND.dll is imported by ordinal. Ordinal 1 is answered here when it is
 * the system's DirectSoundCreate; NULL means bind the real entry. */
void *arctic_dsound_bind(HMODULE dsound, unsigned ord)
{
    FARPROC by_ord;

    if (ord != 1 || !s_on || !dsound)
        return NULL;
    by_ord = GetProcAddress(dsound, MAKEINTRESOURCEA(ord));
    if (!by_ord || by_ord != GetProcAddress(dsound, "DirectSoundCreate")) {
        LOGW("arctic: DSOUND ordinal 1 is not DirectSoundCreate here; the"
             " game's sound plays unrouted");
        return NULL;
    }
    s_create = (ds_create_fn)by_ord;
    return (void *)b_DirectSoundCreate;
}
