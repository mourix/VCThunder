/* glide-replay.c -- replays captured Glide calls through a 32-bit provider.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "../renderer/api/glide2.h"
#include "../renderer/core/capture.h"
#include "../renderer/core/png.h"

/* Duplicate reports to stdout and the optional report file. */
static FILE *s_report;

static void rep(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (s_report) {
        va_start(ap, fmt);
        vfprintf(s_report, fmt, ap);
        va_end(ap);
        fflush(s_report);
    }
}

/* ---- the capture -------------------------------------------------------- */

typedef struct {
    char     name[64];
    uint16_t argbytes;
    uint8_t  payload;
    void    *fn;                /* resolved in the provider, or NULL */
    unsigned calls;
} entry_t;

#define ENTRY_MAX 512u
#define ARG_MAX    16u

static entry_t  s_entry[ENTRY_MAX];
static unsigned s_entries;
static const unsigned char *s_base, *s_cur, *s_end;
static unsigned s_vertex_bytes = GR_VERTEX_BYTES;

/* ---- calling a provider ------------------------------------------------- */

/* Relay a known byte count through the provider's __stdcall interface. */
static int call_stdcall(void *fn, const uint32_t *a, unsigned words)
{
    int ret;

    __asm__ __volatile__(
        "testl %%ecx, %%ecx\n\t"
        "jz 2f\n"
        "1:\n\t"
        "decl %%ecx\n\t"
        "pushl (%%esi,%%ecx,4)\n\t"
        "testl %%ecx, %%ecx\n\t"
        "jnz 1b\n"
        "2:\n\t"
        "call *%%edi\n\t"
        : "=a"(ret), "+c"(words)
        : "S"(a), "D"(fn)
        : "edx", "memory", "cc");
    return ret;
}

/* Preserve caller-buffer identity with one 64 KB shadow per address. */
#define SHADOW_MAX   256
#define SHADOW_BYTES 65536

static struct { uint32_t orig; unsigned char *buf; } s_shadow[SHADOW_MAX];
static unsigned s_shadows;

static void *shadow_for(uint32_t orig)
{
    unsigned i;

    if (!orig)
        return NULL;
    for (i = 0; i < s_shadows; i++)
        if (s_shadow[i].orig == orig)
            return s_shadow[i].buf;
    if (s_shadows >= SHADOW_MAX) {
        fprintf(stderr, "replay: more than %d caller buffers; refusing to "
                        "alias unrelated captured state\n", SHADOW_MAX);
        return NULL;
    }
    s_shadow[s_shadows].orig = orig;
    s_shadow[s_shadows].buf = (unsigned char *)calloc(1, SHADOW_BYTES);
    if (!s_shadow[s_shadows].buf) {
        fprintf(stderr, "replay: cannot allocate a caller-buffer shadow\n");
        return NULL;
    }
    return s_shadow[s_shadows++].buf;
}

/* ---- reading the stream ------------------------------------------------- */

static int take(const void **p, size_t n)
{
    if (n > (size_t)(s_end - s_cur))
        return 0;
    *p = s_cur;
    s_cur += n;
    return 1;
}

static unsigned payload_min_words(unsigned payload)
{
    switch (payload) {
    case PL_STATE_IN: case PL_STATE_OUT: case PL_HWCFG: case PL_CALLBACK:
        return 1;
    case PL_WINOPEN:
        return 2;
    case PL_LFBINFO:
        return 6;
    default:
        return 0;
    }
}

