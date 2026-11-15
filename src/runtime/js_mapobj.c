/*
 * Map: construction, mutators, lookups, and its iterators (js_iter.h). See js_mapobj.h for the
 * storage and equality.
 */
#include "js_bytecode.h"
#include "js_error.h"
#include "js_iter.h"
#include "js_mapobj.h"
#include "js_valindex.h"

#define ARG(i) ((i) < argc ? args[i] : js_undefined())

static bool nthrow(JsContext *ctx, JsValue *r, const char *msg) {
    *r = js_error_from_ascii(ctx, msg); /* Error object; string on OOM */
    return false;
}

/* ---- storage ---- */

static bool mapobj_reserve(JsVm *vm, JsMapObj *m, uint32_t want) {
    if (want <= m->cap)
        return true;
    uint32_t ncap = m->cap ? m->cap * 2 : 8;
    while (ncap < want)
        ncap *= 2;
    JsValue *nk = js_realloc_raw(vm, m->keys, (size_t)m->cap * sizeof(JsValue),
                                 (size_t)ncap * sizeof(JsValue));
    if (!nk)
        return false;
    m->keys = nk;
    JsValue *nv = js_realloc_raw(vm, m->values, (size_t)m->cap * sizeof(JsValue),
                                 (size_t)ncap * sizeof(JsValue));
    if (!nv)
        return false; /* m->keys already grown; harmless, freed at cell teardown */
    m->values = nv;
    m->cap = ncap;
    return true;
}

static int64_t mapobj_find(const JsMapObj *m, JsValue key) {
    return js_valindex_find(m->index, m->index_cap, m->keys, key);
}

/* Existing key: value updated in place, insertion order unchanged (matches
 * spec). New key: appended to the ordered arrays and recorded in the index. */
static bool mapobj_set(JsVm *vm, JsMapObj *m, JsValue key, JsValue value) {
    key = js_normalize_map_key(key); /* store +0, never -0 */
    int64_t idx = mapobj_find(m, key);
    if (idx >= 0) {
        m->values[(uint32_t)idx] = value;
        return true;
    }
    if (!mapobj_reserve(vm, m, m->count + 1))
        return false;
    m->keys[m->count] = key;
    m->values[m->count] = value;
    m->count++;
    if (!js_valindex_add(vm, &m->index, &m->index_cap, m->keys, m->count, m->count - 1)) {
        m->count--; /* roll back the append; index unchanged */
        return false;
    }
    return true;
}

/* Shifts later entries left to close the gap, preserving insertion order
 * (this is the O(n) storage's delete cost; sets/gets/has pay linear scan
 * instead). */
static bool mapobj_delete(JsVm *vm, JsMapObj *m, JsValue key) {
    int64_t idx = mapobj_find(m, key);
    if (idx < 0)
        return false;
    uint32_t i = (uint32_t)idx;
    for (uint32_t k = i; k + 1 < m->count; k++) {
        m->keys[k] = m->keys[k + 1];
        m->values[k] = m->values[k + 1];
    }
    m->count--;
    /* The shift renumbered every entry from i on, so the position index must
     * be rebuilt against the new ordering, and the iterators past i move back. */
    js_valindex_rebuild(m->index, m->index_cap, m->keys, m->count);
    if (vm->live_iters)
        js_iter_on_delete(vm, &m->obj, i);
    return true;
}

static bool alloc_mapobj(JsContext *ctx, JsValue *out) {
    JsGcCell *c = js_gc_new_cell(ctx->vm, JS_KIND_OBJECT, sizeof(JsMapObj));
    if (!c)
        return false;
    JsMapObj *m = (JsMapObj *)c;
    m->obj.obj_kind = JS_OBJ_MAP;
    m->obj.obj_flags = 0;
    js_map_init(&m->obj.props);
    m->obj.elems = NULL;
    m->obj.elem_count = m->obj.elem_cap = 0;
    m->obj.proto = ctx->map_proto ? js_value_from_cell(&ctx->map_proto->gc) : js_undefined();
    m->keys = NULL;
    m->values = NULL;
    m->count = m->cap = 0;
    m->index = NULL;
    m->index_cap = 0;
    *out = js_value_from_cell(c);
    return true;
}

