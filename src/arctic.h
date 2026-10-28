/* arctic.h -- Arctic Thunder: a win32 title, mapped as images and bound.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The ETS titles' profile G does not exist on this path; nothing here reads
 * it.
 */
#ifndef VCT_ARCTIC_H
#define VCT_ARCTIC_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

/* The game's executable first, then its own DLLs, in the order
 * scripts/titles.py registers them: the slots are positional. */
#define ARCTIC_IMAGES 3
#define ARCTIC_SNOW   0         /* the executable                            */
#define ARCTIC_GOOSE  1         /* its process/I-O library, imported by both  */
#define ARCTIC_GOBJ   2         /* loaded by goose at init                    */

typedef struct {
    const char *name;           /* "snow.exe"                                    */
    const char *sha256;         /* pinned in scripts/titles.py                   */
    uint32_t    size;
} arctic_file_t;

/* The cabinet's operator system, which runs in a host process of its own
 * (src/arctic_diag.c): diag.exe from the package's diagnostics folder, then
 * the game's diagdll.dll it loads from the game folder. */
#define ARCTIC_DIAG_EXE 0
#define ARCTIC_DIAG_DLL 1

/* disk_dir and diag_dir are where the import finds the install package's two
 * folders on MAME's disk ('/' between components); the pack keeps them as
 * data/<id> and data/<id>-diagnostics. `settings` is the title whose ini
 * sections this one reads, [<settings>.*]: its own id, or the title whose
 * cabinet it runs in. The game's own files stay its own either way.
 * An update title's disc holds only what changed: `base` is the title whose
 * imported folders it is laid over, and update_file one more file the disc
 * copies, to update_to in the game folder. All three 0 otherwise. */
typedef struct {
    const char *id, *name, *build, *settings;
    arctic_file_t file[ARCTIC_IMAGES];
    arctic_file_t diag[2];
    const char *disk_dir, *diag_dir;
    const char *base, *update_file, *update_to;
} arctic_ids_t;

/* The whole Arctic startup. Never returns: it exits through the host's own
 * teardown, on --dry-run, on failure, or when the game exits. */
__attribute__((noreturn)) void arctic_run(void);


/* arctic_seh.c: frame-based SEH for images whose scope tables msvcrt refuses. */
int __cdecl arctic_except_handler3(void *rec, void *frame, void *ctx, void *disp);
void arctic_seh_report(void);

/* ---- widescreen (arctic_widescreen.c) ------------------------------------ */

/* One site in snow.exe that decides visibility in the game's own 640-wide
 * coordinates before the picture is moved to the raster's centre, and what it
 * becomes. generated/<title>_widescreen.c lists them (scripts/gen-gamedefs.py
 * derives each from the dump); the host checks every one before it writes
 * any. Roles, with e the half-width the culling is widened by:
 *   LO 0 -> -e, HI w -> w+e, NHI -w -> -(w+e)    a clip or test bound on x
 *   FRUSTUM +-1 -> +-(w/2+e)/(w/2)               a culling-frustum corner
 *   RASTER  v -> v + (raster - w)                 in raster coordinates
 *   ORIGIN  v -> v + the picture's offset         native x to raster x
 *   NORIGIN v -> v - the picture's offset         the same, subtracted   */
enum { AWS_F32, AWS_F64, AWS_I32, AWS_OPND, AWS_XSUB, AWS_XRANGE,
       AWS_OPND64, AWS_XDIV, AWS_XSUBG, AWS_JTAB, AWS_ST1LO };
enum { AWS_LO, AWS_HI, AWS_NHI, AWS_FRUSTUM, AWS_RASTER, AWS_ORIGIN,
       AWS_NORIGIN };
