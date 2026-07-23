/*
 * Map: construction, mutators, lookups, and snapshot iteration views. See
 * js_mapobj.h for the storage/equality tradeoffs (linear scan, no lazy
 * MapIterator).
 */
#include "js_bytecode.h"
#include "js_mapobj.h"
#include "js_valindex.h"

#define ARG(i) ((i) < argc ? args[i] : js_undefined())

static bool nthrow(JsContext *ctx, JsValue *r, const char *msg) {
    JsString *s = js_ascii_cell(ctx->vm, msg);
    *r = s ? js_value_from_cell(&s->gc) : js_undefined();
    return false;
}

static bool is_pair_like(JsValue v) {
    return js_is_object(v) && js_value_object(v)->obj_kind == JS_OBJ_ARRAY &&
          js_value_object(v)->elem_count >= 2;
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
static bool mapobj_delete(JsMapObj *m, JsValue key) {
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
     * be rebuilt against the new ordering. */
    js_valindex_rebuild(m->index, m->index_cap, m->keys, m->count);
    return true;
}

static bool alloc_mapobj(JsContext *ctx, JsValue *out) {
    JsGcCell *c = js_gc_new_cell(ctx->vm, JS_KIND_OBJECT, sizeof(JsMapObj));
    if (!c)
        return false;
    JsMapObj *m = (JsMapObj *)c;
    m->obj.obj_kind = JS_OBJ_MAP;
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
        return nthrow(ctx, r, "TypeError: Map.prototype method called on a non-Map value");
    *out = (JsMapObj *)js_value_object(tv);
    return true;
}

/* ---- constructor ---- */

static bool g_Map(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue src = ARG(0);
    JsValue mv;
    if (!alloc_mapobj(ctx, &mv))
        return nthrow(ctx, r, "out of memory");
    if (!js_is_undefined(src) && !js_is_null(src)) {
        JsMapObj *m = (JsMapObj *)js_value_object(mv);
        if (js_mapobj_is(src)) {
            JsMapObj *sm = (JsMapObj *)js_value_object(src);
            for (uint32_t i = 0; i < sm->count; i++) {
                if (!mapobj_set(ctx->vm, m, sm->keys[i], sm->values[i]))
                    return nthrow(ctx, r, "out of memory");
            }
        } else if (js_is_object(src) && js_value_object(src)->obj_kind == JS_OBJ_ARRAY) {
            JsObject *a = js_value_object(src);
            for (uint32_t i = 0; i < a->elem_count; i++) {
                if (!is_pair_like(a->elems[i]))
                    return nthrow(ctx, r,
                                 "TypeError: iterable for new Map() must yield [key, value] pairs");
                JsObject *pair = js_value_object(a->elems[i]);
                if (!mapobj_set(ctx->vm, m, pair->elems[0], pair->elems[1]))
                    return nthrow(ctx, r, "out of memory");
            }
        } else {
            return nthrow(ctx, r, "TypeError: value is not iterable");
        }
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
    *r = js_bool(mapobj_delete(m, ARG(0)));
    return true;
}

static bool map_clear(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args; (void)argc;
    JsMapObj *m;
    if (!this_map(ctx, tv, &m, r))
        return false;
    m->count = 0;
    js_valindex_rebuild(m->index, m->index_cap, m->keys, 0); /* empty the index */
    *r = js_undefined();
    return true;
}

/* callback(value, key, map); re-reads m->keys/values[i] fresh each
 * iteration (never caches the array pointer across the js_call) since the
 * callback may grow the map and move the backing storage via realloc — same
 * discipline js_builtins.c's array_iterate uses for Array.prototype methods.
 * A callback that deletes an earlier-visited key shifts later entries down
 * and can make forEach skip one; matches the storage's general "linear
 * array, not a spec-exact live list" tradeoff. */
static bool map_forEach(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsMapObj *m;
    if (!this_map(ctx, tv, &m, r))
        return false;
    if (!js_is_function(ARG(0)))
        return nthrow(ctx, r, "TypeError: callback is not a function");
    JsValue cb = ARG(0), thisArg = ARG(1), mapv = tv;
    js_gc_protect(ctx->vm, &mapv);
    js_gc_protect(ctx->vm, &cb);
    js_gc_protect(ctx->vm, &thisArg);
    bool ok = true;
    for (uint32_t i = 0; i < m->count && ok; i++) {
        JsValue argv[3] = {m->values[i], m->keys[i], mapv};
        JsValue cbres;
        if (!js_call(ctx, cb, thisArg, argv, 3, &cbres)) {
            *r = cbres;
            ok = false;
        }
    }
    js_gc_unprotect(ctx->vm, &thisArg);
    js_gc_unprotect(ctx->vm, &cb);
    js_gc_unprotect(ctx->vm, &mapv);
    if (!ok)
        return false;
    *r = js_undefined();
    return true;
}

static bool map_keys(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args; (void)argc;
    JsMapObj *m;
    if (!this_map(ctx, tv, &m, r))
        return false;
    JsObject *out = js_array_new_cell(ctx, m->count);
    if (!out)
        return nthrow(ctx, r, "out of memory");
    for (uint32_t i = 0; i < m->count; i++) {
        if (!js_array_append(ctx->vm, out, m->keys[i]))
            return nthrow(ctx, r, "out of memory");
    }
    *r = js_value_from_cell(&out->gc);
    return true;
}

static bool map_values(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args; (void)argc;
    JsMapObj *m;
    if (!this_map(ctx, tv, &m, r))
        return false;
    JsObject *out = js_array_new_cell(ctx, m->count);
    if (!out)
        return nthrow(ctx, r, "out of memory");
    for (uint32_t i = 0; i < m->count; i++) {
        if (!js_array_append(ctx->vm, out, m->values[i]))
            return nthrow(ctx, r, "out of memory");
    }
    *r = js_value_from_cell(&out->gc);
    return true;
}

JsObject *js_mapobj_entries_array(JsContext *ctx, JsObject *o) {
    JsMapObj *m = (JsMapObj *)o;
    JsVm *vm = ctx->vm;
    JsObject *out = js_array_new_cell(ctx, m->count);
    if (!out)
        return NULL;
    JsValue outv = js_value_from_cell(&out->gc);
    js_gc_protect(vm, &outv);
    bool ok = true;
    for (uint32_t i = 0; ok && i < m->count; i++) {
        JsObject *pair = js_array_new_cell(ctx, 2);
        if (!pair) {
            ok = false;
            break;
        }
        JsValue pairv = js_value_from_cell(&pair->gc);
        js_gc_protect(vm, &pairv);
        pair->elems[0] = m->keys[i];
        pair->elems[1] = m->values[i];
        pair->elem_count = 2;
        ok = js_array_append(vm, out, pairv);
        js_gc_unprotect(vm, &pairv);
    }
    js_gc_unprotect(vm, &outv);
    return ok ? out : NULL;
}

static bool map_entries(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args; (void)argc;
    JsMapObj *m;
    if (!this_map(ctx, tv, &m, r))
        return false;
    JsObject *out = js_mapobj_entries_array(ctx, &m->obj);
    if (!out)
        return nthrow(ctx, r, "out of memory");
    *r = js_value_from_cell(&out->gc);
    return true;
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
    };
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        if (!def_method(ctx, ctx->map_proto, methods[i].name, methods[i].fn))
            return false;
    }

    JsValue ctor = js_native_new(ctx, "Map", g_Map, NULL);
    if (!js_is_function(ctor))
        return false;
    ((JsNative *)js_value_cell(ctor))->prototype = ctx->map_proto;
    return js_object_set_ascii(ctx, ctx->map_proto, "constructor", ctor) &&
        js_object_set_ascii(ctx, ctx->globals, "Map", ctor);
}
