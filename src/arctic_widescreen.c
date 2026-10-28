/* arctic_widescreen.c -- Arctic Thunder's Hor+ widescreen.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The ETS titles' design (widescreen.c) on a game with no symbols: the game
 * keeps its own 640x480 coordinates, the
 * provider draws its picture at the centre of a 16:9 raster (glide3.c, from
 * the field published here), and what decides visibility before that move is
 * widened in snow.exe's memory: the culling frustum's corners, the four
 * screen-space clippers' x planes, and the tests of a projected point against
 * the screen. The projection itself is untouched, so the picture is widened,
 * never rescaled. The running title's ws table (generated/<title>_widescreen.c)
 * lists every site; Ultimate's MSVC build reaches the same roles through
 * different instruction shapes, hence the extra kinds (arctic.h).
 *
 * Bounds use a culling half-width e that is the origin rounded UP to cover
 * the raster's last column (widescreen.c, widescreen_settle()): with an odd
 * margin the origin is the lower whole pixel and the culling the upper one.
 *
 * [graphics] widescreen_hud = edges moves the race HUD's side groups to the
 * raster's edges, as widescreen.c does for the ETS titles: the dials to the
 * right, the radar, track map and place to the left, the timer centred. The
 * HUD is gobj.dll sprites and strings, so an element is one gobj object, and
 * an object is the HUD when one of the snow.exe calls the generator lists
 * created it (the running title's hud table). gobj's create
 * entries are hooked in its function table to see who calls them; every
 * creation overwrites what is known about its pointer, so a reused one never
 * keeps the last owner's verdict. Each draw of a HUD object is moved whole,
 * by whole pixels, on a copy of its vertices: nothing of the game's changes.
 */
#include "vcthunder.h"
#include "arctic.h"
#include "glide_int.h"

#include <string.h>

static int   s_armed;
static int   s_raster_w;          /* the visible raster                      */
static int   s_dx;                /* where the native picture starts in it   */
static float s_lo, s_hi, s_nhi;   /* the widened x bounds, native coords     */

typedef uint32_t (__stdcall *win_open_fn)(uint32_t, uint32_t, uint32_t,
                                          uint32_t, uint32_t, uint32_t,
                                          uint32_t);
static win_open_fn s_real_open;

/* widescreen_hud = edges, armed with the planes (below). */
typedef void (__stdcall *vac_fn)(uint32_t, uint32_t, void *, uint32_t);
static vac_fn s_real_vac;
static void hud_arm(HMODULE snow, HMODULE gobj);
static void __stdcall hud_draw(uint32_t mode, uint32_t count, void *v,
                               uint32_t stride);
static int       s_hud;
static uintptr_t s_snow_base, s_draw_ret;

int arctic_ws_wanted_width(void)
{
    return g_cfg.widescreen ? glide_widescreen_width(AT->ws->native_h)
                            : AT->ws->native_w;
}

int arctic_ws_picture_width(void)
{
    return s_armed ? s_raster_w : AT->ws->native_w;
}

