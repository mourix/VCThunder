/* capture.c -- record Glide calls and the caller-owned payloads they reference.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Provider outputs, callbacks, and window handles are replaced by the replay.
 * Pointer identities are retained for state-buffer pairing. Capture is not a
 * performance-neutral mode.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "vcglide.h"
#include "vgl.h"

static FILE    *s_f;
static unsigned s_records;
static unsigned s_frames;
static uint64_t s_bytes;
static unsigned s_dropped;      /* payloads that could not be read */

/* The write-mode LFB region currently held by the caller, if any. */
static struct {
    int      active;
    void    *ptr;
    unsigned stride, buffer, origin, bytes;
} s_lfb;

/* Validate caller payloads against readable memory regions. Cache the last
 * region to avoid repeated VirtualQuery calls for the vertex arena. */
static struct { const unsigned char *base; size_t size; } s_ok;

static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    const unsigned char *c = (const unsigned char *)p;

    if (!p || !n)
        return 0;
    /* An argument that is not a pointer can be any 32-bit value, including
     * 0xFFFFFFFF (grConstantColorValue passes exactly that), and c + n
     * would then WRAP and compare below the region it is supposed to be
     * above. Rule it out before any arithmetic depends on it. */
    if ((uintptr_t)c + n < (uintptr_t)c)
        return 0;
    if (c >= s_ok.base && c + n <= s_ok.base + s_ok.size)
        return 1;
    if (!VirtualQuery(p, &mbi, sizeof mbi))
        return 0;
    if (mbi.State != MEM_COMMIT)
        return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return 0;
    s_ok.base = (const unsigned char *)mbi.BaseAddress;
    s_ok.size = mbi.RegionSize;
    return c >= s_ok.base && c + n <= s_ok.base + s_ok.size;
}

/* ---- the writer --------------------------------------------------------- */

/* A failed write stops the capture where it stands rather than going through
 * vgl_cap_close(), which writes a terminating record and would re-enter here
 * on the same failing stream. The file is left short and the log says so;
 * a truncated capture is a readable capture, because every record carries
 * its own length. */
static void put(const void *p, size_t n)
{
    if (!s_f)
        return;
    if (fwrite(p, 1, n, s_f) != n) {
        FILE *f = s_f;
        s_f = NULL;
        vgl_capturing = 0;
        fclose(f);
        vgl_log(0, "capture: WRITE FAILED after %llu bytes; capture stopped "
                   "and the file is truncated at the last complete record",
                (unsigned long long)s_bytes);
        return;
    }
    s_bytes += n;
}

static void put_pad(size_t n)
{
    static const char zero[4] = { 0, 0, 0, 0 };
    size_t r = n & 3u;
    if (r)
        put(zero, 4u - r);
}

static void put_blob(uint32_t tag, const void *data, uint32_t size)
{
    vglc_blob_t b;
    b.tag = tag;
    b.size = size;
    put(&b, sizeof b);
    put(data, size);
    put_pad(size);
}

/* ---- payload rules ------------------------------------------------------
 *
 * One function, because the rule and the size belong together: whoever adds
 * an entry point with a new payload shape edits exactly this switch and
 * entries.def, and a replay reads the rule out of the capture rather than
 * having to agree with either. */

static uint32_t s_blob_tag[16];
static const void *s_blob_ptr[16];
static uint32_t s_blob_size[16];
static unsigned s_blob_n;

static void want(uint32_t tag, const void *p, uint32_t size)
{
    if (s_blob_n >= 16)
        return;
    if (!readable(p, size)) {
        s_dropped++;
        if (s_dropped <= 8)
            vgl_log(0, "capture: UNREADABLE payload %u bytes at %p (tag %u) "
                       "-- record %u will replay short", size, p, tag,
                    s_records);
        return;
    }
    s_blob_tag[s_blob_n] = tag;
    s_blob_ptr[s_blob_n] = p;
    s_blob_size[s_blob_n] = size;
    s_blob_n++;
}

