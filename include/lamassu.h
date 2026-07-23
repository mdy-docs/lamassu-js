/*
 * lamassu-js — public embedding API, phase 1.
 *
 * Strings are UTF-16 code units carried as pointer + length, never
 * NUL-terminated. All state hangs off a JsVm; there are no globals.
 *
 * GC contract: creating a string/object/context, or calling js_object_set,
 * is a GC safe point. Every JsValue the caller holds across a safe point —
 * including the arguments being passed in — must be reachable from a slot
 * registered with js_gc_protect (or from something that is, e.g. a
 * protected object the value was stored into). Build with gc_stress to
 * shake out violations.
 */
#ifndef LAMASSU_H
#define LAMASSU_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JsVm JsVm;
typedef struct JsContext JsContext;

/*
 * NaN-boxed 64-bit value. Doubles are stored as their own bit pattern
 * (real NaNs canonicalized to JS_CANONICAL_NAN by js_number, so every
 * pattern with a top-16-bit tag >= 0xFFF9 is free for boxing). Cell
 * payloads are 48-bit pointers.
 */
typedef struct JsValue {
    uint64_t bits;
} JsValue;

#define JS_TAG_MASK      UINT64_C(0xFFFF000000000000)
#define JS_PAYLOAD_MASK  UINT64_C(0x0000FFFFFFFFFFFF)
#define JS_TAG_SPECIAL   UINT64_C(0xFFF9000000000000)
#define JS_TAG_STRING    UINT64_C(0xFFFA000000000000)
#define JS_TAG_OBJECT    UINT64_C(0xFFFB000000000000)
#define JS_TAG_FUNCTION  UINT64_C(0xFFFC000000000000)
#define JS_TAG_PROMISE   UINT64_C(0xFFFD000000000000)
#define JS_CANONICAL_NAN UINT64_C(0x7FF8000000000000)

#define JS_SPECIAL_UNDEFINED (JS_TAG_SPECIAL | 0)
#define JS_SPECIAL_NULL      (JS_TAG_SPECIAL | 1)
#define JS_SPECIAL_FALSE     (JS_TAG_SPECIAL | 2)
#define JS_SPECIAL_TRUE      (JS_TAG_SPECIAL | 3)

static inline JsValue js_undefined(void) {
    JsValue v; v.bits = JS_SPECIAL_UNDEFINED; return v;
}

static inline JsValue js_null(void) {
    JsValue v; v.bits = JS_SPECIAL_NULL; return v;
}

static inline JsValue js_bool(bool b) {
    JsValue v; v.bits = b ? JS_SPECIAL_TRUE : JS_SPECIAL_FALSE; return v;
}

static inline JsValue js_number(double d) {
    union { double d; uint64_t u; } pun;
    JsValue v;
    pun.d = d;
    v.bits = (d != d) ? JS_CANONICAL_NAN : pun.u;
    return v;
}

static inline double js_get_number(JsValue v) {
    union { uint64_t u; double d; } pun;
    pun.u = v.bits;
    return pun.d;
}

static inline bool js_is_number(JsValue v) {
    return (v.bits & JS_TAG_MASK) < JS_TAG_SPECIAL;
}

static inline bool js_is_undefined(JsValue v) { return v.bits == JS_SPECIAL_UNDEFINED; }
static inline bool js_is_null(JsValue v)      { return v.bits == JS_SPECIAL_NULL; }

static inline bool js_is_bool(JsValue v) {
    return v.bits == JS_SPECIAL_FALSE || v.bits == JS_SPECIAL_TRUE;
}

static inline bool js_get_bool(JsValue v) { return v.bits == JS_SPECIAL_TRUE; }

static inline bool js_is_string(JsValue v) { return (v.bits & JS_TAG_MASK) == JS_TAG_STRING; }
static inline bool js_is_object(JsValue v) { return (v.bits & JS_TAG_MASK) == JS_TAG_OBJECT; }
static inline bool js_is_function(JsValue v) { return (v.bits & JS_TAG_MASK) == JS_TAG_FUNCTION; }
static inline bool js_is_promise(JsValue v) { return (v.bits & JS_TAG_MASK) == JS_TAG_PROMISE; }

/* Identity (same bits), not ===: two equal-content heap strings differ. */
static inline bool js_same_value(JsValue a, JsValue b) { return a.bits == b.bits; }

/* ---- VM lifecycle ---- */