/* Write `n` bytes over the game's image, whatever the page's protection. */
static int poke(void *at, const void *src, size_t n)
{
    DWORD old;

    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(at, src, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return 1;
}

/* A VA the generator read from the file, where the image actually is. */
static const uint8_t *live(const uint8_t *img, uint32_t preferred, uint32_t va)
{
    return img + (va - preferred);
}

/* The bytes an XSUB site must hold: `fldl disp(%ebp)` DD 45 d, or
 * `fldl disp(%esp)` DD 44 24 d, then `fchs` D9 E0. Its length, or 0. */
static size_t xsub_len(const uint8_t *p, const arctic_ws_site_t *s)
{
    if (s->base == 5 && p[0] == 0xDD && p[1] == 0x45 &&
        p[2] == (uint8_t)s->disp && p[3] == 0xD9 && p[4] == 0xE0)
        return 5;
    if (s->base == 4 && p[0] == 0xDD && p[1] == 0x44 && p[2] == 0x24 &&
        p[3] == (uint8_t)s->disp && p[4] == 0xD9 && p[5] == 0xE0)
        return 6;
    return 0;
}

/* The bytes an XSUBG site must hold: `flds disp(%esp); fchs`. */
static int xsubg_ok(const uint8_t *p, const arctic_ws_site_t *s)
{
    return s->base == 4 && p[0] == 0xD9 && p[1] == 0x44 && p[2] == 0x24 &&
           p[3] == s->disp && p[4] == 0xD9 && p[5] == 0xE0 &&
           s->disp < 0x7C && s->gdisp < 0x7C;
}

/* A register a mod-00 operand can name without a SIB or a displacement. */
static int plain_reg(uint8_t r)
{
    return r < 8 && r != 4 && r != 5;
}

/* The bytes an XDIV site must hold: `fdivrs (r0)` D8 38+r0, `movl $0,(r1)`
 * C7 00+r1 00000000, then `fchs`, directly or after `inc %ebp`. */
static int xdiv_ok(const uint8_t *p, const arctic_ws_site_t *s)
{
    static const uint8_t zero[4];

    return plain_reg(s->base) && plain_reg(s->disp) &&
           p[0] == 0xD8 && p[1] == (0x38 | s->base) &&
           p[2] == 0xC7 && p[3] == s->disp && !memcmp(p + 4, zero, 4) &&
           ((p[8] == 0xD9 && p[9] == 0xE0) ||
            (p[8] == 0x45 && p[9] == 0xD9 && p[10] == 0xE0));
}

/* The case a JTAB entry names: `mov %ebp,(base)` 89 28+base, `jmp rel8`.
 * Where that jump lands, or NULL. */
static const uint8_t *jtab_cont(const uint8_t *t, const arctic_ws_site_t *s)
{
    if (!plain_reg(s->base) || t[0] != 0x89 || t[1] != (0x28 | s->base) ||
        t[2] != 0xEB)
        return NULL;
    return t + 4 + (int8_t)t[3];
}

/* The bytes an XRANGE site must hold: `test r,r; jl out; cmp r,imm32;
 * jg out` on the site's register, both branches to the same place. That
 * place, or NULL. */
static const uint8_t *xrange_out(const uint8_t *p, const arctic_ws_site_t *s)
{
    const uint8_t *a = p + 4 + (int8_t)p[3], *b = p + 12 + (int8_t)p[11];
    uint8_t r = s->base;

    if (r > 7 || r == 1 || r == 4 || p[0] != 0x85 ||
        p[1] != (0xC0 | r << 3 | r) || p[2] != 0x7C || p[4] != 0x81 ||
        p[5] != (0xF8 | r) || p[10] != 0x7F || a != b)
        return NULL;
    return a;
}

static int site_ok(const uint8_t *img, uint32_t preferred, const arctic_ws_site_t *s)
{
    const uint8_t *p = img + s->rva;
    uint32_t v, hi, named;

    memcpy(&v, p, 4);
    named = v - (uint32_t)(uintptr_t)img + preferred;
    switch (s->kind) {
    case AWS_F32:
    case AWS_I32:
        return v == s->expect;
    case AWS_F64:
        memcpy(&hi, p + 4, 4);
        return v == s->expect && hi == s->expect_hi;
    case AWS_OPND:          /* relocated with the image */
    case AWS_OPND64:
    case AWS_JTAB:
        if (named != s->expect)
            return 0;
        return s->kind != AWS_JTAB ||
               jtab_cont(live(img, preferred, s->expect), s) != NULL;
    case AWS_ST1LO:
        memcpy(&v, p + 2, 4);
        return p[0] == 0xD8 && p[1] == 0x05 &&
               v - (uint32_t)(uintptr_t)img + preferred == s->expect;
    case AWS_XSUB:
        return xsub_len(p, s) != 0;
    case AWS_XSUBG:
        return xsubg_ok(p, s);
    case AWS_XDIV:
        return xdiv_ok(p, s);
    case AWS_XRANGE:
        memcpy(&v, p + 6, 4);
        return xrange_out(p, s) && v == s->expect;
    }
    return 0;
}

/* What each kind needs of the pool: code (stubs) and data (redirected
 * operands' values), so that all of it is known to fit before a byte of the
 * game is written. */
static unsigned stub_bytes(const arctic_ws_site_t *s)
{
    switch (s->kind) {
    case AWS_XSUB: case AWS_XSUBG: case AWS_XDIV: case AWS_JTAB:
    case AWS_ST1LO:
        return 32;
    }
    return 0;
}

static unsigned data_bytes(const arctic_ws_site_t *s)
{
    if (s->kind == AWS_OPND64)
        return 8;
    if (s->kind == AWS_OPND && (s->role == AWS_FRUSTUM || s->role == AWS_RASTER))
        return 8;
    return 0;
}

static uint8_t *put_addr(uint8_t *at, const void *a)
{
    memcpy(at, &a, 4);
    return at + 4;
}

/* `fldl disp(base); fsubrs [s_lo]; ret`: st0 = LO - x0, where the game had
 * -x0 for a left plane at 0. A call pushes the return address, so an
 * esp-relative slot is 4 further away inside it. */
static uint8_t *emit_xsub(uint8_t *at, const arctic_ws_site_t *s)
{
    *at++ = 0xDD;
    if (s->base == 5) {
        *at++ = 0x45;
        *at++ = (uint8_t)s->disp;
    } else {
        *at++ = 0x44;
        *at++ = 0x24;
        *at++ = (uint8_t)(s->disp + 4);
    }
    *at++ = 0xD8;
    *at++ = 0x2D;
    at = put_addr(at, &s_lo);
    *at++ = 0xC3;
    return at;
}

/* The same on a float slot, for the left plane only: the code it replaces
 * is shared with another plane, and a dword of the frame says which one is
 * running. `cmpl $gval,gdisp(%esp); flds disp(%esp); jne 1f;
 * fsubrs [s_lo]; ret; 1: fchs; ret`. The cmpl's flags are dead after the
 * site (the generator checks what follows it). */
static uint8_t *emit_xsubg(uint8_t *at, const arctic_ws_site_t *s)
{
    *at++ = 0x83; *at++ = 0x7C; *at++ = 0x24;
    *at++ = (uint8_t)(s->gdisp + 4); *at++ = s->gval;
    *at++ = 0xD9; *at++ = 0x44; *at++ = 0x24; *at++ = (uint8_t)(s->disp + 4);
    *at++ = 0x75; *at++ = 0x07;
    *at++ = 0xD8; *at++ = 0x2D;
    at = put_addr(at, &s_lo);
    *at++ = 0xC3;
    *at++ = 0xD9; *at++ = 0xE0;
    *at++ = 0xC3;
    return at;
}

/* For `fdivrs (r0); movl $0,(r1)` with st0 = x1 - x0: `flds [s_lo];
 * fsubrs (r0); fdivrp %st,%st(1); movl $LO,(r1); ret` leaves
 * (x0 - LO)/(x1 - x0), which the game's own fchs makes (LO - x0)/(x1 - x0),
 * and clamps the new vertex's x to LO. x0 is read before r1 is written,
 * which matters where r0 is r1 (the line clipper clips in place). */
static uint8_t *emit_xdiv(uint8_t *at, const arctic_ws_site_t *s)
{
    *at++ = 0xD9; *at++ = 0x05;
    at = put_addr(at, &s_lo);
    *at++ = 0xD8; *at++ = (uint8_t)(0x28 | s->base);
    *at++ = 0xDE; *at++ = 0xF1;
    *at++ = 0xC7; *at++ = s->disp;
    memcpy(at, &s_lo, 4);
    at += 4;
    *at++ = 0xC3;
    return at;
}

/* A clipper's case `mov %ebp,(base); jmp cont` with ebp the function's zero
 * becomes `movl $LO,(base); jmp cont`. */
static uint8_t *emit_jtab(uint8_t *at, const arctic_ws_site_t *s,
                          const uint8_t *cont)
{
    int32_t rel;

    *at++ = 0xC7; *at++ = s->base;
    memcpy(at, &s_lo, 4);
    at += 4;
    *at++ = 0xE9;
    rel = (int32_t)((intptr_t)cont - ((intptr_t)at + 4));
    memcpy(at, &rel, 4);
    return at + 4;
}

/* `fxch; fadds [s_lo]; fxch; <the site's own fadds abs32>; ret`: the game
 * computes a left bound as -m on st(1) and stores it after this add. */
static uint8_t *emit_st1lo(uint8_t *at, const uint8_t *site)
{
    *at++ = 0xD9; *at++ = 0xC9;
    *at++ = 0xD8; *at++ = 0x05;
    at = put_addr(at, &s_lo);
    *at++ = 0xD9; *at++ = 0xC9;
    memcpy(at, site, 6);
    at += 6;
    *at++ = 0xC3;
    return at;
}

/* `call stub` over `n` bytes, the rest nops. */
static int call_over(uint8_t *p, size_t n, const uint8_t *stub)
{
    uint8_t b[16];
    int32_t rel = (int32_t)((intptr_t)stub - ((intptr_t)p + 5));

    if (n < 5 || n > sizeof b)
        return 0;
    memset(b, 0x90, sizeof b);
    b[0] = 0xE8;
    memcpy(b + 1, &rel, 4);
    return poke(p, b, n);
}

static int apply(uint8_t *img, uint32_t preferred, const arctic_ws_site_t *s,
                 float e, uint8_t **tramp, uint8_t **data)
{
    uint8_t *p = img + s->rva;
    float f;
    double d;
    int32_t i;
    const void *to;
    uint8_t *stub = *tramp;
    int half = AT->ws->native_w / 2;
    int grow = s_raster_w - AT->ws->native_w;

    switch (s->kind) {
    case AWS_F32:
        memcpy(&f, p, 4);
        switch (s->role) {
        case AWS_LO:      f = -e; break;
        case AWS_HI:      f = (float)AT->ws->native_w + e; break;
        case AWS_NHI:     f = -((float)AT->ws->native_w + e); break;
        case AWS_FRUSTUM: f *= ((float)half + e) / (float)half; break;
        case AWS_RASTER:  f += (float)grow; break;
        default: return 0;
        }
        return poke(p, &f, 4);
    case AWS_F64:
        if (s->role != AWS_ORIGIN)
            return 0;
        memcpy(&d, p, 8);
        d += (double)s_dx;
        return poke(p, &d, 8);
    case AWS_I32:
        if (s->role != AWS_RASTER)
            return 0;
        memcpy(&i, p, 4);
        i += grow;
        return poke(p, &i, 4);
    case AWS_OPND:
        switch (s->role) {
        case AWS_LO:  to = &s_lo;  break;
        case AWS_HI:  to = &s_hi;  break;
        case AWS_NHI: to = &s_nhi; break;
        case AWS_FRUSTUM:
        case AWS_RASTER:
            memcpy(&f, live(img, preferred, s->expect), 4);
            f = s->role == AWS_RASTER ? f + (float)grow
                                      : f * ((float)half + e) / (float)half;
            *data -= 8;
            memcpy(*data, &f, 4);
            to = *data;
            break;
        default: return 0;
        }
        return poke(p, &to, 4);
    case AWS_OPND64:
        if (s->role != AWS_ORIGIN && s->role != AWS_NORIGIN)
            return 0;
        memcpy(&d, live(img, preferred, s->expect), 8);
        d += s->role == AWS_ORIGIN ? (double)s_dx : -(double)s_dx;
        *data -= 8;
        memcpy(*data, &d, 8);
        to = *data;
        return poke(p, &to, 4);
    case AWS_XRANGE: {
        /* lea ecx,[r+e]; cmp ecx,w+2e+1; jae out; nop. ecx is dead at
         * every such site (the generator checks what follows). */
        uint8_t b[12] = { 0x8D, 0x48, 0, 0x81, 0xF9, 0, 0, 0, 0, 0x73, 0, 0x90 };
        uint32_t span = (uint32_t)(AT->ws->native_w + 2 * (int)e + 1);
        const uint8_t *out = xrange_out(p, s);

        if (s->role != AWS_HI || e > 127.0f || !out)
            return 0;
        b[1] |= s->base;
        b[2] = (uint8_t)(int)e;
        memcpy(b + 5, &span, 4);
        b[10] = (uint8_t)(out - (p + 11));
        return poke(p, b, sizeof b);
    }
    case AWS_XSUB:
        *tramp = emit_xsub(*tramp, s);
        return call_over(p, xsub_len(p, s), stub);
    case AWS_XSUBG:
        *tramp = emit_xsubg(*tramp, s);
        return call_over(p, 6, stub);
    case AWS_XDIV:
        *tramp = emit_xdiv(*tramp, s);
        return call_over(p, 8, stub);
    case AWS_ST1LO:
        *tramp = emit_st1lo(*tramp, p);
        return call_over(p, 6, stub);
    case AWS_JTAB: {
        const uint8_t *cont = jtab_cont(live(img, preferred, s->expect), s);

        if (!cont)
            return 0;
        *tramp = emit_jtab(*tramp, s, cont);
        to = stub;
        return poke(p, &to, 4);
    }
    }
    return 0;
}

int arctic_ws_arm(HMODULE snow, HMODULE gobj, uint32_t preferred)
{
    uint8_t *img = (uint8_t *)snow;
    uint8_t *pool, *tramp, *data;
    unsigned k, bad = 0, code = 0, room = 0;
    int cull;
    float e;

    s_armed = 0;
    if (!g_cfg.widescreen)
        return 1;
    for (k = 0; k < AT->ws->count; k++) {
        const arctic_ws_site_t *s = &AT->ws->site[k];

        code += stub_bytes(s);
        room += data_bytes(s);
        if (!site_ok(img, preferred, s)) {
            LOGE("widescreen: snow.exe+0x%06X does not hold what the table says"
                 " (kind %u)", s->rva, s->kind);
            bad++;
        }
    }
    if (bad || !AT->ws->count) {
        LOGE("widescreen: refused; %u of %u sites differ, nothing written",
             bad, AT->ws->count);
        return 1;
    }
    pool = (uint8_t *)VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_EXECUTE_READWRITE);
    if (!pool || code + room > 4096u) {
        LOGE("widescreen: refused; no room for %u bytes of stubs and %u of"
             " values", code, room);
        return 1;
    }

    s_raster_w = glide_widescreen_width(AT->ws->native_h);
    s_dx = (s_raster_w - AT->ws->native_w) / 2;
    cull = AT->ws->native_w + 2 * s_dx +
           (((s_raster_w - AT->ws->native_w) & 1) ? 2 : 0);
    e = (float)(cull - AT->ws->native_w) / 2.0f;
    s_lo = -e;
    s_hi = (float)AT->ws->native_w + e;
    s_nhi = -s_hi;

    tramp = pool;
    data = pool + 4096;
    for (k = 0; k < AT->ws->count; k++)
        if (!apply(img, preferred, &AT->ws->site[k], e, &tramp, &data)) {
            /* Checked above, so only a refused VirtualProtect lands here,
             * and half a set of planes is worse than none: stop loudly. */
            LOGE("widescreen: snow.exe+0x%06X could not be written; the game's"
                 " clipping is now inconsistent", AT->ws->site[k].rva);
            return 0;
        }
    s_armed = 1;
    if (!strcmp(g_cfg.widescreen_hud, "edges"))
        hud_arm(snow, gobj);
    LOGI("widescreen: %u sites widened; the game keeps its %dx%d picture, drawn"
         " at x=%d in a %dx%d raster, and culls and clips x to %d..%d",
         AT->ws->count, AT->ws->native_w, AT->ws->native_h, s_dx,
         s_raster_w, AT->ws->native_h, (int)s_lo, (int)s_hi);
    return 1;
}

