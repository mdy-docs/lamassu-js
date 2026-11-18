/*
 * Iterator objects, the iterator protocol, the Iterator global and its helpers. See js_iter.h.
 */
#include <math.h>
#include "js_bytecode.h"
#include "js_error.h"
#include "js_iter.h"
#include "js_valindex.h"
#include "js_mapobj.h"
#include "js_setobj.h"
#ifdef LAMASSU_HAS_REGEX
#include "js_regexp.h"
#endif

#define ARG(i) ((i) < argc ? args[i] : js_undefined())

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


/* ---- small helpers ---- */

/* An Object in the spec's sense: what typeof calls "object" (not null) or "function" */
static bool is_obj(JsValue v) {
    return js_is_object(v) || js_is_function(v) || js_is_promise(v);
}

static bool is_array(JsValue v) {
    return js_is_object(v) && js_value_object(v)->obj_kind == JS_OBJ_ARRAY;
}

/* V's [[Prototype]] chain reaches Iterator.prototype (OrdinaryHasInstance(%Iterator%, V)) */
static bool inherits_iterator(JsContext *ctx, JsValue v) {
    if (!ctx->iterator_proto || !is_obj(v))
        return false;
    const JsGcCell *ip = &ctx->iterator_proto->gc;
    JsValue p = js_get_proto(ctx, v);
    for (int depth = 0; depth < 10000 && is_obj(p); depth++) {
        if (js_value_cell(p) == ip)
            return true;
        p = js_get_proto(ctx, p);
    }
    return false;
}

/* V is Array.prototype or inherits from it without being an array: what V8 iterates with
 * Array.prototype.values, which it finds as V's [Symbol.iterator] */
static bool inherits_array(JsContext *ctx, JsValue v) {
    if (!ctx->array_proto || !is_obj(v))
        return false;
    const JsGcCell *ap = &ctx->array_proto->gc;
    JsValue p = v;
    for (int depth = 0; depth < 10000 && is_obj(p); depth++) {
        if (js_value_cell(p) == ap)
            return true;
        p = js_get_proto(ctx, p);
    }
    return false;
}

/* O[name] for a property name in ASCII. False thrown, *out the error. */
static bool get_named(JsContext *ctx, JsValue o, const char *name, JsValue *out) {
    JsString *k = js_ascii_cell(ctx->vm, name);
    if (!k)
        return (*out = js_oom_value(ctx->vm)), false;
    if (!js_get_property(ctx, o, js_value_from_cell(&k->gc), out))
        return (*out = js_take_error(ctx)), false;
    return true;
}

static int family(JsIterKind kind) {
    switch (kind) {
    case JS_ITER_MAP_KEYS:
    case JS_ITER_MAP_VALUES:
    case JS_ITER_MAP_ENTRIES:
        return JS_ITER_PROTO_MAP;
    case JS_ITER_SET_VALUES:
    case JS_ITER_SET_ENTRIES:
        return JS_ITER_PROTO_SET;
    case JS_ITER_REGEXP:
        return JS_ITER_PROTO_REGEXP;
    case JS_ITER_ARRAY_KEYS:
    case JS_ITER_ARRAY_VALUES:
    case JS_ITER_ARRAY_ENTRIES:
        return JS_ITER_PROTO_ARRAY;
    case JS_ITER_STRING:
        return JS_ITER_PROTO_STRING;
    case JS_ITER_HELPER:
        return JS_ITER_PROTO_HELPER;
    case JS_ITER_WRAP:
        return JS_ITER_PROTO_WRAP;
    case JS_ITER_RECORD:
        break;
    }
    return -1;
}

/* ---- the objects ---- */

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
    int fam = family(kind);
    JsObject *proto = fam >= 0 ? ctx->iter_protos[fam] : NULL; /* a record has none */
    it->obj.proto = proto ? js_value_from_cell(&proto->gc) : js_undefined();
    it->target = target;
    it->subject = subject;
    it->inner = js_undefined();
    it->index = index;
    it->inner_index = 0;
    it->count = 0;
    it->kind = (uint8_t)kind;
    it->op = 0;
    it->state = JS_HELPER_START;
    it->live = false;
    it->live_prev = it->live_next = NULL;
    if (fam == JS_ITER_PROTO_MAP || fam == JS_ITER_PROTO_SET)
        live_link(ctx->vm, it);
    return js_value_from_cell(c);
}

static void finish(JsVm *vm, JsIterObj *it) {
    live_unlink(vm, it);
    it->target = js_undefined();
    it->subject = js_undefined();
    it->inner = js_undefined();
}

static JsValue pair(JsContext *ctx, JsValue a, JsValue b) {
    /* the caller roots a and b */
    JsObject *p = js_array_new_cell(ctx, 2);
    if (!p)
        return js_undefined();
    p->elems[0] = a;
    p->elems[1] = b;
    p->elem_count = 2;
    return js_value_from_cell(&p->gc);
}

/* The code point of S at *I, as a new string; *I moves past it. NULL on OOM. */
static JsString *code_point_at(JsVm *vm, JsString *s, uint32_t *i) {
    uint32_t at = *i, n = 1;
    if (s->units[at] >= 0xD800 && s->units[at] <= 0xDBFF && at + 1 < s->length &&
        s->units[at + 1] >= 0xDC00 && s->units[at + 1] <= 0xDFFF)
        n = 2;
    JsString *ch = js_string_cell_new(vm, s->units + at, n);
    if (ch)
        *i = at + n;
    return ch;
}

/* IteratorStepValue on an Iterator Record (or Iterator.from's wrapper, which forwards to the
 * same): next() called on the object, its result an object, `done` read, then `value`. A record
 * ends (it is finished) on done or on anything thrown; a wrapper keeps forwarding. */
static int protocol_step(JsContext *ctx, JsIterObj *it, JsValue *out) {
    JsVm *vm = ctx->vm;
    bool record = it->kind == JS_ITER_RECORD;
    JsValue obj = it->target, next = it->subject;
    if (!js_is_function(next)) {
        *out = js_error_fmt(ctx, "TypeError: %t is not a function", next);
        if (record)
            finish(vm, it);
        return -1;
    }
    JsValue res;
    if (!js_call(ctx, next, obj, NULL, 0, &res)) {
        *out = res;
        if (record)
            finish(vm, it);
        return -1;
    }
    if (!is_obj(res)) {
        *out = js_error_fmt(ctx, "TypeError: Iterator result %v is not an object", res);
        if (record)
            finish(vm, it);
        return -1;
    }
    js_gc_protect(vm, &res);
    JsValue done;
    bool ok = get_named(ctx, res, "done", &done);
    if (ok && js_to_boolean(done)) {
        js_gc_unprotect(vm, &res);
        if (record)
            finish(vm, it);
        return 0;
    }
    if (ok)
        ok = get_named(ctx, res, "value", out);
    else
        *out = done;
    js_gc_unprotect(vm, &res);
    if (!ok) {
        if (record)
            finish(vm, it);
        return -1;
    }
    return 1;
}

