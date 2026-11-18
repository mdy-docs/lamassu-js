/*
 * WeakMap, WeakSet, WeakRef, FinalizationRegistry. See js_weak.h for how they meet the collector.
 */
#include "js_bytecode.h"
#include "js_error.h"
#include "js_iter.h"
#include "js_valindex.h"
#include "js_weak.h"

#define ARG(i) ((i) < argc ? args[i] : js_undefined())

static bool nthrow(JsContext *ctx, JsValue *r, const char *msg) {
    *r = js_error_from_ascii(ctx, msg); /* Error object; string on OOM */
    return false;
}

/* CanBeHeldWeakly: an object (a function and a promise are objects); there are no symbols */
static bool weak_ok(JsValue v) {
    return js_is_object(v) || js_is_function(v) || js_is_promise(v);
}

static bool cell_marked(JsValue v) {
    return js_value_cell(v)->mark != 0;
}

/* ---- storage ---- */

static bool grow(JsVm *vm, JsValue **a, uint32_t cap, uint32_t ncap) {
    JsValue *na = js_realloc_raw(vm, *a, (size_t)cap * sizeof(JsValue), (size_t)ncap * sizeof(JsValue));
    if (!na)
        return false;
    *a = na;
    return true;
}

/* Room for WANT entries in keys (and values, tokens where the kind has them) */
static bool weak_reserve(JsVm *vm, JsWeakObj *w, uint32_t want) {
    if (want <= w->cap)
        return true;
    uint32_t ncap = w->cap ? w->cap * 2 : 8;
    while (ncap < want)
        ncap *= 2;
    uint8_t k = w->obj.obj_kind;
    if (!grow(vm, &w->keys, w->cap, ncap))
        return false;
    if ((k == JS_OBJ_WEAKMAP || k == JS_OBJ_FINREG) && !grow(vm, &w->values, w->cap, ncap))
        return false; /* what grew is freed with the cell */
    if (k == JS_OBJ_FINREG && !grow(vm, &w->tokens, w->cap, ncap))
        return false;
    w->cap = ncap;
    return true;
}

static int64_t weak_find(const JsWeakObj *w, JsValue key) {
    return js_valindex_find(w->index, w->index_cap, w->keys, key);
}

/* A weak map's or set's KEY gets VALUE (ignored for a set). False on OOM. */
static bool weak_put(JsVm *vm, JsWeakObj *w, JsValue key, JsValue value) {
    int64_t i = weak_find(w, key);
    if (i >= 0) {
        if (w->values)
            w->values[(uint32_t)i] = value;
        return true;
    }
    if (!weak_reserve(vm, w, w->count + 1))
        return false;
    w->keys[w->count] = key;
    if (w->obj.obj_kind == JS_OBJ_WEAKMAP)
        w->values[w->count] = value;
    w->count++;
    if (!js_valindex_add(vm, &w->index, &w->index_cap, w->keys, w->count, w->count - 1)) {
        w->count--;
        return false;
    }
    return true;
}

/* Removes the entry at I, moving the last one into its place (there is no order to keep);
 * REINDEX rebuilds the index now (a prune rebuilds it once at the end). Allocates nothing. */
static void weak_remove_at(JsWeakObj *w, uint32_t i, bool reindex) {
    uint32_t last = --w->count;
    if (i != last) {
        w->keys[i] = w->keys[last];
        if (w->values)
            w->values[i] = w->values[last];
        if (w->tokens)
            w->tokens[i] = w->tokens[last];
    }
    if (reindex)
        js_valindex_rebuild(w->index, w->index_cap, w->keys, w->count);
}

static bool weak_alloc(JsContext *ctx, uint8_t kind, JsObject *proto, bool listed, JsValue *out) {
    JsGcCell *c = js_gc_new_cell(ctx->vm, JS_KIND_OBJECT, sizeof(JsWeakObj));
    if (!c)
        return false;
    JsWeakObj *w = (JsWeakObj *)c;
    w->obj.obj_kind = kind;
    w->obj.obj_flags = 0;
    w->obj.error_cause = 0;
    js_map_init(&w->obj.props);
    w->obj.elems = NULL;
    w->obj.elem_count = w->obj.elem_cap = 0;
    w->obj.proto = proto ? js_value_from_cell(&proto->gc) : js_undefined();
    w->keys = w->values = w->tokens = NULL;
    w->count = w->cap = 0;
    w->index = NULL;
    w->index_cap = 0;
    w->callback = js_undefined();
    w->target = js_undefined();
    w->pending = w->pending_tokens = NULL;
    w->pending_count = w->pending_cap = 0;
    w->weak_next = NULL;
    if (listed) {
        w->weak_next = ctx->vm->weak_list;
        ctx->vm->weak_list = w;
    }
    *out = js_value_from_cell(c);
    return true;
}