/* The game opens its native surface; it gets one containing the raster, and
 * the provider is told where the native picture sits inside it. */
static uint32_t __stdcall ws_grSstWinOpen(uint32_t hwnd, uint32_t res,
                                          uint32_t refresh, uint32_t format,
                                          uint32_t origin, uint32_t ncolor,
                                          uint32_t naux)
{
    int sw, sh;
    int surface = glide_surface_for_raster((int)res, s_raster_w,
                                           AT->ws->native_h, &sw, &sh);

    glide_publish_field(s_raster_w, AT->ws->native_h, AT->ws->native_w);
    LOGI("widescreen: resolution enum %u -> %d (%dx%d surface) for the %dx%d"
         " raster", res, surface, sw, sh, s_raster_w, AT->ws->native_h);
    return s_real_open(hwnd, (uint32_t)surface, refresh, format, origin,
                       ncolor, naux);
}

void *arctic_ws_bind(const char *imp, void *real)
{
    if (!s_armed || !real)
        return NULL;
    if (s_hud && !strcmp(imp, "_grDrawVertexArrayContiguous@16")) {
        s_real_vac = (vac_fn)real;
        return (void *)hud_draw;
    }
    if (strcmp(imp, "_grSstWinOpen@28"))
        return NULL;
    s_real_open = (win_open_fn)real;
    return (void *)ws_grSstWinOpen;
}

