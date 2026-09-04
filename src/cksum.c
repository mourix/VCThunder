/* cksum.c -- what each title computes for its own CODE checksum.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Both games sum their loaded CODE in three ranges and exchange the result as
 * the link's compatibility gate: a peer whose value differs is marked
 * incompatible and its game traffic is discarded. This host writes `E9 rel32`
 * into those same ranges, so the number a running game computes is not the
 * number an unmodified image computes. Three quantities separate the two
 * effects and all three are logged: the sum over CODE before the first patch,
 * the same ranges with every patch installed, and the cells the game leaves
 * its own answer in. Their difference must equal the sum of the dword deltas
 * across the patches we wrote, which is reported alongside.
 */
#include "vcthunder.h"

#include <string.h>

/* The summed span is CODE plus the alignment padding that follows it, up to
 * data_va, because one range genuinely runs past the end of the CODE section:
 * Offroad's third ends at 0x0023DC89, whose containing dword needs two bytes
 * the section does not have. Those bytes are real in the process (game_map
 * zeroes the remainder of the span and __pl_unpackrom writes nothing below
 * data_va, which was checked in both titles), so the game sums them as zero.
 * Bounding this at code_size instead would refuse the very range the game
 * walks, which is exactly what the first run of this file did. */
#define CKSUM_LO   (G->code_va)
#define CKSUM_LEN  (G->data_va - G->code_va)

/* CODE as game_map() left it: the only window in which nothing is patched. */
static uint8_t *s_pristine;
static uint32_t s_pristine_sum;
static uint32_t s_live_sum;
static int s_have_pristine, s_have_live;

/* One line per cell, the first time the game puts something in it. */
static int s_seen_result, s_seen_getter, s_seen_link;
static unsigned s_polls;

/* Long enough to cover a title whose link module initialises late, short enough
 * that a title which never writes a cell stops being asked. Both titles reach
 * every cell they have within the first few frames. */
#define CKSUM_POLL_FRAMES 1800u

static uint32_t rd32(uint32_t va)
{
    return *(const volatile uint32_t *)(uintptr_t)va;
}

/* The game's own loop, over a chosen copy of CODE.
 *
 *      esi = &table[0].end
 *      while ((end = [esi]) != 0) { start = [esi-4]; sum += range; esi += 8; }
 *      range: dwords from (start+3)&~3 to end&~7, INCLUSIVE
 *
 * `code` is a buffer holding CODE as it appears at `code_lo`. Ranges are read
 * from live DATA in both cases: __pl_unpackrom materialises the table and
 * nothing patches it, so there is only ever one copy of it.
 *
 * Returns 0 and logs if the table names bytes outside CODE, which would mean
 * the derived table address is not a table. Range and dword counts are
 * reported through *ranges / *dwords for the caller to print once. */
static int sum_ranges(uint32_t table_va, const uint8_t *code, uint32_t code_lo,
                      uint32_t code_len, uint32_t *out,
                      unsigned *ranges, uint32_t *dwords)
{
    uint32_t acc = 0, esi = table_va, end;
    unsigned nr = 0;
    uint32_t nd = 0;

    for (end = rd32(esi); end; esi += 8, end = rd32(esi)) {
        uint32_t start = rd32(esi - 4);
        uint32_t c = (start + 3u) & ~3u;
        uint32_t last = end & ~7u;
        uint32_t n, sub = 0;

        if (++nr > 64u) {
            LOGE("code checksum: table @0x%08X does not terminate within 64 "
                 "ranges; the derived address is not a range table", table_va);
            return 0;
        }
        if (last < c)                      /* the game's shr leaves 0: skip */
            continue;
        n = ((last - c) + 4u) >> 2;
        if (c < code_lo || last < code_lo || (last - code_lo) + 4u > code_len) {
            LOGE("code checksum: range 0x%08X..0x%08X from table @0x%08X is "
                 "not inside 0x%08X..0x%08X", start, end, table_va,
                 code_lo, code_lo + code_len);
            return 0;
        }
        for (; n; n--, c += 4)
            sub += *(const uint32_t *)(code + (c - code_lo));
        acc += sub;
        nd += ((last - ((start + 3u) & ~3u)) + 4u) >> 2;
    }
    *out = acc;
    if (ranges)
        *ranges = nr;
    if (dwords)
        *dwords = nd;
    return 1;
}