static int load(const char *path, HANDLE *fh, HANDLE *mh)
{
    const vglc_header_t *h;
    LARGE_INTEGER size;
    unsigned i;

    *fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (*fh == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "replay: cannot open %s\n", path);
        return 0;
    }
    if (!GetFileSizeEx(*fh, &size) || size.QuadPart < (LONGLONG)sizeof *h) {
        fprintf(stderr, "replay: %s is too short to be a capture\n", path);
        return 0;
    }
    if ((ULONGLONG)size.QuadPart > (ULONGLONG)SIZE_MAX) {
        fprintf(stderr, "replay: %s is too large for this 32-bit tool\n", path);
        return 0;
    }
    *mh = CreateFileMappingA(*fh, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!*mh) {
        fprintf(stderr, "replay: cannot map %s\n", path);
        return 0;
    }
    s_base = (const unsigned char *)MapViewOfFile(*mh, FILE_MAP_READ, 0, 0, 0);
    if (!s_base) {
        fprintf(stderr, "replay: cannot view %s\n", path);
        return 0;
    }
    s_end = s_base + size.QuadPart;

    h = (const vglc_header_t *)s_base;
    if (h->magic != VGLC_MAGIC) {
        fprintf(stderr, "replay: %s is not a .vglc capture\n", path);
        return 0;
    }
    if (h->version != VGLC_VERSION) {
        fprintf(stderr, "replay: capture version %u, this tool speaks %u\n",
                h->version, VGLC_VERSION);
        return 0;
    }
    if (!h->entry_count || h->entry_count > ENTRY_MAX) {
        fprintf(stderr, "replay: capture declares %u entries; limit is %u\n",
                h->entry_count, ENTRY_MAX);
        return 0;
    }
    s_vertex_bytes = h->vertex_bytes;
    rep("capture   %s\n", path);
    rep("  title   %.31s, taken through %.63s\n", h->title, h->backend);
    rep("  entries %u, GrVertex %u bytes, %.1f MB\n",
           h->entry_count, h->vertex_bytes,
           (double)size.QuadPart / (1024.0 * 1024.0));

    /* The capture names its own entries. That is what lets this tool replay a
     * capture taken by a build whose table has since grown. */
    s_cur = s_base + sizeof *h;
    for (i = 0; i < h->entry_count; i++) {
        const void *p;
        const vglc_entry_t *e;
        size_t adv;
        if (!take(&p, sizeof *e))
            return 0;
        e = (const vglc_entry_t *)p;
        if (!e->namelen || e->namelen >= sizeof s_entry[i].name) {
            fprintf(stderr, "replay: entry %u has invalid name length %u\n",
                    i, e->namelen);
            return 0;
        }
        if ((e->argbytes & 3u) || e->argbytes > ARG_MAX * 4u) {
            fprintf(stderr, "replay: entry %u has invalid argument size %u\n",
                    i, e->argbytes);
            return 0;
        }
        if (e->payload > PL_VIDTIMING) {
            fprintf(stderr, "replay: entry %u has unknown payload rule %u\n",
                    i, e->payload);
            return 0;
        }
        if (e->argbytes / 4u < payload_min_words(e->payload)) {
            fprintf(stderr, "replay: entry %u's payload rule %u needs more "
                            "than %u argument bytes\n", i, e->payload,
                    e->argbytes);
            return 0;
        }
        s_entry[i].argbytes = e->argbytes;
        s_entry[i].payload  = e->payload;
        if (!take(&p, e->namelen))
            return 0;
        memcpy(s_entry[i].name, p, e->namelen);
        s_entry[i].name[e->namelen] = 0;
        adv = (sizeof *e + e->namelen) & 3u;
        if (adv && !take(&p, 4u - adv))
            return 0;
    }
    if (h->header_bytes < (uint32_t)(s_cur - s_base) ||
        h->header_bytes > (uint32_t)(s_end - s_base)) {
        fprintf(stderr, "replay: invalid header size %u (parsed %u, file %u)\n",
                h->header_bytes, (unsigned)(s_cur - s_base),
                (unsigned)(s_end - s_base));
        return 0;
    }
    s_entries = h->entry_count;
    s_cur = s_base + h->header_bytes;
    return 1;
}

/* ---- the provider ------------------------------------------------------- */

static HMODULE s_provider;
static void   *s_read_region;   /* grLfbReadRegion, if the provider has it */

static int bind_provider(const char *dll)
{
    unsigned i, bound = 0;

    s_provider = LoadLibraryA(dll);
    if (!s_provider) {
        fprintf(stderr, "replay: cannot load provider %s (err=%lu)\n",
                dll, GetLastError());
        return 0;
    }
    for (i = 0; i < s_entries; i++) {
        char decorated[128];
        /* %.*s: name comes from the capture's own header, so its length is
           whatever the file says; bound it to what a decorated stdcall name
           can hold here. A truncated name simply fails to bind and is counted,
           which is the behaviour we want for a malformed capture. */
        snprintf(decorated, sizeof decorated, "_%.*s@%u",
                 (int)(sizeof decorated - sizeof "_@4294967295"),
                 s_entry[i].name, s_entry[i].argbytes);
        s_entry[i].fn = (void *)GetProcAddress(s_provider, decorated);
        if (s_entry[i].fn)
            bound++;
        else
            rep("  provider does NOT export %s\n", decorated);
    }
    /* Not one of the 69 the games call, so it is not in the capture, but a
     * provider that has it gives this tool its picture back for nothing.
     * Without it there is a replay and no frames to compare. */
    s_read_region = (void *)GetProcAddress(s_provider, "_grLfbReadRegion@28");
    rep("provider  %s: %u/%u entry points, framebuffer readback %s\n",
           dll, bound, s_entries, s_read_region ? "available" : "MISSING");
    return 1;
}