/* ---- the HUD at the edges ------------------------------------------------ */

/* The real create entries, and the argument dwords each reads, for the thunks
 * below; named for them. */
static void   *s_hud_real[3]  __asm__("_vct_hud_real")  __attribute__((used));
static uint8_t s_hud_words[3] __asm__("_vct_hud_words") __attribute__((used));
static void __cdecl hud_created(uint32_t slot, uint32_t obj, uint32_t ra)
    __asm__("_vct_hud_created") __attribute__((used));
void vct_hud_thunk0(void), vct_hud_thunk1(void), vct_hud_thunk2(void);

/* One thunk per hooked entry: the caller's arguments are copied (a printf
 * string's are variadic, so an entry's room is read, never its count), the
 * real entry called, and what it made reported with the caller's return
 * address. The game's FPU is untouched, and its registers are as the real
 * entry leaves them but for ecx and edx, which cdecl does not keep. */
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl _vct_hud_thunk0\n_vct_hud_thunk0: pushl $0\n jmp 1f\n"
    ".globl _vct_hud_thunk1\n_vct_hud_thunk1: pushl $1\n jmp 1f\n"
    ".globl _vct_hud_thunk2\n_vct_hud_thunk2: pushl $2\n"
    "1:  pushl %ebp\n"
    "    movl  %esp, %ebp\n"            /* 4: slot, 8: return, 12: arguments */
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    movl  4(%ebp), %eax\n"
    "    movzbl _vct_hud_words(%eax), %ecx\n"
    "    leal  0(,%ecx,4), %edx\n"
    "    subl  %edx, %esp\n"
    "    leal  12(%ebp), %esi\n"
    "    movl  %esp, %edi\n"
    "    cld\n"
    "    rep movsl\n"
    "    call  *_vct_hud_real(,%eax,4)\n"
    "    movl  %eax, %esi\n"
    "    pushl 8(%ebp)\n"
    "    pushl %eax\n"
    "    pushl 4(%ebp)\n"
    "    call  _vct_hud_created\n"
    "    movl  %esi, %eax\n"
    "    leal  -8(%ebp), %esp\n"
    "    popl  %edi\n"
    "    popl  %esi\n"
    "    popl  %ebp\n"
    "    leal  4(%esp), %esp\n"         /* the slot */
    "    ret\n");