static bool this_weak(JsContext *ctx, JsValue tv, uint8_t kind, const char *cls, JsWeakObj **out, JsValue *r) {
    if (!js_is_object(tv) || js_value_object(tv)->obj_kind != kind)
        return (*r = js_error_receiver(ctx, cls, tv)), false;
    *out = (JsWeakObj *)js_value_object(tv);
    return true;
}

bool js_weak_side_set(JsContext *ctx, JsObject **table, JsValue key, JsValue value) {
    if (!*table) {
        JsValue tv;
        js_gc_protect(ctx->vm, &key);
        js_gc_protect(ctx->vm, &value);
        bool ok = weak_alloc(ctx, JS_OBJ_WEAKMAP, NULL, true, &tv);
        js_gc_unprotect(ctx->vm, &value);
        js_gc_unprotect(ctx->vm, &key);
        if (!ok)
            return false;
        *table = js_value_object(tv);
    }
    return weak_put(ctx->vm, (JsWeakObj *)*table, key, value);
}

bool js_weak_side_get(const JsObject *table, JsValue key, JsValue *out) {
    if (!table || !weak_ok(key))
        return false;
    const JsWeakObj *w = (const JsWeakObj *)table;
    int64_t i = weak_find(w, key);
    if (i < 0)
        return false;
    *out = w->values[(uint32_t)i];
    return true;
}

/* ---- the collector's side ---- */

void js_weak_mark(JsVm *vm, JsObject *o) {
    JsWeakObj *w = (JsWeakObj *)o;
    if (o->obj_kind != JS_OBJ_FINREG)
        return; /* keys, values and targets are weak: see js_weak_ephemerons */
    js_gc_mark_value(vm, w->callback);
    for (uint32_t i = 0; i < w->count; i++)
        js_gc_mark_value(vm, w->values[i]); /* held values: strong */
    for (uint32_t i = 0; i < w->pending_count; i++)
        js_gc_mark_value(vm, w->pending[i]);
}

size_t js_weak_release(JsVm *vm, JsObject *o) {
    JsWeakObj *w = (JsWeakObj *)o;
    size_t n = (size_t)w->cap * sizeof(JsValue);
    js_realloc_raw(vm, w->keys, n, 0);
    js_realloc_raw(vm, w->values, w->values ? n : 0, 0);
    js_realloc_raw(vm, w->tokens, w->tokens ? n : 0, 0);
    js_realloc_raw(vm, w->index, (size_t)w->index_cap * sizeof(int32_t), 0);
    js_realloc_raw(vm, w->pending, (size_t)w->pending_cap * sizeof(JsValue), 0);
    js_realloc_raw(vm, w->pending_tokens, (size_t)w->pending_cap * sizeof(JsValue), 0);
    return sizeof(JsWeakObj);
}

bool js_weak_ephemerons(JsVm *vm) {
    bool marked = false;
    for (JsWeakObj *w = vm->weak_list; w; w = w->weak_next) {
        if (w->obj.obj_kind != JS_OBJ_WEAKMAP || !w->obj.gc.mark)
            continue;
        for (uint32_t i = 0; i < w->count; i++) {
            JsValue v = w->values[i];
            if (cell_marked(w->keys[i]) && js_value_is_cell(v) && !cell_marked(v)) {
                js_gc_mark_value(vm, v);
                marked = true;
            }
        }
    }
    return marked;
}