static bool this_map(JsContext *ctx, JsValue tv, JsMapObj **out, JsValue *r) {
    if (!js_mapobj_is(tv))
        return (*r = js_error_receiver(ctx, "Map", tv)), false;
    *out = (JsMapObj *)js_value_object(tv);
    return true;
}

/* ---- constructor ---- */

static bool map_set(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r);

static bool g_Map(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    if (!js_native_is_new(ctx))
        return nthrow(ctx, r, "TypeError: Constructor Map requires 'new'");
    JsValue src = ARG(0);
    JsValue mv;
    if (!alloc_mapobj(ctx, &mv))
        return nthrow(ctx, r, "out of memory");
    if (!js_is_undefined(src) && !js_is_null(src)) {
        js_gc_protect(ctx->vm, &mv);
        bool ok = js_collection_fill(ctx, mv, src, true, map_set, r);
        js_gc_unprotect(ctx->vm, &mv);
        if (!ok)
            return false;
    }
    *r = mv;
    return true;
}

/* ---- methods ---- */

static bool map_set(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsMapObj *m;
    if (!this_map(ctx, tv, &m, r))
        return false;
    if (!mapobj_set(ctx->vm, m, ARG(0), ARG(1)))
        return nthrow(ctx, r, "out of memory");
    *r = tv; /* chainable */
    return true;
}

static bool map_get(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsMapObj *m;
    if (!this_map(ctx, tv, &m, r))
        return false;
    int64_t idx = mapobj_find(m, ARG(0));
    *r = idx >= 0 ? m->values[(uint32_t)idx] : js_undefined();
    return true;
}

static bool map_has(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsMapObj *m;
    if (!this_map(ctx, tv, &m, r))
        return false;
    *r = js_bool(mapobj_find(m, ARG(0)) >= 0);
    return true;
}

static bool map_delete(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsMapObj *m;
    if (!this_map(ctx, tv, &m, r))
        return false;
    *r = js_bool(mapobj_delete(ctx->vm, m, ARG(0)));
    return true;
}

static bool map_clear(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args; (void)argc;
    JsMapObj *m;
    if (!this_map(ctx, tv, &m, r))
        return false;
    m->count = 0;
    js_valindex_rebuild(m->index, m->index_cap, m->keys, 0); /* empty the index */
    if (ctx->vm->live_iters)
        js_iter_on_clear(ctx->vm, &m->obj);
    *r = js_undefined();
    return true;
}

/* callback(value, key, map), live as V8's is: it walks the map with an iterator (js_iter.h), so
 * an entry the callback adds is visited and one it deletes before reaching it is not. The
 * entry is re-read after each step, never cached across the js_call. */
static bool map_forEach(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsMapObj *m;
    if (!this_map(ctx, tv, &m, r))
        return false;
    if (!js_is_function(ARG(0)))
        return (*r = js_error_fmt(ctx, "TypeError: %t is not a function", ARG(0))), false;
    JsValue cb = ARG(0), thisArg = ARG(1), mapv = tv;
    JsValue itv = js_iterobj_new(ctx, JS_ITER_MAP_KEYS, mapv, js_undefined(), 0);
    if (js_is_undefined(itv))
        return nthrow(ctx, r, "out of memory");
    js_gc_protect(ctx->vm, &itv);
    js_gc_protect(ctx->vm, &cb);
    js_gc_protect(ctx->vm, &thisArg);
    JsIterObj *it = (JsIterObj *)js_value_object(itv);
    bool ok = true;
    JsValue key;
    int rc;
    while ((rc = js_iterobj_step(ctx, it, &key)) > 0) {
        JsValue argv[3] = {m->values[it->index - 1], key, mapv};
        JsValue cbres;
        if (!js_call(ctx, cb, thisArg, argv, 3, &cbres)) {
            *r = cbres;
            ok = false;
            break;
        }
    }
    js_gc_unprotect(ctx->vm, &thisArg);
    js_gc_unprotect(ctx->vm, &cb);
    js_gc_unprotect(ctx->vm, &itv);
    if (!ok)
        return false;
    *r = js_undefined();
    return true;
}

