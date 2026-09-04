/* loader.c -- map the game at its link-time base.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The executable image already reserves the required low-address span; this
 * loader makes it writable, copies CODE, zeros the remainder, and lets
 * __pl_unpackrom materialise DATA and .bss.
 */
#include "vcthunder.h"

#include <bcrypt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPAN_BASE  G->image_base
#define SPAN_SIZE  (G->span_end - G->image_base)

/* The image the profile's addresses were derived from is identified by BOTH
 * G->file_size and G->sha256. They are an execution boundary, not metadata:
 * every address below assumes that exact build, and a plausible address in a
 * different build is more dangerous than an obvious failure. Verify before the
 * first byte of the fixed image is made writable or copied. */

static void *s_region;

/* If the span is not free, say exactly what is in the way. This is worth real
 * effort: the failure is deterministic per-build and completely opaque
 * otherwise. Reached only if the stub's guarantee broke: e.g. a toolchain
 * change moved .bss so SizeOfImage no longer covers the span, or the exe was
 * linked without -Wl,--image-base,0x100000. */
static void report_conflicts(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = SPAN_BASE;

    LOGE("walking 0x%08X..0x%08X to find what is in the way:",
         SPAN_BASE, G->span_end);

    while (addr < G->span_end &&
           VirtualQuery((LPCVOID)addr, &mbi, sizeof mbi) == sizeof mbi) {
        if (mbi.State != MEM_FREE) {
            char name[MAX_PATH] = "";
            HMODULE mod = NULL;

            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)mbi.BaseAddress, &mod) && mod)
                GetModuleFileNameA(mod, name, sizeof name);

            LOGE("  0x%08lX..0x%08lX  %s %-7s alloc_base=0x%08lX  %s",
                 (unsigned long)(uintptr_t)mbi.BaseAddress,
                 (unsigned long)((uintptr_t)mbi.BaseAddress + mbi.RegionSize),
                 mbi.State == MEM_COMMIT ? "COMMIT " : "RESERVE",
                 mbi.Type == MEM_IMAGE   ? "IMAGE"  :
                 mbi.Type == MEM_MAPPED  ? "MAPPED" :
                 mbi.Type == MEM_PRIVATE ? "PRIVATE" : "?",
                 (unsigned long)(uintptr_t)mbi.AllocationBase,
                 name[0] ? name : "");
        }
        addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.RegionSize == 0)
            break;
    }
}

static int read_all(const char *path, unsigned char **buf, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    long sz;

    if (!fp) {
        LOGE("cannot open '%s'", path);
        return 0;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (sz = ftell(fp)) < 0) {
        LOGE("cannot size '%s'", path);
        fclose(fp);
        return 0;
    }
    rewind(fp);

    *buf = (unsigned char *)malloc((size_t)sz);
    if (!*buf) {
        LOGE("out of memory reading '%s' (%ld bytes)", path, sz);
        fclose(fp);
        return 0;
    }
    if (fread(*buf, 1, (size_t)sz, fp) != (size_t)sz) {
        LOGE("short read on '%s'", path);
        free(*buf);
        *buf = NULL;
        fclose(fp);
        return 0;
    }
    fclose(fp);
    *len = (size_t)sz;
    return 1;
}

static int sha256_hex(const unsigned char *data, size_t len, char out[65])
{
    static const char hex[] = "0123456789abcdef";
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32];
    NTSTATUS status;
    unsigned i;

    if (len > ULONG_MAX) {
        LOGE("image is too large to hash (%zu bytes)", len);
        return 0;
    }
    status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                         NULL, 0);
    if (status < 0)
        goto fail;
    status = BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0);
    if (status < 0)
        goto fail;
    status = BCryptHashData(hash, (PUCHAR)data, (ULONG)len, 0);
    if (status < 0)
        goto fail;
    status = BCryptFinishHash(hash, digest, sizeof digest, 0);
    if (status < 0)
        goto fail;

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    for (i = 0; i < sizeof digest; i++) {
        out[2 * i]     = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15u];
    }
    out[64] = '\0';
    return 1;

fail:
    LOGE("BCrypt SHA-256 failed (status=0x%08lX)", (unsigned long)status);
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    return 0;
}