/* Compute texture-chain size with the renderer's format model. Capture must not
 * depend on an optional backend being present to provide payload sizes. */
static uint32_t tex_bytes(uint32_t even_odd, const void *info)
{
    if (!readable(info, GR_TEXINFO_BYTES))
        return 0;
    return vgl_tex_mem_required((int)even_odd, info);
}

/* Capture a full mip-chain source regardless of the selected parity mask.
 * Fall back to the selected footprint only when the full extent is unreadable. */
static uint32_t tex_download_bytes(uint32_t even_odd, const void *info,
                                   const void *data)
{
    uint32_t whole = tex_bytes(GR_MIPMAPLEVELMASK_BOTH, info);
    uint32_t part;

    if (whole && readable(data, whole))
        return whole;
    part = tex_bytes(even_odd, info);
    if (whole && part && part != whole) {
        static int reported;
        if (!reported) {
            reported = 1;
            vgl_log(0, "capture: the %u-byte chain at %p is not readable in "
                       "full: falling back to %u bytes for mask %u. If this "
                       "title packs one parity per buffer that is correct; if "
                       "not, this capture under-reads and its replay will "
                       "differ on textured surfaces.", whole, data, part,
                    even_odd);
        }
    }
    return part;
}

static void collect(unsigned ix, const uint32_t *a)
{
    const unsigned char *info;
    uint32_t n, bytes;

    s_blob_n = 0;
    switch (vgl_entries[ix].payload) {
    case PL_V3:
        want(VGLC_TAG_ARG(0), (const void *)(uintptr_t)a[0], GR_VERTEX_BYTES);
        want(VGLC_TAG_ARG(1), (const void *)(uintptr_t)a[1], GR_VERTEX_BYTES);
        want(VGLC_TAG_ARG(2), (const void *)(uintptr_t)a[2], GR_VERTEX_BYTES);
        break;

    case PL_V2:
        want(VGLC_TAG_ARG(0), (const void *)(uintptr_t)a[0], GR_VERTEX_BYTES);
        want(VGLC_TAG_ARG(1), (const void *)(uintptr_t)a[1], GR_VERTEX_BYTES);
        break;

    case PL_VLIST:
        n = a[0];
        if (n > 4096u)          /* a polygon, not an array of them */
            break;
        want(VGLC_TAG_ARG(1), (const void *)(uintptr_t)a[1],
             n * (uint32_t)GR_VERTEX_BYTES);
        break;

    case PL_FOG:
        want(VGLC_TAG_ARG(0), (const void *)(uintptr_t)a[0], GR_FOG_TABLE_BYTES);
        break;

    case PL_TEXDOWN:
    case PL_TEXINFO3:
        info = (const unsigned char *)(uintptr_t)a[3];
        want(VGLC_TAG_ARG(3), info, GR_TEXINFO_BYTES);
        if (vgl_entries[ix].payload == PL_TEXDOWN &&
            readable(info, GR_TEXINFO_BYTES)) {
            const void *data = (const void *)(uintptr_t)
                *(const uint32_t *)(info + GR_TEXINFO_DATA);
            bytes = tex_download_bytes(a[2], info, data);
            if (bytes)
                want(VGLC_TAG_TEXDATA, data, bytes);
        }
        break;

    case PL_TEXINFO1:
        want(VGLC_TAG_ARG(1), (const void *)(uintptr_t)a[1], GR_TEXINFO_BYTES);
        break;

    case PL_TEXPART: {
        /* Unverified path: treat arg7 as one complete mip level and ignore the
         * row subset to avoid under-capture. */
        unsigned char tmp[GR_TEXINFO_BYTES];
        *(uint32_t *)(tmp + GR_TEXINFO_SMALLLOD) = a[2];
        *(uint32_t *)(tmp + GR_TEXINFO_LARGELOD) = a[2];
        *(uint32_t *)(tmp + GR_TEXINFO_ASPECT)   = a[4];
        *(uint32_t *)(tmp + GR_TEXINFO_FORMAT)   = a[5];
        *(uint32_t *)(tmp + GR_TEXINFO_DATA)     = 0;
        bytes = tex_bytes(GR_MIPMAPLEVELMASK_BOTH, tmp);
        if (bytes)
            want(VGLC_TAG_ARG(7), (const void *)(uintptr_t)a[7], bytes);
        break;
    }

    case PL_VIDTIMING:
        /* A Glide build that passed a mode enum here rather than a table
         * would put a small integer in the same slot, and reading it as an
         * address would be the mistake this rule exists to avoid. */
        if (a[1] >= 0x10000u)
            want(VGLC_TAG_ARG(1), (const void *)(uintptr_t)a[1],
                 GR_VIDTIMING_BYTES);
        break;

    case PL_TEXTABLE:
        bytes = (a[1] == GR_TEXTABLE_PALETTE) ? GR_PALETTE_BYTES
                                              : GR_NCCTABLE_BYTES;
        want(VGLC_TAG_ARG(2), (const void *)(uintptr_t)a[2], bytes);
        break;

    default:
        break;
    }
}

