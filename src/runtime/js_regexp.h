/*
 * RegExp binding layer over third_party/baru-re (ECMAScript-flavored
 * backtracking regex engine). Feature-flagged behind LAMASSU_HAS_REGEX; the
 * compiler rejects regex literals with a clear message when it's off.
 *
 * A regexp is a JS_KIND_OBJECT cell with obj_kind == JS_OBJ_REGEXP whose
 * cell embeds the compiled Program. source/flags/lastIndex and the flag
 * booleans are synthesized properties (js_regexp_prop_get/_set), not
 * entries in the props map — so Object.keys(/a/g) is empty and only
 * lastIndex is writable, matching the spec's prototype-accessor shape.
 * Scripts may still add their own expando properties via the normal map.
 */
#ifndef JS_REGEXP_H
#define JS_REGEXP_H

#include "lamassu_internal.h"

#ifdef LAMASSU_HAS_REGEX

#include "regexp.h"

/*
 * Cap on live compiled patterns per VM. A Program is a large fixed-size
 * struct (~1.8 MB — see regexp.h), allocated through the VM allocator so
 * heap_limit accounting sees it; this cap is defense-in-depth on top for
 * hosts that run with an unlimited heap.
 */
#define JS_REGEXP_MAX_LIVE 64

/*
 * Per-match-call step budget handed to the engine
 * (vm_context_set_step_budget): a base allowance plus a linear
 * per-subject-unit term. Legitimate matching costs a bounded number of VM
 * steps per text position (tens, typically), so the linear term leaves
 * orders of magnitude of headroom while still cutting off the superlinear
 * blowup of catastrophic backtracking. Exhaustion surfaces to scripts as a
 * catchable RangeError rather than a hung VM.
 */
#define JS_REGEXP_STEP_BASE (1u << 20)
#define JS_REGEXP_STEPS_PER_UNIT 2000u

typedef struct JsRegExp {
    JsObject obj;     /* obj_kind == JS_OBJ_REGEXP; props hold expandos only */
    JsString *source; /* pattern text as written ("(?:)" for empty) */
    JsString *flags;  /* normalized flags, spec order "dgimsuvy" */
    double last_index;
    bool global;      /* /g lives here; the engine VM never needs it */
    Program prog;     /* embedded compiled pattern (large; see regexp.h) */
} JsRegExp;

static inline bool js_regexp_is(JsValue v) {
    return js_is_object(v) && js_value_object(v)->obj_kind == JS_OBJ_REGEXP;
}

/*
 * Compiles pattern+flags into a fresh regexp object. Returns undefined with
 * *err set (static message, "Kind: detail" shape) on bad flags, compile
 * error, the live-pattern cap, or OOM. The caller must keep the pattern and
 * flag unit buffers alive across the call (cell allocation is a GC safe
 * point).
 */
JsValue js_regexp_new(JsContext *ctx, const uint16_t *pat, uint32_t pat_len,
                      const uint16_t *flags, uint32_t flags_len, const char **err);

/* Compile-time syntax check for regex literals (no object created).
 * NULL if the pattern+flags are valid, else a static error message. */
const char *js_regexp_validate(JsVm *vm, const uint16_t *pat, uint32_t pat_len,
                               const uint16_t *flags, uint32_t flags_len);

/* GC hooks (js_gc.c). Release frees the engine-owned class string buffers,
 * decrements the live count, and returns the true cell size. */
void   js_regexp_mark(JsVm *vm, JsObject *o);
size_t js_regexp_release(JsVm *vm, JsObject *o);

/* Synthesized-property hooks (js_interp.c get_property/set_property).
 * Get: false = OOM; *handled says whether the key was a builtin name.
 * Set: false = read-only violation with *errmsg set. */
bool js_regexp_prop_get(JsContext *ctx, JsObject *o, JsValue key, JsValue *out,
                        bool *handled);
bool js_regexp_prop_set(JsContext *ctx, JsObject *o, JsValue key, JsValue val,
                        bool *handled, const char **errmsg);

/* "/source/flags" for ToString; NULL on OOM. */
JsString *js_regexp_repr(JsContext *ctx, JsObject *o);

/* Installs the RegExp global and a real, script-visible RegExp.prototype
 * (exec/test/toString) — every instance's [[Prototype]] points there.
 * Called from js_builtins_init. */
bool js_regexp_builtins_init(JsContext *ctx);

/* Regex-aware String methods (registered/delegated from js_builtins.c).
 * match/matchAll/search coerce a non-regexp argument through ToString into
 * a pattern per spec; replace/split are called only for regexp arguments. */
bool js_re_str_match(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                     JsValue *r);
bool js_re_str_matchAll(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                        JsValue *r);
bool js_re_str_search(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                      JsValue *r);
bool js_re_str_replace(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                       JsValue *r, bool all);
bool js_re_str_split(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                     JsValue *r);

#endif /* LAMASSU_HAS_REGEX */

#endif /* JS_REGEXP_H */