/*
 * realloc-style allocator: new_size == 0 frees (returns NULL); ptr == NULL
 * allocates. old_size is always the exact size of the existing block.
 */
typedef void *(*JsReallocFn)(void *ud, void *ptr, size_t old_size, size_t new_size);

typedef struct JsVmConfig {
    JsReallocFn realloc_fn; /* NULL: use libc realloc */
    void *alloc_ud;
    size_t gc_threshold;    /* live bytes before first auto-collect; 0 = default */
    size_t heap_limit;      /* hard cap on live bytes; 0 = unlimited */
    uint64_t rng_seed;      /* Math.random seed; 0 = fixed default (deterministic) */
    bool gc_stress;         /* collect at every safe point (for tests) */
} JsVmConfig;

/*
 * Host function. args points into the fiber stack (valid for the duration
 * of the call; copy anything kept). Return true with *result set, or false
 * with *result = the value to throw.
 */
typedef bool (*JsNativeFn)(JsContext *ctx, JsValue this_val, const JsValue *args,
                           int argc, JsValue *result);

JsVm *js_vm_new(const JsVmConfig *cfg); /* cfg may be NULL */
void  js_vm_free(JsVm *vm);
size_t js_vm_allocated_bytes(const JsVm *vm);

JsContext *js_context_new(JsVm *vm);
void       js_context_free(JsContext *ctx);
JsValue    js_context_globals(JsContext *ctx);

/* ---- strings ---- */

JsValue js_string_new(JsVm *vm, const uint16_t *units, size_t len); /* undefined on OOM */
JsValue js_atom(JsVm *vm, const uint16_t *units, size_t len);       /* interned; undefined on OOM */
const uint16_t *js_string_units(JsValue str, size_t *len);          /* NULL if not a string */
size_t  js_string_length(JsValue str);
bool    js_string_equals(JsValue a, JsValue b);                     /* content equality */

/*
 * ---- objects (phase 1: string-keyed property bag) ----
 *
 * js_object_new takes the context (not just the VM) because [[Prototype]]
 * is a realm concept: the new object's [[Prototype]] is ctx->object_proto,
 * the same real, script-visible Object.prototype (hasOwnProperty, toString,
 * valueOf) a guest `{}` literal gets — a native constructing an object this
 * way is indistinguishable from guest code doing it. get/set/delete/size
 * stay VM-scoped: they only ever touch OWN properties (no [[Prototype]]
 * walk), so they need no realm context.
 */
JsValue js_object_new(JsContext *ctx);                              /* undefined on OOM */
JsValue js_object_get(JsVm *vm, JsValue obj, JsValue key);          /* undefined if absent */
bool    js_object_set(JsVm *vm, JsValue obj, JsValue key, JsValue value); /* false on OOM/bad args */
bool    js_object_delete(JsVm *vm, JsValue obj, JsValue key);       /* true if a property was removed */
size_t  js_object_size(JsValue obj);

/* ---- compile & run (single module top level; imports arrive in phase 7) ---- */

/*
 * The exact *err_msg js_compile_module reports when the source uses top-level
 * import/export (so it must go through the module pipeline, not run as a plain
 * script). A host distinguishes "this source is a module" by comparing *err_msg
 * to this string — an exact match on a shared constant, not a brittle substring
 * search.
 */
#define JS_ERR_NEEDS_MODULE_LOADER \
    "import/export requires the module loader (js_eval_module)"

/*
 * Compiles UTF-16 source as a strict-mode module body. Returns a function
 * value (root it per the GC contract), or undefined on error with *err_msg
 * (static ASCII) and *err_pos (source offset) set. If the source uses top-level
 * import/export, compilation fails with *err_msg == JS_ERR_NEEDS_MODULE_LOADER.
 */
JsValue js_compile_module(JsContext *ctx, const uint16_t *src, size_t len,
                          const char **err_msg, uint32_t *err_pos);

/*
 * Like js_compile_module, but top-level let/const/function declarations
 * become persistent globals on the context — so successive evaluations in
 * the same context share state (a REPL session).
 */
JsValue js_compile_module_repl(JsContext *ctx, const uint16_t *src, size_t len,
                               const char **err_msg, uint32_t *err_pos);

/*
 * Runs a compiled module function and returns its completion promise:
 * fulfilled with the completion value (the value of the last expression
 * statement), rejected with the thrown error (js_context_error_pos() gives
 * its source offset), or still pending when top-level await suspended on a
 * promise the host hasn't settled yet. In that last case, protect the
 * returned promise, settle the host promises it is waiting on, and call
 * js_run_jobs(); observe completion with js_promise_state /
 * js_promise_result. Returns undefined on OOM or if fn is not a function.
 */
