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
    if (!js_native_is_new(ctx))
        return nthrow(ctx, r, "TypeError: Constructor Set requires 'new'");
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

/* ---- union, intersection, difference, symmetricDifference, isSubsetOf, isSupersetOf,
 * isDisjointFrom (ES2025 24.2.4): the other set is anything with a numeric `size` and callable
 * `has` and `keys` (a Set Record), read in that order ---- */

static bool get_named(JsContext *ctx, JsValue o, const char *name, JsValue *out) {
    JsString *k = js_ascii_cell(ctx->vm, name);
    if (!k)
        return (*out = js_oom_value(ctx->vm)), false;
    if (!js_get_property(ctx, o, js_value_from_cell(&k->gc), out))
        return (*out = js_take_error(ctx)), false;
    return true;
}

typedef struct {
    JsValue obj, has, keys; /* rooted by set_record_root */
    double size;            /* an integer, or +Infinity */
} SetRecord;

static void set_record_root(JsVm *vm, SetRecord *rec, bool on) {
    JsValue *vs[3] = {&rec->obj, &rec->has, &rec->keys};
    for (int i = 0; i < 3; i++) {
        if (on)
            js_gc_protect(vm, vs[i]);
        else
            js_gc_unprotect(vm, vs[i]);
    }
}

/* GetSetRecord(other); on success the record is rooted (set_record_root(.., false) releases it) */
static bool get_set_record(JsContext *ctx, const char *method, JsValue other, SetRecord *rec, JsValue *r) {
    if (!js_is_object(other) && !js_is_function(other) && !js_is_promise(other))
        return (*r = js_error_fmt(ctx, "TypeError: Set.prototype.%s argument must be an object", method)), false;
    rec->obj = other;
    rec->has = rec->keys = js_undefined();
    set_record_root(ctx->vm, rec, true);
    JsValue raw;
    double n;
    bool ok = get_named(ctx, other, "size", &raw);
    if (ok && !js_op_to_number(ctx, raw, &n))
        { raw = js_take_error(ctx); ok = false; }
    if (!ok) {
        *r = raw;
    } else if (n != n) {
        *r = js_error_fmt(ctx, "TypeError: The .size property is NaN");
        ok = false;
    } else {
        n = n == __builtin_inf() || n == -__builtin_inf() ? n : __builtin_trunc(n);
        if (n < 0) {
            *r = js_error_fmt(ctx, "RangeError: '%v' is an invalid size", js_number(n));
            ok = false;
        }
        rec->size = n + 0.0;
    }
    if (ok && !get_named(ctx, other, "has", &rec->has))
        { *r = rec->has; ok = false; }
    if (ok && !js_is_function(rec->has))
        { *r = js_error_fmt(ctx, "TypeError: string \"has\" is not a function"); ok = false; }
    if (ok && !get_named(ctx, other, "keys", &rec->keys))
        { *r = rec->keys; ok = false; }
    if (ok && !js_is_function(rec->keys))
        { *r = js_error_fmt(ctx, "TypeError: string \"keys\" is not a function"); ok = false; }
    if (!ok)
        set_record_root(ctx->vm, rec, false);
    return ok;
}

/* GetIteratorFromMethod(other, keys) as a cursor over an Iterator Record; the caller roots
 * cur->src */
static bool set_record_keys(JsContext *ctx, SetRecord *rec, JsIterCursor *cur, JsValue *r) {
    JsValue it, next;
    if (!js_call(ctx, rec->keys, rec->obj, NULL, 0, &it))
        return (*r = it), false;
    if (!js_is_object(it) && !js_is_function(it) && !js_is_promise(it))
        return (*r = js_error_fmt(ctx, "TypeError: Result of the keys method is not an object")), false;
    js_gc_protect(ctx->vm, &it);
    bool ok = get_named(ctx, it, "next", &next);
    if (!ok)
        *r = next;
    else if (!js_is_function(next))
        { *r = js_error_fmt(ctx, "TypeError: string \"next\" is not a function"); ok = false; }
    if (ok) {
        js_gc_protect(ctx->vm, &next);
        cur->src = js_iterobj_new(ctx, JS_ITER_RECORD, it, next, 0);
        cur->i = 0;
        js_gc_unprotect(ctx->vm, &next);
        if (js_is_undefined(cur->src))
            { *r = js_oom_value(ctx->vm); ok = false; }
    }
    js_gc_unprotect(ctx->vm, &it);
    return ok;
}