void js_weak_prune(JsVm *vm) {
    JsWeakObj **link = &vm->weak_list;
    while (*link) {
        JsWeakObj *w = *link;
        if (!w->obj.gc.mark) { /* about to be freed itself */
            *link = w->weak_next;
            continue;
        }
        link = &w->weak_next;
        switch (w->obj.obj_kind) {
        case JS_OBJ_WEAKMAP:
        case JS_OBJ_WEAKSET: {
            uint32_t before = w->count;
            for (uint32_t i = w->count; i-- > 0;)
                if (!cell_marked(w->keys[i]))
                    weak_remove_at(w, i, false);
            if (w->count != before)
                js_valindex_rebuild(w->index, w->index_cap, w->keys, w->count);
            break;
        }
        case JS_OBJ_WEAKREF:
            if (js_value_is_cell(w->target) && !cell_marked(w->target))
                w->target = js_undefined();
            break;
        case JS_OBJ_FINREG:
            for (uint32_t i = 0; i < w->pending_count; i++)
                if (js_value_is_cell(w->pending_tokens[i]) && !cell_marked(w->pending_tokens[i]))
                    w->pending_tokens[i] = js_undefined();
            for (uint32_t i = w->count; i-- > 0;) {
                if (js_value_is_cell(w->tokens[i]) && !cell_marked(w->tokens[i]))
                    w->tokens[i] = js_undefined();
                if (cell_marked(w->keys[i]))
                    continue;
                /* room reserved at register: count + pending_count never exceeds pending_cap */
                w->pending[w->pending_count] = w->values[i];
                w->pending_tokens[w->pending_count] = w->tokens[i];
                w->pending_count++;
                weak_remove_at(w, i, false);
                if (js_is_function(w->callback))
                    vm->weak_cleanup_pending = true;
            }
            break;
        default:
            break;
        }
    }
}

bool js_weak_run_cleanups(JsContext *ctx) {
    JsVm *vm = ctx->vm;
    bool ran = false;
    vm->weak_cleanup_pending = false;
    for (;;) {
        JsWeakObj *w = vm->weak_list;
        while (w && !(w->obj.obj_kind == JS_OBJ_FINREG && w->pending_count))
            w = w->weak_next;
        if (!w || vm->interrupt)
            return ran;
        JsValue wv = js_value_from_cell(&w->obj.gc), held = js_undefined(), res;
        js_gc_protect(vm, &wv);
        js_gc_protect(vm, &held);
        while (w->pending_count && !vm->interrupt) {
            held = w->pending[0]; /* in the order they died */
            for (uint32_t i = 1; i < w->pending_count; i++) {
                w->pending[i - 1] = w->pending[i];
                w->pending_tokens[i - 1] = w->pending_tokens[i];
            }
            w->pending_count--;
            ran = true;
            /* an error the callback throws goes nowhere, as a job's does; the rest still run */
            if (!js_call(ctx, w->callback, js_undefined(), &held, 1, &res))
                (void)js_take_error(ctx);
        }
        js_gc_unprotect(vm, &held);
        js_gc_unprotect(vm, &wv);
    }
}

/* ---- WeakMap ---- */

static bool wm_set(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsWeakObj *w;
    if (!this_weak(ctx, tv, JS_OBJ_WEAKMAP, "WeakMap", &w, r))
        return false;
    if (!weak_ok(ARG(0)))
        return nthrow(ctx, r, "TypeError: Invalid value used as weak map key");
    if (!weak_put(ctx->vm, w, ARG(0), ARG(1)))
        return nthrow(ctx, r, "out of memory");
    *r = tv;
    return true;
}

static bool wm_get(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsWeakObj *w;
    if (!this_weak(ctx, tv, JS_OBJ_WEAKMAP, "WeakMap", &w, r))
        return false;
    int64_t i = weak_ok(ARG(0)) ? weak_find(w, ARG(0)) : -1;
    *r = i >= 0 ? w->values[(uint32_t)i] : js_undefined();
    return true;
}

static bool wm_has(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsWeakObj *w;
    if (!this_weak(ctx, tv, JS_OBJ_WEAKMAP, "WeakMap", &w, r))
        return false;
    *r = js_bool(weak_ok(ARG(0)) && weak_find(w, ARG(0)) >= 0);
    return true;
}

static bool wm_delete(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsWeakObj *w;
    if (!this_weak(ctx, tv, JS_OBJ_WEAKMAP, "WeakMap", &w, r))
        return false;
    int64_t i = weak_ok(ARG(0)) ? weak_find(w, ARG(0)) : -1;
    if (i >= 0)
        weak_remove_at(w, (uint32_t)i, true);
    *r = js_bool(i >= 0);
    return true;
}

