/*
 * Set: construction, mutators, lookups, and its iterators (js_iter.h). See js_setobj.h for the
 * storage and equality.
 */
#include "js_bytecode.h"
#include "js_error.h"
#include "js_iter.h"
#include "js_setobj.h"
#include "js_valindex.h"

#define ARG(i) ((i) < argc ? args[i] : js_undefined())

static bool nthrow(JsContext *ctx, JsValue *r, const char *msg) {
    *r = js_error_from_ascii(ctx, msg); /* Error object; string on OOM */
    return false;
}

/* ---- storage ---- */

static bool setobj_reserve(JsVm *vm, JsSetObj *s, uint32_t want) {
    if (want <= s->cap)
        return true;
    uint32_t ncap = s->cap ? s->cap * 2 : 8;
    while (ncap < want)
        ncap *= 2;
    JsValue *ni = js_realloc_raw(vm, s->items, (size_t)s->cap * sizeof(JsValue),
                                 (size_t)ncap * sizeof(JsValue));
    if (!ni)
        return false;
    s->items = ni;
    s->cap = ncap;
    return true;
}

static int64_t setobj_find(const JsSetObj *s, JsValue value) {
    return js_valindex_find(s->index, s->index_cap, s->items, value);
}

/* Already present: no-op (insertion order is unchanged, matching spec).
 * New: appended to the ordered array and recorded in the index. */
static bool setobj_add(JsVm *vm, JsSetObj *s, JsValue value) {
    value = js_normalize_map_key(value); /* store +0, never -0 */
    if (setobj_find(s, value) >= 0)
        return true;
    if (!setobj_reserve(vm, s, s->count + 1))
        return false;
    s->items[s->count++] = value;
    if (!js_valindex_add(vm, &s->index, &s->index_cap, s->items, s->count, s->count - 1)) {
        s->count--; /* roll back the append; index unchanged */
        return false;
    }
    return true;
}

/* Shifts later entries left to close the gap, preserving insertion order
 * (the same O(n) tradeoff Map's delete makes). */
static bool setobj_delete(JsVm *vm, JsSetObj *s, JsValue value) {
    int64_t idx = setobj_find(s, value);
    if (idx < 0)
        return false;
    uint32_t i = (uint32_t)idx;
    for (uint32_t k = i; k + 1 < s->count; k++)
        s->items[k] = s->items[k + 1];
    s->count--;
    /* The shift renumbered every entry from i on; rebuild the position index, and the
     * iterators past i move back. */
    js_valindex_rebuild(s->index, s->index_cap, s->items, s->count);
    if (vm->live_iters)
        js_iter_on_delete(vm, &s->obj, i);
    return true;
}

static bool alloc_setobj(JsContext *ctx, JsValue *out) {
    JsGcCell *c = js_gc_new_cell(ctx->vm, JS_KIND_OBJECT, sizeof(JsSetObj));
    if (!c)
        return false;
    JsSetObj *s = (JsSetObj *)c;
    s->obj.obj_kind = JS_OBJ_SET;
    s->obj.obj_flags = 0;
    js_map_init(&s->obj.props);
    s->obj.elems = NULL;
    s->obj.elem_count = s->obj.elem_cap = 0;
    s->obj.proto = ctx->set_proto ? js_value_from_cell(&ctx->set_proto->gc) : js_undefined();
    s->items = NULL;
    s->count = s->cap = 0;
    s->index = NULL;
    s->index_cap = 0;
    *out = js_value_from_cell(c);
    return true;
}

static bool this_set(JsContext *ctx, JsValue tv, JsSetObj **out, JsValue *r) {
    if (!js_setobj_is(tv))
        return (*r = js_error_receiver(ctx, "Set", tv)), false;
    *out = (JsSetObj *)js_value_object(tv);
    return true;
}

/* ---- constructor ---- */

/* Source may be another Set (copied) or an Array (each element added).
 * Strings and other array-likes aren't iterated — same scope cut as
 * Map's constructor. */
static bool set_add(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r);

static bool g_Set(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue src = ARG(0);
    JsValue sv;
    if (!alloc_setobj(ctx, &sv))
        return nthrow(ctx, r, "out of memory");
    if (!js_is_undefined(src) && !js_is_null(src)) {
        js_gc_protect(ctx->vm, &sv);
        bool ok = js_collection_fill(ctx, sv, src, false, set_add, r);
        js_gc_unprotect(ctx->vm, &sv);
        if (!ok)
            return false;
    }
    *r = sv;
    return true;
}

/* ---- methods ---- */

static bool set_add(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsSetObj *s;
    if (!this_set(ctx, tv, &s, r))
        return false;
    if (!setobj_add(ctx->vm, s, ARG(0)))
        return nthrow(ctx, r, "out of memory");
    *r = tv; /* chainable */
    return true;
}

static bool set_has(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsSetObj *s;
    if (!this_set(ctx, tv, &s, r))
        return false;
    *r = js_bool(setobj_find(s, ARG(0)) >= 0);
    return true;
}

static bool set_delete(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsSetObj *s;
    if (!this_set(ctx, tv, &s, r))
        return false;
    *r = js_bool(setobj_delete(ctx->vm, s, ARG(0)));
    return true;
}