/* Objects a HUD call created: pointer -> 1 + its site. Only HUD objects are
 * entered; a later creation of the same pointer by anything else clears it. */
#define HUD_MAP 4096u
static struct { uint32_t obj; uint16_t site; } s_hud_map[HUD_MAP];
static unsigned s_hud_full;

static unsigned hud_slot(uint32_t obj, int add)
{
    unsigned h = (obj >> 3) & (HUD_MAP - 1), n;

    for (n = 0; n < HUD_MAP; n++, h = (h + 1) & (HUD_MAP - 1)) {
        if (s_hud_map[h].obj == obj)
            return h;
        if (!s_hud_map[h].obj) {
            if (!add)
                return HUD_MAP;
            s_hud_map[h].obj = obj;
            return h;
        }
    }
    return HUD_MAP;
}

static void __cdecl hud_created(uint32_t slot, uint32_t obj, uint32_t ra)
{
    uint32_t at = ra - (uint32_t)s_snow_base;
    unsigned k, h;

    if (!obj)
        return;
    for (k = 0; k < AT->hud->count; k++)
        if (AT->hud->site[k].rva + AT->hud->site[k].len == at &&
            AT->hud->site[k].slot == slot)
            break;
    h = hud_slot(obj, k < AT->hud->count);
    if (h < HUD_MAP)
        s_hud_map[h].site = k < AT->hud->count ? (uint16_t)(k + 1) : 0;
    else if (k < AT->hud->count && !s_hud_full++)
        LOGE("widescreen: HUD object table full; a HUD piece created by"
             " snow.exe+0x%06X stays in the 4:3 centre", at);
}