static bool wm_getOrInsert(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsWeakObj *w;
    if (!this_weak(ctx, tv, JS_OBJ_WEAKMAP, "WeakMap", &w, r))
        return false;
    if (!weak_ok(ARG(0)))
        return nthrow(ctx, r, "TypeError: Invalid value used as weak map key");
    int64_t i = weak_find(w, ARG(0));
    if (i >= 0) {
        *r = w->values[(uint32_t)i];
        return true;
    }
    if (!weak_put(ctx->vm, w, ARG(0), ARG(1)))
        return nthrow(ctx, r, "out of memory");
    *r = ARG(1);
    return true;
}

static bool wm_getOrInsertComputed(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsWeakObj *w;
    if (!this_weak(ctx, tv, JS_OBJ_WEAKMAP, "WeakMap", &w, r))
        return false;
    if (!weak_ok(ARG(0)))
        return nthrow(ctx, r, "TypeError: Invalid value used as weak map key");
    if (!js_is_function(ARG(1)))
        return nthrow(ctx, r, "TypeError: WeakMap.prototype.getOrInsertComputed is not a function");
    int64_t i = weak_find(w, ARG(0));
    if (i >= 0) {
        *r = w->values[(uint32_t)i];
        return true;
    }
    JsValue wv = tv, key = ARG(0), val;
    js_gc_protect(ctx->vm, &wv);
    js_gc_protect(ctx->vm, &key);
    bool ok = js_call(ctx, ARG(1), js_undefined(), &key, 1, &val);
    if (ok) {
        js_gc_protect(ctx->vm, &val);
        if (!weak_put(ctx->vm, w, key, val)) {
            js_gc_unprotect(ctx->vm, &val);
            val = js_error_from_ascii(ctx, "out of memory");
            ok = false;
        } else {
            js_gc_unprotect(ctx->vm, &val);
        }
    }
    js_gc_unprotect(ctx->vm, &key);
    js_gc_unprotect(ctx->vm, &wv);
    *r = val;
    return ok;
}

static bool g_WeakMap(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    if (!js_native_is_new(ctx))
        return nthrow(ctx, r, "TypeError: Constructor WeakMap requires 'new'");
    JsValue wv;
    if (!weak_alloc(ctx, JS_OBJ_WEAKMAP, ctx->weakmap_proto, true, &wv))
        return nthrow(ctx, r, "out of memory");
    if (!js_is_undefined(ARG(0)) && !js_is_null(ARG(0))) {
        js_gc_protect(ctx->vm, &wv);
        bool ok = js_collection_fill(ctx, wv, ARG(0), true, wm_set, r);
        js_gc_unprotect(ctx->vm, &wv);
        if (!ok)
            return false;
    }
    *r = wv;
    return true;
}

/* ---- WeakSet ---- */

static bool ws_add(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsWeakObj *w;
    if (!this_weak(ctx, tv, JS_OBJ_WEAKSET, "WeakSet", &w, r))
        return false;
    if (!weak_ok(ARG(0)))
        return nthrow(ctx, r, "TypeError: Invalid value used in weak set");
    if (!weak_put(ctx->vm, w, ARG(0), js_undefined()))
        return nthrow(ctx, r, "out of memory");
    *r = tv;
    return true;
}

static bool ws_has(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsWeakObj *w;
    if (!this_weak(ctx, tv, JS_OBJ_WEAKSET, "WeakSet", &w, r))
        return false;
    *r = js_bool(weak_ok(ARG(0)) && weak_find(w, ARG(0)) >= 0);
    return true;
}

static bool ws_delete(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsWeakObj *w;
    if (!this_weak(ctx, tv, JS_OBJ_WEAKSET, "WeakSet", &w, r))
        return false;
    int64_t i = weak_ok(ARG(0)) ? weak_find(w, ARG(0)) : -1;
    if (i >= 0)
        weak_remove_at(w, (uint32_t)i, true);
    *r = js_bool(i >= 0);
    return true;
}

static bool g_WeakSet(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    if (!js_native_is_new(ctx))
        return nthrow(ctx, r, "TypeError: Constructor WeakSet requires 'new'");
    JsValue wv;
    if (!weak_alloc(ctx, JS_OBJ_WEAKSET, ctx->weakset_proto, true, &wv))
        return nthrow(ctx, r, "out of memory");
    if (!js_is_undefined(ARG(0)) && !js_is_null(ARG(0))) {
        js_gc_protect(ctx->vm, &wv);
        bool ok = js_collection_fill(ctx, wv, ARG(0), false, ws_add, r);
        js_gc_unprotect(ctx->vm, &wv);
        if (!ok)
            return false;
    }
    *r = wv;
    return true;
}

