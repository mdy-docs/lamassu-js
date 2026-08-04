/*
 * The JS-level Set collection. Same storage/equality tradeoffs as Map
 * (js_mapobj.h): a dense array scanned linearly (O(n) add/has/delete, not
 * a hash table) with SameValueZero equality. Always available, no libc
 * dependency.
 *
 * A set is a JS_KIND_OBJECT cell with obj_kind == JS_OBJ_SET whose cell
 * embeds the items array directly (mirrors JsMapObj/JsRegExp/JsDateObject).
 */
#ifndef JS_SETOBJ_H
#define JS_SETOBJ_H

#include "lamassu_internal.h"

typedef struct JsSetObj {
    JsObject obj; /* obj_kind == JS_OBJ_SET; props hold expandos only */
    JsValue *items;
    uint32_t count, cap;
    /* Open-addressed hash index over `items` (hash(value) -> position); makes
     * add/has O(1). See js_valindex.h. NULL/0 until the first insert. */
    int32_t *index;
    uint32_t index_cap;
} JsSetObj;

static inline bool js_setobj_is(JsValue v) {
    return js_is_object(v) && js_value_object(v)->obj_kind == JS_OBJ_SET;
}

/* GC hooks (js_gc.c). Release frees the owned items array and returns the
 * true cell size. */
void   js_setobj_mark(JsVm *vm, JsObject *o);
size_t js_setobj_release(JsVm *vm, JsObject *o);

/* Synthesized `size` property (js_interp.c get_property), mirroring Map's. */
bool js_setobj_prop_get(JsObject *o, JsValue key, JsValue *out, bool *handled);

/* Snapshots in insertion order — used for values()/keys() and for
 * `for (const x of set)`, which this engine implements by substituting
 * this snapshot for the set at JS_OP_ITER_NEW (a Set's default iterator is
 * its values, unlike Map's, which is its entries). NULL on OOM. */
JsObject *js_setobj_values_array(JsContext *ctx, JsObject *o);

/* Snapshots [v, v] pairs, matching Set.prototype.entries()'s spec'd shape
 * (kept symmetric with Map's entries()). NULL on OOM. */
JsObject *js_setobj_entries_array(JsContext *ctx, JsObject *o);

/* Installs the Set global and a real, script-visible Set.prototype (every
 * instance's [[Prototype]] points there — see js_setobj.c). Called from
 * js_builtins_init. */
bool js_setobj_builtins_init(JsContext *ctx);

#endif /* JS_SETOBJ_H */
