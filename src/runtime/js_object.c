#include "lamassu_internal.h"

JsValue js_object_new(JsContext *ctx) {
    JsGcCell *c = js_gc_new_cell(ctx->vm, JS_KIND_OBJECT, sizeof(JsObject));
    if (!c)
        return js_undefined();
    JsObject *o = (JsObject *)c;
    o->obj_kind = JS_OBJ_PLAIN;
    js_map_init(&o->props);
    o->elems = NULL;
    o->elem_count = o->elem_cap = 0;
    /* Bootstrap case: while js_builtins_init is creating ctx->object_proto
     * itself, ctx->object_proto is still NULL here, so this correctly
     * produces the one object with no [[Prototype]] — matching real JS's
     * `Object.getPrototypeOf(Object.prototype) === null`. Every other call,
     * made after that assignment, chains to it normally. */
    o->proto = ctx->object_proto ? js_value_from_cell(&ctx->object_proto->gc) : js_undefined();
    return js_value_from_cell(c);
}

JsObject *js_array_new_cell(JsContext *ctx, uint32_t reserve) {
    JsVm *vm = ctx->vm;
    JsGcCell *c = js_gc_new_cell(vm, JS_KIND_OBJECT, sizeof(JsObject));
    if (!c)
        return NULL;
    JsObject *o = (JsObject *)c;
    o->obj_kind = JS_OBJ_ARRAY;
    js_map_init(&o->props);
    o->elems = NULL;
    o->elem_count = o->elem_cap = 0;
    o->proto = ctx->array_proto ? js_value_from_cell(&ctx->array_proto->gc) : js_undefined();
    if (reserve) {
        o->elems = js_realloc_raw(vm, NULL, 0, (size_t)reserve * sizeof(JsValue));
        if (!o->elems)
            return NULL;
        o->elem_cap = reserve;
    }
    return o;
}

static bool js_array_reserve(JsVm *vm, JsObject *arr, uint32_t want) {
    if (want <= arr->elem_cap)
        return true;
    uint32_t ncap = arr->elem_cap ? arr->elem_cap * 2 : 8;
    while (ncap < want)
        ncap *= 2;
    JsValue *ne = js_realloc_raw(vm, arr->elems,
                                 (size_t)arr->elem_cap * sizeof(JsValue),
                                 (size_t)ncap * sizeof(JsValue));
    if (!ne)
        return false;
    arr->elems = ne;
    arr->elem_cap = ncap;
    return true;
}

bool js_array_append(JsVm *vm, JsObject *arr, JsValue v) {
    /* Root v across the reserve: growth reallocates through js_realloc_raw,
     * which can collect, and callers routinely pass a freshly-allocated cell
     * that is otherwise only held in a C local. */
    if (!js_gc_protect(vm, &v))
        return false;
    bool ok = js_array_reserve(vm, arr, arr->elem_count + 1);
    if (ok)
        arr->elems[arr->elem_count++] = v;
    js_gc_unprotect(vm, &v);
    return ok;
}

bool js_array_set_index(JsVm *vm, JsObject *arr, uint32_t idx, JsValue v) {
    if (idx < arr->elem_count) {
        arr->elems[idx] = v;
        return true;
    }
    if (!js_array_reserve(vm, arr, idx + 1))
        return false;
    for (uint32_t i = arr->elem_count; i < idx; i++)
        arr->elems[i] = js_undefined();
    arr->elems[idx] = v;
    arr->elem_count = idx + 1;
    return true;
}

/*
 * Canonical atom for a read-only key lookup: an uninterned key string with
 * no canonical twin cannot name an existing property.
 */
static JsString *js_object_key_lookup(JsVm *vm, JsValue key) {
    if (!js_is_string(key))
        return NULL;
    JsString *s = js_value_string(key);
    if (s->interned)
        return s;
    return js_atoms_find(vm, s->units, s->length, s->hash);
}

JsValue js_object_get(JsVm *vm, JsValue obj, JsValue key) {
    if (!js_is_object(obj))
        return js_undefined();
    JsString *k = js_object_key_lookup(vm, key);
    if (!k)
        return js_undefined();
    bool found;
    return js_map_get(&js_value_object(obj)->props, k, &found);
}

bool js_object_set(JsVm *vm, JsValue obj, JsValue key, JsValue value) {
    if (!js_is_object(obj) || !js_is_string(key))
        return false;
    js_gc_maybe(vm); /* safe point: obj/key/value are caller-rooted */
    JsString *k = js_intern_cell(vm, js_value_string(key));
    if (!k)
        return false;
    return js_map_set(vm, &js_value_object(obj)->props, k, value);
}

bool js_object_delete(JsVm *vm, JsValue obj, JsValue key) {
    if (!js_is_object(obj))
        return false;
    JsString *k = js_object_key_lookup(vm, key);
    if (!k)
        return false;
    return js_map_delete(&js_value_object(obj)->props, k);
}

size_t js_object_size(JsValue obj) {
    return js_is_object(obj) ? js_value_object(obj)->props.count : 0;
}