/* ToBoolean(Call(has, other, v)): 1 true, 0 false, -1 thrown (*r) */
static int set_record_has(JsContext *ctx, SetRecord *rec, JsValue v, JsValue *r) {
    JsValue res;
    if (!js_call(ctx, rec->has, rec->obj, &v, 1, &res))
        return (*r = res), -1;
    return js_to_boolean(res) ? 1 : 0;
}

/* A copy of THIS's entries, as a new Set (rooted by the caller) */
static bool set_copy(JsContext *ctx, JsSetObj *s, JsValue *out) {
    if (!alloc_setobj(ctx, out))
        return false;
    JsSetObj *c = (JsSetObj *)js_value_object(*out);
    for (uint32_t i = 0; i < s->count; i++)
        if (!setobj_add(ctx->vm, c, s->items[i]))
            return false;
    return true;
}

typedef enum {
    SETOP_UNION,
    SETOP_INTERSECTION,
    SETOP_DIFFERENCE,
    SETOP_SYMDIFF,
    SETOP_SUBSET,
    SETOP_SUPERSET,
    SETOP_DISJOINT,
} SetOp;

static const char *const setop_names[] = {"union", "intersection", "difference", "symmetricDifference",
                                          "isSubsetOf", "isSupersetOf", "isDisjointFrom"};