/* Each site's first draw on each side, logged once. */
static uint8_t s_hud_told[64];

/* gobj's sprite loop draws each sprite as one fan of its four vertices at
 * sprite + vert_off. A glyph's string, or else the sprite, is the element;
 * the element's own x says which edge it keeps its distance from. The whole
 * offset is whole pixels, as the origin is. */
static void __stdcall hud_draw(uint32_t mode, uint32_t count, void *v,
                               uint32_t stride)
{
    const uint8_t *spr = (const uint8_t *)v - AT->hud->vert_off;
    uint8_t moved[8 * 32];
    uint32_t owner, key;
    unsigned h, i, site;
    float x;
    int bias;

    if (!v || (uintptr_t)__builtin_return_address(0) != s_draw_ret ||
        count > 8 || stride < 8 || stride > 32) {
        s_real_vac(mode, count, v, stride);
        return;
    }
    memcpy(&owner, spr + AT->hud->owner_off, 4);
    key = owner ? owner : (uint32_t)(uintptr_t)spr;
    h = hud_slot(key, 0);
    site = h < HUD_MAP ? s_hud_map[h].site : 0;
    if (!site) {
        s_real_vac(mode, count, v, stride);
        return;
    }
    memcpy(&x, owner ? (const uint8_t *)(uintptr_t)owner + AT->hud->string_x
                     : spr + AT->hud->sprite_x, 4);
    bias = x < (float)AT->ws->native_w * 0.5f
         ? -s_dx : s_raster_w - AT->ws->native_w - s_dx;
    memcpy(moved, v, (size_t)count * stride);
    for (i = 0; i < count; i++) {
        float vx;
        memcpy(&vx, moved + i * stride, 4);
        vx += (float)bias;
        memcpy(moved + i * stride, &vx, 4);
    }
    if (site - 1 < sizeof s_hud_told && !(s_hud_told[site - 1] & (bias < 0 ? 1 : 2))) {
        s_hud_told[site - 1] |= bias < 0 ? 1 : 2;
        LOGI("widescreen: HUD; what snow.exe+0x%06X creates goes to the %s"
             " edge (%+d)", AT->hud->site[site - 1].rva,
             bias < 0 ? "left" : "right", bias);
    }
    s_real_vac(mode, count, moved, stride);
}

