/* widescreen_shift_test.c -- white-box test of the widescreen primitive shift.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * ws_shift() runs per vertex, on the game's own thread, in the shipped default
 * configuration, and it is the one place this host changes a coordinate the
 * game computed. The vertex path only runs once frames are being produced, so
 * `--dry-run` cannot reach it and it is tested here against the frozen
 * reference pasted in verbatim below as ws_shift_before(). The claim is exact:
 * for every state either function can be called in, the bytes they leave in the
 * destination vertex are identical.
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

/* ---- widescreen HUD at the edges --------------------------------------- */

static game_profile_t s_profile;
static const game_hud_t s_hud_table[1] = { { 0, 0, 0, 0, 0, "test element" } };

static void hud_state(void)
{
    set_state(99.0f, 0, 0, 1);
    s_ws_raster_width = 711;
    s_hud_armed = 1;
    s_hud_bias = 0;
    s_hud_pending = 0;
    s_hud_seen_used = 0;
    s_ws_world_now = s_ws_world_prev = 1;   /* the world fills the strips */
    s_profile.id = "test";
    s_profile.hud_elem = s_hud_table;
    s_profile.hud_elem_count = 1;
    g_game = &s_profile;
    s_ws_real_tri = capture_tri;
}

/* Draw one triangle at x0, x0+20, x0+40 and return the x the provider saw for
 * its first vertex, minus the game's: the whole offset it was given. */
static float hud_tri(float x0)
{
    char v[3 * WS_VSIZE];
    unsigned i;

    memset(v, 0, sizeof v);
    for (i = 0; i < 3; i++)
        *(float *)(v + i * WS_VSIZE) = x0 + 20.0f * (float)i;
    ws_draw_triangle(v, v + WS_VSIZE, v + 2 * WS_VSIZE);
    return *(const float *)s_seen_copy[0] - x0;
}

static void case_hud_element(float first, float later, float want)
{
    ws_hud_scope_t scope;
    char label[160];
    float got1, got2, after;

    hud_state();
    ws_hud_enter(&scope, 1u, "test element", 0);
    got1 = hud_tri(first);
    got2 = hud_tri(later);
    ws_hud_leave(&scope);
    after = hud_tri(later);
    snprintf(label, sizeof label, "HUD element first at x=%g: every primitive "
             "moves %g (got %g, then %g for x=%g), and %g again outside it",
             (double)first, (double)want, (double)got1, (double)got2,
             (double)later, (double)after);
    check(got1 == want && got2 == want && after == 99.0f && !s_hud_pending &&
          s_hud_bias == 0, label);
}

/* An element whose side is not yet known moves nothing, and the mode being off
 * means no element is ever opened. */
static void case_hud_disarmed(void)
{
    ws_hud_scope_t scope;

    hud_state();
    s_hud_armed = 0;
    ws_hud_enter(&scope, 1u, "test element", 0);
    check(!s_hud_pending && hud_tri(20.0f) == 99.0f,
          "HUD element with the mode off: the origin alone");
    ws_hud_leave(&scope);
}

/* A buffer clear after the world (Hydro's finish screen wipes the race to
 * black) makes what follows a page: an element there keeps the origin. */
static void case_hud_after_clear(void)
{
    ws_hud_scope_t scope;
    float got;

    hud_state();
    s_ws_world_prev = 0;
    s_ws_real_clear = NULL;
    ws_buffer_clear(0, 0, 0);
    ws_hud_enter(&scope, 1u, "test element", 0);
    got = hud_tri(20.0f);
    ws_hud_leave(&scope);
    check(!s_ws_world_now && got == 99.0f,
          "HUD element after a buffer clear: the origin alone, page layout");
}

/* Hydro's text: a fake _text_PrintStr appends records the way the game does,
 * and the hook must move exactly those, by the side of their own extent. */
#define GLYPH_STRIDE 40u
static uint16_t s_glyph_count;
static char s_glyph_buf[8 * GLYPH_STRIDE];
static float s_glyph_x[3];
static uint32_t s_finish;

static uint32_t __cdecl fake_printstr(uint32_t a0, uint32_t a1, uint32_t a2,
                                      uint32_t a3, uint32_t a4, uint32_t a5,
                                      uint32_t a6, uint32_t a7)
{
    unsigned i;

    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    (void)a7;
    for (i = 0; i < 3; i++)
        *(float *)(s_glyph_buf + (s_glyph_count + i) * GLYPH_STRIDE + 4) =
            s_glyph_x[i];
    s_glyph_count += 3;
    return 0;
}