/* ---- replay ------------------------------------------------------------- */

static unsigned s_surface_w, s_surface_h;
static unsigned char *s_lfb_ptr;        /* while a write lock is held */
static unsigned s_lfb_stride;

static void pump(void)
{
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}

/* FNV-1a, 64-bit. Not a cryptographic claim and does not need to be: the
 * question it answers is "did this build draw the same bytes as that one",
 * where the alternative is comparing nothing at all because PNGs are too
 * slow to write. */
static uint64_t fnv1a(const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    uint64_t h = 1469598103934665603ull;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

static FILE *s_hash;

static void dump_frame(const char *dir, unsigned frame)
{
    typedef int (__stdcall *read_fn)(uint32_t, uint32_t, uint32_t, uint32_t,
                                     uint32_t, uint32_t, void *);
    read_fn rd = (read_fn)s_read_region;
    unsigned w = s_surface_w, h = s_surface_h;
    uint16_t *px;
    char path[MAX_PATH];

    if (!rd || !w || !h)
        return;
    px = (uint16_t *)malloc((size_t)w * h * 2u);
    if (!px)
        return;
    if (rd(GR_BUFFER_BACKBUFFER, 0, 0, w, h, w * 2u, px)) {
        if (s_hash)
            fprintf(s_hash, "%05u %016llx\n", frame,
                    (unsigned long long)fnv1a(px, (size_t)w * h * 2u));
        if (dir) {
            snprintf(path, sizeof path, "%s\\frame%05u.png", dir, frame);
            if (png_write565(path, px, w, h, w))
                rep("  frame %u -> %s (%ux%u)\n", frame, path, w, h);
        }
    } else {
        rep("  frame %u: grLfbReadRegion REFUSED\n", frame);
    }
    free(px);
}

int main(int argc, char **argv)
{
    /* Omit frame readback and PNG encoding unless output is requested. */
    const char *cap = NULL, *provider = NULL, *out = NULL, *hash_path = NULL;
    unsigned max_frames = 0, from_frame = 0, frame = 0, records = 0, calls = 0, i;
    unsigned at_frame = 0, at_draw = 0, draw_in_frame = 0;
    int list_only = 0, present = 0, no_fog = 0, point_filter = 0;
    int target_depth_always = 0;
    int stream_error = 0, saw_end = 0, stopped_early = 0;
    HANDLE fh = INVALID_HANDLE_VALUE, mh = NULL;
    LARGE_INTEGER qpf, t0, t1;

    for (i = 1; i < (unsigned)argc; i++) {
        if (strcmp(argv[i], "--provider") == 0 && i + 1 < (unsigned)argc)
            provider = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < (unsigned)argc)
            out = argv[++i];
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < (unsigned)argc)
            max_frames = (unsigned)strtoul(argv[++i], NULL, 0);
        /* Write only from frame N. The replay still has to REPLAY every record
         * before it (state is cumulative and there is no seeking in a call
         * stream), but a PNG per frame is most of the wall clock, so bisecting
         * one late frame costs a minute instead of ten. */
        else if (strcmp(argv[i], "--from") == 0 && i + 1 < (unsigned)argc)
            from_frame = (unsigned)strtoul(argv[++i], NULL, 0);
        /* Stop immediately after draw N of frame F and read the incomplete
         * back buffer.  This makes it possible to locate the first differing
         * pass without changing the captured call stream or either provider. */
        else if (strcmp(argv[i], "--at-frame") == 0 && i + 1 < (unsigned)argc)
            at_frame = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--at-draw") == 0 && i + 1 < (unsigned)argc)
            at_draw = (unsigned)strtoul(argv[++i], NULL, 0);
        /* The proof-of-no-change instrument: one line per frame, cheap enough
         * to run over the whole capture. See the header. */
        else if (strcmp(argv[i], "--hash") == 0 && i + 1 < (unsigned)argc)
            hash_path = argv[++i];
        else if (strcmp(argv[i], "--report") == 0 && i + 1 < (unsigned)argc)
            s_report = fopen(argv[++i], "w");
        else if (strcmp(argv[i], "--present") == 0)
            present = 1;
        else if (strcmp(argv[i], "--no-fog") == 0)
            no_fog = 1;
        else if (strcmp(argv[i], "--point-filter") == 0)
            point_filter = 1;
        else if (strcmp(argv[i], "--target-depth-always") == 0)
            target_depth_always = 1;
        else if (strcmp(argv[i], "--list") == 0)
            list_only = 1;
        else if (argv[i][0] != '-')
            cap = argv[i];
    }
    /* Disable presentation unless the caller explicitly tests that path. */
    if (!present)
        SetEnvironmentVariableA("VCGLIDE_PRESENT", "0");

    if (!cap || (!provider && !list_only)) {
        fprintf(stderr,
            "usage: glide-replay CAPTURE.vglc --provider DLL [--out DIR]\n"
            "       (no --out: replay only, write no frames; for timing)\n"
            "                                 [--frames N] [--from N] [--report FILE]\n"
            "                                 [--at-frame F --at-draw D] [--out DIR]\n"
            "                                 [--hash FILE] (per-frame hash, all frames)\n"
            "                                 [--present]   (show it; NOT a timing run)\n"
            "                                 [--no-fog]    (diagnostic call-stream override)\n"
            "                                 [--point-filter] (diagnostic override)\n"
            "                                 [--target-depth-always] (at F,D only)\n"
            "       glide-replay CAPTURE.vglc --list\n");
        return 2;
    }
    if (!load(cap, &fh, &mh))
        return 1;
    if (!list_only) {
        if (!bind_provider(provider))
            return 1;
        if (out) CreateDirectoryA(out, NULL);
        if (hash_path) {
            s_hash = fopen(hash_path, "w");
            if (!s_hash)
                rep("replay: cannot write %s; no hashes\n", hash_path);
        }
    }

    /* Time only the mapped replay after provider initialization. */
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&t0);

    while ((size_t)(s_end - s_cur) >= sizeof(vglc_record_t)) {
        const void *p;
        vglc_record_t r;
        uint32_t args[ARG_MAX] = { 0 };
        unsigned words, b;
        int is_control;

        if (!take(&p, sizeof r))
            goto corrupt;
        memcpy(&r, p, sizeof r);
        is_control = (r.idx == VGLC_CONTROL);
        if (!is_control && r.idx >= s_entries) {
            fprintf(stderr, "replay: record %u names entry %u of %u; the "
                            "capture is corrupt from here\n",
                    records, r.idx, s_entries);
            goto corrupt;
        }
        words = is_control ? VGLC_CONTROL_ARGS / 4u
                           : (unsigned)s_entry[r.idx].argbytes / 4u;
        if (!take(&p, (size_t)words * 4u))
            goto corrupt;
        memcpy(args, p, (size_t)words * 4u);

        /* Blobs replace the pointer they were captured through. The blob
         * lives in the mapped capture, which is read-only and is exactly
         * what a provider wants: it only reads these. */
        for (b = 0; b < r.blobs; b++) {
            const vglc_blob_t *bl;
            const unsigned char *data;
            size_t pad;
            if (!take(&p, sizeof *bl)) {
                fprintf(stderr, "replay: record %u ends inside blob %u's "
                                "descriptor\n", records, b);
                goto corrupt;
            }
            bl = (const vglc_blob_t *)p;
            if (!take(&p, bl->size)) {
                fprintf(stderr, "replay: record %u ends inside blob %u's "
                                "%u-byte payload\n", records, b, bl->size);
                goto corrupt;
            }
            data = (const unsigned char *)p;
            pad = bl->size & 3u;
            if (pad && !take(&p, 4u - pad)) {
                fprintf(stderr, "replay: record %u ends inside blob %u's "
                                "padding\n", records, b);
                goto corrupt;
            }

            if (bl->tag < ARG_MAX) {
                if (bl->tag >= words) {
                    fprintf(stderr, "replay: record %u has blob for argument "
                                    "%u but only %u arguments\n",
                            records, bl->tag, words);
                    goto corrupt;
                }
                args[bl->tag] = (uint32_t)(uintptr_t)data;
            } else if (bl->tag == VGLC_TAG_TEXDATA) {
                /* The GrTexInfo blob still holds the ORIGINAL data pointer.
                 * Patch a mutable copy: the mapping is read-only and the
                 * provider must see an address that exists in this process. */
                static unsigned char info[GR_TEXINFO_BYTES];
                const unsigned char *captured_info;
                uintptr_t captured_at, mapped_begin, mapped_end;

                if (words <= 3) {
                    fprintf(stderr, "replay: record %u has texture data but "
                                    "no GrTexInfo argument\n", records);
                    goto corrupt;
                }
                captured_info = (const unsigned char *)(uintptr_t)args[3];
                captured_at = (uintptr_t)captured_info;
                mapped_begin = (uintptr_t)s_base;
                mapped_end = (uintptr_t)s_end;
                if (captured_at < mapped_begin || captured_at > mapped_end ||
                    GR_TEXINFO_BYTES > (size_t)(mapped_end - captured_at)) {
                    fprintf(stderr, "replay: record %u has texture data before "
                                    "a valid GrTexInfo blob\n", records);
                    goto corrupt;
                }
                memcpy(info, captured_info, GR_TEXINFO_BYTES);
                *(uint32_t *)(info + GR_TEXINFO_DATA) =
                    (uint32_t)(uintptr_t)data;
                args[3] = (uint32_t)(uintptr_t)info;
            } else if (bl->tag == VGLC_TAG_LFB && s_lfb_ptr) {
                /* The 2D layer the caller software-rendered. Copy it into
                 * whatever this provider handed back, row by row, because
                 * its stride is its own and need not match the capture's. */
                unsigned cap_stride = args[2];
                unsigned rows = cap_stride ? bl->size / cap_stride : 0;
                unsigned n = cap_stride < s_lfb_stride ? cap_stride
                                                       : s_lfb_stride;
                unsigned y;

                if (!cap_stride || bl->size % cap_stride ||
                    rows > s_surface_h) {
                    fprintf(stderr, "replay: record %u has invalid LFB blob "
                                    "geometry (%u bytes, stride %u, surface %u "
                                    "rows)\n", records, bl->size, cap_stride,
                            s_surface_h);
                    goto corrupt;
                }
                for (y = 0; y < rows; y++)
                    memcpy(s_lfb_ptr + (size_t)y * s_lfb_stride,
                           data + (size_t)y * cap_stride, n);
            }
        }
        records++;

        if (is_control) {
            if (args[0] == VGLC_FRAME) {
                if (frame && !list_only)
                    pump();
                frame = args[1];
                draw_in_frame = 0;
                if (max_frames && frame > max_frames)
                {
                    stopped_early = 1;
                    break;
                }
            } else if (args[0] == VGLC_END) {
                rep("capture ends cleanly: %u records, %u frames\n",
                       args[1], args[2]);
                saw_end = 1;
                break;
            }
            continue;
        }

        s_entry[r.idx].calls++;
        if (list_only)
            continue;

        /* Substitutions: everything that belonged to the process that was
         * captured and cannot belong to this one. */
        switch (s_entry[r.idx].payload) {
        case PL_WINOPEN:
            /* Normal boot passes 0 because the cabinet had no window. Hydro's
             * service reopen passes the small Glide context value 3 instead;
             * it is not an HWND either. A genuine captured handle would still
             * belong to a dead process, so every replay substitutes NULL. */
            args[0] = 0;
            break;
        case PL_CALLBACK:
            args[0] = 0;
            break;
        case PL_STATE_IN: case PL_STATE_OUT: case PL_HWCFG:
            args[0] = (uint32_t)(uintptr_t)shadow_for(args[0]);
            if (!args[0])
                goto resource_failure;
            break;
        case PL_LFBINFO:
            args[5] = (uint32_t)(uintptr_t)shadow_for(args[5]);
            if (!args[5])
                goto resource_failure;
            *(uint32_t *)(uintptr_t)args[5] = GR_LFBINFO_BYTES;
            break;
        default:
            break;
        }

        if (no_fog && strcmp(s_entry[r.idx].name, "grFogMode") == 0)
            args[0] = 0;
        if (point_filter && strcmp(s_entry[r.idx].name, "grTexFilterMode") == 0)
            args[1] = args[2] = 0;

        if (!s_entry[r.idx].fn)
            continue;
        if (target_depth_always && at_frame && at_draw && frame == at_frame &&
            draw_in_frame + 1 == at_draw &&
            strcmp(s_entry[r.idx].name, "grDrawTriangle") == 0) {
            unsigned di;
            for (di = 0; di < s_entries; di++)
                if (strcmp(s_entry[di].name, "grDepthBufferFunction") == 0 &&
                    s_entry[di].fn) {
                    uint32_t always[1] = { 7 }; /* GR_CMP_ALWAYS */
                    call_stdcall(s_entry[di].fn, always, 1);
                    break;
                }
        }
        /* Before the swap, not after: the frame being dumped is the one that
         * has just been finished. The provider dumps golden frames on exactly
         * the same entry, and if the two used different conventions every
         * comparison would be off by a frame. */
        if (s_entry[r.idx].payload == PL_SWAP)
            if ((out || s_hash) && frame >= from_frame)
                dump_frame(out, frame);
        {
            int ret = call_stdcall(s_entry[r.idx].fn, args, words);
            calls++;

            if (s_entry[r.idx].payload == PL_WINOPEN) {
                if (args[1] < vgl_resolution_count) {
                    s_surface_w = (unsigned)vgl_resolutions[args[1]].width;
                    s_surface_h = (unsigned)vgl_resolutions[args[1]].height;
                }
                rep("grSstWinOpen(res=%u -> %ux%u) = %d\n",
                       args[1], s_surface_w, s_surface_h, ret);
            } else if (s_entry[r.idx].payload == PL_LFBINFO) {
                const unsigned char *info =
                    (const unsigned char *)(uintptr_t)args[5];
                s_lfb_ptr = ret ? (unsigned char *)(uintptr_t)
                    *(const uint32_t *)(info + GR_LFBINFO_PTR) : NULL;
                s_lfb_stride = ret ? *(const uint32_t *)
                    (info + GR_LFBINFO_STRIDE) : 0;
            } else if (s_entry[r.idx].payload == PL_LFBUNLOCK) {
                s_lfb_ptr = NULL;
            }

            if (strcmp(s_entry[r.idx].name, "grDrawTriangle") == 0) {
                draw_in_frame++;
                if (at_frame && at_draw && frame == at_frame &&
                    draw_in_frame == at_draw) {
                    dump_frame(out, frame);
                    rep("stopped after frame %u draw %u\n", frame,
                        draw_in_frame);
                    stopped_early = 1;
                    goto done;
                }
            }
        }
    }
    if (!saw_end && !stopped_early) {
        if (s_cur != s_end)
            fprintf(stderr, "replay: capture ends with %u trailing bytes, less "
                            "than one record\n", (unsigned)(s_end - s_cur));
        else
            fprintf(stderr, "replay: capture has no terminating VGLC_END record\n");
        stream_error = 1;
    }
    goto done;

