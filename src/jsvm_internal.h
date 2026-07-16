#ifndef JSVM_INTERNAL_H
#define JSVM_INTERNAL_H

#include "jsvm.h"

/*
 * The core is freestanding: no libc calls anywhere. The mem* functions are
 * the one exception (compilers emit them for struct copies regardless), so
 * freestanding builds get prototypes here and definitions from
 * js_wasm_shim.c.
 */
#ifdef JSVM_FREESTANDING
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
#else
#include <string.h>
#endif

typedef enum JsGcKind {
    JS_KIND_STRING = 1,
    JS_KIND_OBJECT = 2,
    JS_KIND_FUNCTION = 3, /* compiled prototype (bytecode + consts) */
    JS_KIND_FIBER = 4,
    JS_KIND_CLOSURE = 5,  /* callable: prototype + captured upvalues */
    JS_KIND_UPVALUE = 6,
    JS_KIND_NATIVE = 7,   /* callable host function */
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
} JsObjKind;

typedef struct JsObject {
    JsGcCell gc;
    uint8_t obj_kind;
    JsMap props;
    /* JS_OBJ_ARRAY: dense elements; sparse writes fall back to props. */
    JsValue *elems;
    uint32_t elem_count, elem_cap;
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
    JsString *name; /* may be NULL */
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
    JsUpvalue *upvals[];
} JsClosure;

typedef struct JsNative {
    JsGcCell gc;
    JsNativeFn fn;
    void *ud;
    JsString *name; /* may be NULL */
} JsNative;

typedef struct JsFrame {
    JsClosure *closure;
    uint32_t ip;
    uint32_t base; /* stack offset: locals at base..base+n_locals;
                      callee at base-2, this at base-1 */
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
    JsValue error;
    bool failed;
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
    /* hidden builtin method tables; property lookup falls back to these
     * (prototype-lite: invisible and immutable from scripts) */
    JsObject *string_methods;
    JsObject *array_methods;
    JsObject *number_methods;
    JsFiber *fiber;     /* current/last fiber; GC root */
    uint64_t fuel;      /* budget for new runs; 0 = unlimited */
    uint32_t error_pos; /* source offset of last runtime error */
};

struct JsVm {
    JsReallocFn realloc_fn;
    void *alloc_ud;
    size_t bytes_live; /* all bytes obtained through js_realloc_raw */
    size_t heap_limit; /* 0 = unlimited; enforced at cell allocation */
    uint64_t rng_state; /* Math.random xorshift state */

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
    return tag == JS_TAG_STRING || tag == JS_TAG_OBJECT || tag == JS_TAG_FUNCTION;
}

static inline JsString *js_value_string(JsValue v) { return (JsString *)js_value_cell(v); }
static inline JsObject *js_value_object(JsValue v) { return (JsObject *)js_value_cell(v); }
static inline JsFunctionCell *js_value_function(JsValue v) {
    return (JsFunctionCell *)js_value_cell(v);
}

/* js_object.c: array helpers used by the interpreter */
JsObject *js_array_new_cell(JsVm *vm, uint32_t reserve);
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

uint32_t  js_units_hash(const uint16_t *units, size_t len);
JsString *js_string_cell_new(JsVm *vm, const uint16_t *units, size_t len);
JsString *js_atoms_find(JsVm *vm, const uint16_t *units, size_t len, uint32_t hash);
JsString *js_intern_cell(JsVm *vm, JsString *s); /* canonical string, or NULL on OOM */
void      js_atoms_remove(JsVm *vm, JsString *s);
void      js_atoms_free(JsVm *vm);

#endif /* JSVM_INTERNAL_H */