static bool map_iterator(JsContext *ctx, JsValue tv, JsValue *r, JsIterKind kind) {
    JsMapObj *m;
    if (!this_map(ctx, tv, &m, r))
        return false;
    *r = js_iterobj_new(ctx, kind, tv, js_undefined(), 0);
    return js_is_undefined(*r) ? nthrow(ctx, r, "out of memory") : true;
}

static bool map_keys(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args; (void)argc;
    return map_iterator(ctx, tv, r, JS_ITER_MAP_KEYS);
}

static bool map_values(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args; (void)argc;
    return map_iterator(ctx, tv, r, JS_ITER_MAP_VALUES);
}

static bool map_entries(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args; (void)argc;
    return map_iterator(ctx, tv, r, JS_ITER_MAP_ENTRIES);
}

/* ---- synthesized `size` (js_interp.c get_property) ---- */

bool js_mapobj_prop_get(JsObject *o, JsValue key, JsValue *out, bool *handled) {
    *handled = false;
    if (!js_is_string(key))
        return true;
    JsString *ks = js_value_string(key);
    if (ks->length == 4 && ks->units[0] == 's' && ks->units[1] == 'i' &&
        ks->units[2] == 'z' && ks->units[3] == 'e') {
        *out = js_number(((JsMapObj *)o)->count);
        *handled = true;
    }
    return true;
}

/* ---- GC ---- */

void js_mapobj_mark(JsVm *vm, JsObject *o) {
    JsMapObj *m = (JsMapObj *)o;
    for (uint32_t i = 0; i < m->count; i++) {
        js_gc_mark_value(vm, m->keys[i]);
        js_gc_mark_value(vm, m->values[i]);
    }
}

size_t js_mapobj_release(JsVm *vm, JsObject *o) {
    JsMapObj *m = (JsMapObj *)o;
    js_realloc_raw(vm, m->keys, (size_t)m->cap * sizeof(JsValue), 0);
    js_realloc_raw(vm, m->values, (size_t)m->cap * sizeof(JsValue), 0);
    js_realloc_raw(vm, m->index, (size_t)m->index_cap * sizeof(int32_t), 0);
    return sizeof(JsMapObj);
}

/* ---- getOrInsert, getOrInsertComputed, Map.groupBy ---- */

/* m.getOrInsert(key, value): the value under KEY, after setting it to VALUE if it had none */
static bool map_getOrInsert(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsMapObj *m;
    if (!this_map(ctx, tv, &m, r))
        return false;
    int64_t idx = mapobj_find(m, js_normalize_map_key(ARG(0)));
    if (idx >= 0) {
        *r = m->values[(uint32_t)idx];
        return true;
    }
    if (!mapobj_set(ctx->vm, m, ARG(0), ARG(1)))
        return nthrow(ctx, r, "out of memory");
    *r = ARG(1);
    return true;
}

/* m.getOrInsertComputed(key, callback): the value under KEY, or callback(key) set under it (over
 * whatever the callback itself set there) */
static bool map_getOrInsertComputed(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsMapObj *m;
    if (!this_map(ctx, tv, &m, r))
        return false;
    if (!js_is_function(ARG(1)))
        return nthrow(ctx, r, "TypeError: Map.prototype.getOrInsertComputed is not a function");
    JsValue key = js_normalize_map_key(ARG(0));
    int64_t idx = mapobj_find(m, key);
    if (idx >= 0) {
        *r = m->values[(uint32_t)idx];
        return true;
    }
    JsValue mv = tv, val;
    js_gc_protect(ctx->vm, &mv);
    js_gc_protect(ctx->vm, &key);
    bool ok = js_call(ctx, ARG(1), js_undefined(), &key, 1, &val);
    if (ok && !mapobj_set(ctx->vm, m, key, val)) {
        val = js_error_from_ascii(ctx, "out of memory");
        ok = false;
    }
    js_gc_unprotect(ctx->vm, &key);
    js_gc_unprotect(ctx->vm, &mv);
    *r = val;
    return ok;
}

