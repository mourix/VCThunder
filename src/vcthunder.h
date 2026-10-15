/* vcthunder.h -- common declarations for the native Windows host.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Runtime profiles provide all title-specific addresses; callers must reject
 * absent anchors.
 */
#ifndef SHIM_H
#define SHIM_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#include "game.h"

/* ---------------------------------------------------------------- log.c: */

typedef enum { LOG_ERR = 0, LOG_WARN, LOG_INFO, LOG_TRACE } log_level_t;

extern log_level_t g_log_level;

void log_open(const char *path, log_level_t level);
void log_close(void);
/* Reroute a release build's compiled-out printf into this log. See log.c. */
int  log_install_game_printf(void);
void log_flush_game_printf(void);
void log_printf(log_level_t level, const char *fmt, ...);

#define LOGE(...) log_printf(LOG_ERR,   __VA_ARGS__)
#define LOGW(...) log_printf(LOG_WARN,  __VA_ARGS__)
#define LOGI(...) log_printf(LOG_INFO,  __VA_ARGS__)
#define LOGT(...) do { if (g_log_level >= LOG_TRACE) log_printf(LOG_TRACE, __VA_ARGS__); } while (0)

/* ------------------------------------------------------------- loader.c: */

/* Reserve VA 0x100000..0x71d830 and copy CODE into it. The region is
 * PAGE_EXECUTE_READWRITE on purpose: the __imp__ table and the __pl_unpackrom
 * record stream both live *inside* CODE, so it is simultaneously code, data and
 * a patch target. */
int  game_map(const char *exe_path);
void game_unmap(void);

/* Call __pl_unpackrom (0x19396c). It expands DATA + .bss out of the record
 * stream in CODE. Returns the game's own status: 0 == success. */
int  game_unpack(void);

/* Call _mainCRTStartup (0x1d9c68) -> __cinit -> _main. Does not return
 * under normal operation.
 *
 * We deliberately do NOT enter at __p_start (0x193190): it writes fs:[4] and
 * fs:[8], which on Win32 are the TEB's StackBase/StackLimit, and clobbering
 * them breaks the host thread. Its other two jobs (__pl_unpackrom and
 * __p_EtsInitSubsystems) are done by game_unpack() and the shim's own init. */
void game_run(void);

/* Record and report each thread observed executing game code. */
void     game_thread_seen(const char *where);
/* The same, for a boundary reached from the game's own LOGIC rather than from
 * one the shim chose to call: the game's own printf, and nothing else. That
 * distinction is the whole value of the census: a swap or an I/O callback
 * says which thread the shim asked, while a printf says which thread the game
 * is. Only log.c may call this. */
void     game_thread_logic_seen(const char *where);
/* The thread the game's own logic has been observed on, or 0 if the title has
 * no printf anchor and it was therefore never observed. */
DWORD    game_thread_logic(void);
/* One line per thread seen: id, crossings so far, and where it was first seen.
 * A thread that handed off and parked shows a crossing count that stops. */
void     game_thread_report(void);
/* How many distinct threads have been seen running game code so far. */
unsigned game_thread_count(void);
/* The id of thread `i` (0-based, in first-seen order), or 0. */
DWORD    game_thread_id(unsigned i);

/* --------------------------------------------------------------- main.c: */

/* The game's own `cli`/`sti`, emulated rather than healed to nops. See the long
 * comment on heal_privileged() for why, and what it costs.
 *
 * Anything standing in for one of the cabinet's interrupt sources (the DCS
 * audio tick, the I/O poll thread on a title whose frame boundary is not the
 * seam) must call game_irq_wait() before it runs game code. That is what
 * `cli` meant. */
int  game_irq_disabled(void);
/* Shared around game code on a thread other than the game's own; exclusive
 * while a diagnostic replays game code and restores its memory. */
void game_side_thread_enter(void);
void game_side_thread_leave(void);
void game_hold_side_threads(int on);
/* Wait up to max_us for the game to leave its critical section. Returns 1 if it
 * did, 0 if the caller should proceed anyway, which is counted and reported,
 * because a stalled audio tick is worse than a rare unprotected one. */
int  game_irq_wait(unsigned max_us);
/* A point the game cannot legally be inside a critical section at. Forces the
 * depth back to zero and counts it, so a region that never reaches its `sti`
 * costs one frame rather than the rest of the run. Call only from the game's
 * own thread. */
void game_irq_resync(const char *where);
/* The cli depth, and a return to one read earlier, for a replay of game code
 * abandoned past its `sti`. Game thread only, with side threads held. */
int  game_irq_depth(void);
void game_irq_unwind(int depth);
/* Per-site fault census, into the periodic report. */
void game_irq_report(void);

/* --------------------------------------------------------------- diag.c: */

/* The vocabulary the instruments are written in: a read that cannot fault, an
 * address that names itself, and a timestamp. See src/diag.c for why each one
 * is here rather than in the file that first needed it. */

/* One dword from this process, or 0 and nothing written. Never faults. */
int  diag_peek32(uint32_t addr, uint32_t *out);
/* "0xADDR(module+off)", or "(unsampled)" for 0. Returns what snprintf did. */
int  diag_describe_addr(char *out, size_t cap, uint32_t addr);
/* Inside the mapped game image? */
int  diag_in_game(uint32_t addr);
/* A validated frame-pointer walk; returns how many links it recovered. */
unsigned diag_return_chain(uint32_t ebp, uint32_t *out, unsigned max);

