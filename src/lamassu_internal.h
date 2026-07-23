#ifndef LAMASSU_INTERNAL_H
#define LAMASSU_INTERNAL_H

#include "lamassu.h"

#include <string.h>

typedef enum JsGcKind {
    JS_KIND_STRING = 1,
    JS_KIND_OBJECT = 2,
    JS_KIND_FUNCTION = 3, /* compiled prototype (bytecode + consts) */
    JS_KIND_FIBER = 4,
    JS_KIND_CLOSURE = 5,  /* callable: prototype + captured upvalues */
    JS_KIND_UPVALUE = 6,
    JS_KIND_NATIVE = 7,   /* callable host function */
    JS_KIND_PROMISE = 8,
    JS_KIND_MODULE = 9,
    JS_KIND_BYTECODE = 10, /* opaque module-bytecode buffer (js_bytecode_value) */
} JsGcKind;

/* Every GC allocation starts with this header; cells live on vm->cells. */
typedef struct JsGcCell JsGcCell;
struct JsGcCell {
    JsGcCell *next;
    uint8_t kind;
    uint8_t mark;
};

typedef struct JsString {
    JsGcCell gc;
    uint32_t length; /* UTF-16 code units; no terminator */
    uint32_t hash;
    bool interned;
    uint16_t units[];
} JsString;

/*
 * Open-addressed map, interned-string keys (pointer identity), linear
 * probing with tombstones. used counts live + tombstoned slots.
 */
typedef struct JsMapEntry {
    JsString *key; /* NULL empty, JS_MAP_TOMBSTONE deleted */
    JsValue value;
} JsMapEntry;

typedef struct JsMap {
    JsMapEntry *entries;
    uint32_t count;
    uint32_t used;
    uint32_t capacity; /* power of two, or 0 */
} JsMap;

#define JS_MAP_TOMBSTONE ((JsString *)(uintptr_t)1)

typedef enum JsObjKind {
    JS_OBJ_PLAIN = 0,
    JS_OBJ_ARRAY = 1,
    JS_OBJ_REGEXP = 2, /* JsRegExp (js_regexp.h); only when LAMASSU_HAS_REGEX */
    JS_OBJ_DATE = 3,   /* JsDateObject (js_date.h) */
    JS_OBJ_MAP = 4,    /* JsMapObj (js_mapobj.h) */
    JS_OBJ_SET = 5,    /* JsSetObj (js_setobj.h) */
} JsObjKind;

typedef struct JsObject {
    JsGcCell gc;
    uint8_t obj_kind;
    JsMap props;
    /* JS_OBJ_ARRAY: dense elements; sparse writes fall back to props. */
    JsValue *elems;
    uint32_t elem_count, elem_cap;
    /* [[Prototype]]: undefined = none. Only ever an object (see `new` in
     * js_interp.c); there is no script-facing way to set it to anything
     * else or to create a cycle. */
    JsValue proto;
} JsObject;

/* Compiled function. code/consts/lines are raw allocations owned by the
 * cell; consts are traced by the GC. */
typedef struct JsLineEntry {
    uint32_t off; /* code offset */
    uint32_t pos; /* source offset */
} JsLineEntry;

typedef struct JsUpvalDesc {
    uint16_t idx;      /* parent local slot, or parent upvalue index */
    uint8_t from_local;
} JsUpvalDesc;

#define JS_FN_ARROW 0x01
#define JS_FN_HAS_REST 0x02
#define JS_FN_ASYNC 0x04

typedef struct JsModule JsModule;

typedef struct JsFunctionCell {
    JsGcCell gc;
    uint8_t *code;
    uint32_t code_len, code_cap;
    JsValue *consts;
    uint32_t const_count, const_cap;
    JsLineEntry *lines;
    uint32_t line_count, line_cap;
    JsUpvalDesc *upvals;
    uint16_t n_upvals, upval_cap;
    uint16_t n_params;
    uint8_t fn_flags;
    uint32_t n_locals, max_stack;
    JsString *name;     /* may be NULL */
    JsModule *module;   /* module this fn belongs to (exports/imports); may be NULL */
    /* Retained instruction-start bitmap (one byte per code byte), populated by
     * the bytecode verifier and kept ONLY for functions loaded from an
     * untrusted cache. NULL for compiler-emitted functions, whose RET_SUB
     * return addresses cannot be forged. Lets RET_SUB reject a return address
     * that lands mid-instruction. */
    uint8_t *insn_boundary;
} JsFunctionCell;

