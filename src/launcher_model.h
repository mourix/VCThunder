/* launcher_model.h -- the launcher's settings model, with no window in it.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * WHAT THE LAUNCHER IS A VIEW OF. src/launcher.c draws pages; this half holds
 * the ini document, every key and binding value, which section a key is read from
 * and written to, what is wrong with the file, what a value has to satisfy, and
 * the known-good copy. The line between them is Win32, not subject matter:
 * nothing here creates a window, reads a control or shows a message, which lets
 * tests/launcher_model_test.c link it without a GUI.
 */
#ifndef VCT_LAUNCHER_MODEL_H
#define VCT_LAUNCHER_MODEL_H

#include <stddef.h>

#include "vcthunder.h"
#include "settings.h"
#include "ini.h"
#include "ini_doc.h"

/* The settings catalogue is src/settings.def, shared with config.c.
 *
 * `vct_ui_setting` deliberately excludes the developer switches: model_save()
 * writes every entry of this array back, so a hidden key here would be injected
 * into the user's ini on first open. */
#define SETTING_COUNT VCT_UI_SETTING_COUNT
extern const setting_desc_t *const k_settings;

/* The launcher's half of src/bindings.def: labels and per-title defaults, as
 * the text this file writes back. The catalogue is NOT duplicated here, see
 * the note in bindings.def. */
typedef struct {
    const char *key;                    /* the ini key, in BOTH sections    */
    const char *hydro_label;            /* NULL: Offroad's control alone    */
    const char *offroad_label;
    /* The keyboard default per title, or NULL where the control has no
     * keyboard half, which is the shifter paddles and nothing else. */
    const char *hydro_key, *offroad_key;
    int         has_pad;                /* is there a [gamepad] ordinal?    */
    /* The controller default per title, 0 for unbound. Two columns for the
     * same reason the keyboard half has two: see bindings.def. */
    unsigned    hydro_pad, offroad_pad;
} action_desc_t;

/* ACTION_COUNT HAS TO BE A CONSTANT EXPRESSION, not `sizeof k_actions`, now
 * that the array is declared in a header and defined in one translation unit.
 * The same table, walked to produce one enumerator each. */
enum {
#define BINDING(key, hydro, offroad, pad_hydro, pad_offroad, button, hl, ol)  \
    VCT_ACTION_##key,
#define BINDING_AXIS(key, hydro, offroad, hl, ol)   VCT_ACTION_##key,
#define BINDING_PAD(key, pad_hydro, pad_offroad, hl, ol)  VCT_ACTION_##key,
#include "bindings.def"
#undef BINDING
#undef BINDING_AXIS
#undef BINDING_PAD
    ACTION_COUNT
};

extern const action_desc_t k_actions[ACTION_COUNT];

/* 64, NOT 32, for both halves. A [digital] value is a comma list of MAME key
 * names, so at 32 a second key would be truncated on the way in and written
 * back truncated by an Apply that changed something else. A [gamepad] value is
 * a comma list of `[<label>:]<ordinal>`, so the widest legal value is
 * IO_MAX_PAD_REFS references of a 15-character label, a colon, three digits
 * and a separator, the same truncation-on-read hazard. */
#define MODEL_BINDING_CAP 64

/* ---- the document -------------------------------------------------------- */

/* Read `ini_path` and fill the model from it. 0 when the document itself could
 * not be read, which is the one failure that has no page to show. */
int         model_open(const char *ini_path);
/* Read the same path again, after something has replaced it on disk. */
int         model_reopen(void);
void        model_close(void);
/* Re-read every value out of the document the model already holds. */
void        model_load(void);
ini_doc_t  *model_doc(void);
/* Is there a file at the document's path? A claim about what the file already
 * says may only be made about a file that is there. */
int         model_ini_exists(void);

/* ---- values -------------------------------------------------------------- */

int         setting_find(const char *section, const char *key);
const char *model_value(const char *section, const char *key);
int         model_bool(const char *section, const char *key);
unsigned    model_uint(const char *section, const char *key, unsigned dflt);
/* The model's own storage for setting `i`, INI_VALUE_CAP bytes, writable: the
 * pages read values back out of their controls into it. */
char       *model_slot(unsigned i);
/* The section this key is read from first and written to. */
const char *setting_section(unsigned i);
/* The [devices] label of the device the FILE configures, "" when it pins
 * none. */
const char *model_profile(void);
void        resolve_profile(void);
void        reload_device_settings(void);
/* Re-read any role fact (invert, steering range) whose role now names another
 * controller, from that controller's section; `moved` (may be NULL) is called
 * for each. Returns how many. */
int         model_sync_role_facts(void (*moved)(const char *section,
                                                const char *key));