/* The `cap` largest values of a counter array, descending, for a per-interval
 * report. `n` is how many are held so far and the return is the new count, so
 * one call per counter is the whole loop. A zero value is never inserted. */
typedef struct {
    unsigned idx;
    uint64_t value;
} diag_top_t;

unsigned diag_top_insert(diag_top_t *top, unsigned n, unsigned cap,
                         unsigned idx, uint64_t value);

/* The RAW counter, not the shim's synthetic timebase: this measures the host
 * core, and timebase.c deliberately lies about the host core to the game.
 *
 * The fenced form is the one required around a SHORT call: two bare
 * reads sit in the same out-of-order window and retire at nearly the same
 * value, which is a wrong answer rather than an imprecise one. It honours
 * [diagnostics] time_fence so the A/B that established that stays reproducible.
 * Inline because one caller is per-triangle. */
static inline uint64_t diag_rdtsc(void)
{
    uint32_t lo, hi;

    __asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Not inline: it reads [diagnostics] time_fence, and g_cfg is declared at the
 * bottom of this header. It brackets a call rather than a triangle, so the
 * call it costs is not on any path that could notice. */
uint64_t diag_rdtsc_fenced(void);

/* -------------------------------------------------------------- patch.c: */

void  patch_init(void);
void  patch_shutdown(void);

/* E9 rel32 at va. */
void  patch_jmp(uint32_t va, const void *target);

/* Overwrite an __imp__ pointer slot. */
void  patch_slot(uint32_t slot_va, const void *target);

/* mov eax, imm32 ; ret: stack-safe for cdecl callees (caller cleans up). */
void  patch_ret_imm(uint32_t va, uint32_t eax_value);

/* Rewrite the rel32 of ONE existing `call` so a single call site is redirected
 * and the callee is left byte-identical. Refuses, loudly, if the byte at
 * site_va is not 0xE8. Returns nonzero on success. */
int   patch_call(uint32_t site_va, const void *target);
/* Rewrite ONE `call dword ptr [cell_va]` (FF 15, six bytes) into a direct
 * call to target and a nop. Refuses unless those exact bytes are there. */
int   patch_call_indirect(uint32_t site_va, uint32_t cell_va, const void *target);

/* Build a trampoline that logs the call by name, then returns eax_value.
 * Emitted as cdecl, so it is stack-safe regardless of the callee's arity. */
void *stub_logging(const char *name, uint32_t eax_value);

/* `prologue` is `len | (reloc_off << 8)` and comes from the generated profile:
 * never a literal, because a prologue's LENGTH is per-title data exactly as its
 * address is. See patch.c. */
/* Detour `va` to `replacement`, returning a callable original trampoline. */
void *hook_call_through(uint32_t va, const void *replacement, uint32_t prologue);
/* Build that trampoline without changing game memory. Installers use this in
 * preflight so all fallible work finishes before their first patch commits. */
void *hook_prepare_call_through(uint32_t va, uint32_t prologue);

/* Count invocations of a function of UNKNOWN signature: two instructions
 * (`inc [counter]` / `jmp trampoline`) in front of the original, touching no
 * stack and no register, so arity and calling convention do not matter. */
int patch_count_calls(uint32_t va, uint32_t *counter, int prologue,
                      const char *name);

/* Count calls through a stdcall entry point without changing anything else. */
void *thunk_count_only(const void *target, uint32_t *counter);

/* Report the busiest Win32 entry points, calls per frame. */
void win32_report_calls(uint64_t frames);
/* PAGE_NOCACHE/WRITECOMBINE stripped from the game's own VM requests. */
void win32_report_nocache(void);
/* The game's own memory, as it asked for it: its live VirtualAlloc
 * reservations (copied out, at most `max`) and its HeapCreate heaps (a NULL
 * slot is one it destroyed). NULL while every list holds everything; once one
 * overflowed, what overflowed. */
unsigned win32_game_vallocs(uintptr_t *base, size_t *size,
                            unsigned char *watched, unsigned max);
/* Changes with every VM call the game makes; equal twice = the same VM. */
uint32_t win32_game_vgen(void);
/* Reserve the game's new VM with MEM_WRITE_WATCH (`watched` above). */
void     win32_write_watch(int on);
unsigned win32_game_heaps(void);
void    *win32_game_heap(unsigned i);
const char *win32_game_memory_lost(void);
/* Hold the game's frees while a diagnostic will restore its memory. */
void     win32_hold_frees(int on);
/* The CRITICAL_SECTIONs the game initialised, which a restore must not touch. */
unsigned win32_game_css(void);
void    *win32_game_cs(unsigned i);
/* Drop this thread's voice commands while a diagnostic replays game code. */
void     audio_hold_this_thread(int on);
/* This thread's nesting of the DCS mask, and a release back down to a depth
 * read earlier: for a replay abandoned past its unmask. */
int      audio_mask_depth(void);
void     audio_mask_unwind(int depth);
unsigned audio_held_commands(void);   /* dropped since the last call */
/* This thread is held: drop the voice command it is making, and count it. */
int      audio_held(void);
/* drawreplay.c: in-between pictures (frame_rate = 60), and the replay check. */
int      drawreplay_install(unsigned samples, unsigned skip, int control,
                            int interp, int interp_state);
void     drawreplay_swapped(uint64_t before_qpc, uint64_t after_qpc);
/* After the loop's swap, sleep to just before the tick's deadline. */
void     drawreplay_idle(int interval);
/* Wall ms slept so far; the latest wake past its target and the wakes past
 * the deadline, since the last call. */
double   drawreplay_idle_ms(double *latest_ms, unsigned *past);

/* Per-call timing for functions of unknown signature. Values include callees;
 * instrumentation is restricted to one configured thread. */
#define CALL_TIMER_DEPTH 32

/* A completed call at or below this many cycles counts as FAST.
 *
 * The threshold exists because a MEAN cannot tell "every call is expensive"
 * from "most calls are cheap and a few are enormous", and those two need
 * opposite fixes. Set just above the instrument's own tight-loop cost (~40) plus
 * a couple of L2 hits, so a call that found its stubs warm lands under it and a
 * call that took a memory stall does not. */
#define CALL_TIMER_FAST 128u

struct call_timer {
    /* Hand-assembled code writes every field below by absolute address; the
     * offsets are taken with &, never assumed, so the layout is free to move. */
    uint64_t    cycles;                     /* accumulated inclusive cycles   */
    uint32_t    calls;                      /* completed calls               */
    uint32_t    depth;                      /* live nesting; 0 when balanced  */
    uint32_t    overflow;                   /* calls dropped past the ceiling */
    /* The distribution, not just its mean. See the report in profile.c: an
     * instrument that adds a fixed tax per call moves the mean and leaves the
     * MINIMUM alone, while code that is genuinely stalling moves both. */
    uint32_t    min;                        /* cheapest completed call        */
    uint32_t    fast;                       /* completed under CALL_TIMER_FAST*/
    /* Restrict the LIFO timer state to its owning thread. */
    uint32_t    owner;                      /* thread id allowed to measure   */
    uint32_t    offthread;                  /* entries declined, wrong thread */
    uint64_t    t0[CALL_TIMER_DEPTH];       /* entry stamp, per nesting level */
    uint32_t    ret[CALL_TIMER_DEPTH];      /* displaced return address       */
    const char *name;
};

int patch_time_calls(uint32_t va, struct call_timer *t, int prologue,
                     const char *name);

/* Hand every installed timer to `tid`. Called from the render thread once it is
 * known: installation happens on the loader thread, which for Hydro is the
 * same thread and for Offroad is not. */
void patch_timer_set_owner(uint32_t tid);

/* Clear the accumulators, including the min (which is not zero when empty). */
void patch_timer_reset(struct call_timer *t);

/* Emit `fld dword [esp+4]; fsqrt; ret` into the thunk pool and return its
 * address, so the canary can be instrumented and called by US at a chosen
 * cadence and depth without touching the population the game is filling.
 * Byte-identical to _xmath_sqrt in both titles. */
void *patch_emit_sqrt(void);

/* Cycles the instrumentation itself adds to one call, measured rather than
 * assumed, by timing a synthetic 6-byte function with and without the hook.
 * Subtract it from every figure patch_time_calls() produces. */
uint64_t patch_timer_overhead(void);

/* The same calibration re-run during gameplay, so "the instrument is slow" and
 * "everything in this process is slow" can be told apart from "reaching the
 * game's own code is slow". See the comment on the definition. */
uint64_t patch_timer_recalibrate(void);

/* Per-entry Glide timing distribution. Minimum and fast-call counts distinguish
 * normal call cost from periodic spikes hidden by an aggregate mean. */
#define GLIDE_FAST_CYCLES 1024u

struct glide_timer {
    uint64_t cycles;                    /* accumulated, all calls              */
    uint32_t min;                       /* cheapest call (0xFFFFFFFF = none)   */
    uint32_t fast;                      /* calls under GLIDE_FAST_CYCLES       */
    /* log2 histogram, bucket n = calls costing 2^n .. 2^(n+1)-1 cycles.
     *
     * Two counters can say "min 180, mean 25,652, 19% under 1024" and still
     * leave the two candidate shapes open: most calls dear at ~2,000 with a long
     * tail, or a genuinely bimodal population sitting at 180 and 30,000. Those
     * are different defects. `bsr` gives the bucket in one instruction. */
    uint32_t hist[32];
};

/* Called with a pointer to the caller's argument dwords, once per entry point.
 * seen_flag is REQUIRED alongside it; see the block in
 * thunk_cdecl_to_stdcall(), where emitting the call ungated is shown to break
 * __pl_unpackrom. To be called on every call, write the flag back to 0 from
 * inside the callback. */
typedef void (__cdecl *glide_first_fn)(uint32_t cookie, const uint32_t *args);

/* Build a cdecl -> stdcall thunk. The game's Glide is cdecl; a Glide DLL is
 * stdcall. argbytes comes from G->glide[] and must be a multiple of 4.
 * on_first/cookie/seen_flag are optional (pass NULL/0/NULL to omit). */
void *thunk_cdecl_to_stdcall(const void *target, int argbytes, const char *name,
                             uint32_t *counter, struct glide_timer *timer,
                             glide_first_fn on_first, uint32_t cookie,
                             uint32_t *seen_flag);

/* Observe the asset names the game resolves out of its R2 archive. The
 * detours answer no lookup themselves; see src/r2_name.c. */
int  r2_name_install(void);

/* The most recent asset name the game resolved: the only thing in the
 * process that can put a NAME on a texture upload. See texstate.c. */
const char *r2_last_lookup(void);

/* Cycles for the Glide entry points texstate.c claims away from the thunk. */
void texstate_report_cycles(uint64_t frames);

/* Called by the watchdog: who owns s_dcs_cs, is the DCS tick still running, and
 * where is the DCS thread stuck. The render thread's block names the object but
 * never the holder; see audio.c. */
void audio_report_dcs_state(void);

/* --------------------------------------------------------- win32_bind.c: */

/* Rewrite the 98 __imp__ slots and jmp-patch the 25 direct entries onto real
 * kernel32/user32/ws2_32/ntdll. Anything that fails to resolve is left as ETS's
 * own implementation and logged loudly: a safe fallback, not a silent one.
 * Returns the number of entry points successfully bound. */
int  win32_bind(void);

/* Load every DLL we will need. MUST run before game_map(), because mapping the
 * game destroys this process's PE headers and LoadLibrary stops working. */
int  win32_preload(void);

/* Force the arcade link into its standalone state, which is what happens when
 * [link] link = off: without a synthetic ether0, enabled-but-uninitialised
 * fails _gamenet_ModuleInit. See win32_bind.c, and net_link_install() below
 * for the other half of the choice. */
int  net_link_disable(void);

/* ------------------------------------------------------------ net_link.c:
 *
 * The arcade link: a synthetic ether0 over three ETS calls, and four Winsock
 * entry points this host already owns. Both titles speak ordinary IPv4 UDP
 * above that boundary, so nothing here carries a packet itself.
 *
 *   net_link_preload()  before game_map(), with win32_preload(): resolves the
 *                       real ws2_32 entries our overrides call through and
 *                       reads this machine's adapter address.
 *   net_link_install()  after stubs_install_m1(): patches the three ETS
 *                       device calls. Not called when the link is off.
 *   net_link_frame()    the frame boundary; resets the receive budget.
 *   net_link_report()   into the periodic report.
 *
 * The four socket overrides are installed by win32_bind()'s OVERRIDE table,
 * like every other entry point that needs shim behaviour, and pass straight
 * through when the link is off. */
int  net_link_preload(void);
int  net_link_install(void);
void net_link_frame(void);
void net_link_report(unsigned frames);
int  net_link_enabled(void);

/* The four Winsock entry points this module owns, looked up by undecorated
 * name; NULL means it does not own that one and the real ws2_32 export binds
 * as usual. win32_bind()'s resolver asks after its own table, so the binding
 * is still reported and counted in exactly one place. It is a function rather
 * than four declarations so that the winsock types stay inside net_link.c:
 * <winsock2.h> has to precede <windows.h> and this header is included by
 * thirty files that do not care. */
void *net_link_override(const char *base);

/* ---------------------------------------------------------------- cksum.c:
 *
 * The CODE checksum the link uses as its compatibility gate: measured against
 * the value derived statically from the image, and then answered with it,
 * because this host's own patches move the sum the game computes and a peer
 * rejects what it does not recognise. The call sites are an ordering contract,
 * not a preference:
 *
 *   cksum_snapshot()        after game_map(), BEFORE the first patch
 *   cksum_report_pristine() after game_unpack(), when the range table exists
 *   cksum_install_shim()    after that: the pristine value is what it answers
 *   cksum_report_live()     immediately before game_run(), all patches in
 *   cksum_frame()           from the frame boundary, until the cells fill
 */
void cksum_snapshot(void);
void cksum_report_pristine(void);
void cksum_install_shim(void);
void cksum_report_live(void);
void cksum_frame(void);

/* ------------------------------------------------------------- fs_map.c: */

/* Arcade paths (C:\HT.R2, D:\*.CSH, C:\htlowres.bin) -> portable-pack folders. */
void fs_map_init(const char *data_dir, const char *save_dir);

/* Which directory a path resolves to is decided by what the game is about to
 * do with it, not by its name: whatever it opens for writing is the game's and
 * lives in save/, whatever it opens for reading is the dump's unless the game
 * has written its own. That is what makes "the dump is never written to" true
 * for BOTH titles, and it is what lets two instances of one pack have separate
 * operator settings, which a name list could not give. See src/fs_map.c. */
typedef enum { FS_READ = 0, FS_WRITE, FS_DELETE } fs_access_t;

const char *fs_map_path_mode(const char *in, char *out, size_t out_sz,
                             fs_access_t mode);
/* FS_READ, for the callers that never write. */
const char *fs_map_path(const char *in, char *out, size_t out_sz);

/* Resolve `rel` under `base` in the case the FILESYSTEM uses, component by
 * component. Writes `out` and returns 1 only when every component resolved, so
 * a caller that fails keeps its own path and its own error message.
 *
 * Callers try the exact path FIRST and come here only on a miss; see fs_map.c
 * for why a data directory's case need not be the case the game asks for. */
int fs_resolve_ci(const char *base, const char *rel, char *out, size_t out_sz);

/* -------------------------------------------------------------- stubs.c: */

/* Milestone 1: stub every boundary that would execute a privileged instruction.
 * Without this the shim faults inside _main within milliseconds; the first
 * four things _main touches (_pcspeaker_ToggleSpeaker, _diegoio_comm_Init...,
 * _phide_set_deviceNum, _init3dfx_Init) all reach hardware. */
int  stubs_install_m1(void);

/* --------------------------------------------------------- glide_bind.c: */

/* Load the bundled vcglide.dll. MUST run before game_map(), like
 * win32_preload(). There is no alternate runtime provider. */
int  glide_preload(void);

/* Redirect all 66 _gr* entry points at the wrapper through cdecl->stdcall
 * thunks. Run AFTER stubs_install_m1(), which stubs them as a fallback. */
int  glide_bind(void);

/* Arm the hang watchdog on the CALLING thread, which must be the one about to
 * run the game. Call it immediately before game_run(): armed here it also
 * covers the stretch before the first frame, which is where a port of a new
 * title spends its first several attempts. */
void glide_watchdog_arm(void);

/* -------------------------------------------------------------- nvram.c: */

/* Back the cabinet's non-volatile block with save/htlowres.bin. Without it the
 * game warns that operator settings will not be saved, and they are not. */
int  nvram_install(const char *save_dir);
void nvram_flush(void);

/* -------------------------------------------------- image_probe.c: */

/* Does overwriting our own image below the game actually leave this process
 * working? Preload before game_map(); run after everything is installed.
 * Cases that cannot run report NOT RUN and are never counted as passes. */
void image_probe_preload(void);
void image_probe_run(void);
/* Its own entry point, and its own flag, because a refusal terminates the
 * process rather than falling through to the next frame. */
void image_probe_refusal(void);

/* -------------------------------------------------------------- diego.c: */

/* Parse host-control mappings and preload XInput/WinMM/DirectInput. MUST run
 * before game_map(), because loading another DLL is unsafe after the game
 * replaces the host image headers. Missing analogue APIs are non-fatal:
 * keyboard input remains available. */
int  diego_preload(const char *ini);

/* Emulate the selected cabinet I/O board: Hydro's Diego or Offroad's MagicBus
 * successor. Synthesise the title's packet format, feed the game's own decoder,
 * and restore the comm-layer callback scheduler that transfers decoded values
 * into the game's control-driver state. Starts a poll thread. */
int  diego_install(unsigned dips, unsigned period_ms);

/* Run the board's decoder and the game's control-driver callback, ON THE GAME
 * THREAD. Called from the buffer-swap hook, which is the frame boundary and the
 * only seam this project owns inside the game's own thread. It is not a
 * convenience: the game guards this code against its main loop with cli/sti,
 * main.c heals those to nops, and dispatching it from the poll thread therefore
 * raced the attract sequence into an access violation. */
void diego_frame_boundary(void);
void diego_shutdown(void);

/* ------------------------------------------------------------- dinput.c: */

/* A DirectInput reading, already resolved against the ini's axis map: the same
 * shape diego.c gets from XInput and WinMM, so the board packer above it never
 * learns which provider it came from. */
typedef struct {
    int      present;
    float    steer;      /* -1 full left .. +1 full right         */
    float    drive;      /* -1 full brake .. +1 full throttle     */
    /* bit 0 == button 1, per open device: row 0 is the default device, the
      * rest are the extra slots. io.h owns the accessors and constants;
      * io_reading_width_check holds these literal widths to them. */
    uint32_t buttons[8][4];
    int left, right, up, down;   /* POV hat 0, as digital overrides */
} di_reading_t;

/* Open the configured wheel and, if it has a motor and [ffb] enabled is true,
 * take exclusive access and create the constant-force effect. Same ordering
 * rule as diego_preload(): before game_map(). Returns 0 when there is no
 * DirectInput controller, which is not an error: XInput, WinMM and the
 * keyboard are all still there. */
int  di_preload(const char *ini);
int  di_present(void);
/* Did the opened device publish force-feedback effects? The host's only
 * wheel/pad signal; see the note at its definition. */
int  di_publishes_ffb(void);
const char *di_device_name(void);
int  di_sample(di_reading_t *out);

/* ---- the slots ----------------------------------------------------------
 *
 * Slot 0 is the default device, which owns the motor. The rest are the
 * controllers a [devices] table named, read-only, in the table's order, for
 * their buttons and any axis role that names them.
 *
 * Which slot a [devices] label ended up in, or -1 when it opened nothing. */
int  di_slot_for_label(const char *label);
/* How many slots are open, 0 when there is no DirectInput device at all. */
unsigned di_slot_count(void);
/* The product name of one slot, or "" for a slot that is not open. */
const char *di_slot_name(unsigned slot);

/* Command the wheel's motor. -1..+1, the cabinet's own DAC value rescaled;
 * 0 is no force, not "release the wheel". Cheap to call at the board's poll
 * rate: an unchanged magnitude does not reach the driver. */
void di_set_force(float force);

/* Peak |magnitude| handed to DirectInput since the last call, 0..10000, and
 * reset by reading. 10000 is the API's full scale. */
long di_peak_force(void);
void di_close(void);

/* --------------------------------------------------------- input_probe.c: */

#define INPUT_PROBE_MAX_DEVICES 24
#define INPUT_PROBE_MAX_AXES     8

typedef enum {
    INPUT_PROBE_DINPUT,
    INPUT_PROBE_XINPUT,
    INPUT_PROBE_WINMM
} input_probe_backend_t;

/* ini.h's INI_MATCH_CAP, spelled here because ini.h includes this file. */
#define INPUT_PROBE_ID_CAP (MAX_PATH + 96)

typedef struct {
    input_probe_backend_t backend;
    unsigned backend_index;
    char name[MAX_PATH];
    /* DirectInput only: the device ID and the name it starts with, built as
     * dinput.c builds them. */
    char id[INPUT_PROBE_ID_CAP];
    char id_name[MAX_PATH];
    unsigned axis_count;
    unsigned button_count;
    int force_feedback;
} input_probe_device_t;

typedef struct {
    float axis[INPUT_PROBE_MAX_AXES];
    const char *axis_name[INPUT_PROBE_MAX_AXES];
    unsigned axis_count;
    /* The same 128 the cabinet path carries, for the same reason: the launcher
     * must be able to show, and bind, a button the game can then read. */
    uint32_t buttons[4];
    int pov;                    /* -1 centred, otherwise hundredths of degrees */
} input_probe_state_t;

/* THE SELECTED DEVICE'S AXES, independent of any poll. The launcher lays its
 * rows out from this and fills them from input_probe_poll(), whose axis[] is
 * in exactly this order, one list, read twice, rather than two walks of one
 * rule. `label` is the DRIVER's name for the axis ("Throttle", "Wheel axis")
 * and is NULL where the backend publishes none, which is every WinMM device:
 * JOYCAPS carries ranges and capability bits and not one name. */
unsigned input_probe_axis_count(void);
const char *input_probe_axis_name(unsigned index);
const char *input_probe_axis_label(unsigned index);
/* Any device's axes, by its index in the list, in that device's own names. A
 * DirectInput device reports none until it has been opened. */
unsigned input_probe_device_axis_count(unsigned device);
const char *input_probe_device_axis_name(unsigned device, unsigned index);
const char *input_probe_device_axis_label(unsigned device, unsigned index);
int input_probe_start(HWND owner);
unsigned input_probe_count(void);
const input_probe_device_t *input_probe_device(unsigned index);
int input_probe_select(unsigned index);
int input_probe_poll(input_probe_state_t *state);
/* One controller by index, selected or not, so a binding or an axis can be
 * captured from whatever the user moves. Buttons, hat and axes, each axis
 * under that device's own name. */
int input_probe_poll_device(unsigned index, input_probe_state_t *state);
/* Run a short, bounded motor/rumble pulse on the selected launcher device.
 * DirectInput is made exclusive only for the duration of the test and is
 * returned to non-exclusive foreground polling before gameplay starts. */
int input_probe_test_force(void);
void input_probe_stop(void);

/* -------------------------------------------------------------- audio.c: */

/* Open the default render endpoint, negotiate a format and start the mixer.
 * MUST run before game_map(), for the same reason as win32_preload(): WASAPI is
 * COM and loads its endpoint stack lazily. Returns 0 if there is no usable
 * device; Offroad then uses a host-corrected silent path whose cookies retire
 * immediately instead of remaining "playing" forever. */
int  audio_preload(unsigned dcs_us, unsigned volume_percent,
                   const char *routing, unsigned balance);
int  audio_ready(void);

/* Route the cabinets' two physical audio buses to a host stereo endpoint using
 * [audio] output and [audio] balance: front is the bridged front-speaker mono
 * bus, seat is the seat-woofer bus. Shared by both title-specific mixers. */
void audio_route_buses(float front, float seat, float *left, float *right);

/* Install the selected title's audio backend. Hydro replaces
 * _audio_dspcmd_*; Offroad replaces the 11-entry Audiolib YMF jump table. Run
 * AFTER stubs_install_m1(), which stubs raw hardware as a fallback. */
int  audio_install(void);
/* Offroad's silent path for a missing/disabled endpoint, corrected so natural
 * completion cannot strand sample references. After game_unpack(): it writes a
 * DATA cell. See audio.c. */
int  audio_install_passthrough(void);
void audio_shutdown(void);

/* ------------------------------------------------------ audio_offroad.c: */

int  audio_offroad_install(void);
int  audio_offroad_active(void);
void audio_offroad_mix(float *stereo, uint32_t frames, uint32_t output_rate,
                        float user_gain);
void audio_offroad_census(unsigned starved);
void audio_offroad_shutdown(void);
int  audio_offroad_install_silent_completion(void);

/* ----------------------------------------------------------- texstate.c: */

/* Records the SET of Glide states triangles are actually drawn with, including
 * the colour side nobody has measured, and whether each sourced texture address
 * was ever uploaded. Diagnostic for the white arrow-ramp bases and signs. */
int  texstate_claim(const char *name, void *real, uint32_t va);
void texstate_frame_end(void);
void texstate_report(void);

/* The LOD/mip census: grTexMipMapMode's mode+lodBlend and grTexDownloadMipMap's
 * evenOdd, neither of which any thunk logged. Settles whether Hydro drives
 * Voodoo2 trilinear by SPLITTING the mip chain across the two TMUs. */
void texstate_report_lod(uint64_t frames);

/* ------------------------------------------------------------ profile.c: */

/* Statistical sampler over the render thread. Diagnostic only; addresses are
 * reported raw and resolved with scripts/p1-symbols.py --lookup. */
int  profile_install(unsigned period_ms);
void profile_report(void);
/* Per-frame geometry throughput; call with the frames in the interval. */
void profile_report_geometry(unsigned frames);
void profile_report_call_times(unsigned frames);

/* The interleaved control: one instrumented canary call per submitted triangle,
 * from the render thread, at the game's own cadence. Called from the
 * grDrawTriangle wrapper when time_calls is on; a no-op otherwise. */
void profile_interleave_tick(void);
/* Read-only per-swap poll of the attract stage; logs each transition. */
void profile_attract_poll(void);
void profile_shutdown(void);

/* ----------------------------------------------------------- timebase.c: */

/* Make a modern CPU look like the cabinet's 333 MHz Celeron. Without this the
 * game hangs in _timer_Sleep: its own CPU probe leaves the seconds-per-tick
 * scale at 0.0, so no deadline can ever be met. */
void timebase_init(uint32_t target_hz, unsigned speed_percent);
int  timebase_install(void);

/* The game's own clock. Difference the ticks, then divide by the rate; see
 * the comment at the definition for why seconds are not offered. */
uint64_t timebase_ticks(void);
uint32_t timebase_hz(void);

/* Stop the game's clock while something outside the game stops its thread:
 * a size/move or menu loop inside DefWindowProc. The frozen span is removed
 * from the epoch on release, so the game is handed one ordinary frame instead
 * of a frame that lasted as long as the interruption. Nests; must be balanced.
 * timebase_paused() is for instruments that would otherwise call a deliberate
 * pause a hang. */
void timebase_pause(int on);
int  timebase_paused(void);

/* ------------------------------------------------------------- window.c: */

/* The backend's window is the game's window: Hydro passes grSstWinOpen a null
 * HWND and never learns which one it got. Everything about its size, its mode
 * and Alt+Enter is decided on this side of the Glide boundary. */
HWND window_game(void);
void window_set_hwnd(HWND hwnd);

/* The game's VISIBLE raster, which with a 400-line raster is 512x400 inside a
 * 640x400 Glide surface. vcglide crops to the raster, so the raster is the only
 * rectangle the window needs.
 * Applies the configured mode, so this is also what puts the pack into
 * fullscreen at startup: it is the first moment a window exists. */
void window_geometry(int view_w, int view_h);
/* Bezel art is active: size the window to the art's shape, with its screen
 * hole as tall as the raster at the window scale. 0 for any value clears it. */
void window_set_bezel(int art_w, int art_h, int hole_h);

/* Re-apply the configured geometry. force=1 after a game mode request;
 * force=0 is the bounded per-swap re-apply after one (written for a provider
 * that reset the mode asynchronously, kept as a general watch), which deliberately
 * leaves a normal manual resize alone. */
void window_sync(int force);

void window_set_fullscreen(int on);
int  window_is_fullscreen(void);
void window_hotkeys(void);          /* Alt+Enter; call once per frame */

int  window_gone(void);             /* no visible window, twice running */
int  window_input_focused(void);    /* the [controls] input_background policy */

/* `aspect` names an output shape. Defined in window.c, which is the file that
 * turns one into a client rectangle; config.c uses them to settle `aspect`
 * against `widescreen`, so the two cannot disagree about a forced ratio. */
int  aspect_is_4_3(const char *aspect);
int  aspect_is_16_9(const char *aspect);

/* --------------------------------------------------------------- main.c: */

typedef enum {
    LAUNCHER_CANCEL,
    LAUNCHER_HYDRO,
    LAUNCHER_OFFROAD
} launcher_result_t;

/* Run before any game image is mapped. It owns no state after returning. */
launcher_result_t launcher_run(const char *ini_path);
/* A CHD to start importing as the launcher opens: one dropped on the exe. */
void launcher_queue_import(const wchar_t *file);

/* The DLL's entry point, called by VCThunder.exe (src/stub.c). Never
 * returns: its return address is inside memory the game takes over. */
__declspec(dllexport) int shim_run(void);

/* Orderly process exit from anywhere, including from inside game code: flush
 * NVRAM, stop the mixer and I/O threads, close the log, then ExitProcess.
 *
 * The arcade binary has no quit path (the cabinet was switched off at the
 * mains), so `reason` is always a host event (Escape or a window close).
 * Never returns; a second caller blocks until the first one finishes. */
void shim_exit(int code, const char *reason);

/* ------------------------------------------------------------- config.c: */

typedef struct {
    /* Set from the command line ONLY: `VCThunder.exe offroad`. The ini does
     * not carry it: one pack holds both titles, so a file that chose the game
     * would make every other key in it conditional on a key above it. */
    char title[16];
    char exe[MAX_PATH];
    char data_dir[MAX_PATH];
    char save_dir[MAX_PATH];
    char log_file[MAX_PATH];
    log_level_t log_level;
    unsigned cpu_hz;      /* synthetic CPU rate the game is told it has */
    unsigned speed_percent;/* virtual game-clock rate; 100 = real time  */
    int      limiter_idle; /* sleep the tick's idle instead of spinning */
    unsigned dip_switches;/* cabinet DIP byte reported by the I/O board  */
    unsigned io_ms;       /* I/O board poll period                       */
    char io_backend[16];  /* only `sim` is implemented; `serial` is not yet */
    char io_port[32];     /* the serial port a real board will be on        */
    int  input_background;/* sample cabinet keys even when console is foreground */
    int  lfb_shadow;      /* cached shadow for the software 2D blitter */
    int  sse_ftz;         /* FTZ+DAZ on the render thread; measured inert   */
    unsigned window_scale;/* integer host-window scale; 0 = largest that fits */
    unsigned render_scale;/* wrapper's internal render multiplier; 1 = native */
    int  fullscreen;      /* start borderless-fullscreen; Alt+Enter toggles   */
    char aspect[16];      /* raster | 4:3 | 16:9 | stretch; see window.c   */
    char bezel[MAX_PATH]; /* bezel art, a PNG; "off" or "" = none          */
    char upscale_2d[16];  /* bilinear | catmull_rom | fsr1, above 1x       */
    char texmag[16];      /* bilinear | catmull_rom: 3D textures, above 1x */
    char frame_rate[4];   /* 30 | 60: 60 adds an in-between picture each tick */
    /* Which entry of the GAME'S OWN video-mode table _init3dfx_Init selects.
     * -1 = whatever the game asked for, which is the cabinet's mode 1. Mode 2
     * is 640x480 and exists here to match an 86Box oracle frame; it is NOT
     * safe for an original arcade monitor. See glide_bind.c. */
    int  video_mode;
    /* Expand the visible raster to the nearest 16:9 width at its existing
     * height and select the smallest containing Glide surface. Projection
     * correction is applied separately. */
    int  widescreen;
    char widescreen_hud[8];   /* centre | edges: where the side HUD goes */
    /* Pass vcglide configuration through the environment before loading it. */
    int  vcglide_threads;          /* rasteriser workers: 0 = one per physical
                                    * core, 1 = single-threaded. The split is
                                    * by scanline row and provably changes no
                                    * pixel, so this is a speed knob and never
                                    * a picture one. */
    char vcglide_renderer[16];     /* auto | gpu | cpu; native render backend */
    char vcglide_log[MAX_PATH];    /* its own log file                       */
    char glide_capture[MAX_PATH];  /* write a .vglc call stream here; "" = off */
    char glide_frames[MAX_PATH];   /* dump each frame as a PNG here; "" = off */
    unsigned glide_capture_frames; /* stop after N frames; 0 = until exit    */
    int  vcglide_verbose;          /* per-entry-point detail in its log      */
    int  raster_height;   /* 0 = reconcile the OEM raster to the surface  */
    int  count_loops;     /* instrument mesh3d loop heads (invasive; off) */
    int  time_calls;      /* cycles-per-call on four mesh targets (off)   */
    int  time_fence;      /* lfence around the timer's rdtsc pair (on)    */
    int  null_fog;        /* force grFogMode(DISABLE): diagnostic A/B     */
    int  glide_first_call;/* log each Glide entry's first call + args     */
    unsigned stall_ms;    /* frame period counted as a stall; 0 = analysis off */
    int  glide_timing;    /* per-entry-point cycles in the Glide thunks       */
    int  win32_counting;  /* count calls through the 123 ETS->Win32 entries   */
    int  audio;           /* enable the WASAPI mixer behind the YMF boundary */
    unsigned audio_volume;/* host-side output gain, percent                   */
    unsigned dcs_us;      /* DCS driver tick in us; the cabinet's was 5400 */
    char audio_routing[16];/* mix | cabinet; see audio.c                    */
    unsigned audio_balance;/* front..seat bus balance, 0..100; 50 = both full */
    unsigned profile_ms;  /* diagnostic sampling period, 0 = off */
    unsigned trace_draw_state;/* seconds before dumping distinct draw states */
    unsigned draw_replay;      /* draws to replay from a restore; 0 = off      */
    unsigned draw_replay_start;/* draws to let pass before sampling            */
    int      draw_replay_control;/* no draws: the other threads' noise floor  */
    unsigned draw_replay_watch;/* diagnostic: first page to write-watch       */
    unsigned draw_replay_watch_pages;
    unsigned draw_interp_trace;      /* diagnostic: first word to trace   */
    unsigned draw_interp_trace_words;
    int      draw_interp_profile; /* diagnostic: 60 Hz cost distributions */
    /* Count call sites that submit geometry outside the native field. */
    int      margin_census;
    /* THE OTHER HALF OF A MARGIN REPORT. The census above names the game code
     * that draws outside the native field; this asks the provider what then
     * happened to the two side strips, per frame, as a pattern census, which
     * is the only shape of instrument that can see a flicker at all. */
    int      margin_trace;
    unsigned trace_lod;   /* seconds before dumping the LOD/mip census, 0=off */
    int  no_additive_light;/* bisect the additive colour-combine term      */
    int  null_draw;       /* diagnostic: drop primitive submission entirely */
    int  null_water;      /* diagnostic: stub _water_Draw; destroys rendering */
    /* The arcade link, and it is one key because it selects one behaviour.
     *   off    standalone: no ether0, and Offroad is held there by
     *          net_link_disable().
     *   lan    a synthetic ether0 carrying THIS machine's adapter address,
     *          for two machines sharing a broadcast domain.
     *   local  two instances on this machine: the same, plus each instance
     *          adopting the 192.168.200.x identity its own game wrote, and
     *          its traffic looped back.
     * Whether the link is on and which unit this is remain the GAME'S
     * settings, in its own operator menu; nothing here duplicates them. */
    char link[16];
    /* Datagrams a single frame may take before recvfrom answers
     * WSAEWOULDBLOCK. Hydro's drain has no upper bound of its own. */
    unsigned link_rx_budget;
    int  dry_run;         /* map + unpack + patch, then stop without running */
    /* Ask what of this process still works once the game's CODE is on top of
     * our image, then stop where --dry-run does. Implies dry_run. */
    int  probe_image;
    /* The control for one of that battery's cases, which terminates the
     * process when it passes, so it never shares a run with it. */
    int  probe_refusal;
} shim_config_t;

extern shim_config_t g_cfg;

void config_defaults(shim_config_t *c);

/* Write the shipped ini out of this binary's own resources when the pack has
 * none. Call once, after chdir_to_own_folder() and before any reader. It
 * NEVER overwrites, so a pack whose file was replaced by a newer build's zip
 * cannot exist: the zip does not carry one. Nonzero when a file was created. */
int  config_seed_ini(const char *path);
/* The shipped ini, as embedded in this binary. Nonzero when found. */
int  config_default_ini(const void **data, unsigned long *size);

/* Load one file in two passes: shared sections, then selected-title sections.
 * Command-line values have final precedence. A missing file preserves defaults. */
int  config_load_shared(shim_config_t *c, const char *path);
int  config_load_title(shim_config_t *c, const char *path, const char *title);
/* Anything the shared pass could not report because the log was not open yet.
 * Call once, immediately after log_open. */
void config_report_notes(void);
int  config_parse_argv(shim_config_t *c, int argc, char **argv);
/* Settles [graphics] keys that can contradict each other; call once, after
 * the ini and the command line and before anything reads them. */
void config_resolve_graphics(shim_config_t *c);

#endif /* SHIM_H */