/* Check gobj's table and every HUD site, then hook the three entries. All or
 * nothing: a refusal leaves the HUD where the game puts it. */
static void hud_arm(HMODULE snow, HMODULE gobj)
{
    const arctic_hud_table_t *t = AT->hud;   /* 0: this title has none */
    uint8_t *g = (uint8_t *)gobj, *img = (uint8_t *)snow;
    uint32_t *tab;
    static void (*const thunk[3])(void) = {
        vct_hud_thunk0, vct_hud_thunk1, vct_hud_thunk2 };
    DWORD old;
    unsigned k;

    if (!gobj || !t || !t->count) {
        LOGE("widescreen: widescreen_hud = edges refused; no HUD table");
        return;
    }
    tab = (uint32_t *)(g + t->table);
    for (k = 0; k < 3; k++)
        if (tab[t->slot[k] / 4] != (uint32_t)(uintptr_t)(g + t->target[k])) {
            LOGE("widescreen: widescreen_hud = edges refused; gobj.dll's"
                 " entry 0x%X is not what the table says", t->slot[k]);
            return;
        }
    for (k = 0; k < t->count; k++) {
        const uint8_t *p = img + t->site[k].rva;
        uint32_t named;
        int ok;

        /* A call through a register holding the slot's address does not
         * name it; that it reaches this slot is proved by the thunk seeing
         * this return address, so a wrong claim only never matches. */
        memcpy(&named, p + 2, 4);
        switch (t->site[k].len) {
        case 6:  ok = p[0] == 0xFF && (p[1] & 0xF8) == 0x90 &&
                      (p[1] & 7) != 4 && named == t->slot[t->site[k].slot];
                 break;
        case 2:  ok = p[0] == 0xFF && (p[1] & 0xF8) == 0x10 &&
                      plain_reg(p[1] & 7);
                 break;
        case 3:  ok = p[0] == 0xFF && p[1] == 0x55 && p[2] == 0x00; break;
        default: ok = 0;
        }
        if (!ok) {
            LOGE("widescreen: widescreen_hud = edges refused; snow.exe+0x%06X"
                 " is not the HUD call the table says", t->site[k].rva);
            return;
        }
    }
    if (!VirtualProtect(tab, 0x200, PAGE_READWRITE, &old)) {
        LOGE("widescreen: widescreen_hud = edges refused; gobj.dll's table"
             " cannot be written");
        return;
    }
    for (k = 0; k < 3; k++) {
        s_hud_real[k] = (void *)(uintptr_t)tab[t->slot[k] / 4];
        s_hud_words[k] = t->words[k];
        tab[t->slot[k] / 4] = (uint32_t)(uintptr_t)thunk[k];
    }
    VirtualProtect(tab, 0x200, old, &old);
    s_snow_base = (uintptr_t)snow;
    s_draw_ret = (uintptr_t)(g + t->draw_ret);
    s_hud = 1;
    LOGI("widescreen: widescreen_hud = edges; %u HUD calls in snow.exe, and"
         " gobj.dll's sprite, string and printf-string entries hooked",
         t->count);
}