/* ---- WeakRef ---- */

/* AddToKeptObjects: TARGET stays alive to the end of the job */
static bool keep_during_job(JsContext *ctx, JsValue target) {
    if (!ctx->weak_kept) {
        JsValue kv;
        js_gc_protect(ctx->vm, &target);
        bool ok = weak_alloc(ctx, JS_OBJ_WEAKSET, NULL, false, &kv);
        js_gc_unprotect(ctx->vm, &target);
        if (!ok)
            return false;
        ctx->weak_kept = js_value_object(kv);
    }
    return weak_put(ctx->vm, (JsWeakObj *)ctx->weak_kept, target, js_undefined());
}

void js_weak_mark_kept(JsVm *vm, JsObject *kept) {
    JsWeakObj *w = (JsWeakObj *)kept;
    js_gc_mark_cell(vm, &kept->gc);
    for (uint32_t i = 0; i < w->count; i++)
        js_gc_mark_value(vm, w->keys[i]);
}

void js_weak_clear_kept(JsContext *ctx) {
    JsWeakObj *w = (JsWeakObj *)ctx->weak_kept;
    if (w && w->count) {
        w->count = 0;
        js_valindex_rebuild(w->index, w->index_cap, w->keys, 0);
    }
}

static bool g_WeakRef(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    if (!js_native_is_new(ctx))
        return nthrow(ctx, r, "TypeError: Constructor WeakRef requires 'new'");
    if (!weak_ok(ARG(0)))
        return nthrow(ctx, r, "TypeError: WeakRef: invalid target");
    JsValue wv;
    if (!weak_alloc(ctx, JS_OBJ_WEAKREF, ctx->weakref_proto, true, &wv))
        return nthrow(ctx, r, "out of memory");
    ((JsWeakObj *)js_value_object(wv))->target = ARG(0);
    js_gc_protect(ctx->vm, &wv);
    bool ok = keep_during_job(ctx, ARG(0));
    js_gc_unprotect(ctx->vm, &wv);
    if (!ok)
        return nthrow(ctx, r, "out of memory");
    *r = wv;
    return true;
}

static bool wr_deref(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    JsWeakObj *w;
    if (!this_weak(ctx, tv, JS_OBJ_WEAKREF, "WeakRef", &w, r))
        return false;
    JsValue t = w->target;
    if (!js_is_undefined(t) && !keep_during_job(ctx, t))
        return nthrow(ctx, r, "out of memory");
    *r = t;
    return true;
}

/* ---- FinalizationRegistry ---- */

static bool g_FinalizationRegistry(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    if (!js_native_is_new(ctx))
        return nthrow(ctx, r, "TypeError: Constructor FinalizationRegistry requires 'new'");
    if (!js_is_function(ARG(0)))
        return nthrow(ctx, r, "TypeError: FinalizationRegistry: cleanup must be callable");
    JsValue wv;
    if (!weak_alloc(ctx, JS_OBJ_FINREG, ctx->finreg_proto, true, &wv))
        return nthrow(ctx, r, "out of memory");
    ((JsWeakObj *)js_value_object(wv))->callback = ARG(0);
    *r = wv;
    return true;
}

static bool fr_register(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsWeakObj *w;
    if (!this_weak(ctx, tv, JS_OBJ_FINREG, "FinalizationRegistry", &w, r))
        return false;
    JsValue target = ARG(0), held = ARG(1), token = ARG(2);
    if (!weak_ok(target))
        return nthrow(ctx, r, "TypeError: FinalizationRegistry.prototype.register: invalid target");
    if (js_strict_equals(target, held))
        return nthrow(ctx, r, "TypeError: FinalizationRegistry.prototype.register: target and holdings must not be same");
    if (!weak_ok(token) && !js_is_undefined(token))
        return (*r = js_error_fmt(ctx, "TypeError: Invalid unregisterToken ('%v')", token)), false;
    /* the pending list has room for every registration, so the collector never allocates */
    uint32_t need = w->count + w->pending_count + 1;
    if (need > w->pending_cap) {
        uint32_t ncap = w->pending_cap ? w->pending_cap * 2 : 8;
        while (ncap < need)
            ncap *= 2;
        if (!grow(ctx->vm, &w->pending, w->pending_cap, ncap) ||
            !grow(ctx->vm, &w->pending_tokens, w->pending_cap, ncap))
            return nthrow(ctx, r, "out of memory");
        w->pending_cap = ncap;
    }
    if (!weak_reserve(ctx->vm, w, w->count + 1))
        return nthrow(ctx, r, "out of memory");
    w->keys[w->count] = target;
    w->values[w->count] = held;
    w->tokens[w->count] = token;
    w->count++;
    *r = js_undefined();
    return true;
}