int game_map(const char *exe_path)
{
    unsigned char *file = NULL;
    char actual_sha256[65];
    size_t len = 0;

    if (!read_all(exe_path, &file, &len))
        return 0;

    LOGI("loaded '%s' (%zu bytes)", exe_path, len);
    if (len != G->file_size) {
        LOGE("refusing unexpected file size %zu: %s's profile requires "
             "%u bytes (sha256 %.16s...)", len, G->id, G->file_size,
             G->sha256);
        free(file);
        return 0;
    }
    if (!sha256_hex(file, len, actual_sha256)) {
        free(file);
        return 0;
    }
    if (_stricmp(actual_sha256, G->sha256) != 0) {
        LOGE("refusing image sha256 %s: %s's profile requires %s",
             actual_sha256, G->id, G->sha256);
        free(file);
        return 0;
    }
    LOGI("verified image sha256 %s", actual_sha256);

    if (len < G->code_file_off + G->code_size) {
        LOGE("file is too small to contain CODE (need %u bytes)",
             (unsigned)(G->code_file_off + G->code_size));
        free(file);
        return 0;
    }

    /* The span is already ours as part of VCThunder.exe's image (src/stub.c).
     * It is mapped read-only/execute in places, so make the whole thing RWX:
     * CODE holds the __imp__ table and the record stream as well as
     * instructions, so it is simultaneously code, writable data and a patch
     * target. This also overwrites the stub's own headers and .text, which is
     * why nothing in the stub may be needed after this point. */
    {
        DWORD old_prot;
        if (!VirtualProtect((LPVOID)(uintptr_t)SPAN_BASE, SPAN_SIZE,
                            PAGE_EXECUTE_READWRITE, &old_prot)) {
            LOGE("VirtualProtect(0x%08X, %u KB, RWX) failed, GetLastError=%lu",
                 SPAN_BASE, (unsigned)(SPAN_SIZE / 1024), GetLastError());
            report_conflicts();
            free(file);
            return 0;
        }
        s_region = (void *)(uintptr_t)SPAN_BASE;
    }
    LOGI("span 0x%08X..0x%08X (%u KB) is RWX and ours",
         SPAN_BASE, G->span_end, (unsigned)(SPAN_SIZE / 1024));

    /* The PE headers survive only because they fit below the game's first byte.
     * If a toolchain change grows them past 0x6b0 this must fail loudly rather
     * than quietly corrupt the game's first instructions. */
    {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)(uintptr_t)SPAN_BASE;
        IMAGE_NT_HEADERS *nt =
            (IMAGE_NT_HEADERS *)((char *)dos + dos->e_lfanew);
        DWORD hdr = nt->OptionalHeader.SizeOfHeaders;

        if (hdr > G->code_file_off) {
            LOGE("our PE headers are 0x%lX bytes but the game's CODE starts at "
                 "0x%X: they would overlap", (unsigned long)hdr,
                 G->code_file_off);
            free(file);
            return 0;
        }
        LOGI("PE headers 0x%lX bytes, game CODE at 0x%X: headers preserved",
             (unsigned long)hdr, G->code_file_off);
    }

    /* Preserve the host PE headers below the game's first CODE byte. Runtime
     * libraries may inspect them after mapping. */
    memset((void *)(uintptr_t)G->code_va, 0,
           G->span_end - G->code_va);
    memcpy((void *)(uintptr_t)G->code_va,
           file + G->code_file_off, G->code_size);
    free(file);

    LOGI("CODE  0x%08X..0x%08X copied", G->code_va,
         G->code_va + G->code_size);
    LOGI("DATA  0x%08X  .bss 0x%08X  .stack 0x%08X  (zeroed, awaiting unpack)",
         G->data_va, G->bss_va, G->stack_va);
    return 1;
}

void game_unmap(void)
{
    /* Nothing to release: the span is part of VCThunder.exe's own image, so
     * it lives and dies with the process. VirtualFree would fail on MEM_IMAGE. */
    s_region = NULL;
}

