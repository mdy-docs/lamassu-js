/*
 * jsvm — public embedding API, phase 1.
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
#ifndef JSVM_H
#define JSVM_H

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
    JsReallocFn realloc_fn; /* NULL: libc realloc (unavailable in freestanding builds) */
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

/* ---- objects (phase 1: string-keyed property bag) ---- */

JsValue js_object_new(JsVm *vm);                                    /* undefined on OOM */
JsValue js_object_get(JsVm *vm, JsValue obj, JsValue key);          /* undefined if absent */
bool    js_object_set(JsVm *vm, JsValue obj, JsValue key, JsValue value); /* false on OOM/bad args */
bool    js_object_delete(JsVm *vm, JsValue obj, JsValue key);       /* true if a property was removed */
size_t  js_object_size(JsValue obj);

/* ---- compile & run (single module top level; imports arrive in phase 7) ---- */

/*
 * Compiles UTF-16 source as a strict-mode module body. Returns a function
 * value (root it per the GC contract), or undefined on error with *err_msg
 * (static ASCII) and *err_pos (source offset) set.
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
 * Runs a compiled module function. Returns true with *result = completion
 * value (the value of the last expression statement), or false with
 * *result = the error value; js_context_error_pos() gives its source offset.
 */
bool js_run_module(JsContext *ctx, JsValue fn, JsValue *result);

/*
 * Calls a function value (script closure or native) on a fresh fiber.
 * Same result contract as js_run_module.
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

/* ---- ES modules (phase 7) ---- */

/*
 * Host module resolver. Given an import `specifier` and the `referrer`
 * (importing module's specifier, empty for the root), return the resolved
 * module's source. Write a canonical specifier to *out_specifier (used as
 * the cache/identity key; may equal the input) and the UTF-16 source to
 * *out_source / *out_len. Return true on success, false if not found.
 *
 * The returned buffers must stay valid until the call returns (the engine
 * copies what it needs). Specifiers are compared by content for caching.
 */
typedef bool (*JsModuleResolver)(void *ud, const uint16_t *specifier, size_t spec_len,
                                 const uint16_t *referrer, size_t ref_len,
                                 const uint16_t **out_specifier, size_t *out_spec_len,
                                 const uint16_t **out_source, size_t *out_len);

void js_set_module_resolver(JsContext *ctx, JsModuleResolver fn, void *ud);

/*
 * Compiles, links, and evaluates a root module (its dependencies are pulled
 * in through the resolver). Returns the module's namespace (exports) object
 * on success, or undefined with err_msg/err_pos set on a compile/link
 * error, or the thrown value with return-false semantics via ok.
 */
JsValue js_eval_module(JsContext *ctx, const uint16_t *specifier, size_t spec_len,
                       const uint16_t *source, size_t source_len, bool *ok,
                       const char **err_msg, uint32_t *err_pos);

/* Reads a named export from a module namespace object (host convenience). */
JsValue js_module_get_export(JsContext *ctx, JsValue ns, const uint16_t *name,
                             size_t name_len);

/* ToString for host display; undefined on OOM. */
JsValue js_to_string(JsContext *ctx, JsValue v);

/* ---- GC ---- */

void   js_gc_collect(JsVm *vm);
bool   js_gc_protect(JsVm *vm, JsValue *slot);   /* register *slot as a root */
void   js_gc_unprotect(JsVm *vm, JsValue *slot);
size_t js_gc_live_cells(const JsVm *vm);

#ifdef __cplusplus
}
#endif

#endif /* JSVM_H */
