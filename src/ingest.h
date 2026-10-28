/* ingest.h -- import a game from the user's own MAME CHD.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The front door, inside the program: no Python, no chdman, no build. Which
 * game a CHD holds, and which build of it, is decided by what is on the disk,
 * never by the file's name, and before anything is written. */
#ifndef VCT_INGEST_H
#define VCT_INGEST_H

#include <stdint.h>
#include <wchar.h>
#include <windows.h>

#include "game.h"

typedef enum {
    INGEST_OK,              /* imported and verified */
    INGEST_ALREADY,         /* that build is already in place: nothing to do */
    INGEST_EXISTS,          /* the title's folder holds something else */
    INGEST_NOT_A_GAME,      /* no title this host runs is on the disk */
    INGEST_UNSUPPORTED,     /* a title, but not the build the tables are for */
    INGEST_NEEDS_BASE,      /* an update, and what it updates is not imported */
    INGEST_NO_SPACE,
    INGEST_NOT_WRITABLE,
    INGEST_CANCELLED,
    INGEST_FAILED           /* a damaged CHD, a failed read or write */
} ingest_result_t;

/* What an import reports while it runs. Every member may be NULL. say() gets
 * one line of narration at a time; progress() bytes written of the total;
 * a nonzero *cancel stops the import at the next chunk, leaving nothing. */
typedef struct {
    void (*say)(void *ctx, const char *line);
    void (*progress)(void *ctx, uint64_t done, uint64_t total);
    volatile LONG *cancel;
    void *ctx;
} ingest_ui_t;

/* A title as the importer sees it: an ETS profile's executable, merged flat
 * from every FAT partition, or a win32 title's install package, copied from
 * its folder on an NTFS volume (and the operator system's beside it), or an
 * update's package from its CD, laid over the base title's. */
typedef struct {
    const char *id, *name, *exe, *sha256, *build;
    const char *disk_dir;           /* win32: the game folder on the disk */
    const char *diag_dir;           /* win32: the operator system's folder */
    const char *base;               /* an update: the title it is laid over */
    const char *update_file;        /* ... one more file on its disc */
    const char *update_to;          /* ... and where it goes in the game */
} ingest_title_t;

typedef struct {
    const ingest_title_t *title;    /* NULL: not a game this host runs */
    char sha256[65];                /* of the executable, read in place */
    int supported;                  /* it is the registered build */
    const game_mame_disk_t *mame;   /* the disk MAME lists it as, or NULL */
} ingest_ident_t;

/* data_dir as the ini holds it, relative to the pack or absolute. With
 * replace, a title folder that holds something else is swapped out, but only
 * once the new one has verified. `why` gets a sentence for the user. */
ingest_result_t ingest_chd(const wchar_t *chd, const char *data_dir,
                           int replace, unsigned jobs, const ingest_ui_t *ui,
                           ingest_ident_t *ident, char *why, size_t whysz);

/* Whatever `path` is: a folder (ingest_folder), a CD image or its cue or toc
 * sheet, or a CHD of a hard disk or a CD. */
ingest_result_t ingest_file(const wchar_t *path, const char *data_dir,
                            int replace, unsigned jobs, const ingest_ui_t *ui,
                            ingest_ident_t *ident, char *why, size_t whysz);

/* A win32 title from a folder already on the host: its disk extracted, as
 * the drive, or any folder down to the game folder. Copied and verified
 * exactly as from a CHD. */
ingest_result_t ingest_folder(const wchar_t *dir, const char *data_dir,
                              int replace, const ingest_ui_t *ui,
                              ingest_ident_t *ident, char *why, size_t whysz);

/* The CHDs and CD images in `dir` to import for every title not yet in
 * data_dir, at most one per title and in title order (a base before its
 * update): the registered build if one is there, else another build of it,
 * so the user is told why it cannot run rather than nothing at all. A title
 * already present is never offered, so nothing is replaced. Decided by the
 * executable on each disk; a file of no title here is passed over. */
int ingest_find(const wchar_t *dir, const char *data_dir,
                wchar_t (*out)[MAX_PATH], int max);

/* The folder VCThunder.exe is in, with no trailing separator. */
void ingest_own_folder(wchar_t *out, size_t outsz);

/* `VCThunder.exe import [--replace] [--data DIR] [--jobs N] [FILE|DIR...]`;
 * with no file, the CHDs ingest_find() picks beside the exe.
 * Returns the exit status. */
int ingest_cli(const char *data_dir);

/* 1 when the command line asks for an import rather than a game. */
int ingest_is_command(int argc, char **argv);
/* 1 when every argument is a .chd or a CD image or sheet: files dropped on
 * the exe, which the launcher imports. */
int ingest_dropped(int argc, char **argv);

#endif