/* ---- the ETS kernel public info block -----------------------------------
 *
 * __pl_unpackrom does not just copy records. Before every write it calls a
 * local overlap test with the record's destination, the record's length, and a
 * 0x100-byte BY-VALUE COPY of the ETS kernel public info block, and it refuses
 * the record (returning 2 for the whole unpack) if the destination range
 * intersects either of two reserved (base, length) regions carried in that
 * block. Both titles' unpackers are the same code to the byte; the pairs live
 * at block offsets 0x44/0x48 and 0x4C/0x50 (helper frame +0x54/+0x58/+0x5C/
 * +0x60), and the test is
 *
 *     refuse if  (A.base <  dst+len && A.base+A.len >  dst)
 *             || (B.base <  dst+len && B.base+B.len >  dst)
 *
 * The block reaches the unpacker through _EtsGetSystemInfo, which asks
 * __p_EtsGetKernelPublicInfoPointer for it: an `int 0xFE` gate, ax=0x254A,
 * bx=5, answered by a host kernel this process does not have. stubs_install_m1
 * gives every ETSGATE entry a stub returning 0, and _EtsGetSystemInfo copies
 * NOTHING when the pointer is 0: it leaves the unpacker's 0x110-byte stack
 * buffer exactly as it found it. The game then tests all 6,578 records against
 * whatever the host left on its own stack at that depth.
 *
 * That is the whole of the trap. Whether the unpack succeeds depended on
 * host stack residue, which is why it moved when code moved between
 * translation units, why it was insensitive to SSE, image base and the thunk
 * pool, and why a getenv/fopen/fwrite diagnostic placed before the unpack
 * "fixed" it: the diagnostic overwrote the residue.
 *
 * A zeroed block is the truthful answer here, not a placation: base 0 with
 * length 0 describes a region that intersects nothing, and this host reserves
 * nothing inside the span (the span is ours end to end, and the unpacker is
 * the first thing to write into it). We answer the gate's question; we do not
 * defeat its check. Anything this host ever does reserve inside the span must
 * be declared in these two pairs rather than hidden by zeroing them.
 *
 * Nothing here is derived from any ETS header: the two offsets and the
 * comparison come from the games' own disassembly of __pl_unpackrom, which is
 * the only statement of the contract that binds us. The remaining 0xF0 bytes
 * are unread by this code path and stay zero.
 */
#define ETS_PUBLIC_INFO_SIZE 0x100

static uint32_t s_ets_public_info[ETS_PUBLIC_INFO_SIZE / 4];

static uint32_t __cdecl ov_ets_kernel_public_info(void)
{
    return (uint32_t)(uintptr_t)s_ets_public_info;
}

/* Overwrites the ETSGATE stub installed by stubs_install_m1(); must therefore
 * run after it and before the unpack. game_unpack() calls it so the ordering
 * cannot be got wrong from outside. */
static int ets_public_info_install(void)
{
    if (!game_require("loader", "__p_EtsGetKernelPublicInfoPointer",
                      G->ets_kernel_public_info))
        return 0;

    memset(s_ets_public_info, 0, sizeof s_ets_public_info);
    patch_jmp(G->ets_kernel_public_info, (void *)ov_ets_kernel_public_info);
    LOGI("ETS kernel public info @ 0x%08X -> %zu-byte host block at %p "
         "(no reserved regions inside the span)",
         G->ets_kernel_public_info, sizeof s_ets_public_info,
         (void *)s_ets_public_info);
    return 1;
}

int game_unpack(void)
{
    int (*unpackrom)(void) = (int (*)(void))(uintptr_t)G->unpackrom;
    int rc;

    /* Without this the unpacker's reserved-region test reads uninitialised
     * stack, and its verdict is decided by the host's code layout. */
    if (!ets_public_info_install())
        return 0;

    LOGI("calling __pl_unpackrom @ 0x%08X", G->unpackrom);
    rc = unpackrom();
    if (rc != 0) {
        /* __p_start maps nonzero onto one of two fatal strings; 2 is a distinct
         * case there, so surface it rather than flattening to "failed". 2 is
         * the reserved-region refusal above; 1 is an unknown record type. */
        LOGE("__pl_unpackrom returned %d (expected 0): DATA/.bss are not "
             "initialised; do not continue", rc);
        return 0;
    }
    LOGI("__pl_unpackrom OK: DATA + .bss expanded");
    return 1;
}

/* Maintain a bounded lock-free census of threads observed in game code. CAS
 * claims distinct slots; excess threads are counted and reported. */
