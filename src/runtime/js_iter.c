/*
 * Iterator objects and the cursor natives step iterables with. See js_iter.h.
 */
#include "js_bytecode.h"
#include "js_error.h"
#include "js_iter.h"
#include "js_mapobj.h"
#include "js_setobj.h"
#ifdef LAMASSU_HAS_REGEX
#include "js_regexp.h"
#endif

/* ---- the live list ---- */

static void live_link(JsVm *vm, JsIterObj *it) {
    it->live_prev = NULL;
    it->live_next = vm->live_iters;
    if (vm->live_iters)
        vm->live_iters->live_prev = it;
    vm->live_iters = it;
    it->live = true;
}

static void live_unlink(JsVm *vm, JsIterObj *it) {
    if (!it->live)
        return;
    if (it->live_prev)
        it->live_prev->live_next = it->live_next;
    else
        vm->live_iters = it->live_next;
    if (it->live_next)
        it->live_next->live_prev = it->live_prev;
    it->live_prev = it->live_next = NULL;
    it->live = false;
}

void js_iter_on_delete(JsVm *vm, const JsObject *coll, uint32_t pos) {
    for (JsIterObj *it = vm->live_iters; it; it = it->live_next) {
        if (js_value_cell(it->target) == &coll->gc && it->index > pos)
            it->index--;
    }
}

void js_iter_on_clear(JsVm *vm, const JsObject *coll) {
    for (JsIterObj *it = vm->live_iters; it; it = it->live_next) {
        if (js_value_cell(it->target) == &coll->gc)
            it->index = 0;
    }
}

void js_iter_prune(JsVm *vm) {
    JsIterObj *it = vm->live_iters;
    while (it) {
        JsIterObj *next = it->live_next;
        if (!it->obj.gc.mark)
            live_unlink(vm, it);
        it = next;
    }
}

/* ---- the objects ---- */

static int proto_family(JsIterKind kind) {
    switch (kind) {
    case JS_ITER_MAP_KEYS:
    case JS_ITER_MAP_VALUES:
    case JS_ITER_MAP_ENTRIES:
        return JS_ITER_PROTO_MAP;
    case JS_ITER_SET_VALUES:
    case JS_ITER_SET_ENTRIES:
        return JS_ITER_PROTO_SET;
    case JS_ITER_REGEXP:
        break;
    }
    return JS_ITER_PROTO_REGEXP;
}

JsValue js_iterobj_new(JsContext *ctx, JsIterKind kind, JsValue target, JsValue subject,
                       uint32_t index) {
    JsGcCell *c = js_gc_new_cell(ctx->vm, JS_KIND_OBJECT, sizeof(JsIterObj));
    if (!c)
        return js_undefined();
    JsIterObj *it = (JsIterObj *)c;
    it->obj.obj_kind = JS_OBJ_ITER;
    it->obj.obj_flags = 0;
    it->obj.error_cause = 0;
    js_map_init(&it->obj.props);
    it->obj.elems = NULL;
    it->obj.elem_count = it->obj.elem_cap = 0;
    JsObject *proto = ctx->iter_protos[proto_family(kind)];
    it->obj.proto = proto ? js_value_from_cell(&proto->gc) : js_undefined();
    it->target = target;
    it->subject = subject;
    it->index = index;
    it->kind = (uint8_t)kind;
    it->live = false;
    it->live_prev = it->live_next = NULL;
    if (kind != JS_ITER_REGEXP)
        live_link(ctx->vm, it);
    return js_value_from_cell(c);
}

static void finish(JsVm *vm, JsIterObj *it) {
    live_unlink(vm, it);
    it->target = js_undefined();
    it->subject = js_undefined();
}

static JsValue pair(JsContext *ctx, JsValue a, JsValue b) {
    /* a and b belong to the collection the (rooted) iterator holds */
    JsObject *p = js_array_new_cell(ctx, 2);
    if (!p)
        return js_undefined();
    p->elems[0] = a;
    p->elems[1] = b;
    p->elem_count = 2;
    return js_value_from_cell(&p->gc);
}

int js_iterobj_step(JsContext *ctx, JsIterObj *it, JsValue *out) {
    JsVm *vm = ctx->vm;
    if (js_is_undefined(it->target))
        return 0;
    switch ((JsIterKind)it->kind) {
    case JS_ITER_MAP_KEYS:
    case JS_ITER_MAP_VALUES:
    case JS_ITER_MAP_ENTRIES: {
        JsMapObj *m = (JsMapObj *)js_value_object(it->target);
        if (it->index >= m->count) {
            finish(vm, it);
            return 0;
        }
        uint32_t i = it->index++;
        if (it->kind == JS_ITER_MAP_KEYS) {
            *out = m->keys[i];
        } else if (it->kind == JS_ITER_MAP_VALUES) {
            *out = m->values[i];
        } else {
            *out = pair(ctx, m->keys[i], m->values[i]);
            if (js_is_undefined(*out))
                return (*out = js_oom_value(vm)), -1;
        }
        return 1;
    }
    case JS_ITER_SET_VALUES:
    case JS_ITER_SET_ENTRIES: {
        JsSetObj *s = (JsSetObj *)js_value_object(it->target);
        if (it->index >= s->count) {
            finish(vm, it);
            return 0;
        }
        uint32_t i = it->index++;
        if (it->kind == JS_ITER_SET_VALUES) {
            *out = s->items[i];
        } else {
            *out = pair(ctx, s->items[i], s->items[i]);
            if (js_is_undefined(*out))
                return (*out = js_oom_value(vm)), -1;
        }
        return 1;
    }
    case JS_ITER_REGEXP: {
#ifdef LAMASSU_HAS_REGEX
        int rc = js_re_iter_step(ctx, it->target, it->subject, &it->index, out);
        if (rc == 0)
            finish(vm, it);
        return rc;
#else
        break;
#endif
    }
    }
    finish(vm, it);
    return 0;
}