resource_failure:
    fprintf(stderr, "replay: cannot prepare caller storage for record %u\n",
            records);
    stream_error = 1;
    goto done;

corrupt:
    stream_error = 1;
done:
    QueryPerformanceCounter(&t1);
    rep("\nreplayed %u records, %u calls, %u frames\n",
           records, calls, frame);
    {
        double secs = qpf.QuadPart
            ? (double)(t1.QuadPart - t0.QuadPart) / (double)qpf.QuadPart : 0.0;
        /* Label readback and encoding overhead in elapsed-time output. */
        rep("elapsed %.3f s, %.2f frames/s%s\n", secs,
            secs > 0.0 ? (double)frame / secs : 0.0,
            out ? "  (INCLUDES writing a PNG per frame: not a renderer figure)"
                : (s_hash ? "  (includes a framebuffer read and hash per frame)"
                          : ""));
    }
    if (list_only) {
        rep("\ncall counts:\n");
        for (i = 0; i < s_entries; i++)
            if (s_entry[i].calls)
                rep("  %8u  %s\n", s_entry[i].calls, s_entry[i].name);
    }
    if (s_base) UnmapViewOfFile(s_base);
    if (mh) CloseHandle(mh);
    if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
    return stream_error ? 1 : 0;
}

/* vgl_resolutions lives in the provider, which this tool does not link
 * against. It is one table of Glide's own screen-resolution enum and it is
 * needed here for exactly one thing: knowing how big a frame is. */
const vgl_res_t vgl_resolutions[] = {
    {  320,  200 }, {  320,  240 }, {  400,  256 }, {  512,  384 },
    {  640,  200 }, {  640,  350 }, {  640,  400 }, {  640,  480 },
    {  800,  600 }, {  960,  720 }, {  856,  480 }, { 512,   256 },
    { 1024,  768 }, { 1280, 1024 }, { 1600, 1200 }, {  400,  300 },
};
const unsigned vgl_resolution_count =
    sizeof vgl_resolutions / sizeof vgl_resolutions[0];