static bool set_op(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r, SetOp op) {
    JsVm *vm = ctx->vm;
    JsSetObj *s;
    if (!this_set(ctx, tv, &s, r))
        return false;
    SetRecord rec;
    if (!get_set_record(ctx, setop_names[op], ARG(0), &rec, r))
        return false;
    double this_size = s->count;
    JsValue setv = tv, result = js_undefined(), v = js_undefined();
    JsIterCursor cur = {js_undefined(), 0};
    js_gc_protect(vm, &setv);
    js_gc_protect(vm, &result);
    js_gc_protect(vm, &v);
    js_gc_protect(vm, &cur.src);
    bool ok = true, answer = true, oom_hit = false;
    /* the branch that walks THIS calls `has` per entry, live (what `has` adds is visited, what it
     * deletes is not); the other walks the other set's keys */
    bool walk_this = (op == SETOP_INTERSECTION || op == SETOP_DIFFERENCE || op == SETOP_DISJOINT)
                         ? this_size <= rec.size
                         : op == SETOP_SUBSET;
    if (op == SETOP_SUBSET && this_size > rec.size) {
        answer = false;
        goto done;
    }
    if (op == SETOP_SUPERSET && this_size < rec.size) {
        answer = false;
        goto done;
    }
    if (op == SETOP_UNION || op == SETOP_DIFFERENCE || op == SETOP_SYMDIFF) {
        if (!set_copy(ctx, s, &result)) {
            oom_hit = true;
            goto done;
        }
    } else if (op == SETOP_INTERSECTION) {
        if (!alloc_setobj(ctx, &result)) {
            oom_hit = true;
            goto done;
        }
    }
    if (walk_this && op == SETOP_DIFFERENCE) {
        /* over the copy's entries as they were (only this loop changes the copy) */
        JsSetObj *res = (JsSetObj *)js_value_object(result);
        JsObject *snap = js_array_new_cell(ctx, res->count);
        if (!snap) {
            oom_hit = true;
            goto done;
        }
        for (uint32_t i = 0; i < res->count; i++)
            snap->elems[snap->elem_count++] = res->items[i];
        v = js_value_from_cell(&snap->gc); /* rooted: the snapshot */
        JsValue e;
        for (uint32_t i = 0; ok && i < snap->elem_count; i++) {
            e = snap->elems[i];
            int in = set_record_has(ctx, &rec, e, r);
            if (in < 0)
                ok = false;
            else if (in)
                setobj_delete(vm, res, e);
        }
    } else if (walk_this) {
        JsValue itv = js_iterobj_new(ctx, JS_ITER_SET_VALUES, setv, js_undefined(), 0);
        if (js_is_undefined(itv)) {
            oom_hit = true;
            goto done;
        }
        cur.src = itv; /* rooted; a Set iterator is never closed */
        JsIterObj *it = (JsIterObj *)js_value_object(itv);
        int st;
        while (ok && (st = js_iterobj_step(ctx, it, &v)) != 0) {
            if (st < 0) {
                *r = v;
                ok = false;
                break;
            }
            int in = set_record_has(ctx, &rec, v, r);
            if (in < 0) {
                ok = false;
            } else if (op == SETOP_INTERSECTION) {
                if (in && !setobj_add(vm, (JsSetObj *)js_value_object(result), v))
                    oom_hit = true, ok = false;
            } else if (op == SETOP_SUBSET ? !in : in) { /* subset: one missing; disjoint: one shared */
                answer = false;
                break;
            }
        }
    } else {
        if (!set_record_keys(ctx, &rec, &cur, r)) {
            ok = false;
            goto done;
        }
        int st;
        while (ok && (st = js_iter_next(ctx, &cur, &v)) != 0) {
            if (st < 0) {
                *r = v;
                ok = false;
                break;
            }
            v = js_normalize_map_key(v); /* -0 is +0 */
            bool in_this = setobj_find(s, v) >= 0;
            JsSetObj *res = js_is_undefined(result) ? NULL : (JsSetObj *)js_value_object(result);
            switch (op) {
            case SETOP_UNION:
                if (!setobj_add(vm, res, v))
                    oom_hit = true, ok = false;
                break;
            case SETOP_INTERSECTION:
                if (in_this && !setobj_add(vm, res, v))
                    oom_hit = true, ok = false;
                break;
            case SETOP_DIFFERENCE:
                setobj_delete(vm, res, v);
                break;
            case SETOP_SYMDIFF:
                if (in_this)
                    setobj_delete(vm, res, v);
                else if (!setobj_add(vm, res, v))
                    oom_hit = true, ok = false;
                break;
            default: /* superset: one this lacks; disjoint: one this has */
                if (op == SETOP_SUPERSET ? !in_this : in_this) {
                    answer = false;
                    JsValue err;
                    if (!js_iter_close(ctx, &cur, false, false, &err))
                        { *r = err; ok = false; }
                }
                break;
            }
            if (!answer)
                break;
        }
    }
done:
    js_gc_unprotect(vm, &cur.src);
    js_gc_unprotect(vm, &v);
    js_gc_unprotect(vm, &result);
    js_gc_unprotect(vm, &setv);
    set_record_root(vm, &rec, false);
    if (oom_hit)
        return nthrow(ctx, r, "out of memory");
    if (!ok)
        return false;
    *r = op >= SETOP_SUBSET ? js_bool(answer) : result;
    return true;
}

static bool set_union(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    return set_op(ctx, tv, args, argc, r, SETOP_UNION);
}
static bool set_intersection(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    return set_op(ctx, tv, args, argc, r, SETOP_INTERSECTION);
}
static bool set_difference(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    return set_op(ctx, tv, args, argc, r, SETOP_DIFFERENCE);
}
static bool set_symmetricDifference(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    return set_op(ctx, tv, args, argc, r, SETOP_SYMDIFF);
}
static bool set_isSubsetOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    return set_op(ctx, tv, args, argc, r, SETOP_SUBSET);
}
static bool set_isSupersetOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    return set_op(ctx, tv, args, argc, r, SETOP_SUPERSET);
}
static bool set_isDisjointFrom(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    return set_op(ctx, tv, args, argc, r, SETOP_DISJOINT);
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
        {"union", set_union}, {"intersection", set_intersection},
        {"difference", set_difference}, {"symmetricDifference", set_symmetricDifference},
        {"isSubsetOf", set_isSubsetOf}, {"isSupersetOf", set_isSupersetOf},
        {"isDisjointFrom", set_isDisjointFrom},
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
