/*
 * Iterator objects and the iterator protocol.
 *
 * Every iterator a script can hold is an object of obj_kind JS_OBJ_ITER whose prototype holds
 * `next` (and, for the helpers and Iterator.from's wrapper, `return`): Map/Set keys(), values()
 * and entries(), Array.prototype keys(), values() and entries(), String matchAll, Iterator.from of
 * a string, the Iterator helpers (map, filter, take, drop, flatMap, Iterator.concat) and
 * Iterator.from's wrapper of an iterator that does not inherit from Iterator.prototype. Their
 * prototypes (V8's %MapIteratorPrototype%, %ArrayIteratorPrototype%, %IteratorHelperPrototype%,
 * ...) chain to %IteratorPrototype% (Iterator.prototype), then Object.prototype.
 *
 * Map and Set iterators are live, as the spec has them: an entry added before the iterator
 * reaches the end is visited, one deleted before it is reached is not, and clear() sends the
 * iterator back to the start of whatever is added next. The collections keep their entries in
 * a dense array that a delete closes up, so the VM keeps a list of the collection iterators not
 * yet done (JsVm.live_iters) and a delete or clear moves the position of each one over that
 * collection (js_iter_on_delete / js_iter_on_clear). The list holds them weakly: the collector
 * drops the unmarked ones before it sweeps (js_iter_prune). An iterator leaves the list when it
 * is done, and a done iterator stays done. Array iterators read the array (or array-like) live.
 *
 * There is no Symbol, so iterability is the engine's own: strings, arrays, Maps, Sets, the
 * iterator objects above, and any object that inherits from Iterator.prototype (whose
 * [Symbol.iterator] returns the object itself, so its own `next` is what steps it). Stepping one of
 * those goes through an Iterator Record (JS_ITER_RECORD): an internal JsIterObj holding the object
 * and the `next` read from it once, as GetIterator does; it never reaches a script.
 *
 * Natives step any iterable through a JsIterCursor, and close it (IteratorClose: call `return`)
 * when they stop before the end; the interpreter does the same for for-of, spread and
 * destructuring (ITER_CLOSE, ITER_CLOSE_THROW, ITER_CLOSE_LOCAL).
 */
#ifndef JS_ITER_H
#define JS_ITER_H

#include "lamassu_internal.h"

typedef enum JsIterKind {
    JS_ITER_MAP_KEYS,
    JS_ITER_MAP_VALUES,
    JS_ITER_MAP_ENTRIES,
    JS_ITER_SET_VALUES,
    JS_ITER_SET_ENTRIES,
    JS_ITER_REGEXP,        /* RegExp String Iterator: matchAll's matches, one exec per step */
    JS_ITER_ARRAY_KEYS,    /* Array Iterator over an array, an array-like or a string's units */
    JS_ITER_ARRAY_VALUES,
    JS_ITER_ARRAY_ENTRIES,
    JS_ITER_STRING,        /* String Iterator: a string's code points */
    JS_ITER_WRAP,          /* Iterator.from's wrapper: target the iterator, subject its next */
    JS_ITER_HELPER,        /* an Iterator helper (JsIterHelperOp) */
    JS_ITER_RECORD,        /* internal: an Iterator Record (target, subject its next) */
} JsIterKind;

/* the prototypes, by family (JsContext.iter_protos) */
enum {
    JS_ITER_PROTO_MAP,
    JS_ITER_PROTO_SET,
    JS_ITER_PROTO_REGEXP,
    JS_ITER_PROTO_ARRAY,
    JS_ITER_PROTO_STRING,
    JS_ITER_PROTO_HELPER,
    JS_ITER_PROTO_WRAP,
};

/* what an Iterator helper does (JsIterObj.op) */
typedef enum JsIterHelperOp {
    JS_HELPER_MAP,
    JS_HELPER_FILTER,
    JS_HELPER_TAKE,
    JS_HELPER_DROP,
    JS_HELPER_FLATMAP,
    JS_HELPER_CONCAT,
} JsIterHelperOp;

/* an Iterator helper's generator state (JsIterObj.state) */
enum { JS_HELPER_START, JS_HELPER_YIELD, JS_HELPER_RUNNING, JS_HELPER_DONE };

typedef struct JsIterObj {
    JsObject obj;     /* obj_kind == JS_OBJ_ITER; props hold expandos only */
    /* what it iterates; undefined once done. A helper: the underlying iterator, as a cursor's
     * src (an iterator object or a record), stepped from `index` */
    JsValue target;
    /* JS_ITER_REGEXP: the string searched. A record or wrapper: the next method. A helper: its
     * callback (concat: the array of iterables). Else undefined */
    JsValue subject;
    /* a flatMap or concat helper: the inner iterator being stepped (a cursor's src, from
     * inner_index), or undefined */
    JsValue inner;
    uint32_t index;       /* the next entry's position, or (regexp) where the next search starts */
    uint32_t inner_index;
    /* a helper: the count passed to its callback; take/drop: what is left to take or drop
     * (Infinity: no limit); concat: the next iterable's index */
    double count;
    uint8_t kind;     /* JsIterKind */
    uint8_t op;       /* JS_ITER_HELPER: JsIterHelperOp */
    uint8_t state;    /* JS_ITER_HELPER: JS_HELPER_* */
    bool live;        /* on JsVm.live_iters */
    struct JsIterObj *live_prev, *live_next;
} JsIterObj;