static bool set_clear(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args; (void)argc;
    JsSetObj *s;
    if (!this_set(ctx, tv, &s, r))
        return false;
    s->count = 0;
    js_valindex_rebuild(s->index, s->index_cap, s->items, 0); /* empty the index */
    if (ctx->vm->live_iters)
        js_iter_on_clear(ctx->vm, &s->obj);
    *r = js_undefined();
    return true;
}

/* callback(value, value, set) per spec (a Set has no separate key), live as Map's forEach is:
 * it walks the set with an iterator (js_iter.h). */
static bool set_forEach(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsSetObj *s;
    if (!this_set(ctx, tv, &s, r))
        return false;
    if (!js_is_function(ARG(0)))
        return (*r = js_error_fmt(ctx, "TypeError: %t is not a function", ARG(0))), false;
    JsValue cb = ARG(0), thisArg = ARG(1), setv = tv;
    JsValue itv = js_iterobj_new(ctx, JS_ITER_SET_VALUES, setv, js_undefined(), 0);
    if (js_is_undefined(itv))
        return nthrow(ctx, r, "out of memory");
    js_gc_protect(ctx->vm, &itv);
    js_gc_protect(ctx->vm, &cb);
    js_gc_protect(ctx->vm, &thisArg);
    JsIterObj *it = (JsIterObj *)js_value_object(itv);
    bool ok = true;
    JsValue v;
    while (js_iterobj_step(ctx, it, &v) > 0) {
        JsValue argv[3] = {v, v, setv};
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

static bool set_iterator(JsContext *ctx, JsValue tv, JsValue *r, JsIterKind kind) {
    JsSetObj *s;
    if (!this_set(ctx, tv, &s, r))
        return false;
    *r = js_iterobj_new(ctx, kind, tv, js_undefined(), 0);
    return js_is_undefined(*r) ? nthrow(ctx, r, "out of memory") : true;
}

static bool set_values(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args; (void)argc;
    return set_iterator(ctx, tv, r, JS_ITER_SET_VALUES);
}

static bool set_entries(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args; (void)argc;
    return set_iterator(ctx, tv, r, JS_ITER_SET_ENTRIES);
}

/* ---- synthesized `size` (js_interp.c get_property) ---- */

bool js_setobj_prop_get(JsObject *o, JsValue key, JsValue *out, bool *handled) {
    *handled = false;
    if (!js_is_string(key))
        return true;
    JsString *ks = js_value_string(key);
    if (ks->length == 4 && ks->units[0] == 's' && ks->units[1] == 'i' &&
        ks->units[2] == 'z' && ks->units[3] == 'e') {
        *out = js_number(((JsSetObj *)o)->count);
        *handled = true;
    }
    return true;
}

/* ---- GC ---- */

void js_setobj_mark(JsVm *vm, JsObject *o) {
    JsSetObj *s = (JsSetObj *)o;
    for (uint32_t i = 0; i < s->count; i++)
        js_gc_mark_value(vm, s->items[i]);
}

size_t js_setobj_release(JsVm *vm, JsObject *o) {
    JsSetObj *s = (JsSetObj *)o;
    js_realloc_raw(vm, s->items, (size_t)s->cap * sizeof(JsValue), 0);
    js_realloc_raw(vm, s->index, (size_t)s->index_cap * sizeof(int32_t), 0);
    return sizeof(JsSetObj);
}

/* ---- registration ---- */

static bool def_method(JsContext *ctx, JsObject *table, const char *name, JsNativeFn fn) {
    JsValue nf = js_native_new(ctx, name, fn, NULL);
    return js_is_function(nf) && js_object_set_ascii(ctx, table, name, nf);
}

bool js_setobj_builtins_init(JsContext *ctx) {
    /* Set.prototype: a real object, same deal as Map's — ctx->set_proto is
     * set before any instance exists (alloc_setobj reads it) and is also
     * the constructor's `.prototype`, so `Set.prototype` and every
     * instance's [[Prototype]] are the same reachable object. */
    JsValue t = js_object_new(ctx);
    if (!js_is_object(t))
        return false;
    ctx->set_proto = js_value_object(t); /* rooted via the context now */

    static const struct {
        const char *name;
        JsNativeFn fn;
    } methods[] = {
        {"add", set_add}, {"has", set_has}, {"delete", set_delete}, {"clear", set_clear},
        {"forEach", set_forEach},
        {"entries", set_entries},
    };
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        if (!def_method(ctx, ctx->set_proto, methods[i].name, methods[i].fn))
            return false;
    }
    /* keys is the very same function as values, as the spec has it */
    JsValue values = js_native_new(ctx, "values", set_values, NULL);
    if (!js_is_function(values) || !js_object_set_ascii(ctx, ctx->set_proto, "values", values) ||
        !js_object_set_ascii(ctx, ctx->set_proto, "keys", values))
        return false;

    JsValue ctor = js_native_new(ctx, "Set", g_Set, NULL);
    if (!js_is_function(ctor))
        return false;
    js_native_builtin_ctor(ctor, ctx->set_proto);
    return js_object_set_ascii(ctx, ctx->set_proto, "constructor", ctor) &&
        js_object_set_ascii(ctx, ctx->globals, "Set", ctor);
}
