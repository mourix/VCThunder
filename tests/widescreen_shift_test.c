/* widescreen_shift_test.c -- white-box test of the widescreen primitive shift.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * ws_shift() runs per vertex, on the game's own thread, in the shipped default
 * configuration, and it is the one place this host changes a coordinate the
 * game computed. Its per-vertex census is gated and the primitive wrappers
 * carry an identity fast path, and neither is reachable from `--dry-run`: the
 * vertex path only runs once frames are being produced.
 *
 * So it is tested here instead, against the frozen reference implementation
 * pasted in verbatim below as ws_shift_before(). The claim being tested is
 * exact: for every state either function can be called in, the bytes they
 * leave in the destination vertex are identical.
 */
/* The unit under test is one translation unit, included whole so the test can
 * reach its statics. `ws_shift` and the primitive wrappers live in
 * widescreen.c; the binding on the other side of glide_int.h is stubbed below
 * along with the rest of the host, which is what keeps this a white-box test of
 * the shift and not an integration test of the binding. */
#include "../src/widescreen.c"

#include <stdio.h>

/* ---- the collaborators widescreen.c reaches for ------------------------- */

/* glide_int.h's one call in the binding's direction. The shift never asks for
 * it (every caller is a setup or repair path), so a fixed answer is enough
 * and a wrong one would be caught by the states below. */
int glide_view_width(void) { return 0; }

shim_config_t g_cfg;
const game_profile_t *g_game;
log_level_t g_log_level = LOG_ERR;

void log_printf(log_level_t level, const char *fmt, ...)
{
    (void)level; (void)fmt;             /* the census reports are not the test */
}
int game_require(const char *s, const char *w, uint32_t va)
{ (void)s; (void)w; return va != 0; }
void patch_jmp(uint32_t va, const void *target) { (void)va; (void)target; }
void *hook_call_through(uint32_t va, const void *rep, uint32_t prologue)
{ (void)va; (void)rep; (void)prologue; return NULL; }
void patch_ret_imm(uint32_t va, uint32_t v) { (void)va; (void)v; }
int  texstate_claim(const char *n, void *r, uint32_t va)
{ (void)n; (void)r; (void)va; return 0; }

/* ---- the implementation this change replaced ---------------------------- */

#include "ws_shift_before.inc"

/* ---- the test ----------------------------------------------------------- */

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

#define VERTS 5
static char s_src[VERTS * WS_VSIZE];

/* A spread that straddles every branch: left of the field, inside it, right of
 * it, exactly on both edges, and a negative zero, which is the one value the
 * identity fast path can treat differently from `x + 0.0f`. */
static const float X[VERTS] = { -37.5f, 0.0f, 255.25f, 512.0f, 690.75f };

static void seed_source(void)
{
    unsigned i, k;

    for (i = 0; i < VERTS; i++) {
        float *v = (float *)(s_src + (size_t)i * WS_VSIZE);
        for (k = 0; k < WS_VSIZE / sizeof(float); k++)
            v[k] = (float)(100 + i * 20 + k);   /* the rest must be untouched */
        v[0] = X[i];
        v[1] = 40.0f + (float)i;                /* y, which the census reads  */
    }
}

/* Put the engine in one specific state, so the two implementations are called
 * under identical conditions. */
static void set_state(float dx, int ortho, int margin, int reported)
{
    s_ws_dx = dx;
    s_ws_native_width = 512;
    s_ws_cull_width = 512;
    s_ws_ortho_depth = ortho;
    s_ws_margin_enabled = margin;
    s_ws_extent_reported = reported;
    s_ws_seen_verts = s_ws_left_verts = s_ws_right_verts = 0;
    s_ws_seen_min = 1e30f; s_ws_seen_max = -1e30f;
    s_ws_bx0 = s_ws_by0 = 1e30f;
    s_ws_bx1 = s_ws_by1 = -1e30f;
}

static void case_shift(float dx, int ortho, int margin, int reported)
{
    char before[VERTS * WS_VSIZE], after[VERTS * WS_VSIZE];
    char label[128];

    seed_source();
    set_state(dx, ortho, margin, reported);
    ws_shift_before(before, s_src, VERTS);

    seed_source();
    set_state(dx, ortho, margin, reported);
    ws_shift(after, s_src, VERTS);

    snprintf(label, sizeof label,
             "ws_shift dx=%g ortho=%d margin=%d reported=%d: same bytes",
             (double)dx, ortho, margin, reported);
    check(memcmp(before, after, sizeof before) == 0, label);
}

/* The census is not part of the vertex output (that is the whole reason it
 * could be gated), so case_shift() above cannot see it at all. It still has to
 * produce the same answers when it runs, and it still has to actually stop when
 * it is gated off. Both are checked here, or "gate the census" and "delete the
 * census" would be the same test result. */
typedef struct {
    unsigned verts, left, right;
    float min, max, bx0, bx1, by0, by1;
} census_t;