typedef struct {
    uint32_t rva;        /* in snow.exe                                      */
    uint8_t  kind;       /* F32/F64/I32: the value there, in data or an      */
                         /* instruction's immediate; OPND/OPND64: an abs32   */
                         /* operand redirected to a host float/double;       */
                         /* XSUB: `fld slot; fchs` (t = -x0/dx at a left     */
                         /* plane of 0) becomes a call computing LO - x0;    */
                         /* XSUBG: the same on a float slot of esp, for the  */
                         /* left plane only when a stack dword names it;     */
                         /* XDIV: `fdivrs (r0); movl $0,(r1)` ahead of an    */
                         /* fchs becomes a call leaving (x0 - LO)/dx and     */
                         /* storing LO; XRANGE: `test r,r; jl out; cmp r,w;  */
                         /* jg out` on an integer x becomes one unsigned     */
                         /* test of LO..HI; JTAB: a jump-table entry naming  */
                         /* a case that stores a zero register as the        */
                         /* clamped x, sent to one that stores LO; ST1LO:    */
                         /* `fadds abs32` gains LO on st(1) first            */
    uint8_t  role;
    uint8_t  base;       /* XSUB/XSUBG: the slot's base register, 4 esp or  */
                         /* 5 ebp; XRANGE: the x register; XDIV: r0; JTAB:  */
                         /* the store's pointer register                     */
    uint8_t  disp;       /* XSUB/XSUBG: the slot's disp8, as encoded; XDIV: */
                         /* r1                                               */
    uint32_t expect;     /* the file's bytes there (OPND/OPND64/ST1LO: the   */
                         /* VA the operand names; JTAB: the case's VA)       */
    uint32_t expect_hi;  /* F64: the high dword                              */
    uint8_t  gdisp;      /* XSUBG: the esp disp8 of the dword naming the     */
    uint8_t  gval;       /* plane, and the value that is the left one        */
} arctic_ws_site_t;

typedef struct {
    int native_w, native_h;         /* the picture the game composes      */
    unsigned count;
    const arctic_ws_site_t *site;
} arctic_ws_table_t;

/* [graphics] widescreen_hud = edges: the race HUD is gobj.dll objects, and an
 * object is the HUD when one of these snow.exe calls created it. gobj's three
 * create entries (a sprite, a string, a printf string) are hooked to see who
 * calls them; a glyph sprite belongs to the string that owns it. */
typedef struct {
    uint32_t rva;        /* snow.exe: the call                              */
    uint8_t  slot;       /* which entry: 0 sprite, 1 string, 2 printf string */
    uint8_t  len;        /* the call's length: 6 `call *slot(%reg)`, 2 or 3 */
                         /* `call *(%reg)` through a register holding the    */
                         /* slot's address                                   */
} arctic_hud_site_t;

typedef struct {
    uint32_t table;      /* gobj.dll: the function table                    */
    uint32_t slot[3];    /* each entry's offset in it                       */
    uint32_t target[3];  /* the gobj RVA each entry holds                   */
    uint8_t  words[3];   /* argument dwords each reads (printf: room)       */
    uint32_t draw_ret;   /* after the sprite loop's one vertex-array call   */
    uint8_t  vert_off;   /* a sprite's four vertices                        */
    uint8_t  owner_off;  /* a glyph's string, 0 for a sprite                */
    uint8_t  sprite_x;   /* a sprite's x (its centre), float                */
    uint8_t  string_x;   /* a string's x as placed, float                   */
    unsigned count;
    const arctic_hud_site_t *site;
} arctic_hud_table_t;

/* Check and apply every site to the mapped snow.exe when [graphics]
 * widescreen is on; all or nothing, and a refusal leaves the game 4:3. With
 * widescreen_hud = edges, also hook gobj.dll's create entries (all or
 * nothing again: a refusal leaves the HUD centred). Before binding: the
 * binding asks arctic_ws_bind() for the surface open and the sprite draw.
 * 0 only when a write failed part-way, which the caller must not run. */
int   arctic_ws_arm(HMODULE snow, HMODULE gobj, uint32_t preferred);
/* The host's grSstWinOpen, or its grDrawVertexArrayContiguous with the HUD
 * at the edges, when armed, wrapping `real`; else NULL. */
void *arctic_ws_bind(const char *imp, void *real);
/* The width of the picture the window shows: the raster when armed. The
 * window scale is resolved before arming, against the width asked for. */
int   arctic_ws_picture_width(void);
int   arctic_ws_wanted_width(void);

/* ---- the cabinet's feel, to a pad (arctic_ffb.c) ------------------------- */

/* The cells the game's feedback is made of, as RVAs in snow.exe, and for each
 * the RVA of the abs32 operand that names it, which the host checks names it
 * in the mapped image before reading anything. generated/arctic_ffb.c. */
enum { AFFB_JOLT, AFFB_SHAKE, AFFB_SHAKER_ON, AFFB_PATTERN, AFFB_CELLS };
typedef struct {
    uint32_t cell[AFFB_CELLS];
    uint32_t site[AFFB_CELLS];
    uint32_t boost_pattern;     /* the PATTERN value the game calls BOOST */
} arctic_ffb_cells_t;

/* ---- the select screens' notches (menu_wheel.c) -------------------------- */