/* An argument that looks like a pointer into the caller's address space, on
 * an entry declared to have no payload, is the one way this table can be
 * quietly wrong: the replay would then pass an address that means nothing.
 * Say so once per entry rather than discover it as a wrong picture later.
 * The game's own span starts at 0x100000 and this is a 32-bit process, so a
 * committed, readable address above that is the test. */
static void suspect_pointer(unsigned ix, const uint32_t *a)
{
    static int reported[VGL_COUNT];
    unsigned i, words = vgl_entries[ix].argbytes / 4u;

    if (reported[ix] || vgl_entries[ix].payload != PL_NONE)
        return;
    for (i = 0; i < words; i++) {
        if (a[i] < 0x100000u)
            continue;
        if (!readable((const void *)(uintptr_t)a[i], 4))
            continue;
        reported[ix] = 1;
        vgl_log(0, "capture: %s argument %u = 0x%08X is a READABLE ADDRESS and "
                   "this entry is declared PL_NONE. Either it is a coincidence "
                   "or entries.def is missing a payload rule for it: check "
                   "before trusting a replay of this capture.",
                vgl_entries[ix].name, i, a[i]);
        return;
    }
}

/* ---- the interface used by the seam ------------------------------------- */

void vgl_cap_call(unsigned ix, const uint32_t *args)
{
    vglc_record_t r;
    unsigned i;

    if (!s_f)
        return;
    suspect_pointer(ix, args);
    collect(ix, args);

    r.idx = (uint16_t)ix;
    r.blobs = (uint16_t)s_blob_n;
    put(&r, sizeof r);
    put(args, vgl_entries[ix].argbytes);
    for (i = 0; i < s_blob_n; i++)
        put_blob(s_blob_tag[i], s_blob_ptr[i], s_blob_size[i]);
    s_records++;
}

static void control(uint32_t kind, uint32_t a1, uint32_t a2, uint32_t a3,
                    const void *blob, uint32_t blob_size)
{
    vglc_record_t r;
    uint32_t args[4];

    if (!s_f)
        return;
    r.idx = VGLC_CONTROL;
    r.blobs = blob ? 1u : 0u;
    args[0] = kind; args[1] = a1; args[2] = a2; args[3] = a3;
    put(&r, sizeof r);
    put(args, sizeof args);
    if (blob)
        put_blob(VGLC_TAG_LFB, blob, blob_size);
    s_records++;
}

void vgl_cap_frame(unsigned frame)
{
    s_frames = frame;
    control(VGLC_FRAME, frame, 0, 0, NULL, 0);
}

/* After grLfbLock returned. A read-only lock is not interesting: the caller
 * is taking data OUT, and a replay produces its own. A write lock is: what
 * the caller puts there is input, and it exists only until the unlock. */