static inline bool js_iterobj_is(JsValue v) {
    return js_is_object(v) && js_value_object(v)->obj_kind == JS_OBJ_ITER;
}

/* A new iterator over TARGET (and SUBJECT for a regexp one), starting at INDEX. Both values must
 * be rooted by the caller. Undefined on OOM. */
JsValue js_iterobj_new(JsContext *ctx, JsIterKind kind, JsValue target, JsValue subject,
                       uint32_t index);

/* The iterator's next value: 1 and *out the value, 0 when done (and from then on), -1 thrown
 * with *out the error. IT must be rooted by the caller. */
int js_iterobj_step(JsContext *ctx, JsIterObj *it, JsValue *out);

/* A Map or Set lost the entry at POS (the later ones moved down one), or all of them. */
void js_iter_on_delete(JsVm *vm, const JsObject *coll, uint32_t pos);
void js_iter_on_clear(JsVm *vm, const JsObject *coll);

/* GC hooks (js_gc.c): prune runs after marking, before the sweep. */
void   js_iter_prune(JsVm *vm);
void   js_iterobj_mark(JsVm *vm, JsObject *o);
size_t js_iterobj_release(JsVm *vm, JsObject *o);

/* The prototypes, the Iterator global and its helpers (called from js_builtins_init, before
 * Map and Set exist). */
bool js_iter_builtins_init(JsContext *ctx);

/* Array.prototype keys/values/entries (JS_ITER_ARRAY_*): an Array Iterator over ToObject(this). */
bool js_array_iter_method(JsContext *ctx, JsValue tv, JsIterKind kind, JsValue *r);

/*
 * A cursor over any iterable, for natives that take one (Array.from, Object.fromEntries, the
 * Map and Set constructors, the helpers). SRC is the string, array or iterator object (a record
 * for an object of a script's) being stepped; the caller roots it (js_gc_protect(&cur.src)) for
 * as long as it steps. Arrays and strings are read live, as their iterators are; a string steps
 * by code point.
 */
typedef struct JsIterCursor {
    JsValue src;
    uint32_t i;
} JsIterCursor;

/* A cursor at this position has finished (and stays finished): its iterator said done, or its
 * next threw, so there is nothing to close. */
#define JS_ITER_DONE_INDEX 4294967294u

/* Whether V is iterable: a string, an array, a Map, a Set, an iterator object, or an object
 * that inherits from Iterator.prototype. */
bool js_iter_is_iterable(JsContext *ctx, JsValue v);

/* 1 opened, 0 V is not iterable (nothing thrown; the caller words the error), -1 OOM. */
int js_iter_open(JsContext *ctx, JsValue v, JsIterCursor *c);
/* As js_iterobj_step; a cursor that ends (0) or throws (-1) is left done. */
int js_iter_next(JsContext *ctx, JsIterCursor *c, JsValue *out);

/* IteratorClose of what the cursor steps, unless it is done (it is done afterwards). THROWING:
 * the completion that stops it is a throw, so whatever return() does is ignored and true comes
 * back. SYNTAX: for-of or destructuring is closing it, which words a non-object result V8's way
 * for those ("Iterator result x is not an object"; a builtin's is "return called on
 * non-object"). False thrown, *err the error. */
bool js_iter_close(JsContext *ctx, JsIterCursor *c, bool throwing, bool syntax, JsValue *err);

/* {value, done}, as next() returns it. Undefined on OOM. */
JsValue js_iter_result(JsContext *ctx, JsValue value, bool done);


/* An entry of new Map(iterable) or Object.fromEntries: ENTRY must be an object, and its "0"
 * and "1" are the key and value. False thrown, *err the error. ENTRY must be rooted. */
bool js_iter_entry(JsContext *ctx, JsValue entry, JsValue *k, JsValue *v, JsValue *err);

/* new Map(iterable) / new Set(iterable), as V8 has it: the collection's own `set` (Map, PAIRS)
 * or `add` is looked up first and must be a function, then each value of ITERABLE (an entry's
 * key and value for a Map) goes through it — straight to BUILTIN when it is still the builtin
 * method. COLL must be rooted. False thrown, *r the error. */
bool js_collection_fill(JsContext *ctx, JsValue coll, JsValue iterable, bool pairs,
                        JsNativeFn builtin, JsValue *r);

/* The error a native throws for a value it cannot iterate, as V8 words it. */
JsValue js_iter_not_iterable(JsContext *ctx, JsValue v);

#endif /* JS_ITER_H */