void js_iterobj_mark(JsVm *vm, JsObject *o) {
    JsIterObj *it = (JsIterObj *)o;
    js_gc_mark_value(vm, it->target);
    js_gc_mark_value(vm, it->subject);
}

size_t js_iterobj_release(JsVm *vm, JsObject *o) {
    /* Not unlinked here: a collection pruned it from the live list before the sweep, and the
     * VM's teardown frees every cell without looking at the list. */
    (void)vm;
    (void)o;
    return sizeof(JsIterObj);
}

/* ---- next() ---- */

JsValue js_iter_result(JsContext *ctx, JsValue value, bool done) {
    JsVm *vm = ctx->vm;
    js_gc_protect(vm, &value);
    JsValue o = js_object_new(ctx);
    bool ok = js_is_object(o);
    if (ok) {
        js_gc_protect(vm, &o);
        ok = js_object_set_ascii(ctx, js_value_object(o), "value", value) &&
             js_object_set_ascii(ctx, js_value_object(o), "done", js_bool(done));
        js_gc_unprotect(vm, &o);
    }
    js_gc_unprotect(vm, &value);
    return ok ? o : js_undefined();
}

static bool iter_next(JsContext *ctx, JsValue tv, JsValue *r, int family, const char *cls) {
    if (!js_iterobj_is(tv) ||
        proto_family((JsIterKind)((JsIterObj *)js_value_object(tv))->kind) != family)
        return (*r = js_error_receiver(ctx, cls, tv)), false;
    JsValue v = js_undefined();
    int rc = js_iterobj_step(ctx, (JsIterObj *)js_value_object(tv), &v);
    if (rc < 0)
        return (*r = v), false;
    *r = js_iter_result(ctx, rc ? v : js_undefined(), rc == 0);
    if (js_is_undefined(*r))
        return (*r = js_oom_value(ctx->vm)), false;
    return true;
}

static bool map_iter_next(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args; (void)argc;
    return iter_next(ctx, tv, r, JS_ITER_PROTO_MAP, "Map Iterator");
}

static bool set_iter_next(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args; (void)argc;
    return iter_next(ctx, tv, r, JS_ITER_PROTO_SET, "Set Iterator");
}

static bool regexp_iter_next(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args; (void)argc;
    return iter_next(ctx, tv, r, JS_ITER_PROTO_REGEXP, "%RegExpStringIterator%"); /* as V8 names it */
}

bool js_iter_builtins_init(JsContext *ctx) {
    /* %IteratorPrototype%: each family's prototype chains to it; it holds nothing yet (the
     * Iterator global and its helpers are not implemented) */
    JsValue ip = js_object_new(ctx);
    if (!js_is_object(ip))
        return false;
    ctx->iterator_proto = js_value_object(ip); /* rooted via the context now */
    static const JsNativeFn nexts[JS_ITER_PROTO_COUNT] = {map_iter_next, set_iter_next,
                                                          regexp_iter_next};
    for (int k = 0; k < JS_ITER_PROTO_COUNT; k++) {
        JsValue p = js_object_new(ctx);
        if (!js_is_object(p))
            return false;
        js_value_object(p)->proto = ip;
        ctx->iter_protos[k] = js_value_object(p);
        JsValue nf = js_native_new(ctx, "next", nexts[k], NULL);
        if (!js_is_function(nf) || !js_object_set_ascii(ctx, ctx->iter_protos[k], "next", nf))
            return false;
    }
    return true;
}

/* ---- cursors ---- */

int js_iter_open(JsContext *ctx, JsValue v, JsIterCursor *c) {
    c->i = 0;
    c->src = v;
    if (js_is_string(v) || js_iterobj_is(v) ||
        (js_is_object(v) && js_value_object(v)->obj_kind == JS_OBJ_ARRAY))
        return 1;
    if (js_mapobj_is(v) || js_setobj_is(v)) {
        /* a Map's default iterator is its entries, a Set's its values */
        c->src = js_iterobj_new(ctx, js_mapobj_is(v) ? JS_ITER_MAP_ENTRIES : JS_ITER_SET_VALUES,
                                v, js_undefined(), 0);
        return js_is_undefined(c->src) ? -1 : 1;
    }
    return 0;
}