void vgl_cap_lfb_locked(const uint32_t *a, int ret)
{
    const unsigned char *info = (const unsigned char *)(uintptr_t)a[5];
    unsigned h;

    s_lfb.active = 0;
    if (!s_f || !ret || a[0] == GR_LFB_READ_ONLY)
        return;
    if (!readable(info, GR_LFBINFO_BYTES))
        return;

    s_lfb.ptr    = (void *)(uintptr_t)*(const uint32_t *)(info + GR_LFBINFO_PTR);
    s_lfb.stride = *(const uint32_t *)(info + GR_LFBINFO_STRIDE);
    s_lfb.origin = *(const uint32_t *)(info + GR_LFBINFO_ORIGIN);
    s_lfb.buffer = a[1];

    /* Nothing in the lock arguments carries a height: the region is the whole
     * surface, and the surface is what grSstWinOpen produced. */
    h = (unsigned)(vgl_surface_h > 0 ? vgl_surface_h : 0);
    if (!h || !s_lfb.stride || !s_lfb.ptr)
        return;
    s_lfb.bytes = s_lfb.stride * h;
    if (!readable(s_lfb.ptr, s_lfb.bytes))
        return;
    s_lfb.active = 1;
}

/* Immediately before grLfbUnlock. */
void vgl_cap_lfb_pending(void)
{
    if (!s_f || !s_lfb.active)
        return;
    control(VGLC_LFB, s_lfb.buffer, s_lfb.stride, s_lfb.origin,
            s_lfb.ptr, s_lfb.bytes);
    s_lfb.active = 0;
}

/* ---- open and close ----------------------------------------------------- */

void vgl_cap_open(void)
{
    vglc_header_t h;
    unsigned i;

    s_f = fopen(vgl_cfg.capture, "wb");
    if (!s_f) {
        vgl_log(0, "capture: cannot write '%s'; capture is OFF",
                vgl_cfg.capture);
        return;
    }
    /* Big buffer: the writer is on the game's own render thread. */
    setvbuf(s_f, NULL, _IOFBF, 1u << 22);

    memset(&h, 0, sizeof h);
    h.magic = VGLC_MAGIC;
    h.version = VGLC_VERSION;
    h.entry_count = VGL_COUNT;
    h.vertex_bytes = GR_VERTEX_BYTES;
    snprintf(h.title, sizeof h.title, "%s", vgl_cfg.title);
    /* The header field is older than this line: it named the forwarding
     * backend a capture was taken through, back when there could be one. There
     * is exactly one provider now, so it records that, and the field STAYS in
     * the header rather than being removed, because dropping it would change
     * vglc's layout and every capture on disk is a comparand this project still
     * replays. */
    snprintf(h.backend, sizeof h.backend, "%s", "vcglide");

    h.header_bytes = sizeof h;
    for (i = 0; i < VGL_COUNT; i++) {
        size_t n = sizeof(vglc_entry_t) + strlen(vgl_entries[i].name);
        h.header_bytes += (uint32_t)((n + 3u) & ~(size_t)3u);
    }
    put(&h, sizeof h);
    for (i = 0; i < VGL_COUNT; i++) {
        vglc_entry_t e;
        size_t len = strlen(vgl_entries[i].name);
        e.argbytes = vgl_entries[i].argbytes;
        e.payload  = vgl_entries[i].payload;
        e.namelen  = (uint8_t)len;
        put(&e, sizeof e);
        put(vgl_entries[i].name, len);
        put_pad(sizeof e + len);
    }

    vgl_capturing = 1;
    vgl_cap_frame(0);
    vgl_log(0, "capture: writing '%s' (%u entry points, %s frames)",
            vgl_cfg.capture, VGL_COUNT,
            vgl_cfg.capture_frames ? "limited" : "unlimited");
}

void vgl_cap_close(const char *why)
{
    FILE *f = s_f;

    if (!f)
        return;
    vgl_capturing = 0;
    control(VGLC_END, s_records, s_frames, 0, NULL, 0);
    s_f = NULL;                 /* control() above is the last write */
    fclose(f);
    vgl_log(0, "capture: closed (%s); %u records, %u frames, %llu bytes, "
               "%u payloads dropped", why, s_records, s_frames,
            (unsigned long long)s_bytes, s_dropped);
}