/* An Array Iterator's step: TARGET is an array, or anything else read through its `length` and
 * indices (an array-like, a string's units). */
static int array_step(JsContext *ctx, JsIterObj *it, JsValue *out) {
    JsVm *vm = ctx->vm;
    JsValue t = it->target;
    double len;
    if (is_array(t)) {
        len = js_value_object(t)->elem_count;
    } else if (js_is_string(t)) {
        len = js_value_string(t)->length;
    } else {
        JsValue lv;
        if (!get_named(ctx, t, "length", &lv)) {
            *out = lv;
            finish(vm, it);
            return -1;
        }
        if (!js_op_to_number(ctx, lv, &len)) {
            *out = js_take_error(ctx);
            finish(vm, it);
            return -1;
        }
        len = len != len || len <= 0 ? 0 : len > 9007199254740991.0 ? 9007199254740991.0 : floor(len);
    }
    if ((double)it->index >= len) {
        finish(vm, it);
        return 0;
    }
    uint32_t i = it->index++;
    if (it->kind == JS_ITER_ARRAY_KEYS) {
        *out = js_number(i);
        return 1;
    }
    JsValue v;
    if (is_array(t)) {
        v = js_value_object(t)->elems[i];
    } else if (!js_get_property(ctx, t, js_number(i), &v)) {
        *out = js_take_error(ctx);
        finish(vm, it);
        return -1;
    }
    if (it->kind == JS_ITER_ARRAY_VALUES) {
        *out = v;
        return 1;
    }
    js_gc_protect(vm, &v);
    *out = pair(ctx, js_number(i), v);
    js_gc_unprotect(vm, &v);
    if (js_is_undefined(*out))
        return (*out = js_oom_value(vm)), -1;
    return 1;
}

static int helper_step(JsContext *ctx, JsIterObj *h, JsValue *out);

int js_iterobj_step(JsContext *ctx, JsIterObj *it, JsValue *out) {
    JsVm *vm = ctx->vm;
    if (it->kind == JS_ITER_HELPER)
        return helper_step(ctx, it, out);
    if (js_is_undefined(it->target)) /* finished (a wrapper's target is never undefined) */
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
            *out = pair(ctx, m->keys[i], m->values[i]); /* both held by the rooted map */
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
        int rc = js_re_iter_step(ctx, it, out);
        if (rc == 0)
            finish(vm, it);
        return rc;
#else
        break;
#endif
    }
    case JS_ITER_ARRAY_KEYS:
    case JS_ITER_ARRAY_VALUES:
    case JS_ITER_ARRAY_ENTRIES:
        return array_step(ctx, it, out);
    case JS_ITER_STRING: {
        JsString *s = js_value_string(it->target);
        if (it->index >= s->length) {
            finish(vm, it);
            return 0;
        }
        JsString *ch = code_point_at(vm, s, &it->index);
        if (!ch)
            return (*out = js_oom_value(vm)), -1;
        *out = js_value_from_cell(&ch->gc);
        return 1;
    }
    case JS_ITER_WRAP:
    case JS_ITER_RECORD:
        return protocol_step(ctx, it, out);
    case JS_ITER_HELPER:
        break;
    }
    finish(vm, it);
    return 0;
}