/* Map.groupBy's ADD: the group array under KEY */
static bool group_add_key(JsContext *ctx, JsValue groups, JsValue key, JsValue value) {
    JsMapObj *m = (JsMapObj *)js_value_object(groups);
    int64_t idx = mapobj_find(m, key);
    if (idx < 0) {
        js_gc_protect(ctx->vm, &value);
        JsObject *a = js_array_new_cell(ctx, 1);
        js_gc_unprotect(ctx->vm, &value);
        if (!a || !mapobj_set(ctx->vm, m, key, js_value_from_cell(&a->gc)))
            return false;
        idx = (int64_t)m->count - 1;
    }
    return js_array_append(ctx->vm, js_value_object(m->values[(uint32_t)idx]), value);
}

/* Map.groupBy(items, callback): a Map of arrays, one per key */
static bool map_groupBy(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    if (js_is_undefined(ARG(0)) || js_is_null(ARG(0)))
        return nthrow(ctx, r, "TypeError: Map.groupBy called on null or undefined");
    if (!js_is_function(ARG(1)))
        return (*r = js_error_fmt(ctx, "TypeError: %v is not a function", ARG(1))), false;
    JsValue mv;
    if (!alloc_mapobj(ctx, &mv))
        return nthrow(ctx, r, "out of memory");
    js_gc_protect(ctx->vm, &mv);
    bool ok = js_group_by(ctx, ARG(0), ARG(1), false, mv, group_add_key, r);
    js_gc_unprotect(ctx->vm, &mv);
    if (ok)
        *r = mv;
    return ok;
}

/* ---- registration ---- */

static bool def_method(JsContext *ctx, JsObject *table, const char *name, JsNativeFn fn) {
    JsValue nf = js_native_new(ctx, name, fn, NULL);
    return js_is_function(nf) && js_object_set_ascii(ctx, table, name, nf);
}

bool js_mapobj_builtins_init(JsContext *ctx) {
    /* Map.prototype: a real object. ctx->map_proto is set before creating
     * any instance (alloc_mapobj reads it), and also assigned as the Map
     * constructor's `.prototype` below so `Map.prototype` and every
     * instance's [[Prototype]] are the exact same object — reachable,
     * mutable, no hidden table. */
    JsValue t = js_object_new(ctx);
    if (!js_is_object(t))
        return false;
    ctx->map_proto = js_value_object(t); /* rooted via the context now */

    static const struct {
        const char *name;
        JsNativeFn fn;
    } methods[] = {
        {"set", map_set}, {"get", map_get}, {"has", map_has}, {"delete", map_delete},
        {"clear", map_clear}, {"forEach", map_forEach},
        {"keys", map_keys}, {"values", map_values}, {"entries", map_entries},
        {"getOrInsert", map_getOrInsert}, {"getOrInsertComputed", map_getOrInsertComputed},
    };
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        if (!def_method(ctx, ctx->map_proto, methods[i].name, methods[i].fn))
            return false;
    }

    JsValue ctor = js_native_new(ctx, "Map", g_Map, NULL);
    if (!js_is_function(ctor))
        return false;
    js_native_builtin_ctor(ctor, ctx->map_proto);
    if (!js_object_set_ascii(ctx, ctx->map_proto, "constructor", ctor) ||
        !js_object_set_ascii(ctx, ctx->globals, "Map", ctor))
        return false;
    /* statics: groupBy */
    JsValue st = js_object_new(ctx);
    if (!js_is_object(st))
        return false;
    ((JsNative *)js_value_cell(ctor))->statics = js_value_object(st);
    return def_method(ctx, js_value_object(st), "groupBy", map_groupBy);
}
