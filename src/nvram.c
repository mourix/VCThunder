/* nvram.c -- persist the cabinet's 32,000-byte NVRAM block in a host file.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * This replaces raw sector I/O while preserving operator settings, audits, and
 * scores.
 */
#include "vcthunder.h"

#include <stdio.h>
#include <string.h>

/* The game's own cache is 128 KB (0x8000 dwords at 0x5c14e8) even though only
 * the first 32,000 bytes are ever used by _cmos_driver_*. Matching that size
 * means an unexpected block length cannot run off the end of our buffer. */
#define NV_SIZE      0x20000u
#define NV_USED      0x7D00u        /* what _cmos_driver_Init actually asks for */

static unsigned char s_nv[NV_SIZE];
static char          s_path[MAX_PATH];
static int           s_dirty;

static void nv_load(void)
{
    FILE *fp = fopen(s_path, "rb");
    size_t n;

    memset(s_nv, 0, sizeof s_nv);
    if (!fp) {
        LOGI("nvram: '%s' absent; starting from defaults, the game will "
             "write it on the first settings change", s_path);
        return;
    }
    n = fread(s_nv, 1, sizeof s_nv, fp);
    fclose(fp);
    LOGI("nvram: loaded %zu bytes from '%s'", n, s_path);
}

static int nv_store(void)
{
    char temp[MAX_PATH];
    FILE *fp;
    size_t n;
    int flushed, closed;
    int path_len;

    path_len = snprintf(temp, sizeof temp, "%s.tmp", s_path);
    if (path_len < 0 || (size_t)path_len >= sizeof temp) {
        LOGE("nvram: temporary path for '%s' is too long", s_path);
        return 0;
    }
    fp = fopen(temp, "wb");

    if (!fp) {
        LOGE("nvram: cannot write temporary '%s'; settings will not persist",
             temp);
        return 0;
    }
    n = fwrite(s_nv, 1, NV_USED, fp);
    flushed = fflush(fp);
    closed = fclose(fp);
    if (n != NV_USED || flushed != 0 || closed != 0) {
        LOGE("nvram: write failed after %zu/%u bytes to '%s'; keeping the "
             "previous save", n, NV_USED, temp);
        DeleteFileA(temp);
        return 0;
    }
    if (!MoveFileExA(temp, s_path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LOGE("nvram: cannot replace '%s' with the completed temporary file "
             "(err=%lu); keeping the previous save", s_path, GetLastError());
        DeleteFileA(temp);
        return 0;
    }
    s_dirty = 0;
    LOGI("nvram: atomically wrote %zu bytes to '%s'", n, s_path);
    return 1;
}

/* --- replacements -------------------------------------------------------- */

static int nv_module_init(void)
{
    return 1;
}

static int nv_init(int a, int b)
{
    (void)a; (void)b;
    nv_load();
    return 1;                       /* nonzero == success; _menus checks this */
}

static int nv_read_block(void *dst, unsigned len)
{
    if (!dst)
        return 0;
    if (len > NV_SIZE) {
        LOGW("nvram: read of %u bytes clamped to %u", len, NV_SIZE);
        len = NV_SIZE;
    }
    memcpy(dst, s_nv, len);
    LOGT("nvram: read %u bytes", len);
    return 1;
}

static int nv_write_block(const void *src, unsigned len)
{
    if (!src)
        return 0;
    if (len > NV_SIZE)
        len = NV_SIZE;
    memcpy(s_nv, src, len);
    s_dirty = 1;
    return nv_store();
}

static int nv_noop(void)
{
    return 1;
}

/* --- install ------------------------------------------------------------- */

int nvram_install(const char *save_dir)
{
    int present;
    int ready = 1;
    int path_len;

    /* Offroad uses ordinary redirected files and has no _wrs_* patch group. */
    present = G->wrs_module_init || G->wrs_init || G->wrs_read_block ||
              G->wrs_write_block || G->wrs_enter_write || G->wrs_exit_write;
    if (!present) {
        LOGI("nvram: %s uses ordinary redirected files; no _wrs_* patch group",
             G->id);
        return 1;
    }
    ready &= game_require("nvram", "_wrs_ModuleInit", G->wrs_module_init);
    ready &= game_require("nvram", "_wrs_Init", G->wrs_init);
    ready &= game_require("nvram", "_wrs_ReadCMOSBlock", G->wrs_read_block);
    ready &= game_require("nvram", "_wrs_WriteCMOSBlockToDisk",
                          G->wrs_write_block);
    ready &= game_require("nvram", "_wrs_EnterWrite", G->wrs_enter_write);
    ready &= game_require("nvram", "_wrs_ExitWrite", G->wrs_exit_write);
    if (!ready)
        return 0;

    path_len = snprintf(s_path, sizeof s_path, "%s\\htlowres.bin", save_dir);
    if (path_len < 0 || (size_t)path_len >= sizeof s_path) {
        LOGE("nvram: save path under '%s' is too long", save_dir);
        return 0;
    }

    patch_jmp(G->wrs_module_init, (void *)nv_module_init);
    patch_jmp(G->wrs_init,        (void *)nv_init);
    patch_jmp(G->wrs_read_block,  (void *)nv_read_block);
    patch_jmp(G->wrs_write_block, (void *)nv_write_block);
    patch_jmp(G->wrs_enter_write, (void *)nv_noop);
    patch_jmp(G->wrs_exit_write,  (void *)nv_noop);

    LOGI("nvram: backed by '%s' (%u bytes in use)", s_path, NV_USED);
    return 1;
}

void nvram_flush(void)
{
    if (s_dirty)
        nv_store();
}
