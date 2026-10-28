/* pe_host.h -- map an ordinary PE image and bind its imports ourselves.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The system loader maps the file, so it is a real image (MEM_IMAGE, in the
 * loader's list, relocated); nothing is bound and no entry point runs until
 * this file says so.
 */
#ifndef VCT_PE_HOST_H
#define VCT_PE_HOST_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char     path[MAX_PATH];
    const char *name;           /* basename inside path                      */
    HMODULE  mod;
    uint32_t preferred;         /* ImageBase in the FILE                     */
    int      relocated;         /* 1: moved, and a fixup was checked         */
    int      is_dll;
    int      placed;            /* 1: pe_place, not in the loader's list      */
} pe_image_t;

/* What one import resolved to. The counts are the dry run's report. */
typedef enum {
    PE_BIND_REAL = 0,           /* a system DLL's own export                 */
    PE_BIND_HOST,               /* a host shim: a boundary                   */
    PE_BIND_GAME,               /* another of the game's own images          */
    PE_BIND_PROVIDER,           /* the renderer provider (vcglide.dll)       */
    PE_BIND_KINDS
} pe_bind_kind_t;

typedef struct {
    unsigned total, missing;
    unsigned kind[PE_BIND_KINDS];
} pe_bind_stats_t;

/* name is NULL for an ordinal import. Return NULL for "unresolvable"; every
 * NULL is logged by name and counted as missing. */
typedef void *(*pe_resolver_fn)(const char *importer, const char *dll,
                                const char *name, unsigned ordinal,
                                pe_bind_kind_t *kind);

/* file/len: the bytes the caller already read and hashed, used to prove a
 * relocated image's fixups were applied. */
int   pe_map(pe_image_t *im, const char *path, const void *file, size_t len);
/* An image with no relocations, at its own base, laid out by hand inside this
 * executable's own image (its span), so it is still in a mapped image as SEH
 * dispatch requires. Refused unless the whole range is this executable's. */
int   pe_place(pe_image_t *im, const char *path, const void *file, size_t len);
int   pe_bind(pe_image_t *im, pe_resolver_fn fn, pe_bind_stats_t *st);
int   pe_attach(pe_image_t *im);
void *pe_entry(const pe_image_t *im);
void *pe_export(const pe_image_t *im, const char *name);

#endif