/* feedback_set_notching()'s count (0 in a race, 2..32 on a select screen),
 * the notch the feedback thread has the bars in, the -1..1 position it
 * maps, and its hysteresis (a float: the current notch is kept while the
 * position is within HYST notch widths of its centre), as RVAs in snow.exe,
 * each with the RVA of the operand naming it. generated/<id>_menu.c. */
enum { AMENU_COUNT, AMENU_INDEX, AMENU_POS, AMENU_HYST, AMENU_CELLS };
typedef struct {
    uint32_t cell[AMENU_CELLS];
    uint32_t site[AMENU_CELLS];
} arctic_menu_cells_t;
/* Check every site names its cell in the mapped image; 0 leaves the screens
 * untraced. Then one line per change, behind input_trace (menu_wheel.c). */
int  menu_wheel_arctic_arm(HMODULE snow);
/* The servo target and strength the game is driving, for the select
 * screens' trace. */
void menu_wheel_arctic_servo(unsigned target, unsigned strength);
void menu_wheel_arctic_trace(unsigned steer_byte);

/* ---- the cabinet's key (arctic_keylok.c) --------------------------------- */

/* Ultimate Arctic Thunder asks for a KEYLOK II and for PPMON.EXE to be
 * watching while it does; a title that does not is untouched by all of this.
 * src/keylok.h is the key itself. */
void arctic_keylok_arm(HMODULE snow);
/* Nonzero when `n` is the key driver's device and the open was answered. */
int  arctic_keylok_open(LPCSTR n, DWORD access, DWORD share,
                        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                        HANDLE tmpl, HANDLE *out);
/* Nonzero when `h` is the host's key, whose close is answered, not done. */
int  arctic_keylok_close(HANDLE h);
/* Nonzero when the handle is the host's key, and then `*result` is what
 * DeviceIoControl must return. */
int  arctic_keylok_ioctl(HANDLE h, DWORD code, LPVOID in, DWORD nin,
                         LPVOID out, DWORD nout, LPDWORD ret, BOOL *result);
BOOL WINAPI arctic_CreateProcessA(LPCSTR app, LPSTR cmd,
                                  LPSECURITY_ATTRIBUTES pa,
                                  LPSECURITY_ATTRIBUTES ta, BOOL inherit,
                                  DWORD flags, LPVOID env, LPCSTR dir,
                                  LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi);
void arctic_keylok_report(void);

/* ---- the win32 titles (arctic_titles.c) ---------------------------------- */

/* ONE WIN32 TITLE: its images, and the tables scripts/gen-gamedefs.py derived
 * from them. These are per-title DATA exactly as an ETS profile's addresses
 * are, so no source file names a title's own symbol:
 * generated/win32_titles.c is the registry, and the host reaches a table only
 * through the title that owns it.
 *
 * A TABLE THE TITLE DOES NOT HAVE IS 0, AND 0 IS A REAL ANSWER. The generator
 * emits one only when every signature for it matched, so a feature whose table
 * is absent must refuse and leave the game as it is. Reading another title's
 * would be a table of plausible RVAs into the wrong image -- the win32 form of
 * patching address 0. `ids` is the one field every title has. */
typedef struct {
    const arctic_ids_t       *ids;
    const arctic_ws_table_t  *ws;   /* [graphics] widescreen                 */
    const arctic_hud_table_t *hud;  /* widescreen_hud = edges                */
    const arctic_ffb_cells_t *ffb;  /* the cabinet's feel, to a pad          */
    const arctic_menu_cells_t *menu;/* the select screens' notches           */
} arctic_title_t;

/* Every win32 title, 0-terminated, in the order titles.py registers them. */
extern const arctic_title_t *const g_arctic_titles[];   /* generated */

/* The title with this id, or 0 when no win32 title has it -- which is how
 * main.c tells this startup path from the ETS one. */
const arctic_title_t *arctic_title_by_id(const char *id);
/* The title arctic_run() selected, and never 0 once it has: everything below
 * that call reaches its tables through this. */
const arctic_title_t *arctic_title(void);
/* Select it, once, before anything else on that path reads a table. 0 when
 * the id is not a win32 title's. */
int arctic_title_select(const char *id);

/* The running win32 title, as G is the running ETS profile (game.h). Valid
 * only below arctic_run(); every file on that path reads its tables through
 * this rather than naming a title's generated symbol. */
#define AT (arctic_title())

/* Resolve the cells against the mapped snow.exe (0 when one does not check,
 * and the pad then takes nothing). Then, from the shell's I/O thread only:
 * poll() every pass with the seat shaker's byte, tick() every feedback period
 * with the cabinet's Attack button. */