static void read_census(census_t *c)
{
    c->verts = s_ws_seen_verts; c->left = s_ws_left_verts;
    c->right = s_ws_right_verts;
    c->min = s_ws_seen_min; c->max = s_ws_seen_max;
    c->bx0 = s_ws_bx0; c->bx1 = s_ws_bx1;
    c->by0 = s_ws_by0; c->by1 = s_ws_by1;
}

static void case_census(float dx, int ortho, int margin, int reported)
{
    char scratch[VERTS * WS_VSIZE];
    census_t before, after, untouched;
    char label[128];
    int wanted = !reported || margin;

    seed_source();
    set_state(dx, ortho, margin, reported);
    read_census(&untouched);
    ws_shift_before(scratch, s_src, VERTS);
    read_census(&before);

    seed_source();
    set_state(dx, ortho, margin, reported);
    ws_shift(scratch, s_src, VERTS);
    read_census(&after);

    snprintf(label, sizeof label,
             "census dx=%g ortho=%d margin=%d reported=%d: %s",
             (double)dx, ortho, margin, reported,
             wanted ? "still counts exactly as it did"
                    : "is genuinely skipped, not just quiet");
    if (wanted)
        check(memcmp(&before, &after, sizeof before) == 0, label);
    else
        check(memcmp(&untouched, &after, sizeof after) == 0, label);
}

/* ---- the wrappers, and the identity fast path --------------------------- */

static const void *s_seen[3];
static char s_seen_copy[3][WS_VSIZE];

static void __stdcall capture_tri(const void *a, const void *b, const void *c)
{
    s_seen[0] = a; s_seen[1] = b; s_seen[2] = c;
    memcpy(s_seen_copy[0], a, WS_VSIZE);
    memcpy(s_seen_copy[1], b, WS_VSIZE);
    memcpy(s_seen_copy[2], c, WS_VSIZE);
}

static void case_triangle(float dx, int ortho, int margin, int reported,
                          int expect_forwarded)
{
    const char *a = s_src, *b = s_src + WS_VSIZE, *c = s_src + 2 * WS_VSIZE;
    char label[128];
    unsigned i;

    seed_source();
    set_state(dx, ortho, margin, reported);
    s_ws_real_tri = capture_tri;
    memset(s_seen, 0, sizeof s_seen);
    ws_draw_triangle(a, b, c);

    snprintf(label, sizeof label,
             "ws_draw_triangle dx=%g ortho=%d margin=%d reported=%d: %s",
             (double)dx, ortho, margin, reported,
             expect_forwarded ? "forwards the game's own vertices"
                              : "submits shifted copies");
    if (expect_forwarded) {
        /* The whole point of the fast path: no copy at all. */
        check(s_seen[0] == (const void *)a && s_seen[1] == (const void *)b &&
              s_seen[2] == (const void *)c, label);
    } else {
        int shifted = 1;
        check(s_seen[0] != (const void *)a, label);
        for (i = 0; i < 3; i++) {
            float got = *(const float *)s_seen_copy[i];
            float want = X[i];
            if (ortho) {
                if (want < 0.0f) want = 0.0f;
                else if (want > 512.0f) want = 512.0f;
            }
            want += dx;
            if (got != want)
                shifted = 0;
        }
        check(shifted, "  ...and each x carries the origin");
    }
}

int main(void)
{
    float dxs[] = { 0.0f, 99.0f, 1.0f };
    unsigned d, o, m, r;

    /* Every combination of the three flags that gate the census, at three
     * origins including the inert one. 36 cases. */
    for (d = 0; d < sizeof dxs / sizeof dxs[0]; d++)
        for (o = 0; o < 2; o++)
            for (m = 0; m < 2; m++)
                for (r = 0; r < 2; r++)
                    case_shift(dxs[d], (int)o, (int)m, (int)r);
    for (d = 0; d < sizeof dxs / sizeof dxs[0]; d++)
        for (o = 0; o < 2; o++)
            for (m = 0; m < 2; m++)
                for (r = 0; r < 2; r++)
                    case_census(dxs[d], (int)o, (int)m, (int)r);

    /* The fast path is legal only when there is nothing at all to do: no
     * origin, no ortho clamp, and no census still reading. Anything else must
     * still go through the copy. */
    case_triangle(0.0f, 0, 0, 1, 1);        /* inert, census done -> forward  */
    case_triangle(0.0f, 0, 0, 0, 0);        /* extent report still wants it   */
    case_triangle(0.0f, 0, 1, 1, 0);        /* margin census still wants it   */
    case_triangle(0.0f, 1, 0, 1, 0);        /* the ortho clamp still applies  */
    case_triangle(99.0f, 0, 0, 1, 0);       /* a real origin                  */
    case_triangle(99.0f, 1, 0, 1, 0);       /* origin and clamp together      */

    if (failures) {
        fprintf(stderr, "widescreen shift tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("widescreen shift tests: all passed");
    return 0;
}