void js_iterobj_mark(JsVm *vm, JsObject *o) {
    JsIterObj *it = (JsIterObj *)o;
    js_gc_mark_value(vm, it->target);
    js_gc_mark_value(vm, it->subject);
    js_gc_mark_value(vm, it->inner);
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

/* the class each family's next() names in a receiver error, as V8 names it */
static const char *const family_class[JS_ITER_PROTO_COUNT] = {
    "Map Iterator", "Set Iterator", "%RegExpStringIterator%", "Array Iterator", "String Iterator",
    "Iterator Helper", "%WrapForValidIteratorPrototype%"};

/* V8's error for next or return on the wrong receiver; the wrapper's names no prototype */
static JsValue bad_receiver(JsContext *ctx, int fam, const char *method, JsValue tv) {
    if (fam == JS_ITER_PROTO_WRAP)
        return js_error_fmt(ctx, "TypeError: Method %%WrapForValidIteratorPrototype%%.%s called on incompatible receiver %v",
                            method, tv);
    return js_error_receiver(ctx, family_class[fam], tv);
}

static JsIterObj *this_iter(JsValue tv, int fam) {
    if (!js_iterobj_is(tv))
        return NULL;
    JsIterObj *it = (JsIterObj *)js_value_object(tv);
    return family((JsIterKind)it->kind) == fam ? it : NULL;
}

static bool iter_next(JsContext *ctx, JsValue tv, JsValue *r, int fam) {
    JsIterObj *it = this_iter(tv, fam);
    if (!it)
        return (*r = bad_receiver(ctx, fam, "next", tv)), false;
    JsValue v = js_undefined();
    int rc = js_iterobj_step(ctx, it, &v);
    if (rc < 0)
        return (*r = v), false;
    *r = js_iter_result(ctx, rc ? v : js_undefined(), rc == 0);
    if (js_is_undefined(*r))
        return (*r = js_oom_value(ctx->vm)), false;
    return true;
}

#define NEXT_NATIVE(name, fam)                                                                     \
    static bool name(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {      \
        (void)args;                                                                                \
        (void)argc;                                                                                \
        return iter_next(ctx, tv, r, fam);                                                         \
    }
NEXT_NATIVE(map_iter_next, JS_ITER_PROTO_MAP)
NEXT_NATIVE(set_iter_next, JS_ITER_PROTO_SET)
NEXT_NATIVE(regexp_iter_next, JS_ITER_PROTO_REGEXP)
NEXT_NATIVE(array_iter_next, JS_ITER_PROTO_ARRAY)
NEXT_NATIVE(string_iter_next, JS_ITER_PROTO_STRING)
NEXT_NATIVE(helper_next, JS_ITER_PROTO_HELPER)

/* %WrapForValidIteratorPrototype%.next: the wrapped iterator's next, called as it is */
static bool wrap_next(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    JsIterObj *w = this_iter(tv, JS_ITER_PROTO_WRAP);
    if (!w)
        return (*r = bad_receiver(ctx, JS_ITER_PROTO_WRAP, "next", tv)), false;
    if (!js_is_function(w->subject))
        return (*r = js_error_fmt(ctx, "TypeError: %t is not a function", w->subject)), false;
    return js_call(ctx, w->subject, w->target, NULL, 0, r);
}

static const JsNativeFn family_next[JS_ITER_PROTO_COUNT] = {
    map_iter_next, set_iter_next, regexp_iter_next, array_iter_next, string_iter_next, helper_next,
    wrap_next};

/* NEXT is IT's family's own next(), so stepping IT directly is what calling it would do */
static bool is_own_next(const JsIterObj *it, JsValue next) {
    int fam = family((JsIterKind)it->kind);
    if (fam < 0 || !js_is_function(next) || js_value_cell(next)->kind != JS_KIND_NATIVE)
        return false;
    const JsNative *n = (const JsNative *)js_value_cell(next);
    return !n->is_bound && n->fn == family_next[fam];
}

/* GetIteratorDirect(O): O and the next read from it once, as a cursor's src: O itself when that
 * next is its own family's, else a record. O must be rooted. False thrown, *src the error. */
static bool get_direct(JsContext *ctx, JsValue o, JsValue *src) {
    JsValue next;
    if (!get_named(ctx, o, "next", &next))
        return (*src = next), false;
    if (js_iterobj_is(o) && is_own_next((JsIterObj *)js_value_object(o), next)) {
        *src = o;
        return true;
    }
    js_gc_protect(ctx->vm, &next);
    *src = js_iterobj_new(ctx, JS_ITER_RECORD, o, next, 0);
    js_gc_unprotect(ctx->vm, &next);
    if (js_is_undefined(*src))
        return (*src = js_oom_value(ctx->vm)), false;
    return true;
}

/* ---- cursors ---- */

bool js_iter_is_iterable(JsContext *ctx, JsValue v) {
    return js_is_string(v) || js_iterobj_is(v) || js_mapobj_is(v) || js_setobj_is(v) ||
           inherits_iterator(ctx, v) || inherits_array(ctx, v);
}

int js_iter_open(JsContext *ctx, JsValue v, JsIterCursor *c) {
    c->i = 0;
    c->src = v;
    JsValue it = v;
    if (js_is_string(v) || (is_array(v) && inherits_array(ctx, v))) {
        /* (an array whose prototype was taken away is not iterable, as in V8.) The iterator V8
         * makes here is stepped by the `next` it reads from it once. While that is still the
         * Array Iterator's (the String Iterator's) own, stepping the cursor over V is what calling
         * it would do, and no iterator object is made; a `next` a script put there is called. */
        bool str = js_is_string(v);
        if (js_intact(ctx, str ? JS_INTACT_STRING_ITER_NEXT : JS_INTACT_ARRAY_ITER_NEXT))
            return 1;
        it = js_iterobj_new(ctx, str ? JS_ITER_STRING : JS_ITER_ARRAY_VALUES, v, js_undefined(), 0);
        if (js_is_undefined(it))
            return -1;
    } else if (js_mapobj_is(v) || js_setobj_is(v)) {
        /* a Map's default iterator is its entries, a Set's its values */
        it = js_iterobj_new(ctx, js_mapobj_is(v) ? JS_ITER_MAP_ENTRIES : JS_ITER_SET_VALUES, v,
                            js_undefined(), 0);
        if (js_is_undefined(it))
            return -1;
    } else if (!js_iterobj_is(v) && !inherits_iterator(ctx, v)) {
        if (!inherits_array(ctx, v))
            return 0;
        /* an array's inheritor: the array iterator over its `length` and indices */
        it = js_iterobj_new(ctx, JS_ITER_ARRAY_VALUES, v, js_undefined(), 0);
        if (js_is_undefined(it))
            return -1;
    }
    /* an iterator, or what inherits Iterator.prototype: [Symbol.iterator]() is the object itself,
     * and its next is read once (getting a property runs no guest code here) */
    js_gc_protect(ctx->vm, &it);
    bool ok = get_direct(ctx, it, &c->src);
    js_gc_unprotect(ctx->vm, &it);
    if (!ok) {
        c->src = v;
        return -1;
    }
    return 1;
}

int js_iter_next(JsContext *ctx, JsIterCursor *c, JsValue *out) {
    JsValue src = c->src;
    if (c->i == JS_ITER_DONE_INDEX)
        return 0;
    if (js_is_string(src)) {
        JsString *s = js_value_string(src);
        if (c->i >= s->length) {
            c->i = JS_ITER_DONE_INDEX;
            return 0;
        }
        JsString *ch = code_point_at(ctx->vm, s, &c->i);
        if (!ch)
            return (*out = js_oom_value(ctx->vm)), -1;
        *out = js_value_from_cell(&ch->gc);
        return 1;
    }
    if (js_iterobj_is(src)) {
        int rc = js_iterobj_step(ctx, (JsIterObj *)js_value_object(src), out);
        if (rc <= 0)
            c->i = JS_ITER_DONE_INDEX;
        return rc;
    }
    JsObject *a = js_value_object(src);
    if (c->i >= a->elem_count) {
        c->i = JS_ITER_DONE_INDEX;
        return 0;
    }
    *out = a->elems[c->i++];
    return 1;
}

/* IteratorClose on OBJ, an iterator a script can see (rooted by the caller). */
static bool close_object(JsContext *ctx, JsValue obj, bool throwing, bool syntax, JsValue *err) {
    JsValue ret;
    if (!get_named(ctx, obj, "return", &ret)) {
        if (throwing)
            return true;
        return (*err = ret), false;
    }
    if (js_is_undefined(ret) || js_is_null(ret))
        return true;
    if (!js_is_function(ret)) {
        if (throwing)
            return true;
        return (*err = js_error_fmt(ctx, "TypeError: %t is not a function", ret)), false;
    }
    JsValue res;
    bool ok = js_call(ctx, ret, obj, NULL, 0, &res);
    if (throwing)
        return true;
    if (!ok)
        return (*err = res), false;
    if (!is_obj(res))
        return (*err = syntax ? js_error_fmt(ctx, "TypeError: Iterator result %v is not an object", res)
                              : js_error_from_ascii(ctx, "TypeError: return called on non-object")),
               false;
    return true;
}

/* O's [[Prototype]] is P (NULL: O has none) */
static bool proto_is(const JsObject *o, const JsObject *p) {
    if (!p)
        return !is_obj(o->proto);
    return js_is_object(o->proto) && js_value_object(o->proto) == p;
}

/* IteratorClose for a cursor straight over an array or a string (SRC, stopped at I), for which
 * no iterator object was made. V8 reads `return` from the Array Iterator (the String Iterator)
 * it made, and so finds one a script put on that prototype, on Iterator.prototype or on
 * Object.prototype. The engine puts none on any of the three: while they are still the chain
 * and none has gained a key since `return` was last looked for (JsMap.gained), there is nothing
 * to call. Otherwise the iterator V8 would have had is made here, at the cursor's position, and
 * closed as any other. SRC is rooted by the caller. */
static bool close_direct(JsContext *ctx, JsValue src, uint32_t i, bool throwing, bool syntax,
                         JsValue *err) {
    JsVm *vm = ctx->vm;
    bool str = js_is_string(src);
    JsObject *chain[3] = {ctx->iter_protos[str ? JS_ITER_PROTO_STRING : JS_ITER_PROTO_ARRAY],
                          ctx->iterator_proto, ctx->object_proto};
    if (proto_is(chain[0], chain[1]) && proto_is(chain[1], chain[2]) && proto_is(chain[2], NULL)) {
        if (!chain[0]->props.gained && !chain[1]->props.gained && !chain[2]->props.gained)
            return true;
        JsString *k = js_ascii_cell(vm, "return");
        k = k ? js_intern_cell(vm, k) : NULL;
        bool absent = k != NULL;
        for (int n = 0; k && n < 3; n++) {
            if (js_map_position(&chain[n]->props, k) == JS_MAP_EMPTY)
                chain[n]->props.gained = false;
            else
                absent = false;
        }
        if (absent)
            return true;
    }
    JsValue it = js_iterobj_new(ctx, str ? JS_ITER_STRING : JS_ITER_ARRAY_VALUES, src, js_undefined(), i);
    if (js_is_undefined(it))
        return throwing || ((*err = js_oom_value(vm)), false);
    js_gc_protect(vm, &it);
    bool ok = close_object(ctx, it, throwing, syntax, err);
    js_gc_unprotect(vm, &it);
    return ok;
}

bool js_iter_close(JsContext *ctx, JsIterCursor *c, bool throwing, bool syntax, JsValue *err) {
    if (c->i == JS_ITER_DONE_INDEX)
        return true;
    uint32_t at = c->i;
    c->i = JS_ITER_DONE_INDEX;
    JsValue src = c->src;
    if (!js_iterobj_is(src))
        return close_direct(ctx, src, at, throwing, syntax, err);
    JsIterObj *it = (JsIterObj *)js_value_object(src);
    if (it->kind == JS_ITER_RECORD) {
        if (js_is_undefined(it->target))
            return true;
        return close_object(ctx, it->target, throwing, syntax, err); /* held by the record */
    }
    return close_object(ctx, src, throwing, syntax, err);
}

/* ---- natives that take an iterable ---- */

JsValue js_iter_not_iterable(JsContext *ctx, JsValue v) {
    return js_error_fmt(ctx, "TypeError: %t is not iterable (cannot read property Symbol(Symbol.iterator))", v);
}

bool js_iter_entry(JsContext *ctx, JsValue entry, JsValue *k, JsValue *v, JsValue *err) {
    if (!is_obj(entry))
        return (*err = js_error_fmt(ctx, "TypeError: Iterator value %v is not an entry object", entry)),
               false;
    if (is_array(entry) && js_value_object(entry)->elem_count >= 2) {
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
    JsValue adder;
    if (!get_named(ctx, coll, name, &adder))
        return (*r = adder), false;
    if (!js_is_function(adder))
        return (*r = js_error_fmt(ctx, "TypeError: '%v' returned for property '%s' of object '%v' is not a function",
                                  adder, name, coll)),
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
    if (!ok) {
        /* an abrupt completion closes the iterator (unless it was its own next that threw) */
        js_gc_protect(vm, r);
        JsValue ignored;
        (void)js_iter_close(ctx, &cur, true, false, &ignored);
        js_gc_unprotect(vm, r);
    }
    js_gc_unprotect(vm, &v);
    js_gc_unprotect(vm, &k);
    js_gc_unprotect(vm, &item);
    js_gc_unprotect(vm, &cur.src);
    js_gc_unprotect(vm, &adder);
    return ok;
}

/* ---- the Iterator helpers ---- */

/* The helper's underlying iterator's next value (as js_iter_next) */
static int under_next(JsContext *ctx, JsIterObj *h, JsValue *out) {
    JsIterCursor u = {h->target, h->index};
    int rc = js_iter_next(ctx, &u, out);
    h->index = u.i;
    return rc;
}

/* Closes the helper's underlying iterator (none for concat). THROWING keeps *err. */
static bool under_close(JsContext *ctx, JsIterObj *h, bool throwing, JsValue *err) {
    if (js_is_undefined(h->target))
        return true;
    JsIterCursor u = {h->target, h->index};
    bool ok = js_iter_close(ctx, &u, throwing, false, err);
    h->index = u.i;
    return ok;
}

/* An abrupt completion ERR closes the underlying iterator, then is thrown: -1, *out ERR */
static int under_abort(JsContext *ctx, JsIterObj *h, JsValue err, JsValue *out) {
    js_gc_protect(ctx->vm, &err);
    JsValue ignored;
    (void)under_close(ctx, h, true, &ignored);
    js_gc_unprotect(ctx->vm, &err);
    *out = err;
    return -1;
}

/* GetIteratorFlattenable(V): a string (when STRINGS), or an object: an iterable one opened as
 * for-of opens it, else the object itself as the iterator, its next read once. V must be rooted.
 * False thrown, *err the error; WHAT names the method for a primitive's error. */
static bool open_flattenable(JsContext *ctx, JsValue v, bool strings, const char *what,
                             JsIterCursor *c, JsValue *err) {
    if (!is_obj(v) && !(strings && js_is_string(v)))
        return (*err = js_error_fmt(ctx, "TypeError: %s called on non-object", what)), false;
    int rc = js_iter_open(ctx, v, c);
    if (rc < 0)
        return (*err = js_oom_value(ctx->vm)), false;
    if (rc > 0)
        return true;
    c->i = 0;
    return get_direct(ctx, v, &c->src) || ((*err = c->src), false);
}

/* The helper's work for one next(): 1 and *out the value, 0 done, -1 thrown */
static int helper_run(JsContext *ctx, JsIterObj *h, JsValue *out) {
    JsVm *vm = ctx->vm;
    for (;;) {
        switch ((JsIterHelperOp)h->op) {
        case JS_HELPER_MAP:
        case JS_HELPER_FILTER:
        case JS_HELPER_FLATMAP: {
            if (h->op == JS_HELPER_FLATMAP && !js_is_undefined(h->inner)) {
                JsIterCursor ic = {h->inner, h->inner_index};
                int rc = js_iter_next(ctx, &ic, out);
                h->inner_index = ic.i;
                if (rc < 0)
                    return under_abort(ctx, h, *out, out);
                if (rc > 0)
                    return 1;
                h->inner = js_undefined();
            }
            JsValue v;
            int rc = under_next(ctx, h, &v);
            if (rc <= 0) {
                *out = v;
                return rc;
            }
            js_gc_protect(vm, &v);
            JsValue argv[2] = {v, js_number(h->count)};
            JsValue res;
            bool ok = js_call(ctx, h->subject, js_undefined(), argv, 2, &res);
            js_gc_unprotect(vm, &v);
            if (!ok)
                return under_abort(ctx, h, res, out);
            h->count++;
            if (h->op == JS_HELPER_MAP) {
                *out = res;
                return 1;
            }
            if (h->op == JS_HELPER_FILTER) {
                if (js_to_boolean(res)) {
                    *out = v;
                    return 1;
                }
                continue;
            }
            js_gc_protect(vm, &res);
            JsIterCursor ic;
            JsValue err;
            ok = open_flattenable(ctx, res, false, "Iterator.prototype.flatMap", &ic, &err);
            js_gc_unprotect(vm, &res);
            if (!ok)
                return under_abort(ctx, h, err, out);
            h->inner = ic.src;
            h->inner_index = ic.i;
            continue;
        }
        case JS_HELPER_TAKE:
            if (h->count == 0) {
                JsValue err;
                if (!under_close(ctx, h, false, &err))
                    return (*out = err), -1;
                return 0;
            }
            if (h->count != INFINITY)
                h->count--;
            return under_next(ctx, h, out);
        case JS_HELPER_DROP:
            while (h->count > 0) {
                if (h->count != INFINITY)
                    h->count--;
                int rc = under_next(ctx, h, out);
                if (rc <= 0)
                    return rc;
            }
            return under_next(ctx, h, out);
        case JS_HELPER_CONCAT: {
            if (!js_is_undefined(h->inner)) {
                JsIterCursor ic = {h->inner, h->inner_index};
                int rc = js_iter_next(ctx, &ic, out);
                h->inner_index = ic.i;
                if (rc != 0)
                    return rc;
                h->inner = js_undefined();
            }
            JsObject *items = js_value_object(h->subject);
            if (h->count >= items->elem_count)
                return 0;
            JsValue item = items->elems[(uint32_t)h->count];
            h->count++;
            JsIterCursor ic;
            JsValue err;
            if (!open_flattenable(ctx, item, false, "Iterator.concat", &ic, &err))
                return (*out = err), -1;
            h->inner = ic.src;
            h->inner_index = ic.i;
            continue;
        }
        }
        return 0;
    }
}

static int helper_step(JsContext *ctx, JsIterObj *h, JsValue *out) {
    if (h->state == JS_HELPER_DONE)
        return 0;
    if (h->state == JS_HELPER_RUNNING)
        return (*out = js_error_from_ascii(ctx, "TypeError: Generator is already running")), -1;
    h->state = JS_HELPER_RUNNING;
    int rc = helper_run(ctx, h, out);
    if (rc > 0) {
        h->state = JS_HELPER_YIELD;
    } else {
        h->state = JS_HELPER_DONE;
        finish(ctx->vm, h);
    }
    return rc;
}

/* %IteratorHelperPrototype%.return: closes what the helper iterates (a flatMap's inner iterator
 * first, then the underlying one; concat's current iterable) */
static bool helper_return(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    JsIterObj *h = this_iter(tv, JS_ITER_PROTO_HELPER);
    if (!h)
        return (*r = js_error_receiver(ctx, family_class[JS_ITER_PROTO_HELPER], tv)), false;
    if (h->state == JS_HELPER_RUNNING)
        return (*r = js_error_from_ascii(ctx, "TypeError: Generator is already running")), false;
    bool ok = true;
    if (h->state != JS_HELPER_DONE) {
        bool started = h->state == JS_HELPER_YIELD;
        h->state = started ? JS_HELPER_RUNNING : JS_HELPER_DONE;
        if (started && !js_is_undefined(h->inner)) {
            JsIterCursor ic = {h->inner, h->inner_index};
            ok = js_iter_close(ctx, &ic, false, false, r);
            if (!ok && h->op == JS_HELPER_FLATMAP) {
                JsValue ignored;
                js_gc_protect(ctx->vm, r);
                (void)under_close(ctx, h, true, &ignored);
                js_gc_unprotect(ctx->vm, r);
            }
        }
        if (ok && h->op != JS_HELPER_CONCAT)
            ok = under_close(ctx, h, false, r);
        h->state = JS_HELPER_DONE;
        finish(ctx->vm, h);
    }
    if (!ok)
        return false;
    *r = js_iter_result(ctx, js_undefined(), true);
    return js_is_undefined(*r) ? ((*r = js_oom_value(ctx->vm)), false) : true;
}

/* %WrapForValidIteratorPrototype%.return: the wrapped iterator's return, if it has one */
static bool wrap_return(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    JsIterObj *w = this_iter(tv, JS_ITER_PROTO_WRAP);
    if (!w)
        return (*r = bad_receiver(ctx, JS_ITER_PROTO_WRAP, "return", tv)), false;
    JsValue ret;
    if (!get_named(ctx, w->target, "return", &ret))
        return (*r = ret), false;
    if (js_is_undefined(ret) || js_is_null(ret)) {
        *r = js_iter_result(ctx, js_undefined(), true);
        return js_is_undefined(*r) ? ((*r = js_oom_value(ctx->vm)), false) : true;
    }
    if (!js_is_function(ret))
        return (*r = js_error_fmt(ctx, "TypeError: '%v' returned for property 'return' of object '%v' is not a function",
                                  ret, w->target)),
               false;
    return js_call(ctx, ret, w->target, NULL, 0, r);
}

/* Iterator.prototype.<WHAT> on a receiver that is not an object */
static bool non_object(JsContext *ctx, const char *what, JsValue *r) {
    *r = js_error_fmt(ctx, "TypeError: Iterator.prototype.%s called on non-object", what);
    return false;
}

/* A bad argument ERR closes the receiver O, then is thrown */
static bool reject_arg(JsContext *ctx, JsValue o, JsValue err, JsValue *r) {
    js_gc_protect(ctx->vm, &err);
    JsValue ignored;
    (void)close_object(ctx, o, true, false, &ignored);
    js_gc_unprotect(ctx->vm, &err);
    *r = err;
    return false;
}

/* map, filter, flatMap, take and drop: a helper over GetIteratorDirect(this) */
static bool make_helper(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r,
                        JsIterHelperOp op, const char *what) {
    if (!is_obj(tv))
        return non_object(ctx, what, r);
    JsValue fn = js_undefined();
    double count = 0;
    if (op == JS_HELPER_TAKE || op == JS_HELPER_DROP) {
        double n;
        if (!js_op_to_number(ctx, ARG(0), &n))
            return reject_arg(ctx, tv, js_take_error(ctx), r);
        if (n != n)
            return reject_arg(ctx, tv, js_error_fmt(ctx, "RangeError: %v must be positive", ARG(0)), r);
        n = isinf(n) ? n : trunc(n);
        if (n < 0)
            return reject_arg(ctx, tv, js_error_fmt(ctx, "RangeError: %v must be positive", ARG(0)), r);
        count = n + 0.0; /* -0 is 0 */
    } else {
        fn = ARG(0);
        if (!js_is_function(fn))
            return reject_arg(ctx, tv,
                              op == JS_HELPER_FLATMAP
                                  ? js_error_from_ascii(ctx, "TypeError: Iterator.prototype.flatMap is not a function")
                                  : js_error_fmt(ctx, "TypeError: %t is not a function", fn),
                              r);
    }
    JsValue src;
    if (!get_direct(ctx, tv, &src))
        return (*r = src), false;
    js_gc_protect(ctx->vm, &src);
    JsValue hv = js_iterobj_new(ctx, JS_ITER_HELPER, src, fn, 0);
    js_gc_unprotect(ctx->vm, &src);
    if (js_is_undefined(hv))
        return (*r = js_oom_value(ctx->vm)), false;
    JsIterObj *h = (JsIterObj *)js_value_object(hv);
    h->op = (uint8_t)op;
    h->count = count;
    *r = hv;
    return true;
}

#define HELPER_NATIVE(name, op, what)                                                              \
    static bool name(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {      \
        return make_helper(ctx, tv, args, argc, r, op, what);                                      \
    }
HELPER_NATIVE(ip_map, JS_HELPER_MAP, "map")
HELPER_NATIVE(ip_filter, JS_HELPER_FILTER, "filter")
HELPER_NATIVE(ip_take, JS_HELPER_TAKE, "take")
HELPER_NATIVE(ip_drop, JS_HELPER_DROP, "drop")
HELPER_NATIVE(ip_flatMap, JS_HELPER_FLATMAP, "flatMap")

/* reduce, toArray, forEach, some, every and find: they run the iterator here */
enum { RUN_REDUCE, RUN_TO_ARRAY, RUN_FOR_EACH, RUN_SOME, RUN_EVERY, RUN_FIND };

static bool run_iterator(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r,
                         int mode, const char *what) {
    JsVm *vm = ctx->vm;
    if (!is_obj(tv))
        return non_object(ctx, what, r);
    JsValue fn = ARG(0);
    if (mode != RUN_TO_ARRAY && !js_is_function(fn))
        return reject_arg(ctx, tv, js_error_fmt(ctx, "TypeError: %t is not a function", fn), r);
    JsIterCursor cur = {js_undefined(), 0};
    if (!get_direct(ctx, tv, &cur.src))
        return (*r = cur.src), false;
    js_gc_protect(vm, &cur.src);
    JsValue acc = js_undefined(), v = js_undefined();
    js_gc_protect(vm, &acc);
    js_gc_protect(vm, &v);
    double counter = 0;
    bool ok = true;
    int rc;
    if (mode == RUN_TO_ARRAY) {
        JsObject *a = js_array_new_cell(ctx, 0);
        if (!a) {
            *r = js_oom_value(vm);
            ok = false;
        } else {
            acc = js_value_from_cell(&a->gc);
        }
    } else if (mode == RUN_REDUCE) {
        if (argc >= 2) {
            acc = args[1];
        } else {
            rc = js_iter_next(ctx, &cur, &acc);
            if (rc < 0) {
                *r = acc;
                ok = false;
            } else if (rc == 0) {
                *r = js_error_from_ascii(ctx, "TypeError: Reduce of a done iterator with no initial value");
                ok = false;
            }
            counter = 1;
        }
    }
    bool decided = false;
    while (ok && !decided && (rc = js_iter_next(ctx, &cur, &v)) != 0) {
        if (rc < 0) {
            *r = v;
            ok = false;
            break;
        }
        if (mode == RUN_TO_ARRAY) {
            if (!js_array_append(vm, js_value_object(acc), v)) {
                *r = js_oom_value(vm);
                ok = false;
            }
            continue;
        }
        JsValue res;
        bool called;
        if (mode == RUN_REDUCE) {
            JsValue argv[3] = {acc, v, js_number(counter)};
            called = js_call(ctx, fn, js_undefined(), argv, 3, &res);
        } else {
            JsValue argv[2] = {v, js_number(counter)};
            called = js_call(ctx, fn, js_undefined(), argv, 2, &res);
        }
        if (!called) {
            *r = res;
            js_gc_protect(vm, r);
            JsValue ignored;
            (void)js_iter_close(ctx, &cur, true, false, &ignored);
            js_gc_unprotect(vm, r);
            ok = false;
            break;
        }
        counter++;
        switch (mode) {
        case RUN_REDUCE:
            acc = res;
            break;
        case RUN_SOME:
        case RUN_FIND:
            if (js_to_boolean(res)) {
                acc = mode == RUN_SOME ? js_bool(true) : v;
                decided = true;
            }
            break;
        case RUN_EVERY:
            if (!js_to_boolean(res)) {
                acc = js_bool(false);
                decided = true;
            }
            break;
        default:
            break;
        }
    }
    if (ok && decided)
        ok = js_iter_close(ctx, &cur, false, false, r);
    if (ok && !decided)
        acc = mode == RUN_SOME ? js_bool(false) : mode == RUN_EVERY ? js_bool(true)
              : mode == RUN_FIND || mode == RUN_FOR_EACH ? js_undefined() : acc;
    if (ok)
        *r = acc;
    js_gc_unprotect(vm, &v);
    js_gc_unprotect(vm, &acc);
    js_gc_unprotect(vm, &cur.src);
    return ok;
}

#define RUN_NATIVE(name, mode, what)                                                               \
    static bool name(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {      \
        return run_iterator(ctx, tv, args, argc, r, mode, what);                                   \
    }
RUN_NATIVE(ip_reduce, RUN_REDUCE, "reduce")
RUN_NATIVE(ip_toArray, RUN_TO_ARRAY, "toArray")
RUN_NATIVE(ip_forEach, RUN_FOR_EACH, "forEach")
RUN_NATIVE(ip_some, RUN_SOME, "some")
RUN_NATIVE(ip_every, RUN_EVERY, "every")
RUN_NATIVE(ip_find, RUN_FIND, "find")

/* ---- the Iterator global ---- */

/* Iterator is abstract: `new Iterator()` throws, as V8's does when new.target is Iterator itself
 * (there are no classes, so it always is) */
static bool g_Iterator(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    (void)args;
    (void)argc;
    *r = js_error_from_ascii(ctx, js_native_is_new(ctx)
                                      ? "TypeError: Abstract class Iterator not directly constructable"
                                      : "TypeError: Constructor Iterator requires 'new'");
    return false;
}

/* Iterator.from(O): an iterator that inherits from Iterator.prototype for O's iterator (a
 * string's, an iterable's, or O itself as one), wrapped when it does not */
static bool it_from(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsVm *vm = ctx->vm;
    JsValue o = ARG(0);
    if (js_is_string(o)) {
        *r = js_iterobj_new(ctx, JS_ITER_STRING, o, js_undefined(), 0);
        return js_is_undefined(*r) ? ((*r = js_oom_value(vm)), false) : true;
    }
    if (!is_obj(o))
        return (*r = js_error_from_ascii(ctx, "TypeError: Iterator.from called on non-object")), false;
    JsValue iter = o;
    if (inherits_array(ctx, o)) { /* an array, or what has Array.prototype.values as its iterator */
        iter = js_iterobj_new(ctx, JS_ITER_ARRAY_VALUES, o, js_undefined(), 0);
    } else if (js_mapobj_is(o) || js_setobj_is(o)) {
        iter = js_iterobj_new(ctx, js_mapobj_is(o) ? JS_ITER_MAP_ENTRIES : JS_ITER_SET_VALUES, o,
                              js_undefined(), 0);
    }
    if (js_is_undefined(iter))
        return (*r = js_oom_value(vm)), false;
    js_gc_protect(vm, &iter);
    JsValue next;
    bool ok = get_named(ctx, iter, "next", &next);
    if (!ok) {
        *r = next;
    } else if (inherits_iterator(ctx, iter)) {
        *r = iter;
    } else {
        js_gc_protect(vm, &next);
        *r = js_iterobj_new(ctx, JS_ITER_WRAP, iter, next, 0);
        js_gc_unprotect(vm, &next);
        if (js_is_undefined(*r)) {
            *r = js_oom_value(vm);
            ok = false;
        }
    }
    js_gc_unprotect(vm, &iter);
    return ok;
}

/* Iterator.concat(...iterables): their values in turn, each opened when it is reached */
static bool it_concat(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsVm *vm = ctx->vm;
    for (int i = 0; i < argc; i++) {
        if (!is_obj(args[i]))
            return (*r = js_error_from_ascii(ctx, "TypeError: Iterator.concat called on non-object")), false;
        if (!js_iter_is_iterable(ctx, args[i]))
            return (*r = js_error_fmt(ctx, "TypeError: %v is not iterable", args[i])), false;
    }
    JsObject *items = js_array_new_cell(ctx, (uint32_t)argc);
    if (!items)
        return (*r = js_oom_value(vm)), false;
    for (int i = 0; i < argc; i++)
        items->elems[items->elem_count++] = args[i];
    JsValue iv = js_value_from_cell(&items->gc);
    js_gc_protect(vm, &iv);
    JsValue hv = js_iterobj_new(ctx, JS_ITER_HELPER, js_undefined(), iv, 0);
    js_gc_unprotect(vm, &iv);
    if (js_is_undefined(hv))
        return (*r = js_oom_value(vm)), false;
    ((JsIterObj *)js_value_object(hv))->op = JS_HELPER_CONCAT;
    *r = hv;
    return true;
}

/* ---- Array.prototype keys / values / entries ---- */

bool js_array_iter_method(JsContext *ctx, JsValue tv, JsIterKind kind, JsValue *r) {
    if (js_is_undefined(tv) || js_is_null(tv))
        return (*r = js_error_from_ascii(ctx, "TypeError: Cannot convert undefined or null to object")),
               false;
    *r = js_iterobj_new(ctx, kind, tv, js_undefined(), 0);
    return js_is_undefined(*r) ? ((*r = js_oom_value(ctx->vm)), false) : true;
}

/* ---- registration ---- */

static bool def_method(JsContext *ctx, JsObject *o, const char *name, JsNativeFn fn) {
    JsValue nf = js_native_new(ctx, name, fn, NULL);
    return js_is_function(nf) && js_object_set_hidden(ctx, o, name, nf);
}

/* Iterator.prototype.constructor: an accessor, as the spec has it (27.1.4.1), whose getter and
 * setter these are (js_accessor_call). The getter gives Iterator whatever its receiver; the
 * setter is the spec's SetterThatIgnoresPrototypeProperties: it refuses Iterator.prototype
 * itself, and on any other object makes or sets an own `constructor`, so an assignment to
 * what inherits from Iterator.prototype shadows the accessor as it would a data property.
 * False with the error in *OUT. */
bool js_iter_ctor_accessor(JsContext *ctx, bool set, JsValue recv, JsValue arg, JsValue *out) {
    JsVm *vm = ctx->vm;
    if (!set) {
        *out = ctx->iterator_ctor ? js_value_from_cell(&ctx->iterator_ctor->gc) : js_undefined();
        return true;
    }
    *out = js_undefined();
    if (!js_is_object(recv) && !js_is_function(recv) && !js_is_promise(recv)) {
        *out = js_error_fmt(ctx, "TypeError: set Iterator.prototype.constructor called on non-object");
        return false;
    }
    if (js_is_object(recv) && js_value_object(recv) == ctx->iterator_proto) {
        *out = js_error_fmt(ctx, "TypeError: Cannot assign to read only property 'constructor' of object '%v'", recv);
        return false;
    }
    js_gc_protect(vm, &recv);
    js_gc_protect(vm, &arg);
    JsString *k = js_ascii_cell(vm, "constructor");
    JsValue kv = k ? js_value_from_cell(&k->gc) : js_undefined();
    js_gc_protect(vm, &kv);
    bool ok = k != NULL;
    if (ok) {
        JsValue cur;
        uint8_t attrs;
        int st = js_own_property(ctx, recv, kv, &cur, &attrs);
        if (st < 0)
            ok = false;
        else if (st == 0) { /* CreateDataPropertyOrThrow */
            JsPropDesc d = {JS_DESC_VALUE | JS_DESC_WRITABLE | JS_DESC_ENUMERABLE | JS_DESC_CONFIGURABLE,
                            true, true, true, arg};
            ok = js_define_own_property(ctx, recv, kv, &d);
        } else if (st == 2 && js_accessor_setter(cur) == JS_ACC_ITER_CTOR_SET) {
            /* its own `constructor` is this accessor again (Object.defineProperty put it there):
             * the Set the spec asks for would call this setter without end, which V8 does, to
             * the same error */
            js_set_error(ctx, js_error_fmt(ctx, "RangeError: Maximum call stack size exceeded"));
            ok = false;
        } else
            ok = js_set_property(ctx, recv, kv, arg);
    }
    js_gc_unprotect(vm, &kv);
    js_gc_unprotect(vm, &arg);
    js_gc_unprotect(vm, &recv);
    if (!ok) {
        *out = js_is_undefined(ctx->pending_error) ? js_oom_value(vm) : js_take_error(ctx);
        return false;
    }
    return true;
}

bool js_iter_builtins_init(JsContext *ctx) {
    JsVm *vm = ctx->vm;
    /* %IteratorPrototype%, Iterator.prototype: each family's prototype chains to it */
    JsValue ip = js_object_new(ctx);
    if (!js_is_object(ip))
        return false;
    ctx->iterator_proto = js_value_object(ip); /* rooted via the context now */
    for (int k = 0; k < JS_ITER_PROTO_COUNT; k++) {
        JsValue p = js_object_new(ctx);
        if (!js_is_object(p))
            return false;
        js_value_object(p)->proto = ip;
        ctx->iter_protos[k] = js_value_object(p);
        if (!def_method(ctx, ctx->iter_protos[k], "next", family_next[k]))
            return false;
    }
    if (!def_method(ctx, ctx->iter_protos[JS_ITER_PROTO_HELPER], "return", helper_return) ||
        !def_method(ctx, ctx->iter_protos[JS_ITER_PROTO_WRAP], "return", wrap_return))
        return false;
    /* an array or a string is iterated without an iterator object while these are untouched */
    if (!js_intact_watch(ctx, JS_INTACT_ARRAY_ITER_NEXT, ctx->iter_protos[JS_ITER_PROTO_ARRAY], "next") ||
        !js_intact_watch(ctx, JS_INTACT_STRING_ITER_NEXT, ctx->iter_protos[JS_ITER_PROTO_STRING], "next"))
        return false;

    static const struct {
        const char *name;
        JsNativeFn fn;
    } methods[] = {
        {"reduce", ip_reduce}, {"toArray", ip_toArray}, {"forEach", ip_forEach},
        {"some", ip_some},     {"every", ip_every},     {"find", ip_find},
        {"map", ip_map},       {"filter", ip_filter},   {"take", ip_take},
        {"drop", ip_drop},     {"flatMap", ip_flatMap},
    };
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++) {
        if (!def_method(ctx, ctx->iterator_proto, methods[i].name, methods[i].fn))
            return false;
    }

    /* the Iterator global, with from and concat */
    JsValue ctor = js_native_new(ctx, "Iterator", g_Iterator, NULL);
    if (!js_is_function(ctor))
        return false;
    js_gc_protect(vm, &ctor);
    js_native_builtin_ctor(ctor, ctx->iterator_proto);
    JsValue statics = js_object_new(ctx);
    bool ok = js_is_object(statics);
    if (ok) {
        ((JsNative *)js_value_cell(ctor))->statics = js_value_object(statics);
        ok = def_method(ctx, js_value_object(statics), "from", it_from) &&
             def_method(ctx, js_value_object(statics), "concat", it_concat) &&
             js_object_set_accessor(ctx, ctx->iterator_proto, "constructor", JS_ACC_ITER_CTOR_GET,
                                    JS_ACC_ITER_CTOR_SET) &&
             js_object_set_ascii(ctx, ctx->globals, "Iterator", ctor);
        if (ok)
            ctx->iterator_ctor = (JsNative *)js_value_cell(ctor); /* reachable from the globals */
    }
    js_gc_unprotect(vm, &ctor);
    return ok;
}

bool js_group_by(JsContext *ctx, JsValue items, JsValue cb, bool property_keys, JsValue groups,
                 JsGroupAdd add, JsValue *r) {
    JsVm *vm = ctx->vm;
    JsIterCursor cur;
    int open = js_iter_open(ctx, items, &cur);
    if (open <= 0) {
        *r = open < 0 ? js_oom_value(vm) : js_iter_not_iterable(ctx, items);
        return false;
    }
    JsValue v = js_undefined(), key = js_undefined();
    js_gc_protect(vm, &cur.src);
    js_gc_protect(vm, &cb);
    js_gc_protect(vm, &v);
    js_gc_protect(vm, &key);
    bool ok = true, thrown = false;
    double k = 0;
    int st;
    while ((st = js_iter_next(ctx, &cur, &v)) != 0) {
        if (st < 0) { /* its own next threw: nothing to close */
            *r = v;
            ok = false;
            break;
        }
        JsValue argv[2] = {v, js_number(k)};
        if (!js_call(ctx, cb, js_undefined(), argv, 2, &key)) {
            *r = key;
            thrown = true;
            break;
        }
        if (property_keys) {
            JsString *ks = js_op_to_string(ctx, key);
            if (!ks) {
                *r = js_take_error(ctx);
                thrown = true;
                break;
            }
            key = js_value_from_cell(&ks->gc);
        } else {
            key = js_normalize_map_key(key);
        }
        if (!add(ctx, groups, key, v)) {
            *r = js_oom_value(vm);
            ok = false;
            break;
        }
        k++;
    }
    if (thrown) {
        /* V8 groups an array that still has Array.prototype (and the Array Iterator's own next)
         * by a path of its own, with no iterator to close: a `return` a script put where
         * close_direct looks is not called for one, though the specification would call it */
        bool plain = is_array(cur.src) && proto_is(js_value_object(cur.src), ctx->array_proto);
        js_gc_protect(vm, r);
        JsValue ignored;
        if (!plain)
            (void)js_iter_close(ctx, &cur, true, false, &ignored);
        js_gc_unprotect(vm, r);
        ok = false;
    }
    js_gc_unprotect(vm, &key);
    js_gc_unprotect(vm, &v);
    js_gc_unprotect(vm, &cb);
    js_gc_unprotect(vm, &cur.src);
    return ok;
}