JsValue js_run_module(JsContext *ctx, JsValue fn);

/*
 * Calls a function value (script closure or native) on a fresh fiber.
 * Returns true with *result = the return value, or false with *result =
 * the error value.
 */
bool js_call(JsContext *ctx, JsValue fn, JsValue this_val, const JsValue *args,
             int argc, JsValue *result);

/* Defines a global native function; false on OOM. */
bool js_register_native(JsContext *ctx, const uint16_t *name, size_t name_len,
                        JsNativeFn fn, void *userdata);

uint32_t js_context_error_pos(const JsContext *ctx);
void     js_context_set_fuel(JsContext *ctx, uint64_t fuel); /* 0 = unlimited */

/* ---- promises / async (phase 6) ---- */

/*
 * Host-side promise API. A native that needs time returns a pending promise
 * (js_promise_new); the host settles it later with js_resolve/js_reject and
 * then drains the microtask queue with js_run_jobs. The host MUST keep the
 * returned promise reachable (e.g. js_gc_protect) until it settles it —
 * otherwise the collector may reclaim an in-flight promise.
 *
 * js_resolve adopts a promise/thenable argument (chains), matching JS. To
 * fulfill with an object value verbatim, that object simply must not be a
 * promise.
 */
JsValue js_promise_new(JsContext *ctx);          /* undefined on OOM */
bool    js_resolve(JsContext *ctx, JsValue promise, JsValue value);
bool    js_reject(JsContext *ctx, JsValue promise, JsValue reason);

/* Runs queued microtasks (promise reactions, async resumptions) to quiescence. */
void js_run_jobs(JsContext *ctx);
bool js_has_pending_jobs(const JsContext *ctx);

/* Promise introspection: -1 = not a promise, 0 = pending, 1 = fulfilled,
 * 2 = rejected. */
int js_promise_state(JsValue v);
/* Fulfillment value or rejection reason; undefined if pending or not a promise. */
JsValue js_promise_result(JsValue v);

/* ---- ES modules (phase 7) ---- */

/*
 * Host module loader. Given an import `specifier` (canonical — see the
 * canonicalizer below) and the `referrer` (importing module's specifier,
 * empty for the root or a dynamic import() from plain script), return a
 * Promise (pending or already settled). Fulfill it with:
 *   - a JS string        -> ES module source (compiled; its own imports are
 *                           loaded through this same loader), or
 *   - a bytecode value   -> precompiled module bytecode (js_bytecode_value), or
 *   - any other object   -> adopted directly as the module's exports (a
 *                           synthetic leaf module — no parse, no deps).
 * Reject it to fail the load; the reason propagates to every dependent.
 *
 * For real async work, return js_promise_new() and settle it later with
 * js_resolve/js_reject followed by js_run_jobs() (the standard two-phase
 * pattern above) — or delegate to a JS-authored loader via js_call and
 * return whatever promise it produces.
 */
typedef JsValue (*JsModuleLoader)(void *ud, JsContext *ctx,
                                  const uint16_t *specifier, size_t spec_len,
                                  const uint16_t *referrer, size_t ref_len);

/*
 * Optional synchronous specifier canonicalization, run before dedupe/fetch:
 * maps a raw import specifier + referrer to the canonical specifier that
 * becomes the module's registry identity (e.g. relative path -> absolute
 * URL). Must be deterministic for a given (specifier, referrer) pair. Write
 * the result to *out_specifier / *out_spec_len (valid until the call
 * returns; the engine copies). Return false to fail the load ("cannot
 * resolve module specifier"). A NULL canonicalizer means the raw specifier
 * is the identity.
 */
typedef bool (*JsModuleCanonicalizer)(void *ud, const uint16_t *specifier, size_t spec_len,
                                      const uint16_t *referrer, size_t ref_len,
                                      const uint16_t **out_specifier, size_t *out_spec_len);

/* One userdata serves both callbacks; canon may be NULL. */
void js_set_module_loader(JsContext *ctx, JsModuleLoader load,
                          JsModuleCanonicalizer canon, void *ud);