/* Copy CODE before anything writes to it. Call from main() straight after
 * game_map() and before win32_bind(): that is the whole window.
 *
 * Taking it this early is sound because __pl_unpackrom writes nothing into
 * CODE in either title. That is not an assumption: the record stream's output
 * over the CODE range was compared byte for byte against the file and is
 * identical in both, which is why the snapshot can precede the unpack even
 * though the range table cannot be read until after it. */
void cksum_snapshot(void)
{
    if (!G->code_va || !G->code_size || G->data_va <= G->code_va)
        return;
    s_pristine = (uint8_t *)VirtualAlloc(NULL, CKSUM_LEN,
                                         MEM_RESERVE | MEM_COMMIT,
                                         PAGE_READWRITE);
    if (!s_pristine) {
        LOGW("code checksum: no snapshot (%u bytes, GetLastError=%lu); the "
             "pristine value cannot be measured this run", CKSUM_LEN,
             GetLastError());
        return;
    }
    memcpy(s_pristine, (const void *)(uintptr_t)CKSUM_LO, CKSUM_LEN);
}

/* The pristine sum. Call after game_unpack(): the range table is DATA and does
 * not exist until __pl_unpackrom has run. */
void cksum_report_pristine(void)
{
    unsigned ranges = 0;
    uint32_t dwords = 0;

    if (!game_require("code checksum", "the range table the summing loop walks",
                      G->cksum_table))
        return;
    if (!s_pristine)
        return;
    if (!sum_ranges(G->cksum_table, s_pristine, CKSUM_LO, CKSUM_LEN,
                    &s_pristine_sum, &ranges, &dwords))
        return;
    s_have_pristine = 1;
    LOGI("code checksum: %u ranges, %u dwords, from %s's own table @0x%08X",
         ranges, dwords, G->id, G->cksum_table);
    LOGI("code checksum: pristine (CODE as mapped, before any patch) "
         "= 0x%08X", s_pristine_sum);
}

/* The live sum and the account of the difference. Call immediately before
 * game_run(): every patch this host installs is in by then, and the game has
 * not yet run its own loop, so this is the same CODE the game is about to sum.
 * Releases the snapshot. */