int  arctic_ffb_arm(HMODULE snow, uint32_t preferred);
void arctic_ffb_start(const char *ini);
void arctic_ffb_poll(BYTE shaker);
void arctic_ffb_tick(int attack);
void arctic_ffb_stop(void);

/* Nonzero for a [section] key this title does not use yet: the run resets it
 * and the launcher marks it. */
int arctic_setting_unused(const char *section, const char *key);

/* arctic.c, shared with the operator system's process. */
void  arctic_prepare(char *data, size_t cap);
BYTE *arctic_read_pinned(const char *dir, const arctic_file_t *f, char *path,
                         size_t path_sz, size_t *len);
int   arctic_spawn(int operator_system);
int   arctic_operator_requested(void);
int   arctic_device_open(LPCSTR n, HANDLE *out);
BOOL WINAPI arctic_DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD nin,
                                   LPVOID out, DWORD nout, LPDWORD ret,
                                   LPOVERLAPPED ov);
BOOL WINAPI arctic_GetVersionExA(OSVERSIONINFOA *v);
int  WINAPI arctic_MessageBoxA(HWND w, LPCSTR text, LPCSTR cap, UINT f);

/* arctic_dsound.c: the game's DirectSound routed by [audio] (front bus left,
 * seat bus right). Init runs on the host thread before any binding; bind
 * returns the boundary for a DSOUND.dll ordinal, or NULL for the real one. */
void  arctic_dsound_init(void);
void *arctic_dsound_bind(HMODULE dsound, unsigned ord);

/* arctic_diag.c: the operator system, diag.exe, in a VCThunder.exe process
 * of its own, started by the game on TEST; and the dry run's check of it. */
__attribute__((noreturn)) void arctic_diag_run(void);
int arctic_diag_check(const char *data);

/* arctic_io.c: the cabinet shell (wincsm.exe) inside the process. `game` is
 * nonzero in the game's process, where TEST ends the game for diagnostics. */
void   arctic_io_start(const char *ini, int game);
void   arctic_io_stop(void);
void  *arctic_io_psapi(const char *name);
HANDLE arctic_io_kill_event(void);
int    arctic_io_test_requested(void);

/* The game's folder under \Midway Games and its key under WinCSM\Games. The
 * same for every win32 title: Ultimate installs into Arctic's own folder
 * (its disk holds Midway games\Arctic Thunder). */
#define ARCTIC_CABINET_GAME "Arctic Thunder"

/* arctic_fs.c: the cabinet's folders in this pack, for both processes: the
 * package read-only in data/, every write in save/<id>/. `mode` is an
 * fs_access_t. route() returns the path to use, or NULL when refused; bound()
 * the file boundary for an import, or NULL. */
void        arctic_fs_init(const char *game_data);
const char *arctic_fs_diag_dir(void);
const char *arctic_fs_route(const char *in, char *out, size_t cap, int mode,
                            const char *why);
void       *arctic_fs_bound(const char *dll, const char *name);

/* arctic_reg.c: the cabinet's registry, HKLM\Software\Midway Games only. */
void arctic_reg_init(const char *file, DWORD ioblock);
LONG WINAPI arctic_RegConnectRegistryA(LPCSTR machine, HKEY root, PHKEY out);
LONG WINAPI arctic_RegOpenKeyExA(HKEY root, LPCSTR sub, DWORD opt, REGSAM sam,
                                 PHKEY out);
LONG WINAPI arctic_RegCreateKeyExA(HKEY root, LPCSTR sub, DWORD res, LPSTR cls,
                                   DWORD opt, REGSAM sam,
                                   LPSECURITY_ATTRIBUTES sa, PHKEY out,
                                   LPDWORD disp);
LONG WINAPI arctic_RegCloseKey(HKEY h);
LONG WINAPI arctic_RegFlushKey(HKEY h);
LONG WINAPI arctic_RegQueryValueExA(HKEY h, LPCSTR name, LPDWORD reserved,
                                    LPDWORD type, LPBYTE data, LPDWORD len);
LONG WINAPI arctic_RegSetValueExA(HKEY h, LPCSTR name, DWORD reserved,
                                  DWORD type, const BYTE *data, DWORD len);
LONG WINAPI arctic_RegEnumKeyA(HKEY h, DWORD index, LPSTR out, DWORD cap);
LONG WINAPI arctic_RegDeleteKeyA(HKEY root, LPCSTR sub);

#endif