typedef struct JsFiber JsFiber;

/*
 * Captured variable. While open it aliases fiber->stack[slot] (offset, not
 * pointer — the stack may be reallocated); closing copies the value into
 * `closed`.
 */
typedef struct JsUpvalue JsUpvalue;
struct JsUpvalue {
    JsGcCell gc;
    JsFiber *fiber;
    uint32_t slot;
    bool open;
    JsValue closed;
    JsUpvalue *next_open; /* fiber's open-upvalue list */
};

typedef struct JsClosure {
    JsGcCell gc;
    JsFunctionCell *fn;
    JsValue this_val; /* arrows: lexically captured `this` */
    bool has_this;
    uint16_t n_upvals;
    /* `new`: lazily-created .prototype object (constructor pattern); NULL
     * until first accessed via `fn.prototype` or `new fn()`. */
    JsObject *prototype_obj;
    JsUpvalue *upvals[];
} JsClosure;

/* Bound native: receives a captured value (used for promise resolve/reject). */
typedef bool (*JsBoundFn)(JsContext *ctx, JsValue bound, JsValue this_val,
                          const JsValue *args, int argc, JsValue *result);

typedef struct JsNative {
    JsGcCell gc;
    JsNativeFn fn;       /* used when !is_bound */
    JsBoundFn bound_fn;  /* used when is_bound */
    void *ud;
    JsString *name;      /* may be NULL */
    JsObject *statics;   /* namespace statics (String.fromCharCode, ...); may be NULL */
    /* lazily-created .prototype object (mirrors JsClosure's prototype_obj);
     * instances of builtin types (RegExp, Date, Map, Set, ...) point their
     * [[Prototype]] here. NULL until first accessed via `Ctor.prototype`. */
    JsObject *prototype;
    JsValue bound;       /* captured value for bound natives */
    bool is_bound;
} JsNative;

/* ---- promises & microtasks ---- */

typedef enum JsPromiseState {
    JS_PROMISE_PENDING = 0,
    JS_PROMISE_FULFILLED = 1,
    JS_PROMISE_REJECTED = 2,
} JsPromiseState;

/*
 * A pending promise's subscribers. `then` reactions call handlers and settle
 * a derived promise; `await` reactions resume a suspended async fiber.
 */
typedef struct JsReaction JsReaction;
struct JsReaction {
    JsReaction *next;
    uint8_t kind; /* 0 = then, 1 = await */
    JsValue on_fulfilled; /* then: callable or undefined */
    JsValue on_rejected;  /* then: callable or undefined */
    struct JsPromise *result; /* then: derived promise (may be NULL) */
    struct JsFiber *fiber;    /* await: fiber to resume */
};

typedef struct JsPromise {
    JsGcCell gc;
    uint8_t state;
    JsValue value;     /* fulfillment value or rejection reason */
    JsReaction *reactions; /* non-empty only while PENDING */
} JsPromise;

/* A queued microtask. Raw allocation; the VM's queue is a GC root. */
typedef struct JsJob JsJob;
struct JsJob {
    JsJob *next;
    uint8_t kind;   /* 0 = then, 1 = await */
    bool fulfilled; /* settlement branch */
    JsValue value;  /* settlement value */
    /* then */
    JsValue on_fulfilled, on_rejected;
    struct JsPromise *result;
    /* await */
    struct JsFiber *fiber;
    bool is_throw;
};

typedef enum JsRunStatus {
    JS_RUN_DONE = 0,
    JS_RUN_ERROR = 1,
    JS_RUN_SUSPENDED = 2, /* parked at await */
} JsRunStatus;

/* ---- ES modules ---- */

typedef enum JsImportKind {
    JS_IMPORT_NAMED = 0,   /* import { x } / { x as y } */
    JS_IMPORT_DEFAULT = 1, /* import d from */
    JS_IMPORT_NS = 2,      /* import * as ns */
    JS_IMPORT_SIDE = 3,    /* import 'm' (no bindings) */
} JsImportKind;

