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

/*
 * True once the VM has hit an allocation failure it could not report cleanly —
 * specifically, one that forced the collector off (see js_gc_protect). The VM
 * stays memory-safe and keeps refusing work with out-of-memory errors, but it
 * can no longer reclaim anything, so a host should finish the current call and
 * discard it rather than keep serving from it.
 */
bool js_vm_out_of_memory(const JsVm *vm);

/*
 * Asks the VM to stop executing as soon as it can. This is the one function
 * here that may be called while the VM is running, including from another
 * thread or a signal handler — it only sets a flag, touches nothing else, and
 * needs no lock.
 *
 * It exists because fuel counts instructions, not time, and a host cannot
 * always predict which budget corresponds to an acceptable wall-clock. Arm a
 * timer, call this, and the run stops.
 *
 * The stop surfaces as a thrown "Error: execution interrupted" that re-arms
 * itself, so guest code cannot catch it and continue; the run unwinds and the
 * host gets control back. The flag is NOT cleared automatically — call
 * js_vm_clear_interrupt before using the VM again, or every subsequent
 * instruction will keep throwing.
 *
 * Latency is bounded by the interpreter's dispatch loop, so a long-running
 * builtin (a large sort, a big string build, one regex match) finishes its
 * current step first. Those steps are individually bounded, but if a hard
 * deadline matters more than a clean unwind, keep an outer kill as well —
 * under wasm, the runtime's epoch interruption.
 */
void js_vm_interrupt(JsVm *vm);
void js_vm_clear_interrupt(JsVm *vm);
bool js_vm_interrupted(const JsVm *vm);

JsContext *js_context_new(JsVm *vm);
void       js_context_free(JsContext *ctx);
JsValue    js_context_globals(JsContext *ctx);

/*
 * A pointer the host keeps on the context, and gets back inside a native.
 *
 * A native is called with the context and nothing else of the host's, so
 * without this an embedding with more than one engine in a process has to
 * reach for a static. `js_register_native` takes a `userdata` that is stored
 * and never delivered; this is the thing that was wanted.
 *
 * The engine never reads it, never frees it, and never copies it.
 */
void   js_context_set_userdata(JsContext *ctx, void *ud);
void  *js_context_userdata(JsContext *ctx);
/* The VM a context belongs to. A native is handed only the context, but most of
 * the value and GC API is VM-scoped, so it needs this to use them. */
JsVm      *js_context_vm(JsContext *ctx);

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

/*
 * The nth own property NAME of an object, or undefined past the end.
 *
 * The order is the one guest code sees from Object.keys — so a host walking
 * an object and a script walking the same object agree, which is what matters
 * when both are looking at the same data.
 *
 * NOTE that order is the engine's hash order, not insertion order, and the
 * language specifies insertion order for string keys. That is a conformance
 * gap in this engine rather than a property of this function, and closing it
 * means giving plain objects the ordered-entries-plus-index shape Map and Set
 * already have (js_valindex.h). Until then, a host that needs a particular
 * order has to impose it.
 */
JsValue js_object_key_at(JsValue obj, size_t index);

/* ---- arrays ----
 *
 * An array is an object to `js_is_object`, but its ELEMENTS are not properties
 * and its `length` is not one either — both live outside the property map. So
 * js_object_get and js_object_size cannot see them, and a host reaching for
 * `length` on an array gets `undefined` rather than an error. These are how a
 * host reads and builds one.
 *
 * They matter because structured data crossing the boundary is full of arrays:
 * a template hands back its lines as an array of pairs, and a request carries
 * lists of whatever the host is answering with. Without these a host can only
 * exchange arrays by serialising them, which is what they exist to avoid.
 *
 * Like the object functions, these touch only the array's own storage — no
 * [[Prototype]] walk, no `length` setter, no holes. Arrays here are dense.
 */
bool     js_is_array(JsValue v);
JsValue  js_array_new(JsContext *ctx, uint32_t reserve);            /* undefined on OOM */
uint32_t js_array_length(JsValue arr);                              /* 0 if not an array */
JsValue  js_array_get(JsValue arr, uint32_t index);                 /* undefined past the end */
/* Setting past the end fills the gap with undefined, as assigning to an index
 * does in the interpreter; a gap wider than the engine's limit is refused
 * rather than allowed to allocate without bound. */
bool     js_array_set(JsVm *vm, JsValue arr, uint32_t index, JsValue value);
bool     js_array_push(JsVm *vm, JsValue arr, JsValue value);

/* ---- run ---- */

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

/*
 * Why an error was raised. The three stops the engine imposes on a guest —
 * the fuel budget running out, js_vm_interrupt, and an allocation the heap
 * limit or the allocator refused — carry a structured cause, so a host can
 * map them to its own outcomes (retry, 503, discard the VM, ...) without
 * parsing message text. Everything else, a guest `throw` and the engine's
 * ordinary TypeErrors alike, is JS_CAUSE_GUEST.
 *
 * Works on whatever a failed js_call, a rejected promise, or a catch handler
 * holds: the Error object the engine raised, or the VM's prebuilt
 * "out of memory" string it falls back to when it cannot allocate even that
 * (recognized by identity, not text). A value the guest built itself —
 * `throw new RangeError('execution budget exhausted')`, `throw 'out of
 * memory'` — is JS_CAUSE_GUEST no matter what it says.
 */
typedef enum JsErrorCause {
    JS_CAUSE_GUEST = 0,
    JS_CAUSE_BUDGET = 1,    /* js_context_set_fuel budget spent */
    JS_CAUSE_INTERRUPT = 2, /* js_vm_interrupt */
    JS_CAUSE_OOM = 3        /* heap_limit or the allocator refused */
} JsErrorCause;
JsErrorCause js_error_cause(JsContext *ctx, JsValue error);
/*
 * Arms an execution budget, in bytecode instructions, for the next TURN — a
 * top-level run (js_run_module / js_call / js_eval_module) together with the
 * microtask drain it feeds, including every job js_run_jobs runs. The budget
 * is shared, not per-fiber or per-job: nested calls and queued microtasks all
 * draw from the same remainder, so no amount of re-queueing extends it. Once
 * spent, every further instruction throws a RangeError that re-arms itself, so
 * guest code cannot catch it and keep running.
 *
 * Call it again to arm the next turn — a host running untrusted code should do
 * so per request, not once at startup. 0 = unlimited.
 *
 * It counts instructions, not time: a single instruction may enter a builtin
 * that runs long (see the notes on sort/replace/regex in the security docs), so
 * a host that must bound wall-clock should also cap execution from outside.
 */
void     js_context_set_fuel(JsContext *ctx, uint64_t fuel);

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

/* ---- bytecode loading (phase 8) ---- */

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
/*
 * Registers *slot as a root. Cannot fail: the root array is exempt from
 * heap_limit, and if the underlying allocator refuses, collection is suspended
 * for the VM's lifetime rather than leaving a live value unrooted. That is why
 * there is no status to check — a caller who checked could not have done
 * anything safe with a failure anyway, and the 141-of-149 call sites that did
 * not check were the actual hazard.
 */
void   js_gc_protect(JsVm *vm, JsValue *slot);
void   js_gc_unprotect(JsVm *vm, JsValue *slot);
size_t js_gc_live_cells(const JsVm *vm);

#ifdef __cplusplus
}
#endif

#endif /* LAMASSU_H */