#define GAME_THREADS_MAX 8
static volatile LONG s_gt_id[GAME_THREADS_MAX];
static volatile LONG s_gt_hits[GAME_THREADS_MAX];
static const char *s_gt_where[GAME_THREADS_MAX];
static volatile LONG s_gt_count;
static volatile LONG s_gt_logic;

static void gt_note(int i, LONG me, const char *where, int logic)
{
    LONG n;

    s_gt_where[i] = where;              /* written once, before the count */
    n = InterlockedIncrement(&s_gt_count);
    if (n == 1)
        LOGI("game: thread %lu runs game code (first seen at %s)",
             (unsigned long)me, where);
    else
        LOGW("game: thread %lu ALSO runs game code (first seen at %s); %ld "
             "threads now. The game's INLINE cli/sti are emulated, so a "
             "critical section built from those does exclude these threads "
             "from each other, but stubs.c still stubs _EtsSetInterruptFlag "
             "to a bare return, and one built from THAT does not. See "
             "heal_privileged() and diego_frame_boundary().",
             (unsigned long)me, where, n);
    if (logic)
        InterlockedExchange(&s_gt_logic, me);
}

static void gt_seen(const char *where, int logic)
{
    LONG me = (LONG)GetCurrentThreadId();
    int i;

    for (i = 0; i < GAME_THREADS_MAX; i++) {
        LONG got = InterlockedCompareExchange(&s_gt_id[i], me, 0);

        if (got == me) {                /* already recorded: the hot path */
            InterlockedIncrement(&s_gt_hits[i]);
            if (logic && InterlockedCompareExchange(&s_gt_logic, 0, 0) != me)
                InterlockedExchange(&s_gt_logic, me);
            return;
        }
        if (got == 0) {                 /* we just claimed slot i */
            InterlockedIncrement(&s_gt_hits[i]);
            gt_note(i, me, where, logic);
            return;
        }
    }
    LOGW("game: thread %lu runs game code and the census is full (%d)",
         (unsigned long)me, GAME_THREADS_MAX);
}

void game_thread_seen(const char *where)
{
    gt_seen(where, 0);
}

void game_thread_logic_seen(const char *where)
{
    gt_seen(where, 1);
}

void game_thread_report(void)
{
    int i;

    for (i = 0; i < GAME_THREADS_MAX; i++) {
        LONG id = InterlockedCompareExchange(&s_gt_id[i], 0, 0);

        if (!id)
            break;
        LOGI("game: thread %lu%s; %ld crossings, first at %s",
             (unsigned long)id,
             id == InterlockedCompareExchange(&s_gt_logic, 0, 0) ?
                 " (the game's own logic)" : "",
             InterlockedCompareExchange(&s_gt_hits[i], 0, 0),
             s_gt_where[i] ? s_gt_where[i] : "?");
    }
}

unsigned game_thread_count(void)
{
    LONG n = InterlockedCompareExchange(&s_gt_count, 0, 0);

    return n < 0 ? 0u : (unsigned)n;
}

DWORD game_thread_id(unsigned i)
{
    if (i >= GAME_THREADS_MAX)
        return 0;
    return (DWORD)InterlockedCompareExchange(&s_gt_id[i], 0, 0);
}

DWORD game_thread_logic(void)
{
    return (DWORD)InterlockedCompareExchange(&s_gt_logic, 0, 0);
}

void game_run(void)
{
    void (*crt_startup)(void) = (void (*)(void))(uintptr_t)G->crt_startup;

    LOGI("entering _mainCRTStartup @ 0x%08X (-> __cinit -> _main @ 0x%08X)",
         G->crt_startup, G->main);
    game_thread_seen("game_run");
    crt_startup();

    /* Written with raw Win32, deliberately bypassing the logger.
     *
     * The log stops dead at this call and the process then exits 0, which has
     * exactly two explanations: the CRT startup returned and our logging is
     * broken, or it never returned. Every instrument that could tell them apart
     * is itself the logger, so this one is not. */
    {
        DWORD w;
        HANDLE h = CreateFileA("crt-returned.txt", GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            WriteFile(h, "_mainCRTStartup returned\r\n", 26, &w, NULL);
            CloseHandle(h);
        }
    }
    LOGW("_mainCRTStartup returned: the game normally exits via ExitProcess");
}