typedef struct JsModuleImport {
    JsString *specifier;     /* module to import from */
    JsString *imported_name; /* export name in source (NULL for ns/side) */
    uint8_t kind;            /* JsImportKind */
    JsModule *source;        /* resolved at link */
} JsModuleImport;

/*
 * A re-export: `export * from`, `export * as ns from`, or
 * `export { x as y } from`. Applied at evaluation once `source` has run.
 */
typedef enum JsReExpKind {
    JS_REEXP_ALL = 0,   /* export * from 'm'  -> copy all named exports */
    JS_REEXP_NS = 1,    /* export * as ns from 'm' -> exports[ns] = m namespace */
    JS_REEXP_NAMED = 2, /* export { x as y } from 'm' -> exports[y] = m.exports[x] */
} JsReExpKind;

typedef struct JsStarExport {
    JsString *specifier;
    JsModule *source;
    uint8_t kind;
    JsString *imported; /* NAMED: source export name */
    JsString *exported; /* NS: binding name; NAMED: exported name */
} JsStarExport;

typedef enum JsModuleStatus {
    JS_MOD_FETCHING = 0, /* loader promise in flight; body/exports unknown */
    JS_MOD_UNLINKED = 1,
    JS_MOD_LINKING = 2,
    JS_MOD_LINKED = 3,
    JS_MOD_EVALUATING = 4,
    JS_MOD_EVALUATED = 5,
    JS_MOD_ERRORED = 6,
} JsModuleStatus;

struct JsModule {
    JsGcCell gc;
    JsString *specifier;      /* canonical specifier (cache/identity key) */
    JsFunctionCell *body;     /* compiled module body (NULL until compiled) */
    JsObject *exports;        /* namespace/exports object (live bindings) */
    JsModuleImport *imports;  /* import descriptors (indexed by GET_IMPORT) */
    uint32_t import_count;
    JsStarExport *stars;      /* export * from sources */
    uint32_t star_count;
    JsString **dep_specs;     /* all distinct dependency specifiers (raw) */
    uint32_t dep_spec_count;
    JsModule **deps;          /* resolved dependency modules (parallel to dep_specs) */
    uint32_t dep_count;
    uint8_t status;
    bool synthetic;           /* exports adopted from a loader-resolved object */
    JsValue eval_error;
    /* Settles once this module's source/bytecode/synthetic exports have
     * arrived and been compiled/adopted (dep_specs known; deps NOT yet
     * fetched) — a shallow per-module signal the graph loader composes.
     * Fulfills with the module itself (disguised as a JsValue), rejects with
     * the load error. Rooted through the registry for the module's lifetime. */
    JsPromise *fetch_promise;
};

/* Opaque host-provided module bytecode; bytes owned by the cell. */
typedef struct JsBytecode {
    JsGcCell gc;
    size_t length;
    uint8_t bytes[];
} JsBytecode;

typedef struct JsFrame {
    JsClosure *closure;
    uint32_t ip;
    uint32_t base; /* stack offset: locals at base..base+n_locals;
                      callee at base-2, this at base-1 */
    bool is_construct; /* frame entered via `new`: RETURN substitutes `this`
                           for a non-object return value */
} JsFrame;

typedef struct JsTryEntry {
    uint32_t frame_count; /* unwind to this many frames */
    uint32_t sp;
    uint32_t target;      /* handler ip in that frame's function */
} JsTryEntry;

/* Suspendable execution state; call frames never live on the C stack. */
struct JsFiber {
    JsGcCell gc;
    JsValue *stack;
    uint32_t sp, stack_cap;
    JsFrame *frames;
    uint32_t frame_count, frame_cap;
    JsTryEntry *trys;
    uint32_t try_count, try_cap;
    JsUpvalue *open_upvals;
    JsFiber *caller; /* fiber that invoked a native that re-entered; GC root */
    JsPromise *result_promise; /* async fiber: promise to settle on completion */
    JsValue error;
    bool failed;
    bool is_async;   /* body may suspend at await */
    uint32_t err_pos;
    uint64_t fuel; /* 0 = unlimited */
};

/* Weak set of interned strings, content-keyed. Sweep removes dead entries. */
typedef struct JsAtomTable {
    JsString **slots; /* NULL empty, JS_MAP_TOMBSTONE deleted */
    uint32_t count;
    uint32_t used;
    uint32_t capacity;
} JsAtomTable;