void cksum_report_live(void)
{
    uint32_t changed = 0, delta = 0, esi;

    if (!G->cksum_table)
        return;
    if (!sum_ranges(G->cksum_table, (const uint8_t *)(uintptr_t)CKSUM_LO,
                    CKSUM_LO, CKSUM_LEN, &s_live_sum, NULL, NULL))
        goto done;
    s_have_live = 1;

    if (!s_have_pristine) {
        LOGI("code checksum: live (this process, patched) = 0x%08X; no "
             "pristine value to compare it against", s_live_sum);
        goto done;
    }

    /* Which dwords WE changed, over exactly the summed ranges. The count is
     * the useful half: it says how much of this host lands inside the gate. */
    for (esi = G->cksum_table; rd32(esi); esi += 8) {
        uint32_t c = (rd32(esi - 4) + 3u) & ~3u;
        uint32_t last = rd32(esi) & ~7u;

        for (; c <= last; c += 4) {
            uint32_t was = *(const uint32_t *)(s_pristine + (c - CKSUM_LO));
            uint32_t now = rd32(c);

            if (was != now) {
                changed++;
                delta += now - was;
            }
        }
    }

    LOGI("code checksum: live (this process, patched) = 0x%08X", s_live_sum);
    LOGI("code checksum: we changed %u dwords inside the summed ranges, "
         "delta 0x%08X; pristine+delta %s live",
         changed, delta,
         (uint32_t)(s_pristine_sum + delta) == s_live_sum ? "==" : "!=");

done:
    if (s_pristine) {
        VirtualFree(s_pristine, 0, MEM_RELEASE);
        s_pristine = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * The shim: answer the gate with the value an unmodified image computes.
 *
 * The measurement above is also the fix. `pristine` is what a cabinet running
 * this title computes for itself, arrived at here by running the game's own
 * loop over the game's own bytes before this host had written one of them, so
 * it is derived per run from the image actually loaded and is not a constant
 * anybody has to keep up to date. Answering with it makes this machine's
 * HereIAm compatible with a cabinet and with another VCThunder machine at any
 * other load address, which our own live sum can never be.
 *
 * One site per title, and each is the whole of that title's gate. Every reader
 * was audited rather than assumed, because 5 of the 6 are not in the link at
 * all:
 *
 *   Hydro   _gameinit_GetCodeChecksum, whose two callers are _net_link_Init
 *           (which puts `sum ^ 0x10020` in 0x57a470, read by the three send
 *           paths) and _sysmsg_Display_GameInitStatus_Update, the startup
 *           status screen that prints the checksum as hex. The cells behind it
 *           keep the live sum, which is what cksum_frame() then validates the
 *           range table against: the instrument stays honest and the wire is
 *           corrected.
 *   Offroad the unnamed second copy of the loop, called once by
 *           _network_ModuleInit, which stores what it returns in
 *           __nMyExeChecksum; that cell's three readers (_network_GetVersion,
 *           _network_Receive's compatibility compare, _network_message_HereIAm)
 *           are all downstream of it. _exe_checksum_Generate is left alone
 *           because nothing reads its cell at all.
 *
 * Both sites are inside the summed ranges, so the six bytes written here are
 * accounted for in cksum_report_live()'s delta like every other patch. That is
 * not a problem to solve: the point is precisely that what the game computes
 * about itself no longer has to be what it says.
 */
static int s_shim_getter, s_shim_link;

static int shim_site(uint32_t va, const char *what)
{
    patch_ret_imm(va, s_pristine_sum);
    /* Six bytes into CODE, read back: nothing else here proves the write
     * landed, and a shim that silently did not is the one failure mode that
     * looks exactly like success. */
    if (*(const volatile uint8_t *)(uintptr_t)va != 0xB8 ||
        rd32(va + 1) != s_pristine_sum) {
        LOGE("code checksum: the shim on %s @0x%08X did not take; this machine "
             "would put its own live sum on the wire and be rejected", what, va);
        return 0;
    }
    LOGI("code checksum: shim on %s @0x%08X now answers 0x%08X, the value an "
         "unmodified image computes", what, va, s_pristine_sum);
    return 1;
}

/* Call after cksum_report_pristine(), which is where the value comes from, and
 * before cksum_report_live(), so the live account covers these bytes too. */
void cksum_install_shim(void)
{
    if (!s_have_pristine) {
        LOGW("code checksum: no shim; the pristine value was not measured this "
             "run, so there is nothing to answer with. The link's compatibility "
             "gate will see this host's own live sum and reject every peer");
        return;
    }
    if (G->cksum_getter_fn)
        s_shim_getter = shim_site(G->cksum_getter_fn,
                                  "_gameinit_GetCodeChecksum, every reader "
                                  "this title has");
    if (G->cksum_link_fn)
        s_shim_link = shim_site(G->cksum_link_fn,
                                "the link's own copy of the summing loop");
    if (!G->cksum_getter_fn && !G->cksum_link_fn)
        game_require("code checksum",
                     "shim site the whole compatibility gate is reached through",
                     0);
    if (s_shim_getter || s_shim_link)
        LOGI("code checksum: %s answers the compatibility gate with 0x%08X "
             "however this host is loaded", G->id, s_pristine_sum);
}

/* ---------------------------------------------------------------------------
 * The R2 checksum: the other value in the same HereIAm message.
 *
 * Both titles XOR `entry[+0x3c] + entry[+0x00]` over the R2 archive's own
 * directory as the archive is opened (Hydro in _r2file_OpenForRead, Offroad in
 * _r2file_LoadRefTable_XDir). It is therefore a pure function of the ASSET
 * FILE, not of loaded CODE, and this host neither patches nor hooks anything on
 * that path: r2_cache.c hooks the two name LOOKUPS and nothing else. So unlike
 * the code checksum it is expected to survive untouched, and there is nothing
 * to shim.
 *
 * "Expected" is why this watches rather than samples. The cell is an
 * accumulator that starts at 0 and is reset on the way in, so a single reading
 * proves nothing about where it ends up. This reports the first value and every
 * change after it, then says what it settled on. Stability across the run, and
 * across runs, is the evidence: the code checksum moved with our load address
 * and this must not.
 */
static uint32_t s_r2_value, s_r2_link_value;
static unsigned s_r2_changes, s_r2_link_changes;

#define R2_REPORT_MAX 6u        /* enough to show churn; not a log flood */

static void r2_watch(uint32_t va, uint32_t *last, unsigned *changes,
                     const char *what)
{
    uint32_t v;

    if (!va)
        return;
    v = rd32(va);
    if (!v || v == *last)
        return;
    if (*last)
        (*changes)++;
    *last = v;
    if (*changes < R2_REPORT_MAX)
        LOGI("r2 checksum: %s = 0x%08X in %s @0x%08X%s", G->id, v, what, va,
             *changes ? " (changed)" : "");
    else if (*changes == R2_REPORT_MAX)
        LOGW("r2 checksum: %s %s is still changing after %u reports; it is not "
             "a settled value at this point in the run", G->id, what,
             R2_REPORT_MAX);
}

static void r2_frame(void)
{
    r2_watch(G->cksum_r2_result, &s_r2_value, &s_r2_changes,
             "_r2file_GetComprehensiveChecksum's cell");
    r2_watch(G->cksum_r2_link, &s_r2_link_value, &s_r2_link_changes,
             "__nMyR2Checksum, the value the link sends");
}

static void r2_settled(void)
{
    if (!s_r2_value) {
        LOGW("r2 checksum: %s never populated its R2 checksum cell; the "
             "archive was not opened, or the anchor is wrong", G->id);
        return;
    }
    LOGI("r2 checksum: %s settled at 0x%08X after %u change(s). This is a "
         "function of the R2 archive alone, so it should be identical in every "
         "run and on a cabinet holding the same assets", G->id, s_r2_value,
         s_r2_changes);
    if (G->cksum_r2_link && s_r2_link_value != s_r2_value)
        LOGW("r2 checksum: %s put 0x%08X on the wire but its accessor now "
             "reads 0x%08X; the link's copy was memoised before the archive "
             "finished accumulating", G->id, s_r2_link_value, s_r2_value);
}

/* Read the cells the game leaves its own answer in, from the frame boundary,
 * and say the first time each one is populated whether it agrees with the sum
 * measured over the same bytes. Self-disarming: a title has at most two of
 * these, and 0 stays 0 for the one it does not have.
 *
 * A cell reads 0 until the module that fills it has run, which is why this
 * polls rather than reporting once. Offroad's link value in particular is
 * written by _network_ModuleInit, which runs whether or not the link is
 * enabled: the checksum call precedes the _Network_bEnabled test. */
void cksum_frame(void)
{
    /* `shimmed` selects what the cell is expected to hold. A cell the shim
     * feeds must hold the PRISTINE value: that is the shim working, and
     * comparing it against the live sum would report the fix as a fault. */
    struct { uint32_t va; int *seen, shimmed; const char *what; } cells[3];
    int i, n = 0, outstanding = 0;

    if (!s_have_live || s_polls >= CKSUM_POLL_FRAMES)
        return;
    s_polls++;

    if (G->cksum_result) {
        cells[n].va = G->cksum_result; cells[n].seen = &s_seen_result;
        cells[n].shimmed = 0;
        cells[n++].what = "the summing loop's own cell";
    }
    if (G->cksum_getter) {
        cells[n].va = G->cksum_getter; cells[n].seen = &s_seen_getter;
        cells[n].shimmed = 0;   /* Hydro's shim is on the accessor, not this */
        cells[n++].what = "_gameinit_GetCodeChecksum's cell";
    }
    if (G->cksum_link_result) {
        cells[n].va = G->cksum_link_result; cells[n].seen = &s_seen_link;
        cells[n].shimmed = s_shim_link;
        cells[n++].what = "__nMyExeChecksum, the value the link sends";
    }

    for (i = 0; i < n; i++) {
        uint32_t v, want = cells[i].shimmed ? s_pristine_sum : s_live_sum;

        if (*cells[i].seen)
            continue;
        v = rd32(cells[i].va);
        if (!v) {
            outstanding = 1;
            continue;
        }
        *cells[i].seen = 1;
        LOGI("code checksum: %s computed 0x%08X in %s @0x%08X: %s %s (0x%08X)",
             G->id, v, cells[i].what, cells[i].va,
             v == want ? "MATCHES" : "DISAGREES WITH",
             cells[i].shimmed ? "the pristine value the shim answers with"
                              : "the value measured over the same bytes", want);
        if (v != want && cells[i].shimmed)
            LOGW("code checksum: the shim is installed but %s put a different "
                 "value on the wire; something else writes this cell, and the "
                 "gate is not answered", G->id);
        else if (v != want)
            LOGW("code checksum: the range table or the summing loop this "
                 "host derives does not describe what %s actually does; "
                 "every value derived from it is unproven",
                 G->id);
    }

    r2_frame();

    /* The R2 half has to be WATCHED, not sampled once, so the window runs to
     * the end whenever this title has an R2 cell at all. */
    if (!outstanding && !G->cksum_r2_result)
        s_polls = CKSUM_POLL_FRAMES;   /* every cell this title has reported */
    else if (s_polls == CKSUM_POLL_FRAMES) {
        if (outstanding)
            LOGW("code checksum: %s left a checksum cell at 0 for %u frames; "
                 "the module that fills it never ran", G->id,
                 CKSUM_POLL_FRAMES);
        r2_settled();
    }
}