static void case_hud_text(float x0, int depth, int dials, float want)
{
    char label[160];
    unsigned i;
    int ok = 1;

    hud_state();
    s_profile.text_glyph_count = (uint32_t)(uintptr_t)&s_glyph_count;
    s_profile.text_glyph_buf = (uint32_t)(uintptr_t)s_glyph_buf;
    s_profile.text_glyph_stride = GLYPH_STRIDE;
    s_profile.text_glyph_x = 4;
    s_real_text_printstr = fake_printstr;
    memset(s_glyph_buf, 0, sizeof s_glyph_buf);
    *(float *)(s_glyph_buf + 4) = -5.0f;    /* an earlier string: never moved */
    s_glyph_count = 1;
    for (i = 0; i < 3; i++)
        s_glyph_x[i] = x0 + 12.0f * (float)i;
    s_hud_text_depth = depth;
    s_finish = dials ? 0u : 0x100u;
    s_profile.hud_finish_flags = (uint32_t)(uintptr_t)&s_finish;
    s_profile.hud_finish_bit = 0x100u;
    gl_text_printstr(0, 0, (uint32_t)(uintptr_t)"TEXT", 0, 0, 0, 0, 0);
    s_hud_text_depth = 0;
    for (i = 0; i < 3; i++)
        if (*(const float *)(s_glyph_buf + (1 + i) * GLYPH_STRIDE + 4) !=
            s_glyph_x[i] + want)
            ok = 0;
    snprintf(label, sizeof label, "HUD text at x=%g, depth %d, race %d: its own "
             "records move %g and nothing before them does", (double)x0, depth,
             dials, (double)want);
    check(ok && *(const float *)(s_glyph_buf + 4) == -5.0f &&
          s_glyph_count == 4, label);
}

/* Hydro's tracking cards: a fake _mesh3d_DrawOrtho draws one triangle inside
 * the native field, and the card's own x argument decides the whole offset,
 * with the HUD mode on or off. */
static float s_card_got;

static void __cdecl fake_card_ortho(uint32_t a0, uint32_t a1, uint32_t a2,
                                    uint32_t a3, uint32_t a4, uint32_t a5,
                                    uint32_t a6, uint32_t a7)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    (void)a7;
    s_card_got = hud_tri(200.0f);
}

static void case_track_card(float x, int armed, float want)
{
    static float spread = 245.0f;
    char label[160];
    uint32_t xb;
    float after;

    hud_state();
    s_hud_armed = armed;
    s_profile.track_ra_lo = 1u;             /* every caller is a card draw */
    s_profile.track_ra_hi = 0xFFFFFFFFu;
    s_profile.track_spread = (uint32_t)(uintptr_t)&spread;
    s_real_drawortho = fake_card_ortho;
    memcpy(&xb, &x, sizeof xb);
    gl_draw_ortho(0, xb, 0, 0, 0, 0, 0, 0);
    s_profile.track_ra_lo = s_profile.track_ra_hi = 0;
    after = hud_tri(200.0f);
    snprintf(label, sizeof label, "tracking card at x=%g, HUD mode %d: moves "
             "%g (got %g), and %g again after it", (double)x, armed,
             (double)want, (double)s_card_got, (double)after);
    check(s_card_got == want && after == 99.0f && s_hud_bias == 0 &&
          !s_hud_name && !s_ws_ortho_depth, label);
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

    /* 711-wide raster, 512-wide game, origin 99: a left-third element lands
     * on raster 0 (total 0), a right-third one on the last column (total
     * 199), the middle third keeps the origin. The FIRST primitive decides. */
    case_hud_element(20.0f, 300.0f, 0.0f);
    case_hud_element(420.0f, 30.0f, 199.0f);
    case_hud_element(230.0f, 480.0f, 99.0f);
    /* Halfway up the left ramp (centre 192 = 4.5/12 of 512): half the left
     * offset, rounded away from zero, so a sliding element never jumps. */
    case_hud_element(172.0f, 480.0f, 49.0f);
    case_hud_element(300.0f, 20.0f, 149.0f);    /* centre 320 = 7.5/12 */
    case_hud_disarmed();
    case_hud_after_clear();
    /* Records are moved before the seam, which adds the origin itself. */
    case_hud_text(20.0f, 1, 1, -99.0f);
    case_hud_text(440.0f, 1, 1, 100.0f);
    case_hud_text(240.0f, 1, 1, 0.0f);
    case_hud_text(20.0f, 0, 1, 0.0f);       /* not HUD text: untouched */
    case_hud_text(20.0f, 1, 0, 0.0f);       /* result screen up: stays put */
    /* Sides at +-253 reach the raster's edges whatever the HUD mode; the
     * bottom (|x| <= 245) spreads in proportion, so it meets the sides. */
    case_track_card(253.0f, 1, 199.0f);
    case_track_card(-253.0f, 1, 0.0f);
    case_track_card(253.0f, 0, 199.0f);
    case_track_card(0.0f, 1, 99.0f);
    case_track_card(122.5f, 0, 149.0f);
    case_track_card(-122.5f, 0, 49.0f);

    if (failures) {
        fprintf(stderr, "widescreen shift tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("widescreen shift tests: all passed");
    return 0;
}