struct JsContext {
    JsVm *vm;
    JsContext *next;
    JsContext **prev_link;
    JsObject *globals;
    /* String/Number/Promise are primitives or a distinct GC kind (no
     * [[Prototype]] slot to hang a real chain off yet), so their methods
     * still live in a hidden per-context table that property lookup falls
     * back to directly (prototype-lite: invisible and immutable from
     * scripts). Array/RegExp/Date/Map/Set instances are JS_KIND_OBJECT
     * cells with a real `proto` field, so those use genuine, script-visible
     * X.prototype objects instead — see array_proto etc. below. */
    JsObject *string_methods;
    JsObject *number_methods;
    JsObject *promise_methods;
    /* Real, script-visible prototype objects — Ctor.prototype is this same
     * object, and instances' [[Prototype]] is set to it at allocation, so
     * property lookup reaches methods purely via the normal own-prop-miss
     * -> proto-chain walk in get_property, with no per-kind special case.
     * Cached here so allocation sites (js_array_new_cell, alloc_mapobj, ...)
     * don't need to look "Ctor.prototype" up through a property read on
     * every single instance they create. */
    JsObject *object_proto; /* root of the chain; its own ->proto is undefined
                             * (no [[Prototype]]), matching real JS's `null`.
                             * Created first in js_builtins_init, before
                             * anything below — see js_object_new's bootstrap
                             * comment in js_object.c. */
    JsObject *array_proto;
    JsObject *regexp_proto; /* NULL unless built with LAMASSU_HAS_REGEX */
    JsObject *date_proto;
    JsObject *map_proto;
    JsObject *set_proto;
    /* persistent REPL lexical scope: top-level let/const/function bindings
     * that survive across evaluations. repl_scope holds values (TDZ sentinel
     * for uninitialized); repl_const marks the const names. Both lazily
     * created and only used by REPL-mode compilation. */
    JsObject *repl_scope;
    JsObject *repl_const;
    /* module registry (cache by canonical specifier; all GC roots). The array
     * is the source of truth for iteration/marking; module_index is a lookup
     * accelerator (interned specifier -> module value) so resolving an import
     * is O(1) rather than a linear scan. Its keys/values are kept alive via the
     * array, so it needs no separate marking. */
    JsModule **modules;
    uint32_t module_count, module_cap;
    JsMap module_index;
    JsModuleLoader loader;          /* async module loader; may be NULL */
    JsModuleCanonicalizer canon;    /* sync specifier canonicalizer; may be NULL */
    void *loader_ud;                /* shared userdata for loader + canon */
    JsFiber *fiber;     /* current/last fiber; GC root */
    uint64_t fuel;      /* budget for new runs; 0 = unlimited */
    uint32_t error_pos; /* source offset of last runtime error */
    uint32_t reentry_depth; /* nested interpreter entries (native re-entry);
                             * bounds C-stack recursion the per-fiber frame
                             * limit can't see */
};

struct JsVm {
    JsReallocFn realloc_fn;
    void *alloc_ud;
    size_t bytes_live; /* all bytes obtained through js_realloc_raw */
    size_t heap_limit; /* 0 = unlimited; enforced at cell allocation */
    uint64_t rng_state; /* Math.random xorshift state */
    uint32_t hash_seed; /* per-VM string-hash seed; 0 unless the embedder set a
                         * seed, in which case it randomizes atom/property hashing
                         * to defeat HashDoS. Fixed for the VM's lifetime (unlike
                         * rng_state, which Math.random mutates). */
    uint32_t regexp_live; /* live compiled patterns (each ~sizeof(Program)) */

    /* GC */
    JsGcCell *cells;
    size_t cell_count;
    size_t gc_threshold;
    size_t gc_threshold_init;
    bool gc_stress;
    bool gc_running;
    JsGcCell **mark_stack;
    size_t mark_len, mark_cap;
    bool mark_overflow;
    JsValue **roots;
    size_t roots_len, roots_cap;

    JsAtomTable atoms;
    JsContext *contexts;

    /* microtask FIFO queue (raw allocations; a GC root) */
    JsJob *jobs_head, *jobs_tail;
    JsJob *jobs_free;    /* recycled job nodes */
    JsJob *running_job;  /* dequeued job currently executing; a GC root */
};

