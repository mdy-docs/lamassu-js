/*
 * The JS-level Map collection (distinct from JsMap in lamassu_internal.h,
 * which is the internal string-keyed map object property storage uses).
 * Always available, no libc dependency.
 *
 * Storage is a pair of parallel dense arrays in insertion order, scanned
 * linearly for get/has/set/delete — O(n) rather than a hash table, the
 * same "simple and obviously correct over clever" tradeoff this codebase
 * already makes for Array.prototype.sort (documented as O(n^2)). Key
 * equality is SameValueZero: NaN equals NaN, +0 equals -0, strings compare
 * by content (this engine doesn't intern every string, only property
 * keys), objects/functions by reference identity.
 *
 * A map is a JS_KIND_OBJECT cell with obj_kind == JS_OBJ_MAP whose cell
 * embeds the two arrays directly (mirrors JsRegExp/JsDateObject).
 */
#ifndef JS_MAPOBJ_H
#define JS_MAPOBJ_H

#include "lamassu_internal.h"

typedef struct JsMapObj {
    JsObject obj; /* obj_kind == JS_OBJ_MAP; props hold expandos only */
    JsValue *keys;
    JsValue *values; /* parallel to keys; same length/capacity */
    uint32_t count, cap;
    /* Open-addressed hash index over `keys` (hash(key) -> position); makes
     * get/has/set O(1). See js_valindex.h. NULL/0 until the first insert. */
    int32_t *index;
    uint32_t index_cap;
} JsMapObj;

static inline bool js_mapobj_is(JsValue v) {
    return js_is_object(v) && js_value_object(v)->obj_kind == JS_OBJ_MAP;
}

/* GC hooks (js_gc.c). Release frees the owned key/value arrays and
 * returns the true cell size. */
void   js_mapobj_mark(JsVm *vm, JsObject *o);
size_t js_mapobj_release(JsVm *vm, JsObject *o);

/* Synthesized `size` property (js_interp.c get_property), mirroring how
 * RegExp's source/flags/lastIndex are synthesized rather than stored in
 * the props map. *handled is false for any other key. */
bool js_mapobj_prop_get(JsObject *o, JsValue key, JsValue *out, bool *handled);

/* Snapshots entries as a real (and therefore iterable) array of [k, v]
 * pairs, in insertion order — used for `for (const [k, v] of map)`, which
 * this engine implements by substituting this snapshot for the map at
 * JS_OP_ITER_NEW rather than a lazy MapIterator. NULL on OOM. */
JsObject *js_mapobj_entries_array(JsContext *ctx, JsObject *o);

/* Installs the Map global and a real, script-visible Map.prototype (every
 * instance's [[Prototype]] points there — see js_mapobj.c). Called from
 * js_builtins_init. */
bool js_mapobj_builtins_init(JsContext *ctx);

#endif /* JS_MAPOBJ_H */
