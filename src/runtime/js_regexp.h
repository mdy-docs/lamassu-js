/*
 * RegExp binding layer over third_party/baru-re (ECMAScript-flavored
 * backtracking regex engine). Feature-flagged behind LAMASSU_HAS_REGEX; the
 * compiler rejects regex literals with a clear message when it's off.
 *
 * A regexp is a JS_KIND_OBJECT cell with obj_kind == JS_OBJ_REGEXP whose
 * cell embeds the compiled Program. lastIndex is its one own property, kept
 * in the cell (js_regexp_prop_get/_set), not in the props map: whatever value
 * a script wrote, which a match converts (ToLength); source, flags
 * and the flag booleans are RegExp.prototype's accessors (js_regexp_accessor),
 * found by the ordinary prototype walk, as the spec has them.
 * Scripts may still add their own expando properties via the normal map.
 */
#ifndef JS_REGEXP_H
#define JS_REGEXP_H

#include "lamassu_internal.h"

#ifdef LAMASSU_HAS_REGEX

#include "regexp.h"

/*
 * Cap on live compiled patterns per VM. A Program is a 21 KB struct — the
 * class table and group names are fixed arrays, the instructions are
 * allocated to size (see regexp.h) — through the VM allocator, so heap_limit
 * accounting sees all of it; this cap is defense-in-depth on top for hosts
 * that run with an unlimited heap.
 *
 * It was 64, sized when a Program embedded its instruction array and ran to
 * ~1.8 MB. That made 64 patterns 115 MB; it makes 4096 of today's 86 MB at
 * the very worst, and a guest that holds that many is a grammar, not an
 * attack — highlight.js keeps several hundred alive across its 37 languages,
 * which is what raised it.
 */
#define JS_REGEXP_MAX_LIVE 4096

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
    JsValue last_index;    /* any value; a number unless a script wrote something else */
    bool last_index_ro;    /* Object.defineProperty made it read-only (obj_flags then has JS_OBJ_ATTRS) */
    bool global;      /* /g lives here; the engine VM never needs it */
    /*
     * The engine's match-time scratch, kept between calls instead of built
     * and torn down for each one. It is what baru-re calls a reusable
     * context: an embedded depth[MAX_AST_DEPTH] table plus the buffers each
     * level lazily allocates, so making one per match meant zeroing tens of
     * kilobytes and freeing a few hundred slots to match five characters.
     * vm_context_set_step_budget re-arms it per call, which is the reuse the
     * engine documents. NULL until the first match, and dropped again if a
     * match ever reports exhaustion — that flag also covers a match-time OOM,
     * after which baru-re says the context must never enter the VM again.
     * Bounded by JS_REGEXP_MAX_LIVE, and small beside the Program above.
     */
    VMContext *vctx;
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

/* The flags error both constructors report (compared by text) */
#define JS_REGEXP_FLAGS_ERROR "SyntaxError: invalid regular expression flags"

/* The thrown error for a js_regexp_new / js_regexp_validate failure ERR, worded as V8 words it:
 * "Invalid regular expression: /(/: Unterminated group", "Invalid flags supplied to RegExp
 * constructor 'zz'"; any other error (out of memory, a limit) as it is. */
JsValue js_regexp_error(JsContext *ctx, const char *err, const uint16_t *pat, uint32_t pat_len,
                        const uint16_t *flags, uint32_t flags_len);

/* Compile-time syntax check for regex literals (no object created).
 * NULL if the pattern+flags are valid, else a static error message. */
const char *js_regexp_validate(JsVm *vm, const uint16_t *pat, uint32_t pat_len,
                               const uint16_t *flags, uint32_t flags_len);

/* GC hooks (js_gc.c). Release frees the engine-owned class string buffers,
 * decrements the live count, and returns the true cell size. */
/* RegExp.prototype's accessors and the RegExp constructor's legacy statics (RegExp.$1,
 * lastMatch, input, ...: the JS_REGEXP_LEGACY_COUNT names V8 has), for js_accessor_call: ID a
 * JsAccessorId of theirs. js_regexp_legacy_name is the name of the legacy static numbered IDX. */
bool js_regexp_accessor(JsContext *ctx, unsigned id, JsValue recv, JsValue arg, JsValue *out);
const char *js_regexp_legacy_name(int idx);

void   js_regexp_mark(JsVm *vm, JsObject *o);
size_t js_regexp_release(JsVm *vm, JsObject *o);

/* lastIndex, the own property a regexp keeps in its cell (js_interp.c get_property and
 * set_property). Get: *handled says whether the key was it. Set: true when the key was it, and
 * VAL is written (the caller has asked js_regexp_last_index_readonly). */
bool js_regexp_prop_get(JsContext *ctx, JsObject *o, JsValue key, JsValue *out,
                        bool *handled);
bool js_regexp_prop_set(JsObject *o, JsValue key, JsValue val);
/* Object.defineProperty's write of it: the value, and read-only from then on when READONLY (it
 * is never enumerable or configurable, and cannot be made writable again) */
void js_regexp_define_last_index(JsObject *o, JsValue val, bool readonly);
static inline bool js_regexp_last_index_readonly(const JsObject *o) {
    return ((const JsRegExp *)o)->last_index_ro || (o->obj_flags & JS_OBJ_FROZEN) != 0;
}

/* "/source/flags" for ToString; NULL on OOM. */
JsString *js_regexp_repr(JsContext *ctx, JsObject *o);

/* Installs the RegExp global and a real, script-visible RegExp.prototype
 * (exec/test/toString) — every instance's [[Prototype]] points there.
 * Called from js_builtins_init. */
bool js_regexp_builtins_init(JsContext *ctx);

/* V is RegExp.prototype or inherits from it: what String.prototype's match, matchAll, replace,
 * search and split hand the work to (in V8 through the prototype's Symbol.match and the others;
 * there are no symbols here, so these are the methods and a script cannot replace them, only
 * what they read: exec, flags, constructor, lastIndex). */
bool js_regexp_like(JsContext *ctx, JsValue v);
/* IsRegExp(V) (7.2.8): that, or a regexp whatever its prototype */
bool js_regexp_isregexp(JsContext *ctx, JsValue v);
/* matchAll's and replaceAll's check of an argument that is a regexp (IsRegExp): its `flags`
 * must have "g". False thrown. */
bool js_re_global_check(JsContext *ctx, JsValue v, bool match_all, JsValue *r);

/* The String methods' regexp side (registered/delegated from js_builtins.c). match, matchAll
 * and search make a regexp of an argument that is not js_regexp_like; replace and split are
 * called only for one that is. */
bool js_re_str_match(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                     JsValue *r);
bool js_re_str_matchAll(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                        JsValue *r);
bool js_re_str_search(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                      JsValue *r);
bool js_re_str_replace(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                       JsValue *r);
bool js_re_str_split(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                     JsValue *r);
/* One step of the RegExp String Iterator IT (js_iter.h), which the caller has rooted: 1 with
 * exec's result in *OUT, 0 none left, -1 thrown (*OUT the error). */
struct JsIterObj;
int js_re_iter_step(JsContext *ctx, struct JsIterObj *it, JsValue *out);

#endif /* LAMASSU_HAS_REGEX */

#endif /* JS_REGEXP_H */