/* ---- value <-> cell ---- */

static inline JsValue js_value_from_cell(JsGcCell *c) {
    JsValue v;
    uint64_t tag;
    switch (c->kind) {
    case JS_KIND_STRING: tag = JS_TAG_STRING; break;
    case JS_KIND_FUNCTION:
    case JS_KIND_CLOSURE:
    case JS_KIND_NATIVE: tag = JS_TAG_FUNCTION; break;
    case JS_KIND_PROMISE: tag = JS_TAG_PROMISE; break;
    default: tag = JS_TAG_OBJECT; break;
    }
    v.bits = tag | ((uint64_t)(uintptr_t)c & JS_PAYLOAD_MASK);
    return v;
}

static inline JsGcCell *js_value_cell(JsValue v) {
    return (JsGcCell *)(uintptr_t)(v.bits & JS_PAYLOAD_MASK);
}

static inline bool js_value_is_cell(JsValue v) {
    uint64_t tag = v.bits & JS_TAG_MASK;
    return tag == JS_TAG_STRING || tag == JS_TAG_OBJECT || tag == JS_TAG_FUNCTION ||
           tag == JS_TAG_PROMISE;
}

static inline JsString *js_value_string(JsValue v) { return (JsString *)js_value_cell(v); }
static inline JsObject *js_value_object(JsValue v) { return (JsObject *)js_value_cell(v); }
static inline JsFunctionCell *js_value_function(JsValue v) {
    return (JsFunctionCell *)js_value_cell(v);
}
static inline JsPromise *js_value_promise(JsValue v) { return (JsPromise *)js_value_cell(v); }

/* Largest one-shot growth (by index-assignment gap, or a bare `Array(n)`
 * length/constructor call) a script may force before hitting a RangeError —
 * arrays have no holes here, so every slot up to the requested length is
 * eagerly materialized; this bounds the memory an untrusted script can make
 * one call allocate. Shared by js_interp.c (property-set path) and
 * js_builtins.c (the Array constructor). */
#define JS_MAX_ARRAY_GAP 4096u

/* js_object.c: array helpers used by the interpreter. new_cell takes the
 * context (not just the VM) so it can set the new array's [[Prototype]] to
 * ctx->array_proto — real arrays, not the "no [[Prototype]] at all" that a
 * bare vm-only allocator would leave them with. */
JsObject *js_array_new_cell(JsContext *ctx, uint32_t reserve);
bool js_array_append(JsVm *vm, JsObject *arr, JsValue v);
bool js_array_set_index(JsVm *vm, JsObject *arr, uint32_t idx, JsValue v);

/* ---- allocator (js_vm.c) ---- */

void *js_realloc_raw(JsVm *vm, void *ptr, size_t old_size, size_t new_size);

/* ---- GC (js_gc.c) ---- */

/* Safe point + allocate: may collect before allocating. */
JsGcCell *js_gc_new_cell(JsVm *vm, JsGcKind kind, size_t size);
void js_gc_maybe(JsVm *vm);
void js_gc_mark_value(JsVm *vm, JsValue v);
void js_gc_mark_cell(JsVm *vm, JsGcCell *c);
void js_gc_free_cell(JsVm *vm, JsGcCell *c, bool remove_atoms);

/* ---- map (js_map.c) ---- */

void    js_map_init(JsMap *m);
bool    js_map_set(JsVm *vm, JsMap *m, JsString *key, JsValue value);
JsValue js_map_get(const JsMap *m, const JsString *key, bool *found);
bool    js_map_delete(JsMap *m, const JsString *key);
void    js_map_free(JsVm *vm, JsMap *m);

/* ---- strings / atoms (js_string.c) ---- */

uint32_t  js_units_hash(const uint16_t *units, size_t len, uint32_t seed);
JsString *js_string_cell_new(JsVm *vm, const uint16_t *units, size_t len);
JsString *js_atoms_find(JsVm *vm, const uint16_t *units, size_t len, uint32_t hash);
JsString *js_intern_cell(JsVm *vm, JsString *s); /* canonical string, or NULL on OOM */
void      js_atoms_remove(JsVm *vm, JsString *s);
void      js_atoms_free(JsVm *vm);

#endif /* LAMASSU_INTERNAL_H */