/* An [analog] key as the host reads it for the controller a [devices] label
 * names; the model's own value for the configured device. */
const char *model_device_value(const char *label, const char *key);
/* A [devices] line this session added and Apply has not written yet. */
void        model_device_entry_added(void);

/* The attached controllers of one provider, in enumeration order, from the
 * launcher's probe. `ffb[k]` is whether ids[k] publishes effects, for the
 * auto rule. */
#define MODEL_ATTACHED_MAX 24
typedef unsigned (*model_attached_fn)(const char *provider,
                                      ini_device_id_t *ids, int *ffb,
                                      unsigned max);
void        model_set_attached(model_attached_fn fn);

/* Which device the [devices] lines open, by the host's resolver over the
 * document. `dev` is one provider's devices in enumeration order.
 *
 *   _label_find    the first label whose line opens dev[which]
 *   _for_label     the position `label`'s line opens, or -1
 *   _configured    the position pick_device() opens: `dinput_name`, else the
 *                  first with ffb[k] (none when `ffb` is NULL)
 *   _match         what to write so a new line (as_line) or dinput_name opens
 *                  dev[which]: its name if enough, else its ID; 0 if neither
 *   _label_ensure  _label_find, or a new line from _match */
int         model_device_label_find(const char *provider,
                                    const ini_device_id_t *dev, unsigned count,
                                    unsigned which, char *out, size_t out_size);
int         model_device_for_label(const char *label, const char *provider,
                                   const ini_device_id_t *dev, unsigned count);
int         model_configured_device(const ini_device_id_t *dev,
                                    const int *ffb, unsigned count);
int         model_device_match(const char *provider,
                               const ini_device_id_t *dev, unsigned count,
                               unsigned which, int as_line,
                               char *out, size_t out_size);
int         model_device_label_ensure(const char *provider,
                                      const ini_device_id_t *dev,
                                      unsigned count, unsigned which,
                                      const char *base,
                                      char *out, size_t out_size);

/* ---- bindings ------------------------------------------------------------ */

/* MODEL_BINDING_CAP bytes each, writable. `title` is 0 hydro, 1 offroad. */
char       *model_key_binding(int title, unsigned action);
char       *model_pad_binding(int title, unsigned action);

/* ---- what is wrong with the file ----------------------------------------- */

void        scan_ini_problems(void);
unsigned    model_problem_count(void);  /* how many are listed             */
unsigned    model_problem_total(void);  /* how many there are              */
const char *model_problem(unsigned i);

/* ---- validation and saving ----------------------------------------------- */

/* 0 with `why` filled, and `*page` set to the page the offending row is on. */
int         validate_settings(char *why, size_t why_size, int *page);

/* IS [graphics] video_mode THE VALUE THE TABLE FLAGS AS DANGEROUS? Asked
 * before model_save() rather than inside it, because the answer to it is a
 * question put to the user and this half does not ask questions. */
int         model_unsafe_video_mode(void);

enum {
    MODEL_SAVE_NO_MEMORY = 0,   /* `why` is a sentence; the status bar is not
                                 * the place for it, a modal is             */
    MODEL_SAVE_OK        = 1,
    MODEL_SAVE_FAILED    = -1   /* ini_doc_save()'s own reason, worth both   */
};

/* Write the model into the document and the document to disk. `*changed` is
 * how many keys were actually written; 0 means the file already said all of
 * it and NOTHING was written; saving an identical file would still replace
 * vcthunder.ini.bak with a copy of the current one. Call validate_settings()
 * first; this does not. */
/* The file APIs are on this side, because a document that cannot be written is
 * a fact about the model; a function that ended in a MessageBox is split, the
 * reason here and the saying of it in the window. And nothing here asks the
 * live probe what is plugged in: what a binding row is CALLED depends on what
 * is attached right now, so that stays a view of two things, in the window. */
int         model_save(char *why, size_t why_size, int *changed);

/* ---- the known-good copy ------------------------------------------------- */

void        known_good_path(char *out, size_t out_size);
/* One sentence for the page: whether there is a copy, and when it was made. */
void        known_good_state(char *out, size_t out_size);
void        seed_known_good(void);
int         model_known_good_exists(void);
/* Copy the ini over the known-good copy. Asking first is the window's job. */
int         model_known_good_write(char *why, size_t why_size);
/* Put the known-good copy in place of the ini, keeping the file as it is now
 * as vcthunder.ini.bak. THE DOCUMENT IS NOT RELOADED: the caller flushes what
 * it caches about the file and calls model_reopen(), which can fail on its
 * own and means something different when it does. */
int         model_known_good_restore(char *why, size_t why_size);

#endif /* VCT_LAUNCHER_MODEL_H */