/*
 * Loads, links, and evaluates the module graph rooted at `specifier`
 * (everything, the root included, arrives through the loader). Always
 * returns a live Promise — protect it, drive outstanding loads with
 * js_resolve/js_reject + js_run_jobs(), and observe completion with
 * js_promise_state / js_promise_result. Fulfills with the module's
 * namespace (exports) object; rejects with the load/compile/link/evaluate
 * error. Returns undefined only on OOM.
 */
JsValue js_eval_module(JsContext *ctx, const uint16_t *specifier, size_t spec_len);

/* Reads a named export from a module namespace object (host convenience). */
JsValue js_module_get_export(JsContext *ctx, JsValue ns, const uint16_t *name,
                             size_t name_len);

/* ToString for host display; undefined on OOM. */
JsValue js_to_string(JsContext *ctx, JsValue v);

/* ---- bytecode serialization (phase 8) ---- */

/*
 * Serializes a compiled top-level function (from js_compile_module — NOT a
 * module body; import/export bytecode is not portable) into a self-describing
 * byte buffer. On success returns true with *out / *out_len set to a buffer
 * owned by the VM allocator; free it with js_bytecode_free. Returns false on
 * OOM or if fn is not a plain compiled function.
 *
 * The format is versioned and carries no source; runtime error positions
 * survive (line table is included) but mapping them to line:col needs the
 * original source. The byte order is canonical (little-endian), so a cache
 * is portable across machines of the same pointer width.
 */
bool js_bytecode_serialize(JsContext *ctx, JsValue fn, uint8_t **out, size_t *out_len);
void js_bytecode_free(JsContext *ctx, uint8_t *buf, size_t len);

/*
 * Loads and fully validates a bytecode buffer produced by
 * js_bytecode_serialize, returning a runnable function value (root it, then
 * js_run_module it like a freshly compiled one). Returns undefined with
 * *err_msg (static ASCII) set on ANY structural problem — bad magic/version,
 * truncation, out-of-range constant/local/upvalue indices, jump targets off
 * an instruction boundary, stack underflow/overflow, or a missing terminator.
 * The loader assumes the input may be corrupt or hostile: a rejected buffer
 * never yields an executable function, so a tampered cache cannot become
 * undefined behavior in the interpreter.
 */
JsValue js_bytecode_load(JsContext *ctx, const uint8_t *buf, size_t len,
                         const char **err_msg);

/* ---- module bytecode (phase 8, modules) ---- */

/*
 * Compiles ONE module (import/export source) to a bytecode buffer WITHOUT
 * resolving, linking, or evaluating it — so each page/partial can be compiled
 * independently and cached. The buffer records the module's body plus its link
 * metadata (import descriptors, star re-exports, dependency specifiers); the
 * resolved dependency modules and live exports are runtime state rebuilt at
 * load. `specifier` becomes the module's identity/cache key. On success
 * returns true with *out / *out_len (free via js_bytecode_free); on a
 * parse/compile error returns false with *err_msg / *err_pos set.
 *
 * A module buffer is distinct from a script buffer (js_bytecode_serialize):
 * js_bytecode_load rejects a module buffer and the module loader rejects a
 * script buffer, so the two can't be confused.
 */
bool js_bytecode_compile_module(JsContext *ctx, const uint16_t *specifier,
                                size_t spec_len, const uint16_t *source,
                                size_t source_len, uint8_t **out, size_t *out_len,
                                const char **err_msg, uint32_t *err_pos);

/*
 * Wraps a js_bytecode_compile_module buffer in an opaque GC-managed value
 * (the bytes are copied) for a module loader to fulfill its promise with.
 * Every loaded module buffer is fully validated (including import-index
 * bounds the interpreter trusts), so a tampered cache cannot become
 * undefined behavior. Returns undefined on OOM.
 */
JsValue js_bytecode_value(JsContext *ctx, const uint8_t *buf, size_t len);

/*
 * Reports what a bytecode buffer holds without loading it: 0 = a script
 * (js_bytecode_load / js_run_module), 1 = a module (js_bytecode_value via
 * the module loader), negative = not valid bytecode. Lets a host dispatch a
 * `.jsbc` file correctly.
 */
int js_bytecode_kind(const uint8_t *buf, size_t len);

/* ---- GC ---- */

void   js_gc_collect(JsVm *vm);
bool   js_gc_protect(JsVm *vm, JsValue *slot);   /* register *slot as a root */
void   js_gc_unprotect(JsVm *vm, JsValue *slot);
size_t js_gc_live_cells(const JsVm *vm);

#ifdef __cplusplus
}
#endif

#endif /* LAMASSU_H */
