/*
 * Iterator objects: what Map/Set keys(), values() and entries() and String matchAll return,
 * and what for-of, spread and destructuring step through when given a Map or a Set. Each is
 * an object (obj_kind JS_OBJ_ITER) whose prototype holds `next`, as V8's %MapIteratorPrototype%,
 * %SetIteratorPrototype% and %RegExpStringIteratorPrototype% do; those three chain to one
 * %IteratorPrototype%, then Object.prototype.
 *
 * Map and Set iterators are live, as the spec has them: an entry added before the iterator
 * reaches the end is visited, one deleted before it is reached is not, and clear() sends the
 * iterator back to the start of whatever is added next. The collections keep their entries in
 * a dense array that a delete closes up, so the VM keeps a list of the collection iterators not
 * yet done (JsVm.live_iters) and a delete or clear moves the position of each one over that
 * collection (js_iter_on_delete / js_iter_on_clear). The list holds them weakly: the collector
 * drops the unmarked ones before it sweeps (js_iter_prune). An iterator leaves the list when it
 * is done, and a done iterator stays done.
 *
 * There is no Symbol, so iterability is the engine's own: strings, arrays, Maps, Sets and these
 * iterator objects (an iterator is its own iterable, as %IteratorPrototype%[@@iterator] makes it).
 * Natives step any of them through a JsIterCursor.
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
    JS_ITER_REGEXP, /* RegExp String Iterator: matchAll's matches, one exec per step */
} JsIterKind;

/* the prototypes, by family (JsContext.iter_protos) */
enum { JS_ITER_PROTO_MAP, JS_ITER_PROTO_SET, JS_ITER_PROTO_REGEXP, JS_ITER_PROTO_COUNT };

typedef struct JsIterObj {
    JsObject obj;     /* obj_kind == JS_OBJ_ITER; props hold expandos only */
    JsValue target;   /* the Map, Set or RegExp; undefined once done */
    JsValue subject;  /* JS_ITER_REGEXP: the string searched; else undefined */
    uint32_t index;   /* the next entry's position, or (regexp) where the next search starts */
    uint8_t kind;     /* JsIterKind */
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

/* The prototypes (called from js_builtins_init, before Map and Set exist). */
bool js_iter_builtins_init(JsContext *ctx);

/*
 * A cursor over any iterable, for natives that take one (Array.from, Object.fromEntries, the
 * Map and Set constructors). SRC is the string, array or iterator object being stepped; the
 * caller roots it (js_gc_protect(&cur.src)) for as long as it steps. Arrays and strings are read
 * live, as their iterators are; a string steps by code point.
 */
typedef struct JsIterCursor {
    JsValue src;
    uint32_t i;
} JsIterCursor;

/* An array or string cursor at this position has finished (and stays finished). */
#define JS_ITER_DONE_INDEX 4294967294u

/* 1 opened, 0 V is not iterable (nothing thrown; the caller words the error), -1 OOM. */
int js_iter_open(JsContext *ctx, JsValue v, JsIterCursor *c);
/* As js_iterobj_step. */
int js_iter_next(JsContext *ctx, JsIterCursor *c, JsValue *out);

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