int js_iter_next(JsContext *ctx, JsIterCursor *c, JsValue *out) {
    JsValue src = c->src;
    if (js_is_string(src)) {
        JsString *s = js_value_string(src);
        if (c->i >= s->length) {
            c->i = JS_ITER_DONE_INDEX;
            return 0;
        }
        uint32_t i = c->i, n = 1;
        if (s->units[i] >= 0xD800 && s->units[i] <= 0xDBFF && i + 1 < s->length &&
            s->units[i + 1] >= 0xDC00 && s->units[i + 1] <= 0xDFFF)
            n = 2;
        JsString *ch = js_string_cell_new(ctx->vm, s->units + i, n);
        if (!ch)
            return (*out = js_oom_value(ctx->vm)), -1;
        c->i = i + n;
        *out = js_value_from_cell(&ch->gc);
        return 1;
    }
    if (js_iterobj_is(src))
        return js_iterobj_step(ctx, (JsIterObj *)js_value_object(src), out);
    JsObject *a = js_value_object(src);
    if (c->i >= a->elem_count) {
        c->i = JS_ITER_DONE_INDEX;
        return 0;
    }
    *out = a->elems[c->i++];
    return 1;
}

/* ---- natives that take an iterable ---- */

JsValue js_iter_not_iterable(JsContext *ctx, JsValue v) {
    return js_error_fmt(ctx, "TypeError: %t is not iterable (cannot read property Symbol(Symbol.iterator))", v);
}

bool js_iter_entry(JsContext *ctx, JsValue entry, JsValue *k, JsValue *v, JsValue *err) {
    if (!js_is_object(entry) && !js_is_function(entry) && !js_is_promise(entry))
        return (*err = js_error_fmt(ctx, "TypeError: Iterator value %v is not an entry object", entry)),
               false;
    if (js_is_object(entry) && js_value_object(entry)->obj_kind == JS_OBJ_ARRAY &&
        js_value_object(entry)->elem_count >= 2) {
        *k = js_value_object(entry)->elems[0];
        *v = js_value_object(entry)->elems[1];
        return true;
    }
    if (!js_get_property(ctx, entry, js_number(0), k))
        return (*err = js_take_error(ctx)), false;
    js_gc_protect(ctx->vm, k);
    bool ok = js_get_property(ctx, entry, js_number(1), v);
    js_gc_unprotect(ctx->vm, k);
    if (!ok)
        return (*err = js_take_error(ctx)), false;
    return true;
}

static bool is_builtin(JsValue f, JsNativeFn fn) {
    if (!js_is_function(f) || js_value_cell(f)->kind != JS_KIND_NATIVE)
        return false;
    const JsNative *n = (const JsNative *)js_value_cell(f);
    return !n->is_bound && n->fn == fn;
}

bool js_collection_fill(JsContext *ctx, JsValue coll, JsValue iterable, bool pairs,
                        JsNativeFn builtin, JsValue *r) {
    JsVm *vm = ctx->vm;
    const char *name = pairs ? "set" : "add";
    JsString *key = js_ascii_cell(vm, name);
    if (!key)
        return (*r = js_oom_value(vm)), false;
    JsValue adder;
    if (!js_get_property(ctx, coll, js_value_from_cell(&key->gc), &adder))
        return (*r = js_take_error(ctx)), false;
    if (!js_is_function(adder))
        return (*r = js_error_fmt(ctx, "TypeError: '%v' returned for property '%s' of object '#<%s>' is not a function",
                                  adder, name, pairs ? "Map" : "Set")),
               false;
    js_gc_protect(vm, &adder);
    bool fast = is_builtin(adder, builtin);
    JsIterCursor cur;
    int open = js_iter_open(ctx, iterable, &cur);
    if (open <= 0) {
        js_gc_unprotect(vm, &adder);
        *r = open < 0 ? js_oom_value(vm) : js_iter_not_iterable(ctx, iterable);
        return false;
    }
    js_gc_protect(vm, &cur.src);
    JsValue item = js_undefined(), k = js_undefined(), v = js_undefined();
    js_gc_protect(vm, &item);
    js_gc_protect(vm, &k);
    js_gc_protect(vm, &v);
    bool ok = true;
    int rc;
    while (ok && (rc = js_iter_next(ctx, &cur, &item)) != 0) {
        if (rc < 0) {
            *r = item;
            ok = false;
            break;
        }
        JsValue argv[2];
        int argc = 1;
        if (pairs) {
            if (!js_iter_entry(ctx, item, &k, &v, r)) {
                ok = false;
                break;
            }
            argv[0] = k;
            argv[1] = v;
            argc = 2;
        } else {
            argv[0] = item;
        }
        JsValue res;
        ok = fast ? builtin(ctx, coll, argv, argc, &res) : js_call(ctx, adder, coll, argv, argc, &res);
        if (!ok)
            *r = res;
    }
    js_gc_unprotect(vm, &v);
    js_gc_unprotect(vm, &k);
    js_gc_unprotect(vm, &item);
    js_gc_unprotect(vm, &cur.src);
    js_gc_unprotect(vm, &adder);
    return ok;
}