static bool fr_unregister(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsWeakObj *w;
    if (!this_weak(ctx, tv, JS_OBJ_FINREG, "FinalizationRegistry", &w, r))
        return false;
    JsValue token = ARG(0);
    if (!weak_ok(token))
        return (*r = js_error_fmt(ctx, "TypeError: Invalid unregisterToken ('%v')", token)), false;
    bool removed = false;
    for (uint32_t i = w->count; i-- > 0;) {
        if (js_strict_equals(w->tokens[i], token)) {
            weak_remove_at(w, i, false);
            removed = true;
        }
    }
    for (uint32_t i = w->pending_count; i-- > 0;) { /* dead, its callback not run yet */
        if (js_strict_equals(w->pending_tokens[i], token)) {
            for (uint32_t k = i + 1; k < w->pending_count; k++) {
                w->pending[k - 1] = w->pending[k];
                w->pending_tokens[k - 1] = w->pending_tokens[k];
            }
            w->pending_count--;
            removed = true;
        }
    }
    *r = js_bool(removed);
    return true;
}

/* ---- registration ---- */

static bool def_method(JsContext *ctx, JsObject *table, const char *name, JsNativeFn fn) {
    JsValue nf = js_native_new(ctx, name, fn, NULL);
    return js_is_function(nf) && js_object_set_ascii(ctx, table, name, nf);
}

typedef struct {
    const char *name;
    JsNativeFn fn;
} WeakMethod;

static bool def_class(JsContext *ctx, const char *name, JsNativeFn ctor_fn, JsObject **proto_slot,
                      const WeakMethod *methods, size_t n) {
    JsValue t = js_object_new(ctx);
    if (!js_is_object(t))
        return false;
    *proto_slot = js_value_object(t); /* rooted via the context now */
    JsValue ctor = js_native_new(ctx, name, ctor_fn, NULL);
    if (!js_is_function(ctor))
        return false;
    js_native_builtin_ctor(ctor, *proto_slot);
    if (!js_object_set_ascii(ctx, *proto_slot, "constructor", ctor)) /* first, as V8 lists it */
        return false;
    for (size_t i = 0; i < n; i++)
        if (!def_method(ctx, *proto_slot, methods[i].name, methods[i].fn))
            return false;
    return js_object_set_ascii(ctx, ctx->globals, name, ctor);
}

bool js_weak_builtins_init(JsContext *ctx) {
    static const WeakMethod wm[] = {
        {"delete", wm_delete}, {"get", wm_get}, {"set", wm_set}, {"has", wm_has},
        {"getOrInsert", wm_getOrInsert}, {"getOrInsertComputed", wm_getOrInsertComputed},
    };
    static const WeakMethod ws[] = {{"delete", ws_delete}, {"has", ws_has}, {"add", ws_add}};
    static const WeakMethod wr[] = {{"deref", wr_deref}};
    static const WeakMethod fr[] = {{"register", fr_register}, {"unregister", fr_unregister}};
    return def_class(ctx, "WeakMap", g_WeakMap, &ctx->weakmap_proto, wm, sizeof wm / sizeof wm[0]) &&
           def_class(ctx, "WeakSet", g_WeakSet, &ctx->weakset_proto, ws, sizeof ws / sizeof ws[0]) &&
           def_class(ctx, "WeakRef", g_WeakRef, &ctx->weakref_proto, wr, sizeof wr / sizeof wr[0]) &&
           def_class(ctx, "FinalizationRegistry", g_FinalizationRegistry, &ctx->finreg_proto, fr,
                     sizeof fr / sizeof fr[0]);
}
