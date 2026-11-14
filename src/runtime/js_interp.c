#include <stddef.h>
#include <string.h>
#include "js_bytecode.h"
#include "js_error.h"
#include "js_date.h"
#include "js_mapobj.h"
#include "js_setobj.h"
#ifdef LAMASSU_HAS_REGEX
#include "js_regexp.h"
#endif

/*
 * Bytecode interpreter. All execution state lives in a JsFiber (a GC cell
 * rooted via ctx->fiber), never on the C stack — so any opcode may allocate
 * and trigger collection safely, and later phases get suspension for free.
 *
 * Semantics notes (documented approximations, revisited with builtins):
 * - % is a - trunc(a/b)*b (may differ in the last ulp for extreme ratios).
 * - ** supports integer exponents and ±0.5 exactly; other fractional
 *   exponents yield NaN (TODO transcendental pow).
 * - delete on array elements writes undefined instead of making a hole.
 */

#define JS_MAX_REENTRY 200

/* A property access's outcome. JS_PROP_OOM is any failure whose error is pending in the context
 * (js_take_error): out of memory, or a key's conversion that threw. */
typedef enum { JS_PROP_OK, JS_PROP_OOM, JS_PROP_TYPE_ERROR } JsPropStatus;

/* ---- small helpers ---- */

JsString *js_ascii_cell(JsVm *vm, const char *s) {
    uint16_t units[128];
    size_t n = 0;
    while (s[n] && n < 128) {
        units[n] = (uint16_t)(unsigned char)s[n];
        n++;
    }
    return js_string_cell_new(vm, units, n);
}

static bool units_is_ascii_str(const JsString *s, const char *ascii) {
    for (uint32_t i = 0; i < s->length; i++) {
        if (ascii[i] == '\0' || s->units[i] != (uint16_t)(unsigned char)ascii[i])
            return false;
    }
    return ascii[s->length] == '\0';
}

bool js_to_boolean(JsValue v) {
    if (js_is_number(v)) {
        double d = js_get_number(v);
        return d == d && d != 0.0;
    }
    if (js_is_string(v))
        return js_value_string(v)->length != 0;
    if (v.bits == JS_SPECIAL_TRUE)
        return true;
    if (v.bits == JS_SPECIAL_FALSE || js_is_undefined(v) || js_is_null(v))
        return false;
    return true; /* objects, functions */
}

static bool is_nullish(JsValue v) { return js_is_undefined(v) || js_is_null(v); }
static bool is_array(JsValue v) {
    return js_is_object(v) && js_value_object(v)->obj_kind == JS_OBJ_ARRAY;
}

/* ---- conversions (ECMA-262 7.1) ---- */

static inline bool is_object_type(JsValue v);
static JsPropStatus get_property(JsContext *ctx, JsValue base, JsValue key, JsValue *out);
bool js_native_is_object_valueof(JsValue fn); /* js_builtins.c */

void js_set_error(JsContext *ctx, JsValue error) { ctx->pending_error = error; }

JsValue js_take_error(JsContext *ctx) {
    JsValue e = ctx->pending_error;
    ctx->pending_error = js_undefined();
    return js_is_undefined(e) ? js_oom_value(ctx->vm) : e;
}

static bool fail_oom(JsContext *ctx) {
    ctx->pending_error = js_oom_value(ctx->vm);
    return false;
}

static bool fail_ascii(JsContext *ctx, const char *msg) {
    ctx->pending_error = js_error_from_ascii(ctx, msg);
    return false;
}

JsString *js_primitive_to_string(JsVm *vm, JsValue v) {
    if (js_is_string(v))
        return js_value_string(v);
    if (js_is_number(v))
        return js_number_to_string(vm, js_get_number(v));
    if (js_is_null(v))
        return js_ascii_cell(vm, "null");
    if (v.bits == JS_SPECIAL_TRUE)
        return js_ascii_cell(vm, "true");
    if (v.bits == JS_SPECIAL_FALSE)
        return js_ascii_cell(vm, "false");
    return js_ascii_cell(vm, "undefined");
}

double js_primitive_to_number(JsValue v) {
    if (js_is_number(v))
        return js_get_number(v);
    if (js_is_null(v) || v.bits == JS_SPECIAL_FALSE)
        return 0.0;
    if (v.bits == JS_SPECIAL_TRUE)
        return 1.0;
    if (js_is_string(v)) {
        JsString *s = js_value_string(v);
        return js_units_to_number(s->units, s->length);
    }
    return __builtin_nan(""); /* undefined */
}

/*
 * ToPrimitive (7.1.1) with OrdinaryToPrimitive (7.1.1.1): valueOf then toString, or toString first
 * for the string hint, each looked up through the prototype chain and called when it is a
 * function; the first primitive result wins, and neither giving one is a TypeError. A Date's default
 * hint is string (its @@toPrimitive, the only one there is without symbols). Object.prototype.valueOf
 * gives the object back, so it is skipped without a call. The calls nest on the C stack (a builtin
 * toString converting an element converts again), so they count against the interpreter's
 * re-entry limit.
 */
bool js_to_primitive(JsContext *ctx, JsValue v, int hint, JsValue *out) {
    if (!is_object_type(v)) {
        *out = v;
        return true;
    }
    if (hint == JS_HINT_DEFAULT && js_is_object(v) && js_value_object(v)->obj_kind == JS_OBJ_DATE)
        hint = JS_HINT_STRING;
    if (ctx->reentry_depth >= JS_MAX_REENTRY)
        return fail_ascii(ctx, "RangeError: Maximum call stack size exceeded");
    JsVm *vm = ctx->vm;
    static const uint16_t vo[] = {'v', 'a', 'l', 'u', 'e', 'O', 'f'};
    static const uint16_t ts[] = {'t', 'o', 'S', 't', 'r', 'i', 'n', 'g'};
    JsValue m = js_undefined(), res = js_undefined();
    js_gc_protect(vm, &v);
    js_gc_protect(vm, &m);
    js_gc_protect(vm, &res);
    ctx->reentry_depth++;
    bool ok = false, done = false;
    for (int i = 0; i < 2 && !done; i++) {
        bool want_ts = (hint == JS_HINT_STRING) == (i == 0);
        JsValue key = want_ts ? js_atom(vm, ts, 8) : js_atom(vm, vo, 7);
        m = js_undefined();
        if (!js_is_string(key) || get_property(ctx, v, key, &m) != JS_PROP_OK) {
            if (js_is_undefined(ctx->pending_error))
                ctx->pending_error = js_oom_value(vm);
            done = true;
            break;
        }
        if (!js_is_function(m) || js_native_is_object_valueof(m))
            continue;
        if (!js_call(ctx, m, v, NULL, 0, &res)) {
            ctx->pending_error = res;
            done = true;
            break;
        }
        if (!is_object_type(res)) {
            *out = res;
            ok = done = true;
        }
    }
    ctx->reentry_depth--;
    js_gc_unprotect(vm, &res);
    js_gc_unprotect(vm, &m);
    js_gc_unprotect(vm, &v);
    if (!done)
        return fail_ascii(ctx, "TypeError: Cannot convert object to primitive value");
    return ok;
}

bool js_get_property(JsContext *ctx, JsValue base, JsValue key, JsValue *out) {
    *out = js_undefined();
    JsPropStatus st = get_property(ctx, base, key, out);
    if (st == JS_PROP_OK)
        return true;
    if (st == JS_PROP_TYPE_ERROR)
        return fail_ascii(ctx, "TypeError: Cannot convert undefined or null to object");
    return false;
}

bool js_native_keep(JsContext *ctx, JsValue v) {
    JsNativeAct *a = ctx->native_act;
    if (!js_is_string(v) && !is_object_type(v))
        return true; /* not a cell */
    if (!a || a->nkeep == JS_NATIVE_KEEP_MAX)
        return fail_oom(ctx); /* fails closed: a value it cannot root is never used */
    a->keep[a->nkeep++] = v;
    return true;
}

JsString *js_native_string(JsContext *ctx, JsValue v) {
    JsString *s = js_op_to_string(ctx, v);
    if (s && s != (js_is_string(v) ? js_value_string(v) : NULL) &&
        !js_native_keep(ctx, js_value_from_cell(&s->gc)))
        return NULL;
    return s;
}

JsString *js_op_to_string(JsContext *ctx, JsValue v) {
    if (js_is_string(v))
        return js_value_string(v);
    JsValue p;
    if (!js_to_primitive(ctx, v, JS_HINT_STRING, &p))
        return NULL;
    JsString *s = js_primitive_to_string(ctx->vm, p);
    if (!s)
        fail_oom(ctx);
    return s;
}

bool js_op_to_number(JsContext *ctx, JsValue v, double *out) {
    if (js_is_number(v)) {
        *out = js_get_number(v);
        return true;
    }
    JsValue p;
    if (!js_to_primitive(ctx, v, JS_HINT_NUMBER, &p)) {
        *out = __builtin_nan("");
        return false;
    }
    *out = js_primitive_to_number(p);
    return true;
}

bool js_strict_equals(JsValue a, JsValue b) {
    if (js_is_number(a) && js_is_number(b))
        return js_get_number(a) == js_get_number(b);
    if (js_is_string(a) && js_is_string(b))
        return js_string_equals(a, b);
    return a.bits == b.bits;
}

/* IsLooselyEqual (7.2.14). 1 or 0, or -1 when converting an object failed (error pending). */
__attribute__((noinline)) static int loose_eq_values(JsContext *ctx, JsValue a, JsValue b) {
    for (int guard = 0; guard < 4; guard++) {
        if (is_nullish(a) || is_nullish(b))
            return is_nullish(a) && is_nullish(b);
        bool an = js_is_number(a), bn = js_is_number(b);
        bool as = js_is_string(a), bs = js_is_string(b);
        if (an && bn)
            return js_get_number(a) == js_get_number(b);
        if (as && bs)
            return js_string_equals(a, b);
        bool acell = is_object_type(a), bcell = is_object_type(b);
        if (acell && bcell)
            return a.bits == b.bits;
        if (js_is_bool(a)) {
            a = js_number(js_get_bool(a) ? 1 : 0);
            continue;
        }
        if (js_is_bool(b)) {
            b = js_number(js_get_bool(b) ? 1 : 0);
            continue;
        }
        if (an && bs)
            return js_get_number(a) == js_primitive_to_number(b);
        if (as && bn)
            return js_primitive_to_number(a) == js_get_number(b);
        /* an object against a number or string: its primitive, default hint (a and b are the
         * caller's rooted operands; the primitive replaces one of them) */
        if (acell) {
            if (!js_to_primitive(ctx, a, JS_HINT_DEFAULT, &a))
                return -1;
            continue;
        }
        if (bcell) {
            if (!js_to_primitive(ctx, b, JS_HINT_DEFAULT, &b))
                return -1;
            continue;
        }
        return 0;
    }
    return 0;
}

/* IsLessThan (7.2.13) after both operands' ToPrimitive (number hint), left first.
 * -1: a<b, 0: equal, 1: a>b, 2: unordered (NaN), 3: a conversion failed (error pending).
 * The caller roots a and b; a primitive made from an object is rooted here while b converts. */
__attribute__((noinline)) static int js_compare_values(JsContext *ctx, JsValue a, JsValue b) {
    if (is_object_type(a) || is_object_type(b)) {
        JsValue pa = a, pb = b;
        if (!js_to_primitive(ctx, a, JS_HINT_NUMBER, &pa))
            return 3;
        js_gc_protect(ctx->vm, &pa);
        bool ok = js_to_primitive(ctx, b, JS_HINT_NUMBER, &pb);
        js_gc_unprotect(ctx->vm, &pa);
        if (!ok)
            return 3;
        a = pa;
        b = pb;
    }
    if (js_is_string(a) && js_is_string(b)) {
        JsString *x = js_value_string(a), *y = js_value_string(b);
        uint32_t n = x->length < y->length ? x->length : y->length;
        for (uint32_t i = 0; i < n; i++) {
            if (x->units[i] != y->units[i])
                return x->units[i] < y->units[i] ? -1 : 1;
        }
        return x->length == y->length ? 0 : (x->length < y->length ? -1 : 1);
    }
    double da = js_primitive_to_number(a);
    double db = js_primitive_to_number(b);
    if (da != da || db != db)
        return 2;
    if (da < db)
        return -1;
    return da == db ? 0 : 1;
}

static double js_fmod_approx(double a, double b) {
    if (a != a || b != b || b == 0.0 || a - a != 0.0 /* a inf */)
        return __builtin_nan("");
    if (b - b != 0.0) /* b inf, a finite */
        return a;
    if (a == 0.0)
        return a;
    double q = __builtin_trunc(a / b);
    return a - q * b;
}

static double js_pow_op(double base, double e) {
    if (e == 0.0)
        return 1.0;
    if (e != e)
        return __builtin_nan("");
    if (e == 0.5)
        return __builtin_sqrt(base);
    if (e == -0.5)
        return 1.0 / __builtin_sqrt(base);
    if (e == __builtin_trunc(e) && e >= -9007199254740992.0 &&
        e <= 9007199254740992.0) {
        bool neg = e < 0;
        uint64_t n = (uint64_t)(neg ? -e : e);
        double result = 1.0, acc = base;
        while (n) {
            if (n & 1)
                result *= acc;
            acc *= acc;
            n >>= 1;
        }
        return neg ? 1.0 / result : result;
    }
    return __builtin_nan(""); /* TODO transcendental pow */
}

/* ---- property access ---- */

static bool value_to_index(JsValue v, uint32_t *idx) {
    if (!js_is_number(v))
        return false;
    double d = js_get_number(v);
    /* bounds-check before the cast: (uint32_t)d is UB for d outside
     * [0, UINT32_MAX], e.g. arr[1e20] or arr[-1]. 0xFFFFFFFF is excluded by
     * the strict '<' (not a valid array index, matching real JS). */
    if (!(d >= 0 && d < 4294967295.0))
        return false;
    uint32_t u = (uint32_t)d;
    if ((double)u != d)
        return false;
    *idx = u;
    return true;
}

static bool units_to_index(const uint16_t *units, uint32_t len, uint32_t *idx) {
    if (len == 0 || len > 10)
        return false;
    if (units[0] == '0' && len > 1)
        return false;
    uint64_t v = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (units[i] < '0' || units[i] > '9')
            return false;
        v = v * 10 + (uint64_t)(units[i] - '0');
    }
    if (v >= 0xFFFFFFFF)
        return false;
    *idx = (uint32_t)v;
    return true;
}

static bool key_to_index(JsValue key, uint32_t *idx) {
    if (value_to_index(key, idx))
        return true;
    if (js_is_string(key)) {
        JsString *s = js_value_string(key);
        return units_to_index(s->units, s->length, idx);
    }
    return false;
}

/* Canonical interned key; NULL on OOM. */
static JsString *to_prop_key(JsContext *ctx, JsValue key) {
    JsVm *vm = ctx->vm;
    JsString *s;
    if (js_is_string(key))
        s = js_value_string(key);
    else {
        s = js_op_to_string(ctx, key); /* ToPropertyKey: an object's toString may run */
        if (!s)
            return NULL;
    }
    return js_intern_cell(vm, s);
}

static JsString *single_unit_string(JsVm *vm, uint16_t u) {
    return js_string_cell_new(vm, &u, 1);
}

/* Bound on [[Prototype]] chain walks; script code has no way to construct a
 * cycle (no __proto__/setPrototypeOf), so this only guards hostile/corrupt
 * bytecode. */
#define JS_PROTO_CHAIN_LIMIT 1000

/* Whether V is an object to the language (typeof "object" or "function", not null): functions and
 * promises are their own kinds of cell here */
static inline bool is_object_type(JsValue v) {
    return js_is_object(v) || js_is_function(v) || js_is_promise(v);
}

/* ---- a function's `prototype` ----
 *
 * As in V8: a constructor has `prototype` as its own property (not enumerable, not deletable,
 * writable with any value, read-only on a builtin constructor or a frozen function), made the
 * first time it is read or `new` needs it. A function that is no constructor (an arrow, a method,
 * an async function, a builtin function, a function from bind()) has none, and `prototype` is an
 * ordinary property of it, which a script may add. */

/* The target of a function from bind() (unwrapped to the innermost), else V itself */
static JsValue bind_target(JsValue v) {
    while (js_is_function(v) && js_value_cell(v)->kind == JS_KIND_NATIVE &&
           ((JsNative *)js_value_cell(v))->is_bound &&
           ((JsNative *)js_value_cell(v))->bound_fn == js_bind_call)
        v = js_value_object(((JsNative *)js_value_cell(v))->bound)->elems[0];
    return v;
}

/* Whether C has its own `prototype`: a constructor, other than one from bind() */
static bool has_own_prototype(const JsGcCell *c) {
    if (c->kind == JS_KIND_CLOSURE)
        return !(((const JsClosure *)c)->fn->fn_flags & (JS_FN_ARROW | JS_FN_ASYNC | JS_FN_METHOD));
    if (c->kind == JS_KIND_NATIVE)
        return ((const JsNative *)c)->ctor != JS_CTOR_NONE;
    return false;
}

bool js_function_has_own_prototype(JsValue fn) {
    return js_is_function(fn) && has_own_prototype(js_value_cell(fn));
}

bool js_is_constructor(JsValue v) {
    v = bind_target(v);
    return js_is_function(v) && has_own_prototype(js_value_cell(v));
}

/* C's `prototype` (has_own_prototype(C)), made on first use: an object whose `constructor`, not
 * enumerable, is C. C must be rooted. False on OOM. */
static bool function_prototype(JsContext *ctx, JsGcCell *c, JsValue *out) {
    JsValue *slot = c->kind == JS_KIND_CLOSURE ? &((JsClosure *)c)->prototype
                                               : &((JsNative *)c)->prototype;
    bool *made = c->kind == JS_KIND_CLOSURE ? &((JsClosure *)c)->proto_made
                                            : &((JsNative *)c)->proto_made;
    if (!*made) {
        JsValue pv = js_object_new(ctx);
        if (!js_is_object(pv))
            return false;
        *slot = pv; /* reachable through C from here */
        *made = true;
        if (!js_object_set_hidden(ctx, js_value_object(pv), "constructor", js_value_from_cell(c))) {
            *slot = js_undefined();
            *made = false;
            return false;
        }
    }
    *out = *slot;
    return true;
}

/* The [[Prototype]] `new C` gives the object it makes: C.prototype when that is an object, else
 * Object.prototype (V8's, for `F.prototype = 5`). C must be rooted. NULL on OOM. */
static JsObject *construct_proto(JsContext *ctx, JsGcCell *c) {
    JsValue p;
    if (!function_prototype(ctx, c, &p))
        return NULL;
    return js_is_object(p) ? js_value_object(p) : ctx->object_proto;
}

/* Whether a script may not assign C's own `prototype`: a builtin constructor's, or a frozen
 * function's */
static bool prototype_read_only(JsContext *ctx, JsValue fn) {
    JsGcCell *c = js_value_cell(fn);
    if (c->kind == JS_KIND_NATIVE && ((JsNative *)c)->ctor == JS_CTOR_FACTORY)
        return true;
    JsObject *own = js_function_props(ctx, fn, false);
    return own && (own->obj_flags & JS_OBJ_FROZEN);
}

static bool is_prototype_key(JsValue key) {
    return js_is_string(key) && units_is_ascii_str(js_value_string(key), "prototype");
}

void js_native_builtin_ctor(JsValue ctor, JsObject *proto) {
    JsNative *nf = (JsNative *)js_value_cell(ctor);
    nf->ctor = JS_CTOR_FACTORY;
    if (proto) {
        nf->prototype = js_value_from_cell(&proto->gc);
        nf->proto_made = true;
    }
}

bool js_native_set_constructor(JsContext *ctx, JsValue fn, bool constructor) {
    (void)ctx;
    if (!js_is_function(fn) || js_value_cell(fn)->kind != JS_KIND_NATIVE)
        return false;
    JsNative *nf = (JsNative *)js_value_cell(fn);
    if (nf->builtin || nf->is_bound)
        return false;
    nf->ctor = constructor ? JS_CTOR_ORDINARY : JS_CTOR_NONE;
    if (!constructor) {
        nf->prototype = js_undefined();
        nf->proto_made = false;
    }
    return true;
}

bool js_native_is_construct_call(JsContext *ctx) {
    return ctx && ctx->native_act && ctx->native_act->is_new;
}


/* Builtin method lookup on the hidden per-type tables. */
static JsPropStatus method_lookup(JsContext *ctx, JsObject *table, JsValue key,
                                  JsValue *out) {
    if (!table || !js_is_string(key))
        return JS_PROP_OK;
    JsString *k = js_intern_cell(ctx->vm, js_value_string(key));
    if (!k)
        return JS_PROP_OOM;
    bool found;
    JsValue v = js_map_get(&table->props, k, &found);
    if (found)
        *out = v;
    return JS_PROP_OK;
}

/* A primitive's (or function's, or promise's) method: its table, which is the constructor's
 * prototype, then Object.prototype, the table's own [[Prototype]] as in V8 (`(1).hasOwnProperty`,
 * `f.valueOf`). The tables are fixed objects, so the chain is just these two. */
static JsPropStatus proto_table_lookup(JsContext *ctx, JsObject *table, JsValue key, JsValue *out) {
    JsValue v = js_undefined();
    JsPropStatus st = method_lookup(ctx, table, key, &v);
    if (st == JS_PROP_OK && js_is_undefined(v) && table)
        st = method_lookup(ctx, ctx->object_proto, key, &v);
    if (st == JS_PROP_OK && !js_is_undefined(v))
        *out = v;
    return st;
}

/* The own-property object of a function value (closure or native): what a script wrote onto it.
 * With create, made on first use (NULL then means out of memory); without, NULL if it has none. */
JsObject *js_function_props(JsContext *ctx, JsValue fn, bool create) {
    if (!js_is_function(fn))
        return NULL;
    JsGcCell *c = js_value_cell(fn);
    if (c->kind != JS_KIND_CLOSURE && c->kind != JS_KIND_NATIVE)
        return NULL;
    JsObject **slot = c->kind == JS_KIND_CLOSURE ? &((JsClosure *)c)->props : &((JsNative *)c)->props;
    if (!*slot && create) {
        JsValue o = js_object_new(ctx); /* fn is the caller's (rooted) value; cells do not move */
        if (!js_is_object(o))
            return NULL;
        *slot = js_value_object(o);
    }
    return *slot;
}

JsString *js_function_name(JsValue fn) {
    if (!js_is_function(fn))
        return NULL;
    JsGcCell *c = js_value_cell(fn);
    if (c->kind == JS_KIND_CLOSURE)
        return ((JsClosure *)c)->name ? ((JsClosure *)c)->name : ((JsClosure *)c)->fn->name;
    if (c->kind == JS_KIND_NATIVE)
        return ((JsNative *)c)->name_deleted ? NULL : ((JsNative *)c)->name;
    return ((JsFunctionCell *)c)->name;
}

/* Function.prototype.toString, as V8 gives it: a function's own source text, from `function`,
 * `async`, its first parameter or its method key to its last token; for a built-in or a host
 * function, `function NAME() { [native code] }` with the name it was made with; for a bound
 * function, `function () { [native code] }`. NULL only on OOM. */
JsString *js_function_source(JsContext *ctx, JsValue fn) {
    JsGcCell *c = js_value_cell(fn);
    JsFunctionCell *f = c->kind == JS_KIND_CLOSURE ? ((JsClosure *)c)->fn
                        : c->kind == JS_KIND_FUNCTION ? (JsFunctionCell *)c : NULL;
    if (f && f->source && f->src_end > f->src_start && f->src_end <= f->source->length)
        return js_string_cell_new(ctx->vm, f->source->units + f->src_start, f->src_end - f->src_start);
    const JsString *name = NULL;
    if (f)
        name = f->name;
    else if (c->kind == JS_KIND_NATIVE && !(((JsNative *)c)->is_bound &&
                                           ((JsNative *)c)->bound_fn == js_bind_call))
        name = ((JsNative *)c)->name;
    static const char pre[] = "function ", post[] = "() { [native code] }";
    size_t np = sizeof pre - 1, nq = sizeof post - 1, nn = name ? name->length : 0;
    size_t n = np + nn + nq;
    uint16_t stackbuf[128];
    uint16_t *u = n <= 128 ? stackbuf : js_realloc_raw(ctx->vm, NULL, 0, n * sizeof *u);
    if (!u)
        return NULL;
    for (size_t i = 0; i < np; i++)
        u[i] = (uint16_t)pre[i];
    for (size_t i = 0; i < nn; i++)
        u[np + i] = name->units[i];
    for (size_t i = 0; i < nq; i++)
        u[np + nn + i] = (uint16_t)post[i];
    JsString *s = js_string_cell_new(ctx->vm, u, n); /* the name is copied out first: may collect */
    if (u != stackbuf)
        js_realloc_raw(ctx->vm, u, n * sizeof *u, 0);
    return s;
}

uint32_t js_function_length(JsValue fn) {
    if (!js_is_function(fn))
        return 0;
    JsGcCell *c = js_value_cell(fn);
    if (c->kind == JS_KIND_CLOSURE)
        return ((JsClosure *)c)->length_deleted ? 0 : ((JsClosure *)c)->fn->length;
    if (c->kind == JS_KIND_NATIVE)
        return ((JsNative *)c)->length;
    return ((JsFunctionCell *)c)->length;
}

/* `name`: a read-only own property of every function, as in V8 */
static bool is_name_key(JsValue key) {
    return js_is_string(key) && units_is_ascii_str(js_value_string(key), "name");
}

/* `length`: likewise */
static bool is_length_key(JsValue key) {
    return js_is_string(key) && units_is_ascii_str(js_value_string(key), "length");
}

/* `delete fn[key]`, out of the dispatch loop: 0 done, 1 refused (*err the TypeError), 2 OOM.
 * name and length are configurable: deleting one leaves Function.prototype's, "" or 0. */
JS_COLD static int delete_function_prop(JsContext *ctx, JsValue fn, JsValue key, JsValue *err) {
    if (is_object_type(key)) { /* ToPropertyKey: its toString may run (fn and key are rooted) */
        JsString *ks = js_op_to_string(ctx, key);
        if (!ks)
            return 2;
        JsValue kv = js_value_from_cell(&ks->gc);
        js_gc_protect(ctx->vm, &kv);
        int r = delete_function_prop(ctx, fn, kv, err);
        js_gc_unprotect(ctx->vm, &kv);
        return r;
    }
    JsObject *own = js_function_props(ctx, fn, false);
    bool special = is_name_key(key) || is_length_key(key);
    if ((own && own->obj_flags) || (has_own_prototype(js_value_cell(fn)) && is_prototype_key(key))) {
        /* `prototype` is not configurable */
        *err = js_error_fmt(ctx, "TypeError: Cannot delete property '%v' of %v", key, fn);
        return 1;
    }
    JsGcCell *fc = js_value_cell(fn);
    if (special && is_length_key(key)) {
        if (fc->kind == JS_KIND_CLOSURE)
            ((JsClosure *)fc)->length_deleted = true;
        else if (fc->kind == JS_KIND_NATIVE)
            ((JsNative *)fc)->length = 0;
        return 0;
    }
    if (special) {
        JsString *empty = js_ascii_cell(ctx->vm, ""); /* fn is on the caller's stack: rooted */
        if (!empty)
            return 2;
        if (fc->kind == JS_KIND_CLOSURE)
            ((JsClosure *)fc)->name = empty;
        else if (fc->kind == JS_KIND_NATIVE)
            ((JsNative *)fc)->name_deleted = true;
        return 0;
    }
    if (own) {
        JsString *k = to_prop_key(ctx, key);
        if (!k)
            return 2;
        js_map_delete(&own->props, k);
    }
    return 0;
}

/* A module namespace's let/const export reads as JS_SPECIAL_TDZ until its declaration has run
 * (also through a prototype chain): every caller that hands the value to a script checks it. */
/* The properties some kinds of object make up (RegExp's source/flags/lastIndex, Map and Set's
 * size, an error's stack), which shadow everything else. True when KEY is one: *ST says how
 * the read went. Out of line, so the inlined get_property stays small. */
__attribute__((noinline)) static bool special_prop_get(JsContext *ctx, JsObject *o, JsValue key,
                                                       JsValue *out, JsPropStatus *st) {
    bool handled = false;
    *st = JS_PROP_OK;
#ifdef LAMASSU_HAS_REGEX
    if (o->obj_kind == JS_OBJ_REGEXP && !js_regexp_prop_get(ctx, o, key, out, &handled)) {
        *st = JS_PROP_OOM;
        return true;
    }
#endif
    if (o->obj_kind == JS_OBJ_ERROR && !js_error_stack_get(ctx, o, key, out, &handled)) {
        *st = JS_PROP_OOM;
        return true;
    }
    if (o->obj_kind == JS_OBJ_MAP)
        js_mapobj_prop_get(o, key, out, &handled);
    else if (o->obj_kind == JS_OBJ_SET)
        js_setobj_prop_get(o, key, out, &handled);
    return handled;
}

__attribute__((noinline)) static JsPropStatus parent_ctor_lookup(JsContext *ctx, JsNative *p, JsValue key,
                                                               JsValue *out);

/*
 * ToPropertyKey (7.1.19) of a key that is an object, for an access on a base that has properties:
 * its toString runs (hint string) and the access goes on with the string. Only after the base is
 * known not to be null or undefined, as in V8 (`null[k]` is a TypeError before k converts), and on
 * every access (`o[k] += 1` converts it twice, as V8). Out of line: the property paths are hot. The
 * base and key are the caller's, rooted.
 */
static JsPropStatus get_property(JsContext *ctx, JsValue base, JsValue key, JsValue *out);
static JsPropStatus set_property(JsContext *ctx, JsValue base, JsValue key, JsValue val,
                                 const char **errmsg);
static JsPropStatus has_property(JsContext *ctx, JsValue base, JsValue key, bool *out);

__attribute__((noinline)) static bool object_key(JsContext *ctx, JsValue key, JsValue *skey) {
    JsString *s = js_op_to_string(ctx, key);
    if (!s)
        return false;
    *skey = js_value_from_cell(&s->gc);
    js_gc_protect(ctx->vm, skey);
    return true;
}

__attribute__((noinline)) static JsPropStatus get_property_object_key(JsContext *ctx, JsValue base,
                                                                      JsValue key, JsValue *out) {
    JsValue sk;
    if (!object_key(ctx, key, &sk))
        return JS_PROP_OOM;
    JsPropStatus st = get_property(ctx, base, sk, out);
    js_gc_unprotect(ctx->vm, &sk);
    return st;
}

__attribute__((noinline)) static JsPropStatus set_property_object_key(JsContext *ctx, JsValue base,
                                                                      JsValue key, JsValue val,
                                                                      const char **errmsg) {
    JsValue sk;
    if (!object_key(ctx, key, &sk))
        return JS_PROP_OOM;
    JsPropStatus st = set_property(ctx, base, sk, val, errmsg);
    js_gc_unprotect(ctx->vm, &sk);
    return st;
}

__attribute__((noinline)) static JsPropStatus has_property_object_key(JsContext *ctx, JsValue base,
                                                                      JsValue key, bool *out) {
    JsValue sk;
    if (!object_key(ctx, key, &sk))
        return JS_PROP_OOM;
    JsPropStatus st = has_property(ctx, base, sk, out);
    js_gc_unprotect(ctx->vm, &sk);
    return st;
}

static JsPropStatus get_property(JsContext *ctx, JsValue base, JsValue key,
                                 JsValue *out) {
    JsVm *vm = ctx->vm;
    *out = js_undefined();
    if (is_nullish(base))
        return JS_PROP_TYPE_ERROR;
    if (JS_UNLIKELY(is_object_type(key)))
        return get_property_object_key(ctx, base, key, out);

    if (js_is_string(base)) {
        JsString *s = js_value_string(base);
        uint32_t idx;
        if (key_to_index(key, &idx)) {
            if (idx < s->length) {
                JsString *c = single_unit_string(vm, s->units[idx]);
                if (!c)
                    return JS_PROP_OOM;
                *out = js_value_from_cell(&c->gc);
            }
            return JS_PROP_OK;
        }
        if (js_is_string(key) && units_is_ascii_str(js_value_string(key), "length")) {
            *out = js_number(s->length);
            return JS_PROP_OK;
        }
        return proto_table_lookup(ctx, ctx->string_methods, key, out);
    }
    if (js_is_object(base)) {
        JsObject *o = js_value_object(base);
        if (JS_UNLIKELY(o->obj_kind > JS_OBJ_ARRAY)) { /* one test for plain objects and arrays */
            JsPropStatus st;
            if (special_prop_get(ctx, o, key, out, &st))
                return st;
        }
        if (o->obj_kind == JS_OBJ_ARRAY) {
            uint32_t idx;
            if (key_to_index(key, &idx)) {
                if (idx < o->elem_count) {
                    *out = o->elems[idx];
                    return JS_PROP_OK;
                }
                /* sparse fallback below */
            } else if (js_is_string(key) &&
                       units_is_ascii_str(js_value_string(key), "length")) {
                *out = js_number(o->elem_count);
                return JS_PROP_OK;
            }
        }
        JsString *k = to_prop_key(ctx, key);
        if (!k)
            return JS_PROP_OOM;
        bool found;
        JsValue v = js_map_get(&o->props, k, &found);
        if (found) {
            *out = v;
            return JS_PROP_OK;
        }
        /* own prop miss: walk the [[Prototype]] chain. This is the ONLY
         * path builtin instance methods (Array/RegExp/Date/Map/Set) reach
         * their methods through — X.prototype is a real, script-visible
         * object and instances' .proto was set to it at allocation, same
         * mechanism `new Foo()` uses for user-defined constructors. */
        JsValue proto = o->proto;
        for (int depth = 0; depth < JS_PROTO_CHAIN_LIMIT && js_is_object(proto); depth++) {
            JsObject *po = js_value_object(proto);
            bool pfound;
            JsValue pv = js_map_get(&po->props, k, &pfound);
            if (pfound) {
                *out = pv;
                return JS_PROP_OK;
            }
            proto = po->proto;
        }
        return JS_PROP_OK;
    }
    if (js_is_number(base))
        return proto_table_lookup(ctx, ctx->number_methods, key, out);
    if (js_is_promise(base))
        return proto_table_lookup(ctx, ctx->promise_methods, key, out);
    if (js_is_function(base)) {
        JsGcCell *c = js_value_cell(base);
        if (has_own_prototype(c) && is_prototype_key(key))
            return function_prototype(ctx, c, out) ? JS_PROP_OK : JS_PROP_OOM;
        if (is_name_key(key)) {
            JsString *nm = js_function_name(base);
            if (!nm) {
                nm = js_ascii_cell(vm, "");
                if (!nm)
                    return JS_PROP_OOM;
            }
            *out = js_value_from_cell(&nm->gc);
            return JS_PROP_OK;
        }
        if (is_length_key(key)) {
            *out = js_number(js_function_length(base));
            return JS_PROP_OK;
        }
        JsObject *own = js_function_props(ctx, base, false);
        if (own) {
            JsPropStatus st = method_lookup(ctx, own, key, out);
            if (st != JS_PROP_OK || !js_is_undefined(*out))
                return st;
        }
        if (c->kind == JS_KIND_NATIVE && ((JsNative *)c)->statics) {
            JsPropStatus st = method_lookup(ctx, ((JsNative *)c)->statics, key, out);
            if (st != JS_PROP_OK || !js_is_undefined(*out))
                return st;
        }
        if (c->kind == JS_KIND_NATIVE && ((JsNative *)c)->parent)
            return parent_ctor_lookup(ctx, ((JsNative *)c)->parent, key, out);
        /* Function.prototype methods (call), shared by closures and natives */
        return proto_table_lookup(ctx, ctx->function_methods, key, out);
    }
    if (js_is_bool(base))
        return proto_table_lookup(ctx, ctx->boolean_methods, key, out);
    return JS_PROP_OK;
}

/*
 * Own-property presence, for seal/freeze. A sealed object still accepts writes
 * to properties it already has, so refusing a write means first deciding
 * whether the key is new — and for an array that question is about the element
 * range, not the property map.
 */
static bool own_prop_present(JsContext *ctx, JsObject *o, JsValue key) {
    uint32_t idx;
    if (o->obj_kind == JS_OBJ_ERROR && js_error_stack_has(o, key))
        return true;
    if (o->obj_kind == JS_OBJ_ARRAY && key_to_index(key, &idx))
        return idx < o->elem_count;
    if (o->obj_kind == JS_OBJ_ARRAY && js_is_string(key) && units_is_ascii_str(js_value_string(key), "length"))
        return true;
    JsString *k = to_prop_key(ctx, key);
    if (!k)
        return false;
    bool found = false;
    js_map_get(&o->props, k, &found);
    return found;
}

/*
 * The single gate every guest property write passes through, so freeze cannot
 * be enforced in one path and forgotten in another. Array index writes and
 * `length` assignment reach here too. The in-place array mutators in
 * js_builtins.c write a->elems directly and are gated separately.
 */
/* Refused writes, worded as V8 words them in strict code. set_property hands back one of these
 * templates, which set_error fills in with the base and the key (by identity: any other message
 * is thrown as it is). */
static const char SET_NULLISH[] = "TypeError: Cannot set properties of %v (setting '%v')";
static const char SET_READ_ONLY[] = "TypeError: Cannot assign to read only property '%v' of %s '%v'";
static const char SET_NOT_EXTENSIBLE[] = "TypeError: Cannot add property %v, object is not extensible";
static const char SET_PRIMITIVE[] = "TypeError: Cannot create property '%v' on %s '%v'";
static const char SET_NAMESPACE[] = "TypeError: Cannot assign to property '%v' of [object Module]";

static JsValue set_error(JsContext *ctx, const char *msg, JsValue base, JsValue key) {
    if (msg == SET_NULLISH)
        return js_error_fmt(ctx, msg, base, key);
    if (msg == SET_READ_ONLY)
        return js_error_fmt(ctx, msg, key, js_is_function(base) ? "function" : "object", base);
    if (msg == SET_NOT_EXTENSIBLE || msg == SET_NAMESPACE)
        return js_error_fmt(ctx, msg, key);
#ifdef LAMASSU_HAS_REGEX
    if (strcmp(msg, JS_REGEXP_GETTER_ONLY) == 0)
        return js_error_fmt(ctx, "TypeError: Cannot set property %v of [object Object] which has only a getter", key);
#endif
    if (msg == SET_PRIMITIVE)
        return js_error_fmt(ctx, msg, key, js_is_string(base) ? "string"
                                           : js_is_number(base) ? "number" : "boolean", base);
    return js_error_from_ascii(ctx, msg);
}

static const char *write_blocked(JsContext *ctx, JsObject *o, JsValue key) {
    if (!o->obj_flags)
        return NULL; /* the overwhelmingly common case: one predictable branch */
    if (o->obj_flags & JS_OBJ_NAMESPACE)
        return SET_NAMESPACE; /* a module's exports change only through its own bindings */
    if (!own_prop_present(ctx, o, key))
        return SET_NOT_EXTENSIBLE; /* sealed or frozen: no new properties */
    if (o->obj_flags & JS_OBJ_FROZEN)
        return SET_READ_ONLY;
    return NULL;
}

static JsPropStatus set_property(JsContext *ctx, JsValue base, JsValue key,
                                 JsValue val, const char **errmsg) {
    JsVm *vm = ctx->vm;
    if (is_nullish(base)) {
        *errmsg = SET_NULLISH;
        return JS_PROP_TYPE_ERROR;
    }
    if (JS_UNLIKELY(is_object_type(key)))
        return set_property_object_key(ctx, base, key, val, errmsg);
    if (js_is_function(base)) {
        JsGcCell *c = js_value_cell(base);
        if (has_own_prototype(c) && is_prototype_key(key)) {
            if (prototype_read_only(ctx, base)) {
                *errmsg = SET_READ_ONLY;
                return JS_PROP_TYPE_ERROR;
            }
            if (c->kind == JS_KIND_CLOSURE) {
                ((JsClosure *)c)->prototype = val;
                ((JsClosure *)c)->proto_made = true;
            } else {
                ((JsNative *)c)->prototype = val;
                ((JsNative *)c)->proto_made = true;
            }
            return JS_PROP_OK;
        }
        if (is_name_key(key) || is_length_key(key)) {
            *errmsg = SET_READ_ONLY;
            return JS_PROP_TYPE_ERROR;
        }
        /* any other key: the function's own properties (statics, constants, caches) */
        JsObject *own = js_function_props(ctx, base, true);
        if (!own)
            return JS_PROP_OOM;
        const char *blocked = write_blocked(ctx, own, key);
        if (blocked) {
            *errmsg = blocked;
            return JS_PROP_TYPE_ERROR;
        }
        JsString *k = to_prop_key(ctx, key);
        if (!k)
            return JS_PROP_OOM;
        if (!js_map_set(vm, &own->props, k, val))
            return JS_PROP_OOM;
        return JS_PROP_OK;
    }
    if (!js_is_object(base)) {
        *errmsg = SET_PRIMITIVE;
        return JS_PROP_TYPE_ERROR;
    }
    JsObject *o = js_value_object(base);
    /* an error's `stack` is V8's accessor: a frozen error still takes the write (once deleted,
     * a write makes an ordinary property, as in V8) */
    if (JS_UNLIKELY(o->obj_kind == JS_OBJ_ERROR) && js_error_stack_has(o, key) &&
        js_error_stack_set(ctx, o, key, val))
        return JS_PROP_OK;
    const char *blocked = write_blocked(ctx, o, key);
    if (blocked) {
        *errmsg = blocked;
        return JS_PROP_TYPE_ERROR;
    }
#ifdef LAMASSU_HAS_REGEX
    if (o->obj_kind == JS_OBJ_REGEXP) {
        bool handled = false;
        if (!js_regexp_prop_set(ctx, o, key, val, &handled, errmsg))
            return *errmsg ? JS_PROP_TYPE_ERROR : JS_PROP_OOM;
        if (handled)
            return JS_PROP_OK;
    }
#endif
    if (o->obj_kind == JS_OBJ_ARRAY) {
        uint32_t idx;
        if (key_to_index(key, &idx)) {
            if (idx <= o->elem_count || idx - o->elem_count <= JS_MAX_ARRAY_GAP) {
                if (!js_array_set_index(vm, o, idx, val))
                    return JS_PROP_OOM;
                return JS_PROP_OK;
            }
            /* very sparse: fall through to the props map */
        } else if (js_is_string(key) &&
                   units_is_ascii_str(js_value_string(key), "length")) {
            /* ArraySetLength (10.4.2.4) converts twice, ToUint32 and ToNumber, as V8 does: a
             * valueOf runs twice. val is the caller's rooted operand. */
            double d, d2;
            if (!js_op_to_number(ctx, val, &d) || !js_op_to_number(ctx, val, &d2))
                return JS_PROP_OOM;
            (void)d2;
            /* bounds-check before the cast: (uint32_t)d is UB for d outside
             * [0, UINT32_MAX], e.g. arr.length = -1 or arr.length = 1e20 */
            if (!(d >= 0 && d < 4294967296.0)) {
                *errmsg = "RangeError: Invalid array length";
                return JS_PROP_TYPE_ERROR;
            }
            uint32_t n = (uint32_t)d;
            if ((double)n != d) {
                *errmsg = "RangeError: Invalid array length";
                return JS_PROP_TYPE_ERROR;
            }
            if (n <= o->elem_count) {
                o->elem_count = n;
                return JS_PROP_OK;
            }
            if (n - o->elem_count > JS_MAX_ARRAY_GAP) {
                *errmsg = "RangeError: Invalid array length";
                return JS_PROP_TYPE_ERROR;
            }
            if (!js_array_set_index(vm, o, n - 1, js_undefined()))
                return JS_PROP_OOM;
            return JS_PROP_OK;
        }
    }
    JsString *k = to_prop_key(ctx, key);
    if (!k)
        return JS_PROP_OOM;
    if (!js_map_set(vm, &o->props, k, val))
        return JS_PROP_OOM;
    return JS_PROP_OK;
}

static JsPropStatus has_property(JsContext *ctx, JsValue base, JsValue key,
                                 bool *out) {
    *out = false;
    if (JS_UNLIKELY(is_object_type(key)) && is_object_type(base))
        return has_property_object_key(ctx, base, key, out);
    if (js_is_function(base)) {
        /* `k in fn`: what reading fn[k] would find (own properties, builtin statics, methods) */
        if (has_own_prototype(js_value_cell(base)) && is_prototype_key(key)) {
            *out = true;
            return JS_PROP_OK;
        }
        JsValue v;
        JsPropStatus st = get_property(ctx, base, key, &v);
        if (st == JS_PROP_OK)
            *out = !js_is_undefined(v);
        return st;
    }
    if (!js_is_object(base))
        return JS_PROP_TYPE_ERROR;
    JsObject *o = js_value_object(base);
    /* Synthesized props (RegExp source/flags/lastIndex, Map/Set `size`) must be
     * visible to `in`, exactly as get_property surfaces them for reads. */
    {
        bool handled = false;
        JsValue tmp;
#ifdef LAMASSU_HAS_REGEX
        if (o->obj_kind == JS_OBJ_REGEXP) {
            if (!js_regexp_prop_get(ctx, o, key, &tmp, &handled))
                return JS_PROP_OOM;
        }
#endif
        if (o->obj_kind == JS_OBJ_MAP)
            js_mapobj_prop_get(o, key, &tmp, &handled);
        else if (o->obj_kind == JS_OBJ_SET)
            js_setobj_prop_get(o, key, &tmp, &handled);
        else if (o->obj_kind == JS_OBJ_ERROR)
            handled = js_error_stack_has(o, key);
        if (handled) {
            *out = true;
            return JS_PROP_OK;
        }
    }
    if (o->obj_kind == JS_OBJ_ARRAY) {
        uint32_t idx;
        if (key_to_index(key, &idx) && idx < o->elem_count) {
            *out = true;
            return JS_PROP_OK;
        }
        if (js_is_string(key) && units_is_ascii_str(js_value_string(key), "length")) {
            *out = true;
            return JS_PROP_OK;
        }
    }
    JsString *k = to_prop_key(ctx, key);
    if (!k)
        return JS_PROP_OOM;
    bool found;
    js_map_get(&o->props, k, &found);
    if (found) {
        *out = true;
        return JS_PROP_OK;
    }
    JsValue proto = o->proto;
    for (int depth = 0; depth < JS_PROTO_CHAIN_LIMIT && js_is_object(proto); depth++) {
        JsObject *po = js_value_object(proto);
        js_map_get(&po->props, k, &found);
        if (found) {
            *out = true;
            return JS_PROP_OK;
        }
        proto = po->proto;
    }
    return JS_PROP_OK;
}

bool js_spread_into_object(JsContext *ctx, JsObject *dst, JsValue src, JsString **uninit) {
    JsVm *vm = ctx->vm;
    *uninit = NULL;
    if (!js_is_object(src) && !js_is_string(src))
        return true; /* primitives spread to nothing */
    if (js_is_string(src)) {
        JsString *s = js_value_string(src);
        for (uint32_t i = 0; i < s->length; i++) {
            JsString *c = single_unit_string(vm, s->units[i]);
            if (!c)
                return false;
            /* Root the freshly-made unit string across the key allocation
             * below (js_number_to_string can collect it otherwise). */
            JsValue cv = js_value_from_cell(&c->gc);
            js_gc_protect(vm, &cv);
            JsString *k = js_intern_cell(vm, js_number_to_string(vm, i));
            bool ok = k && js_map_set(vm, &dst->props, k, cv);
            js_gc_unprotect(vm, &cv);
            if (!ok)
                return false;
        }
        return true;
    }
    JsObject *o = js_value_object(src);
    if (o->obj_kind == JS_OBJ_ARRAY) {
        for (uint32_t i = 0; i < o->elem_count; i++) {
            JsString *num = js_number_to_string(vm, i);
            JsString *k = num ? js_intern_cell(vm, num) : NULL;
            if (!k || !js_map_set(vm, &dst->props, k, o->elems[i]))
                return false;
        }
    }
    js_map_sort_keys(&o->props);
    for (uint32_t i = 0; i < o->props.capacity; i++) {
        JsMapEntry *e = &o->props.entries[i];
        if (e->key && e->key != JS_MAP_TOMBSTONE && !js_prop_hidden_at(o, i)) {
            if (e->value.bits == JS_SPECIAL_TDZ) { /* a namespace's uninitialized export */
                *uninit = e->key;
                return false;
            }
            if (!js_map_set(vm, &dst->props, e->key, e->value))
                return false;
        }
    }
    return true;
}

/* ---- string ops ---- */

JsString *js_concat_cells(JsVm *vm, const JsString *a, const JsString *b) {
    if ((uint64_t)a->length + b->length > JS_MAX_STRING_UNITS)
        return NULL;
    uint32_t len = a->length + b->length;
    JsGcCell *c = js_gc_new_cell(vm, JS_KIND_STRING,
                                 sizeof(JsString) + (size_t)len * sizeof(uint16_t));
    if (!c)
        return NULL;
    JsString *s = (JsString *)c;
    s->length = len;
    s->interned = false;
    if (a->length)
        memcpy(s->units, a->units, (size_t)a->length * sizeof(uint16_t));
    if (b->length)
        memcpy(s->units + a->length, b->units, (size_t)b->length * sizeof(uint16_t));
    /* Continue the left operand's fold over b rather than rehashing a ++ b:
     * the result is bit-identical (see js_units_hash_from), and building a
     * string by repeated append stops paying for its whole length each time. */
    s->hash = js_units_hash_from(a->hash, b->units, b->length);
    return s;
}

/* ---- the slow paths of operators, out of line (see RT_THROW_PENDING on why the loop must not
 * grow): operands that are not all numbers, where ToPrimitive can run guest code. Each
 * works on the fiber's top slots, rooted there, first records IP (just past the opcode) as the
 * frame's position, since guest valueOf/toString may run and its stack trace reads it, and
 * returns false with the error pending (RT_THROW_PENDING). ---- */

static inline void slow_path_at(JsFiber *fb, uint32_t ip) { fb->frames[fb->frame_count - 1].ip = ip; }

/* `+` (13.15.3): each operand's ToPrimitive (default hint, left first), then concatenation when
 * either is a string, else addition. The result replaces both operands. */
__attribute__((noinline)) static bool add_slow(JsContext *ctx, JsFiber *fb, uint32_t ip) {
    slow_path_at(fb, ip);
    JsVm *vm = ctx->vm;
    uint32_t ia = fb->sp - 2, ib = fb->sp - 1;
    JsValue p;
    if (is_object_type(fb->stack[ia])) {
        if (!js_to_primitive(ctx, fb->stack[ia], JS_HINT_DEFAULT, &p))
            return false;
        fb->stack[ia] = p;
    }
    if (is_object_type(fb->stack[ib])) {
        if (!js_to_primitive(ctx, fb->stack[ib], JS_HINT_DEFAULT, &p))
            return false;
        fb->stack[ib] = p;
    }
    JsValue a = fb->stack[ia], b = fb->stack[ib];
    if (js_is_string(a) || js_is_string(b)) {
        JsString *sa = js_primitive_to_string(vm, a);
        if (!sa)
            return fail_oom(ctx);
        fb->stack[ia] = js_value_from_cell(&sa->gc);
        JsString *sb = js_primitive_to_string(vm, b);
        if (!sb)
            return fail_oom(ctx);
        fb->stack[ib] = js_value_from_cell(&sb->gc);
        JsString *r = js_concat_cells(vm, sa, sb);
        if (!r)
            return fail_ascii(ctx, "RangeError: Invalid string length");
        fb->sp--;
        fb->stack[fb->sp - 1] = js_value_from_cell(&r->gc);
        return true;
    }
    double x = js_primitive_to_number(a) + js_primitive_to_number(b);
    fb->sp--;
    fb->stack[fb->sp - 1] = js_number(x);
    return true;
}

/* ToNumber of the top two operands, left first. */
JS_COLD static bool number_operands(JsContext *ctx, JsFiber *fb, uint32_t ip,
                                                      double *a, double *b) {
    slow_path_at(fb, ip);
    return js_op_to_number(ctx, fb->stack[fb->sp - 2], a) &&
           js_op_to_number(ctx, fb->stack[fb->sp - 1], b);
}

/* ToNumber of the top operand. */
JS_COLD static bool number_operand(JsContext *ctx, JsFiber *fb, uint32_t ip,
                                                     double *d) {
    slow_path_at(fb, ip);
    return js_op_to_number(ctx, fb->stack[fb->sp - 1], d);
}

/* == on the top two operands: 1 or 0, or -1 with the error pending */
JS_COLD static int eq_slow(JsContext *ctx, JsFiber *fb, uint32_t ip) {
    slow_path_at(fb, ip);
    return loose_eq_values(ctx, fb->stack[fb->sp - 2], fb->stack[fb->sp - 1]);
}

/* < and friends on the top two operands: js_compare_values's answer, 3 with the error pending */
JS_COLD static int compare_slow(JsContext *ctx, JsFiber *fb, uint32_t ip) {
    slow_path_at(fb, ip);
    return js_compare_values(ctx, fb->stack[fb->sp - 2], fb->stack[fb->sp - 1]);
}




/* ---- the dispatch loop ---- */

static uint32_t lookup_pos(const JsFunctionCell *fn, uint32_t ip) {
    uint32_t pos = 0;
    for (uint32_t i = 0; i < fn->line_count; i++) {
        if (fn->lines[i].off > ip)
            break;
        pos = fn->lines[i].pos;
    }
    return pos;
}

static void frame_record(JsStackFrame *out, const JsFrame *fr, uint32_t ip) {
    const JsFunctionCell *fn = fr->closure->fn;
    out->specifier = fn->module && fn->module->specifier
                         ? js_value_from_cell(&fn->module->specifier->gc)
                         : js_undefined();
    JsString *name = fr->closure->name ? fr->closure->name : fn->name;
    out->function = name ? js_value_from_cell(&name->gc) : js_undefined();
    out->pos = lookup_pos(fn, ip);
}

/*
 * Frames of fb and of the fibers it was re-entered from, innermost first.
 * Every frame's saved ip points just past the instruction in flight — the
 * raise macros store op_ip + 1 for the top frame so it reads the same way as
 * a caller's ip saved at CALL — hence ip - 1 lands inside that instruction
 * and lookup_pos finds its position.
 */
static uint32_t capture_trace(const JsFiber *fb, JsStackFrame *out, uint32_t max) {
    uint32_t n = 0;
    for (const JsFiber *f = fb; f && n < max; f = f->caller)
        for (uint32_t i = f->frame_count; i > 0 && n < max; i--) {
            const JsFrame *fr = &f->frames[i - 1];
            frame_record(&out[n++], fr, fr->ip > 0 ? fr->ip - 1 : 0);
        }
    return n;
}

/* Calls the native NF from FIBER's top frame (or from the host when FIBER is NULL), recording
 * the call for js_native_userdata and for a stack trace's builtin frames. Out of line: its
 * record on the C stack would otherwise sit in the interpreter loop's frame. */
__attribute__((noinline)) static bool call_native(JsContext *ctx, JsFiber *fiber, JsNative *nf,
                                                  JsValue this_val, const JsValue *args, int argc,
                                                  bool is_new, JsValue *res) {
    JsNative *outer_native = ctx->running_native;
    ctx->running_native = nf;
    JsNativeAct act;
    act.outer = ctx->native_act;
    act.nf = nf;
    act.this_val = is_new ? js_undefined() : this_val;
    act.fiber = fiber;
    act.frame_count = fiber ? fiber->frame_count : 0;
    act.is_new = is_new;
    act.nkeep = 0;
    ctx->native_act = &act;
    bool ok = nf->is_bound ? nf->bound_fn(ctx, nf->bound, this_val, args, argc, res)
                           : nf->fn(ctx, this_val, args, argc, res);
    ctx->native_act = act.outer;
    ctx->running_native = outer_native;
    return ok;
}

/* The object `new C` makes for `this`: an ordinary object whose [[Prototype]] is construct_proto's.
 * C must be rooted. Undefined on OOM. */
__attribute__((noinline)) static JsValue construct_this(JsContext *ctx, JsGcCell *c) {
    JsObject *proto = construct_proto(ctx, c);
    if (!proto)
        return js_undefined();
    JsValue instv = js_object_new(ctx);
    if (js_is_object(instv))
        js_value_object(instv)->proto = js_value_from_cell(&proto->gc);
    return instv;
}

/* `new` on a native constructor; its arguments at stack[nb...], the slot for `this` at nb - 1. A
 * builtin one (Array, Map, Error, ...) makes its own object: String/Number/Boolean return their
 * primitive, as there are no wrapper objects. A host's is called with `this` made as a script
 * function's is, and gives it unless the native returns an object. */
__attribute__((noinline)) static bool construct_native(JsContext *ctx, JsFiber *fb, JsNative *nf,
                                                       uint32_t nb, uint32_t argc, JsValue *res) {
    if (nf->ctor == JS_CTOR_ORDINARY) {
        JsValue thisv = construct_this(ctx, &nf->gc); /* nf is the callee, on the stack */
        if (!js_is_object(thisv)) {
            *res = js_oom_value(ctx->vm);
            return false;
        }
        fb->stack[nb - 1] = thisv;
    }
    if (!call_native(ctx, fb, nf, fb->stack[nb - 1], &fb->stack[nb], (int)argc, true, res))
        return false;
    if (nf->ctor == JS_CTOR_ORDINARY && !is_object_type(*res))
        *res = fb->stack[nb - 1];
    return true;
}

/* A raise, before any frame is popped: where it was raised (js_context_error_stack), and the
 * frames of an error the interpreter made for it, now that the instruction's position is
 * recorded. Out of line, to keep the interpreter loop small. */
__attribute__((noinline)) static void record_raise(JsContext *ctx, JsFiber *fb) {
    fb->trace_count = capture_trace(fb, fb->trace, JS_TRACE_MAX);
    if (!js_error_capture_pending(ctx, fb->error))
        fb->error = js_oom_value(ctx->vm);
}

/* `delete o[key]` on an object neither sealed nor frozen. False on OOM. */
__attribute__((noinline)) static bool delete_object_prop(JsContext *ctx, JsObject *o, JsValue key) {
    if (is_object_type(key)) { /* ToPropertyKey: its toString may run (o and key are rooted) */
        JsString *ks = js_op_to_string(ctx, key);
        if (!ks)
            return false;
        JsValue kv = js_value_from_cell(&ks->gc);
        js_gc_protect(ctx->vm, &kv);
        bool r = delete_object_prop(ctx, o, kv);
        js_gc_unprotect(ctx->vm, &kv);
        return r;
    }
    uint32_t idx;
    if (o->obj_kind == JS_OBJ_ARRAY && value_to_index(key, &idx)) {
        if (idx < o->elem_count)
            o->elems[idx] = js_undefined();
        return true;
    }
    if (o->obj_kind == JS_OBJ_ERROR && js_error_stack_delete(ctx, o, key))
        return true; /* the error's `stack` */
    JsString *k = to_prop_key(ctx, key);
    if (!k)
        return false;
    js_map_delete(&o->props, k); /* made again, a hidden key enumerates */
    return true;
}

/* `V instanceof C`, as V8 runs it (there is no Symbol.hasInstance): C must be a function; a bound
 * one stands for its target; true when C.prototype is on V's prototype chain. A function's chain
 * starts at the function methods (Function.prototype), a promise's at Promise.prototype; both go
 * on to Object.prototype. 1 or 0, or -1 with *ERR the TypeError. */
__attribute__((noinline)) static int instance_of(JsContext *ctx, JsValue v, JsValue c, JsValue *err) {
    if (!js_is_object(c) && !js_is_function(c) && !js_is_promise(c)) {
        *err = js_error_from_ascii(ctx, "TypeError: Right-hand side of 'instanceof' is not an object");
        return -1;
    }
    if (!js_is_function(c)) {
        *err = js_error_from_ascii(ctx, "TypeError: Right-hand side of 'instanceof' is not callable");
        return -1;
    }
    while (js_value_cell(c)->kind == JS_KIND_NATIVE && ((JsNative *)js_value_cell(c))->is_bound &&
           ((JsNative *)js_value_cell(c))->bound_fn == js_bind_call)
        c = js_value_object(((JsNative *)js_value_cell(c))->bound)->elems[0];
    JsObject *start;
    if (js_is_object(v))
        start = js_is_object(js_value_object(v)->proto) ? js_value_object(js_value_object(v)->proto) : NULL;
    else if (js_is_function(v))
        start = ctx->function_methods;
    else if (js_is_promise(v))
        start = ctx->promise_methods;
    else
        return 0; /* a primitive is no instance */
    static const uint16_t kp[] = {'p', 'r', 'o', 't', 'o', 't', 'y', 'p', 'e'};
    js_gc_protect(ctx->vm, &c);
    JsValue key = js_atom(ctx->vm, kp, 9);
    JsValue proto = js_undefined();
    JsPropStatus st = js_is_string(key) ? get_property(ctx, c, key, &proto) : JS_PROP_OOM;
    js_gc_unprotect(ctx->vm, &c);
    if (st == JS_PROP_OOM) {
        *err = js_take_error(ctx);
        return -1;
    }
    if (!js_is_object(proto)) {
        *err = js_error_fmt(ctx, "TypeError: Function has non-object prototype '%v' in instanceof check", proto);
        return -1;
    }
    const JsObject *target = js_value_object(proto);
    for (const JsObject *p = start; p; p = js_is_object(p->proto) ? js_value_object(p->proto) : NULL)
        if (p == target)
            return 1;
    return 0;
}

/* ---- e.stack: the frames an error is made in (js_error.c turns them into text) ---- */


/* Whether the code running now is a native (rather than the interpreter, which raises its own
 * errors before it has recorded the instruction's position): the innermost native call was made
 * from the frame the current fiber is on. */
bool js_in_native(const JsContext *ctx) {
    const JsNativeAct *a = ctx->native_act;
    return a && a->fiber == ctx->fiber && a->frame_count == (ctx->fiber ? ctx->fiber->frame_count : 0);
}

typedef struct {
    JsErrorFrame *out;
    uint32_t n, limit;
    JsValue skip; /* a function: frames up to and including its call are left out */
    bool skipping;
} FrameSink;

static void sink_add(FrameSink *k, JsValue fn, JsValue receiver, uint32_t pos, uint8_t flags) {
    if (k->skipping) {
        if (js_same_value(fn, k->skip))
            k->skipping = false;
        return;
    }
    if (k->n < k->limit)
        k->out[k->n++] = (JsErrorFrame){fn, receiver, pos, flags};
}

static void sink_frame(FrameSink *k, const JsFiber *f, const JsFrame *fr, uint8_t flags) {
    const JsFunctionCell *fn = fr->closure->fn;
    JsValue receiver = fr->base >= 1 ? f->stack[fr->base - 1] : js_undefined();
    sink_add(k, js_value_from_cell(&fr->closure->gc), receiver,
             lookup_pos(fn, fr->ip > 0 ? fr->ip - 1 : 0),
             (uint8_t)(flags | (fr->is_construct ? JS_EF_CONSTRUCT : 0)));
}

/* A native's frame, as V8 shows a builtin's (`at Array.map (<anonymous>)`): the engine's own
 * natives only, not a bound function's trampoline nor a host's natives. */
static void sink_native(FrameSink *k, const JsNativeAct *a) {
    const JsNative *nf = a->nf;
    if (!nf->builtin || (nf->is_bound && nf->bound_fn == js_bind_call)) {
        if (k->skipping && js_same_value(js_value_from_cell((JsGcCell *)&nf->gc), k->skip))
            k->skipping = false;
        return;
    }
    sink_add(k, js_value_from_cell((JsGcCell *)&nf->gc), a->this_val, 0,
             a->is_new ? JS_EF_CONSTRUCT : 0);
}

/*
 * The frames running now, innermost first: each fiber's frames with the natives called from
 * them in between, out through the fibers natives re-entered from; then, as V8 does, the async
 * functions waiting on the outermost one (an `await` of its promise), and on them in turn.
 * Returns how many were written to OUT (at most LIMIT).
 */
uint32_t js_capture_frames(JsContext *ctx, JsErrorFrame *out, uint32_t limit, JsValue skip) {
    FrameSink k = {out, 0, limit, skip, js_is_function(skip)};
    const JsNativeAct *a = ctx->native_act;
    const JsFiber *last = NULL;
    for (const JsFiber *f = ctx->fiber; f; f = f->caller) {
        for (uint32_t i = f->frame_count; i > 0; i--) {
            while (a && a->fiber == f && a->frame_count >= i) {
                sink_native(&k, a);
                a = a->outer;
            }
            sink_frame(&k, f, &f->frames[i - 1], 0);
        }
        last = f;
    }
    for (; a; a = a->outer) /* natives the host called */
        sink_native(&k, a);
    /* async callers: the fiber awaiting this one's promise, then the one awaiting that */
    for (uint32_t guard = 0; last && last->result_promise && guard < 1000; guard++) {
        const JsFiber *waiting = NULL;
        for (const JsReaction *r = last->result_promise->reactions; r; r = r->next)
            if (r->kind == 1 && r->fiber && r->fiber->frame_count) {
                waiting = r->fiber;
                break;
            }
        if (!waiting)
            break;
        sink_frame(&k, waiting, &waiting->frames[waiting->frame_count - 1], JS_EF_ASYNC);
        last = waiting;
    }
    return k.n;
}

size_t js_context_stack(JsContext *ctx, JsStackFrame *out, size_t max) {
    if (!ctx->fiber || !out)
        return 0;
    return capture_trace(ctx->fiber, out, max > JS_TRACE_MAX ? JS_TRACE_MAX : (uint32_t)max);
}

size_t js_context_error_stack(const JsContext *ctx, JsStackFrame *out, size_t max) {
    size_t n = ctx->error_trace_count < max ? ctx->error_trace_count : max;
    if (out)
        memcpy(out, ctx->error_trace, n * sizeof *out);
    return n;
}

/*
 * Building the error message allocates, so at the moment it matters most --
 * reporting that allocation failed -- it can fail too. Fall back to the string
 * built at VM startup rather than throwing `undefined`, which told a host that
 * something went wrong but not what.
 */
JsValue js_oom_value(JsVm *vm) {
    return vm->oom_error ? js_value_from_cell(&vm->oom_error->gc) : js_undefined();
}

/* Engine-raised errors are Error objects (js_error.h); the "Name: " prefix of
 * the message selects the subclass. Falls back to the bare string on OOM. */
/* RT_THROW. Its "out of memory" sites follow the calls that fail on out of memory, and those calls
 * also fail when a conversion inside them threw (an object key's toString, a template's ${x}):
 * that error is pending, and it is the one raised. Keeping that decision here, rather than a
 * separate raise in each opcode, leaves the dispatch loop's code as it was: see run_fiber. */
static void fiber_throw(JsContext *ctx, JsFiber *fb, const char *ascii_msg) {
    if (!js_is_undefined(ctx->pending_error) && strcmp(ascii_msg, "out of memory") == 0)
        fb->error = js_take_error(ctx);
    else
        fb->error = js_error_from_ascii(ctx, ascii_msg);
    fb->failed = true;
}

/*
 * Builds "<prefix><name><suffix>" as one string. The message is assembled in
 * a heap buffer (only the final js_string_cell_new allocates a GC cell), so
 * there are no unrooted intermediate strings to be freed by a GC that fires
 * mid-build — a bug that only surfaces once the heap is large enough to
 * collect during error formatting.
 */
static void fiber_throw_units(JsContext *ctx, JsFiber *fb, const char *prefix,
                              const uint16_t *units, uint32_t len, const char *suffix) {
    JsVm *vm = ctx->vm;
    size_t plen = 0, slen = 0;
    while (prefix[plen])
        plen++;
    while (suffix[slen])
        slen++;
    size_t total = plen + len + slen;
    uint16_t *buf = js_realloc_raw(vm, NULL, 0, total * sizeof(uint16_t));
    if (!buf) {
        fiber_throw(ctx, fb, prefix);
        return;
    }
    size_t n = 0;
    for (size_t i = 0; i < plen; i++)
        buf[n++] = (uint16_t)(unsigned char)prefix[i];
    for (uint32_t i = 0; i < len; i++)
        buf[n++] = units[i];
    for (size_t i = 0; i < slen; i++)
        buf[n++] = (uint16_t)(unsigned char)suffix[i];
    JsString *s = js_string_cell_new(vm, buf, (uint32_t)total);
    js_realloc_raw(vm, buf, total * sizeof(uint16_t), 0);
    fb->error = s ? js_error_from_cell(ctx, s) : js_oom_value(vm);
    fb->failed = true;
}

static void fiber_throw_name(JsContext *ctx, JsFiber *fb, const char *prefix,
                             const JsString *name, const char *suffix) {
    fiber_throw_units(ctx, fb, prefix, name->units, name->length, suffix);
}

/* The text the compiler recorded for the instruction at IP (a callee, an iterated or destructured
 * expression, a binding's name), or false when it recorded none. An empty text still counts. */
static bool find_note(const JsFunctionCell *fn, uint32_t ip, const uint16_t **units, uint32_t *len) {
    uint32_t lo = 0, hi = fn->callee_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (fn->callees[mid].off < ip) lo = mid + 1; else hi = mid;
    }
    if (lo < fn->callee_count && fn->callees[lo].off == ip) {
        *len = fn->callees[lo].len;
        /* an empty text (the only kind a function may have) has no buffer behind it */
        *units = *len ? fn->callee_text + fn->callees[lo].start : NULL;
        return true;
    }
    return false;
}

static void fiber_throw_value(JsFiber *fb, JsValue error) {
    fb->error = error;
    fb->failed = true;
}

/* "<text><what>" with the instruction's recorded text (s.fillet, foo, a[0]), else with the value
 * as V8 shows it: "TypeError: o.f is not a function", "TypeError: 5 is not iterable". */
static void fiber_throw_operand(JsContext *ctx, JsFiber *fb, const JsFunctionCell *fn, uint32_t ip,
                                JsValue value, const char *what) {
    const uint16_t *u;
    uint32_t len;
    if (find_note(fn, ip, &u, &len) && len) {
        fiber_throw_units(ctx, fb, "TypeError: ", u, len, what);
        return;
    }
    JsValue e = js_error_fmt(ctx, "TypeError: %v%s", value, what);
    fiber_throw_value(fb, e);
}

static void fiber_throw_callee(JsContext *ctx, JsFiber *fb, const JsFunctionCell *fn, uint32_t ip,
                               JsValue callee, const char *what) {
    fiber_throw_operand(ctx, fb, fn, ip, callee, what);
}

/* V8's ReferenceError for a let/const read before its declaration ran, named when the compiler
 * recorded the name for this read. */
JS_COLD static void fiber_throw_tdz(JsContext *ctx, JsFiber *fb, const JsFunctionCell *fn, uint32_t ip) {
    const uint16_t *u;
    uint32_t len;
    if (find_note(fn, ip, &u, &len) && len)
        fiber_throw_units(ctx, fb, "ReferenceError: Cannot access '", u, len, "' before initialization");
    else
        fiber_throw(ctx, fb, "ReferenceError: Cannot access a variable before initialization");
}

/* A read of a namespace's export (or a module binding) still in the TDZ, named by `name`. */
JS_COLD static void fiber_throw_uninit(JsContext *ctx, JsFiber *fb, JsValue name) {
    fiber_throw_value(fb, js_error_fmt(ctx, "ReferenceError: Cannot access '%v' before initialization", name));
}

/* A module binding in the TDZ: named by the compiler's note (the local name, which is what
 * the source says) or else by the export's own name. */
JS_COLD static void fiber_throw_tdz_binding(JsContext *ctx, JsFiber *fb, const JsFunctionCell *fn,
                                    uint32_t ip, JsString *name) {
    const uint16_t *u;
    uint32_t len;
    if (find_note(fn, ip, &u, &len) && len)
        fiber_throw_units(ctx, fb, "ReferenceError: Cannot access '", u, len, "' before initialization");
    else
        fiber_throw_uninit(ctx, fb, js_value_from_cell(&name->gc));
}

/* A property read from null or undefined: "Cannot read properties of null (reading 'x')", or
 * at a destructuring's first read (which the compiler marks with a note, empty when the source
 * has no readable form) "Cannot destructure property 'x' of 'o' as it is undefined." */
static void fiber_throw_read(JsContext *ctx, JsFiber *fb, const JsFunctionCell *fn, uint32_t ip,
                             JsValue base, JsValue key) {
    const uint16_t *u;
    uint32_t len;
    JsValue e;
    if (find_note(fn, ip, &u, &len)) {
        if (len) {
            JsVm *vm = ctx->vm;
            JsString *src = js_string_cell_new(vm, u, len);
            JsValue sv = src ? js_value_from_cell(&src->gc) : js_undefined();
            js_gc_protect(vm, &sv);
            e = src ? js_error_fmt(ctx, "TypeError: Cannot destructure property '%v' of '%v' as it is %v.",
                                   key, sv, base)
                    : js_oom_value(vm);
            js_gc_unprotect(vm, &sv);
        } else {
            e = js_error_fmt(ctx, "TypeError: Cannot destructure property '%v' of '%v' as it is %v.",
                             key, base, base);
        }
    } else {
        e = js_error_fmt(ctx, "TypeError: Cannot read properties of %v (reading '%v')", base, key);
    }
    fiber_throw_value(fb, e);
}

/* ---- closures, upvalues, natives ---- */

#define JS_MAX_FRAMES 2000

/* Upper bound on a spread call/new's argument count. A JsValue-per-arg array
 * this large is unreachable in practice (~128 MB), but the cap keeps argc well
 * inside int range (so the (int)argc handed to natives can't truncate to a
 * negative) and keeps fb->sp + argc from overflowing uint32. */
#define JS_MAX_CALL_ARGS (1u << 24)

/* Bound on nested interpreter entries. A native (e.g. Array.prototype.forEach)
 * that calls back into a JS closure re-enters run_fiber on the C stack with a
 * fresh fiber, so the per-fiber JS_MAX_FRAMES limit cannot see the depth. This
 * caps the real C-stack recursion; kept well under JS_MAX_FRAMES since each
 * level here also nests a full native + interpreter C frame. */

static JsClosure *closure_new(JsVm *vm, JsFunctionCell *fn) {
    size_t sz = sizeof(JsClosure) + (size_t)fn->n_upvals * sizeof(JsUpvalue *);
    JsGcCell *c = js_gc_new_cell(vm, JS_KIND_CLOSURE, sz);
    if (!c)
        return NULL;
    JsClosure *cl = (JsClosure *)c;
    cl->fn = fn;
    cl->this_val = js_undefined();
    cl->has_this = false;
    cl->prototype = js_undefined();
    cl->proto_made = false;
    cl->props = NULL;
    cl->name = NULL;
    cl->length_deleted = false;
    cl->n_upvals = fn->n_upvals;
    for (uint16_t i = 0; i < fn->n_upvals; i++)
        cl->upvals[i] = NULL;
    return cl;
}

static JsUpvalue *upvalue_capture(JsVm *vm, JsFiber *fb, uint32_t slot) {
    for (JsUpvalue *uv = fb->open_upvals; uv; uv = uv->next_open) {
        if (uv->slot == slot)
            return uv;
    }
    JsGcCell *c = js_gc_new_cell(vm, JS_KIND_UPVALUE, sizeof(JsUpvalue));
    if (!c)
        return NULL;
    JsUpvalue *uv = (JsUpvalue *)c;
    uv->fiber = fb;
    uv->slot = slot;
    uv->open = true;
    uv->closed = js_undefined();
    uv->next_open = fb->open_upvals;
    fb->open_upvals = uv;
    return uv;
}

static void upvalues_close_from(JsFiber *fb, uint32_t floor) {
    JsUpvalue **link = &fb->open_upvals;
    while (*link) {
        JsUpvalue *uv = *link;
        if (uv->slot >= floor) {
            uv->closed = fb->stack[uv->slot];
            uv->open = false;
            *link = uv->next_open;
            uv->next_open = NULL;
        } else {
            link = &uv->next_open;
        }
    }
}

void *js_native_userdata(JsContext *ctx) { return ctx->running_native ? ctx->running_native->ud : NULL; }

JsValue js_native_new(JsContext *ctx, const char *name, JsNativeFn fn, void *ud) {
    JsVm *vm = ctx->vm;
    JsString *nm = name ? js_ascii_cell(vm, name) : NULL;
    if (name && !nm)
        return js_undefined();
    /* nm is unreferenced across the next allocation; re-fetch after. */
    JsValue nmv = nm ? js_value_from_cell(&nm->gc) : js_undefined();
    js_gc_protect(vm, &nmv);
    JsGcCell *c = js_gc_new_cell(vm, JS_KIND_NATIVE, sizeof(JsNative));
    js_gc_unprotect(vm, &nmv);
    if (!c)
        return js_undefined();
    JsNative *nf = (JsNative *)c;
    nf->fn = fn;
    nf->bound_fn = NULL;
    nf->ud = ud;
    nf->name = js_is_string(nmv) ? js_value_string(nmv) : NULL;
    nf->statics = NULL;
    nf->props = NULL;
    nf->prototype = js_undefined();
    nf->proto_made = false;
    nf->bound = js_undefined();
    nf->is_bound = false;
    nf->name_deleted = false;
    nf->builtin = !ctx->builtins_ready;
    nf->ctor = nf->builtin ? JS_CTOR_NONE : JS_CTOR_ORDINARY;
    nf->length = 0;
    nf->parent = NULL;
    return js_value_from_cell(c);
}

/* Bound native: js_bytecode.h declares this for the promise module. */
JsValue js_bound_native_new(JsContext *ctx, JsBoundFn fn, JsValue bound) {
    JsVm *vm = ctx->vm;
    js_gc_protect(vm, &bound);
    JsGcCell *c = js_gc_new_cell(vm, JS_KIND_NATIVE, sizeof(JsNative));
    js_gc_unprotect(vm, &bound);
    if (!c)
        return js_undefined();
    JsNative *nf = (JsNative *)c;
    nf->fn = NULL;
    nf->bound_fn = fn;
    nf->ud = NULL;
    nf->name = NULL;
    nf->statics = NULL;
    nf->props = NULL;
    nf->prototype = js_undefined();
    nf->proto_made = false;
    nf->bound = bound;
    nf->is_bound = true;
    nf->name_deleted = false;
    nf->builtin = !ctx->builtins_ready;
    nf->ctor = JS_CTOR_NONE;
    nf->length = 0;
    nf->parent = NULL;
    return js_value_from_cell(c);
}

static bool object_set_ascii(JsContext *ctx, JsObject *obj, const char *key, JsValue v, uint8_t attrs) {
    JsVm *vm = ctx->vm;
    js_gc_protect(vm, &v);
    JsString *k = js_ascii_cell(vm, key);
    JsString *ik = k ? js_intern_cell(vm, k) : NULL;
    js_gc_unprotect(vm, &v);
    if (!ik || !js_map_set(vm, &obj->props, ik, v))
        return false;
    if (attrs)
        js_map_add_attrs(&obj->props, ik, attrs);
    return true;
}

bool js_object_set_ascii(JsContext *ctx, JsObject *obj, const char *key, JsValue v) {
    return object_set_ascii(ctx, obj, key, v, 0);
}

bool js_object_set_hidden(JsContext *ctx, JsObject *obj, const char *key, JsValue v) {
    return object_set_ascii(ctx, obj, key, v, JS_ATTR_HIDDEN);
}

void js_context_set_userdata(JsContext *ctx, void *ud) {
    if (ctx) ctx->host_ud = ud;
}

void *js_context_userdata(JsContext *ctx) {
    return ctx ? ctx->host_ud : NULL;
}

bool js_register_native(JsContext *ctx, const uint16_t *name, size_t name_len,
                        JsNativeFn fn, void *userdata) {
    JsVm *vm = ctx->vm;
    JsValue key = js_atom(vm, name, name_len);
    if (!js_is_string(key))
        return false;
    js_gc_protect(vm, &key);
    JsGcCell *c = js_gc_new_cell(vm, JS_KIND_NATIVE, sizeof(JsNative));
    js_gc_unprotect(vm, &key);
    if (!c)
        return false;
    JsNative *nf = (JsNative *)c;
    nf->fn = fn;
    nf->bound_fn = NULL;
    nf->ud = userdata;
    nf->statics = NULL;
    nf->props = NULL;
    nf->prototype = js_undefined();
    nf->proto_made = false;
    nf->name = js_value_string(key);
    nf->bound = js_undefined();
    nf->is_bound = false;
    nf->name_deleted = false;
    nf->builtin = !ctx->builtins_ready;
    nf->ctor = nf->builtin ? JS_CTOR_NONE : JS_CTOR_ORDINARY;
    nf->length = 0;
    nf->parent = NULL;
    return js_map_set(vm, &ctx->globals->props, js_value_string(key),
                      js_value_from_cell(c));
}

/* ---- fiber stack / frame growth ---- */

static bool stack_ensure(JsVm *vm, JsFiber *fb, uint32_t need) {
    if (need <= fb->stack_cap)
        return true;
    uint32_t ncap = fb->stack_cap ? fb->stack_cap * 2 : 64;
    while (ncap < need)
        ncap *= 2;
    JsValue *ns = js_realloc_raw(vm, fb->stack, (size_t)fb->stack_cap * sizeof(JsValue),
                                 (size_t)ncap * sizeof(JsValue));
    if (!ns)
        return false;
    fb->stack = ns;
    fb->stack_cap = ncap;
    return true;
}

static bool frames_ensure(JsVm *vm, JsFiber *fb, uint32_t need) {
    if (need <= fb->frame_cap)
        return true;
    uint32_t ncap = fb->frame_cap ? fb->frame_cap * 2 : 8;
    while (ncap < need)
        ncap *= 2;
    JsFrame *nf = js_realloc_raw(vm, fb->frames, (size_t)fb->frame_cap * sizeof(JsFrame),
                                 (size_t)ncap * sizeof(JsFrame));
    if (!nf)
        return false;
    fb->frames = nf;
    fb->frame_cap = ncap;
    return true;
}

/*
 * Binds args at stack[base..base+argc) into the callee's locals and pushes
 * a frame. Layout: callee at base-2, this at base-1. Returns 0 on success,
 * -1 on OOM, -2 on stack overflow (caller throws the right error).
 */
static int frame_setup(JsContext *ctx, JsFiber *fb, JsClosure *cl, uint32_t base,
                       uint32_t argc, bool is_construct) {
    JsVm *vm = ctx->vm;
    JsFunctionCell *fn = cl->fn;
    if (fb->frame_count >= JS_MAX_FRAMES)
        return -2;
    if (!stack_ensure(vm, fb, base + fn->n_locals + fn->max_stack + 8))
        return -1;
    bool has_rest = (fn->fn_flags & JS_FN_HAS_REST) != 0;
    bool uses_args = (fn->fn_flags & JS_FN_USES_ARGUMENTS) != 0;
    uint32_t fixed = has_rest ? (uint32_t)fn->n_params - 1 : fn->n_params;

    /* `arguments`: a plain array of every actual arg (this subset's deliberate
     * choice — the iterator opcode only knows arrays, so for-of and spread
     * work without an Arguments exotic). Built first, while the args are
     * still intact on the stack, and rooted across the rest-array allocation
     * below since nothing else holds it yet. */
    JsValue args_val = js_undefined();
    if (uses_args) {
        JsObject *args = js_array_new_cell(ctx, argc);
        if (!args)
            return -1;
        for (uint32_t i = 0; i < argc; i++)
            args->elems[i] = fb->stack[base + i];
        args->elem_count = argc;
        args_val = js_value_from_cell(&args->gc);
    }
    JsObject *rest = NULL;
    if (has_rest) {
        if (uses_args)
            js_gc_protect(vm, &args_val);
        uint32_t rn = argc > fixed ? argc - fixed : 0;
        rest = js_array_new_cell(ctx, rn);
        if (uses_args)
            js_gc_unprotect(vm, &args_val);
        if (!rest)
            return -1;
        for (uint32_t i = 0; i < rn; i++)
            rest->elems[i] = fb->stack[base + fixed + i];
        rest->elem_count = rn;
    }
    /* missing fixed params -> undefined */
    for (uint32_t i = argc; i < fixed; i++)
        fb->stack[base + i] = js_undefined();
    if (has_rest)
        fb->stack[base + fixed] = js_value_from_cell(&rest->gc);
    /* clear declared (non-param) locals (this also wipes surplus args) */
    for (uint32_t i = fn->n_params; i < fn->n_locals; i++)
        fb->stack[base + i] = js_undefined();
    if (uses_args)
        fb->stack[base + fn->n_params] = args_val;

    if (!frames_ensure(vm, fb, fb->frame_count + 1))
        return -1;
    JsFrame *fr = &fb->frames[fb->frame_count++];
    fr->closure = cl;
    fr->ip = 0;
    fr->base = base;
    fr->is_construct = is_construct;
    fb->sp = base + fn->n_locals;
    return 0;
}

/* ---- the dispatch loop ---- */

/* async-call helper: runs cl on its own fiber, returns a result promise */
static bool call_async(JsContext *ctx, JsClosure *cl, JsValue this_val,
                       const JsValue *args, int argc, JsValue *out);

/* Aligned so its speed does not depend on code elsewhere: the loop's branch targets sit at fixed
 * offsets from here, and when its start fell on a 16-byte rather than a 64-byte boundary, because
 * an unrelated file grew, the benchmarks lost up to 22%. */
__attribute__((aligned(64))) static JsRunStatus run_fiber(JsContext *ctx, JsFiber *fb, JsValue *result) {
    JsVm *vm = ctx->vm;

#define S (fb->stack)
#define PUSH(v) (S[fb->sp++] = (v))
#define POPV() (S[--fb->sp])
#define PEEK(n) (S[fb->sp - 1 - (uint32_t)(n)])

run:; /* (re)load the top frame */
    {
        JsFrame *fr = &fb->frames[fb->frame_count - 1];
        JsClosure *cl = fr->closure;
        JsFunctionCell *fn = cl->fn;
        const uint8_t *code = fn->code;
        JsValue *K = fn->consts;
        uint32_t base = fr->base;
        uint32_t ip = fr->ip;
        uint32_t op_ip = ip;
        /* a resume-with-rejection sets failed before re-entering */
        if (fb->failed)
            goto on_error;

#define READ_U16() (ip += 2, (uint16_t)(code[ip - 2] | ((uint16_t)code[ip - 1] << 8)))
#define READ_U32()                                                              \
    (ip += 4, (uint32_t)code[ip - 4] | ((uint32_t)code[ip - 3] << 8) |          \
                  ((uint32_t)code[ip - 2] << 16) | ((uint32_t)code[ip - 1] << 24))
#define RT_THROW(msg)                                                           \
    do {                                                                        \
        fiber_throw(ctx, fb, (msg));                                            \
        fb->err_pos = lookup_pos(fn, op_ip);                                    \
        fb->frames[fb->frame_count - 1].ip = op_ip + 1;                         \
        goto on_error;                                                          \
    } while (0)
#define RT_RAISE()                                                              \
    do {                                                                        \
        fb->err_pos = lookup_pos(fn, op_ip);                                    \
        fb->frames[fb->frame_count - 1].ip = op_ip + 1;                         \
        goto on_error;                                                          \
    } while (0)

/* Raise what a conversion left pending (js_take_error), from an opcode without operands (ip is then
 * op_ip + 1): the shared exit after the loop.
 *
 * The loop's speed hangs on which of its hot values (fb, ip, code, ...) the register allocator
 * keeps in the five callee-saved registers across its calls, a whole-function decision that small
 * edits anywhere in it can flip: adding ToPrimitive first cost the benchmarks 25-55%, with the hot
 * handlers' code unchanged, because the loop head then reloaded fb, ip and four more values from
 * the stack at every instruction. So conversions add almost nothing here: the operators' slow
 * paths are calls (add_slow, number_operands, ...), object keys convert inside the property
 * functions, and the property opcodes' existing "out of memory" raises take a pending conversion
 * error (fiber_throw). Measure against the previous build after any change to this loop. */
#define RT_THROW_PENDING() goto throw_pending

        for (;;) {
            /* Fuel and interrupt are one check because both mean "stop now".
             * Fuel re-arms to 1 and the interrupt flag stays set, so each is
             * sticky: the handler a guest jumps to throws again on its own
             * first instruction, and no try/catch can absorb the stop and keep
             * running. Neither clears itself — fuel is re-armed by
             * js_context_set_fuel, the flag by js_vm_clear_interrupt. */
            if (--fb->fuel == 0 || vm->interrupt) {
                if (vm->interrupt) {
                    fb->fuel++; /* not our budget that ran out; leave it intact */
                    RT_THROW(JS_MSG_INTERRUPT);
                }
                fb->fuel = 1;
                RT_THROW(JS_MSG_BUDGET);
            }
            op_ip = ip;
            JsOp op = (JsOp)code[ip++];
            switch (op) {
            case JS_OP_NOP:
                break;
            case JS_OP_CONST:
                PUSH(K[READ_U16()]);
                break;
            case JS_OP_UNDEFINED: PUSH(js_undefined()); break;
            case JS_OP_NULL: PUSH(js_null()); break;
            case JS_OP_TRUE: PUSH(js_bool(true)); break;
            case JS_OP_FALSE: PUSH(js_bool(false)); break;
            case JS_OP_POP: fb->sp--; break;
            case JS_OP_DUP: {
                JsValue v = PEEK(0);
                PUSH(v);
                break;
            }
            case JS_OP_DUP2: {
                JsValue a = PEEK(1), b = PEEK(0);
                PUSH(a);
                PUSH(b);
                break;
            }
            case JS_OP_SWAP: {
                JsValue a = PEEK(1), b = PEEK(0);
                PEEK(1) = b;
                PEEK(0) = a;
                break;
            }
            case JS_OP_ROT3: {
                JsValue a = PEEK(2), b = PEEK(1), c = PEEK(0);
                PEEK(2) = b;
                PEEK(1) = c;
                PEEK(0) = a;
                break;
            }
            case JS_OP_GET_LOCAL: {
                uint16_t slot = READ_U16();
                JsValue v = S[base + slot];
                if (v.bits == JS_SPECIAL_TDZ) {
                    fiber_throw_tdz(ctx, fb, fn, op_ip);
                    RT_RAISE();
                }
                PUSH(v);
                break;
            }
            case JS_OP_SET_LOCAL:
                S[base + READ_U16()] = PEEK(0);
                break;
            case JS_OP_SET_LOCAL_CHECKED: {
                uint16_t slot = READ_U16();
                if (S[base + slot].bits == JS_SPECIAL_TDZ) {
                    fiber_throw_tdz(ctx, fb, fn, op_ip);
                    RT_RAISE();
                }
                S[base + slot] = PEEK(0);
                break;
            }
            case JS_OP_TDZ:
                S[base + READ_U16()].bits = JS_SPECIAL_TDZ;
                break;
            case JS_OP_GET_UPVAL: {
                JsUpvalue *uv = cl->upvals[READ_U16()];
                /* an open upvalue aliases the stack of the fiber that owns
                 * it, which may differ from fb during a nested call */
                JsValue v = uv->open ? uv->fiber->stack[uv->slot] : uv->closed;
                if (v.bits == JS_SPECIAL_TDZ) {
                    fiber_throw_tdz(ctx, fb, fn, op_ip);
                    RT_RAISE();
                }
                PUSH(v);
                break;
            }
            case JS_OP_SET_UPVAL: {
                JsUpvalue *uv = cl->upvals[READ_U16()];
                if (uv->open)
                    uv->fiber->stack[uv->slot] = PEEK(0);
                else
                    uv->closed = PEEK(0);
                break;
            }
            case JS_OP_SET_UPVAL_CHECKED: {
                JsUpvalue *uv = cl->upvals[READ_U16()];
                JsValue *cell = uv->open ? &uv->fiber->stack[uv->slot] : &uv->closed;
                if (cell->bits == JS_SPECIAL_TDZ) {
                    fiber_throw_tdz(ctx, fb, fn, op_ip);
                    RT_RAISE();
                }
                *cell = PEEK(0);
                break;
            }
            case JS_OP_CLOSE_UPVALS:
                upvalues_close_from(fb, base + READ_U16());
                break;
            case JS_OP_GET_THIS:
                PUSH(cl->has_this ? cl->this_val : S[base - 1]);
                break;
            case JS_OP_GET_CALLEE:
                PUSH(S[base - 2]);
                break;
            case JS_OP_GET_GLOBAL:
            case JS_OP_GET_GLOBAL_SOFT: {
                JsString *name = js_value_string(K[READ_U16()]);
                bool found;
                JsValue v = js_map_get(&ctx->globals->props, name, &found);
                if (!found) {
                    if (op == JS_OP_GET_GLOBAL_SOFT) {
                        PUSH(js_undefined());
                        break;
                    }
                    fiber_throw_name(ctx, fb, "ReferenceError: ", name, " is not defined");
                    RT_RAISE();
                }
                PUSH(v);
                break;
            }
            case JS_OP_SET_GLOBAL: {
                JsString *name = js_value_string(K[READ_U16()]);
                bool found;
                js_map_get(&ctx->globals->props, name, &found);
                if (!found) {
                    fiber_throw_name(ctx, fb, "ReferenceError: ", name, " is not defined");
                    RT_RAISE();
                }
                if (!js_map_set(vm, &ctx->globals->props, name, PEEK(0)))
                    RT_THROW("out of memory");
                break;
            }
            case JS_OP_GET_LEXICAL:
            case JS_OP_GET_LEXICAL_SOFT: {
                JsString *name = js_value_string(K[READ_U16()]);
                bool found = false;
                JsValue v = js_undefined();
                if (ctx->repl_scope)
                    v = js_map_get(&ctx->repl_scope->props, name, &found);
                if (found) {
                    if (v.bits == JS_SPECIAL_TDZ) {
                        fiber_throw_name(ctx, fb, "ReferenceError: Cannot access '", name, "' before initialization");
                        RT_RAISE();
                    }
                    PUSH(v);
                    break;
                }
                v = js_map_get(&ctx->globals->props, name, &found);
                if (!found) {
                    if (op == JS_OP_GET_LEXICAL_SOFT) {
                        PUSH(js_undefined());
                        break;
                    }
                    fiber_throw_name(ctx, fb, "ReferenceError: ", name, " is not defined");
                    RT_RAISE();
                }
                PUSH(v);
                break;
            }
            case JS_OP_SET_LEXICAL: {
                JsString *name = js_value_string(K[READ_U16()]);
                bool found = false;
                if (ctx->repl_scope)
                    js_map_get(&ctx->repl_scope->props, name, &found);
                if (found) {
                    bool is_const = false;
                    if (ctx->repl_const)
                        js_map_get(&ctx->repl_const->props, name, &is_const);
                    if (is_const)
                        RT_THROW("TypeError: Assignment to constant variable.");
                    if (!js_map_set(vm, &ctx->repl_scope->props, name, PEEK(0)))
                        RT_THROW("out of memory");
                    break;
                }
                js_map_get(&ctx->globals->props, name, &found);
                if (!found) {
                    fiber_throw_name(ctx, fb, "ReferenceError: ", name, " is not defined");
                    RT_RAISE();
                }
                if (!js_map_set(vm, &ctx->globals->props, name, PEEK(0)))
                    RT_THROW("out of memory");
                break;
            }
            case JS_OP_DEFINE_LEXICAL: {
                JsString *name = js_value_string(K[READ_U16()]);
                uint8_t is_const = code[ip++];
                if (!ctx->repl_scope) {
                    JsValue s = js_object_new(ctx); /* fiber-rooted; nothing to lose on GC */
                    if (!js_is_object(s))
                        RT_THROW("out of memory");
                    ctx->repl_scope = js_value_object(s);
                }
                if (!js_map_set(vm, &ctx->repl_scope->props, name, PEEK(0)))
                    RT_THROW("out of memory");
                if (is_const) {
                    if (!ctx->repl_const) {
                        JsValue s = js_object_new(ctx);
                        if (!js_is_object(s))
                            RT_THROW("out of memory");
                        ctx->repl_const = js_value_object(s);
                    }
                    if (!js_map_set(vm, &ctx->repl_const->props, name, js_bool(true)))
                        RT_THROW("out of memory");
                } else if (ctx->repl_const) {
                    js_map_delete(&ctx->repl_const->props, name); /* re-decl as non-const */
                }
                break;
            }
            case JS_OP_ADD: {
                JsValue a = PEEK(1), b = PEEK(0);
                if (js_is_number(a) && js_is_number(b)) {
                    fb->sp--;
                    PEEK(0) = js_number(js_get_number(a) + js_get_number(b));
                    break;
                }
                if (!add_slow(ctx, fb, ip))
                    RT_THROW_PENDING();
                break;
            }
            case JS_OP_SUB: case JS_OP_MUL: case JS_OP_DIV:
            case JS_OP_MOD: case JS_OP_POW: {
                double a, b;
                if (js_is_number(PEEK(1)) && js_is_number(PEEK(0))) {
                    a = js_get_number(PEEK(1));
                    b = js_get_number(PEEK(0));
                } else if (!number_operands(ctx, fb, ip, &a, &b)) {
                    RT_THROW_PENDING();
                }
                double r;
                switch (op) {
                case JS_OP_SUB: r = a - b; break;
                case JS_OP_MUL: r = a * b; break;
                case JS_OP_DIV: r = a / b; break;
                case JS_OP_MOD: r = js_fmod_approx(a, b); break;
                default: r = js_pow_op(a, b); break;
                }
                fb->sp--;
                PEEK(0) = js_number(r);
                break;
            }
            case JS_OP_NEG: case JS_OP_POS: case JS_OP_BITNOT: {
                double d;
                if (js_is_number(PEEK(0))) {
                    d = js_get_number(PEEK(0));
                } else if (!number_operand(ctx, fb, ip, &d)) {
                    RT_THROW_PENDING();
                }
                PEEK(0) = js_number(op == JS_OP_NEG ? -d : op == JS_OP_POS ? d : ~js_to_int32(d));
                break;
            }
            case JS_OP_NOT:
                PEEK(0) = js_bool(!js_to_boolean(PEEK(0)));
                break;
            case JS_OP_BITAND: case JS_OP_BITOR: case JS_OP_BITXOR:
            case JS_OP_SHL: case JS_OP_SHR: case JS_OP_USHR: {
                double da, db;
                if (js_is_number(PEEK(1)) && js_is_number(PEEK(0))) {
                    da = js_get_number(PEEK(1));
                    db = js_get_number(PEEK(0));
                } else if (!number_operands(ctx, fb, ip, &da, &db)) {
                    RT_THROW_PENDING();
                }
                fb->sp--;
                double r;
                switch (op) {
                case JS_OP_BITAND: r = js_to_int32(da) & js_to_int32(db); break;
                case JS_OP_BITOR: r = js_to_int32(da) | js_to_int32(db); break;
                case JS_OP_BITXOR: r = js_to_int32(da) ^ js_to_int32(db); break;
                case JS_OP_SHL:
                    r = (double)(int32_t)((uint32_t)js_to_int32(da) << (js_to_uint32(db) & 31));
                    break;
                case JS_OP_SHR: r = js_to_int32(da) >> (js_to_uint32(db) & 31); break;
                default: r = js_to_uint32(da) >> (js_to_uint32(db) & 31); break;
                }
                PEEK(0) = js_number(r);
                break;
            }
            case JS_OP_EQ: case JS_OP_NEQ: {
                int eq;
                if (js_is_number(PEEK(1)) && js_is_number(PEEK(0))) {
                    eq = js_get_number(PEEK(1)) == js_get_number(PEEK(0));
                } else if ((eq = eq_slow(ctx, fb, ip)) < 0) {
                    RT_THROW_PENDING();
                }
                fb->sp--;
                PEEK(0) = js_bool(op == JS_OP_EQ ? eq : !eq);
                break;
            }
            case JS_OP_STRICT_EQ: case JS_OP_STRICT_NEQ: {
                bool eq = js_strict_equals(PEEK(1), PEEK(0));
                fb->sp--;
                PEEK(0) = js_bool(op == JS_OP_STRICT_EQ ? eq : !eq);
                break;
            }
            case JS_OP_LT: case JS_OP_GT: case JS_OP_LE: case JS_OP_GE: {
                int c;
                if (js_is_number(PEEK(1)) && js_is_number(PEEK(0))) {
                    double x = js_get_number(PEEK(1)), y = js_get_number(PEEK(0));
                    c = x != x || y != y ? 2 : x < y ? -1 : x == y ? 0 : 1;
                } else if ((c = compare_slow(ctx, fb, ip)) == 3) {
                    RT_THROW_PENDING();
                }
                fb->sp--;
                bool r;
                switch (op) {
                case JS_OP_LT: r = c == -1; break;
                case JS_OP_GT: r = c == 1; break;
                case JS_OP_LE: r = c == -1 || c == 0; break;
                default: r = c == 1 || c == 0; break;
                }
                PEEK(0) = js_bool(c == 2 ? false : r);
                break;
            }
            case JS_OP_IN: {
                bool has;
                JsPropStatus st = has_property(ctx, PEEK(0), PEEK(1), &has);
                if (st == JS_PROP_TYPE_ERROR) {
                    fiber_throw_value(fb, js_error_fmt(ctx, "TypeError: Cannot use 'in' operator to search for '%v' in %v",
                                                       PEEK(1), PEEK(0)));
                    RT_RAISE();
                }
                if (st == JS_PROP_OOM)
                    RT_THROW("out of memory");
                fb->sp--;
                PEEK(0) = js_bool(has);
                break;
            }
            case JS_OP_INSTANCEOF: {
                JsValue ierr;
                int is = instance_of(ctx, PEEK(1), PEEK(0), &ierr);
                if (is < 0) {
                    fiber_throw_value(fb, ierr);
                    RT_RAISE();
                }
                fb->sp--;
                PEEK(0) = js_bool(is == 1);
                break;
            }
            case JS_OP_TYPEOF: {
                JsValue v = PEEK(0);
                const char *t;
                if (js_is_number(v)) t = "number";
                else if (js_is_string(v)) t = "string";
                else if (js_is_bool(v)) t = "boolean";
                else if (js_is_undefined(v)) t = "undefined";
                else if (js_is_function(v)) t = "function";
                else t = "object";
                JsString *s = js_ascii_cell(vm, t);
                if (!s)
                    RT_THROW("out of memory");
                PEEK(0) = js_value_from_cell(&s->gc);
                break;
            }
            case JS_OP_TO_STRING: {
                fr->ip = ip; /* a template's ${x}: its toString may run, and read the frame */
                JsString *s = js_op_to_string(ctx, PEEK(0));
                if (!s)
                    RT_THROW("out of memory");
                PEEK(0) = js_value_from_cell(&s->gc);
                break;
            }
            case JS_OP_JUMP:
                ip = READ_U32();
                break;
            case JS_OP_JUMP_IF_FALSE: {
                uint32_t t = READ_U32();
                if (!js_to_boolean(POPV()))
                    ip = t;
                break;
            }
            case JS_OP_JUMP_IF_TRUE: {
                uint32_t t = READ_U32();
                if (js_to_boolean(POPV()))
                    ip = t;
                break;
            }
            case JS_OP_JF_PEEK: {
                uint32_t t = READ_U32();
                if (!js_to_boolean(PEEK(0)))
                    ip = t;
                break;
            }
            case JS_OP_JT_PEEK: {
                uint32_t t = READ_U32();
                if (js_to_boolean(PEEK(0)))
                    ip = t;
                break;
            }
            case JS_OP_JNN_PEEK: {
                uint32_t t = READ_U32();
                if (!is_nullish(PEEK(0)))
                    ip = t;
                break;
            }
            case JS_OP_OPT_CHAIN: {
                uint32_t t = READ_U32();
                if (is_nullish(PEEK(0))) {
                    PEEK(0) = js_undefined();
                    ip = t;
                }
                break;
            }
            case JS_OP_OPT_CALL_CHECK: {
                uint32_t t = READ_U32();
                if (is_nullish(PEEK(1))) { /* callee under this */
                    fb->sp -= 2;
                    ip = t;
                }
                break;
            }
            case JS_OP_CASE: {
                uint32_t t = READ_U32();
                JsValue test = POPV();
                if (js_strict_equals(test, PEEK(0))) {
                    fb->sp--;
                    ip = t;
                }
                break;
            }
            case JS_OP_NEW_OBJECT: {
                JsValue no = js_object_new(ctx);
                if (!js_is_object(no))
                    RT_THROW("out of memory");
                PUSH(no);
                break;
            }
            case JS_OP_NEW_REGEXP: {
                uint16_t csrc = READ_U16();
                uint16_t cflags = READ_U16();
#ifdef LAMASSU_HAS_REGEX
                /* pattern/flag atoms stay alive as consts of the running fn */
                JsString *src = js_value_string(K[csrc]);
                JsString *flg = js_value_string(K[cflags]);
                fr->ip = ip; /* GC-safe point: construction allocates */
                const char *em = NULL;
                JsValue re = js_regexp_new(ctx, src->units, src->length,
                                           flg->units, flg->length, &em);
                if (!js_is_object(re)) {
                    fiber_throw_value(fb, js_regexp_error(ctx, em ? em : "out of memory", src->units, src->length,
                                                          flg->units, flg->length));
                    RT_RAISE();
                }
                PUSH(re);
#else
                (void)csrc;
                (void)cflags;
                RT_THROW("SyntaxError: regex support is not compiled into this build");
#endif
                break;
            }
            case JS_OP_NEW_ARRAY: {
                uint16_t count = READ_U16();
                JsObject *a = js_array_new_cell(ctx, count);
                if (!a)
                    RT_THROW("out of memory");
                for (uint16_t i = 0; i < count; i++)
                    a->elems[i] = S[fb->sp - count + i];
                a->elem_count = count;
                fb->sp -= count;
                PUSH(js_value_from_cell(&a->gc));
                break;
            }
            case JS_OP_ARRAY_APPEND: {
                /* PEEK(1) is the array under construction. The compiler always
                 * emits this after NEW_ARRAY, but validated-yet-hostile
                 * bytecode (a depth-preserving swap of NEW_ARRAY for another
                 * +1 op) could put a non-array here; guard the cast so loaded
                 * bytecode can never turn a bad type into a wild pointer. */
                if (!is_array(PEEK(1)))
                    RT_THROW("internal error: array builder on non-array");
                JsObject *a = js_value_object(PEEK(1));
                if (!js_array_append(vm, a, PEEK(0)))
                    RT_THROW("out of memory");
                fb->sp--;
                break;
            }
            case JS_OP_ARRAY_SPREAD: {
                JsValue src = PEEK(0);
                if (!is_array(PEEK(1)))
                    RT_THROW("internal error: array builder on non-array");
                JsObject *a = js_value_object(PEEK(1));
                if (js_is_object(src) && js_value_object(src)->obj_kind == JS_OBJ_ARRAY) {
                    JsObject *s = js_value_object(src);
                    for (uint32_t i = 0; i < s->elem_count; i++) {
                        if (!js_array_append(vm, a, s->elems[i]))
                            RT_THROW("out of memory");
                    }
                } else if (js_is_string(src)) {
                    JsString *s = js_value_string(src);
                    for (uint32_t i = 0; i < s->length;) {
                        uint32_t n = 1;
                        if (s->units[i] >= 0xD800 && s->units[i] <= 0xDBFF &&
                            i + 1 < s->length && s->units[i + 1] >= 0xDC00 &&
                            s->units[i + 1] <= 0xDFFF)
                            n = 2;
                        JsString *c = js_string_cell_new(vm, s->units + i, n);
                        if (!c || !js_array_append(vm, a, js_value_from_cell(&c->gc)))
                            RT_THROW("out of memory");
                        i += n;
                    }
                } else {
                    fiber_throw_operand(ctx, fb, fn, op_ip, src, " is not iterable");
                    RT_RAISE();
                }
                fb->sp--;
                break;
            }
            case JS_OP_ARRAY_REST: {
                /* PEEK(0) is the "skip this many" count the compiler always
                 * pushes as a small non-negative integer; validated-yet-
                 * hostile bytecode could put something else here, so guard
                 * the cast (see ARRAY_APPEND above for the same idea
                 * applied to a type instead of a number). */
                uint32_t from;
                if (!value_to_index(PEEK(0), &from))
                    RT_THROW("internal error: bad rest-count operand");
                JsValue src = PEEK(1);
                JsObject *out = js_array_new_cell(ctx, 0);
                if (!out)
                    RT_THROW("out of memory");
                if (js_is_object(src) && js_value_object(src)->obj_kind == JS_OBJ_ARRAY) {
                    JsObject *s = js_value_object(src);
                    for (uint32_t i = from; i < s->elem_count; i++) {
                        if (!js_array_append(vm, out, s->elems[i]))
                            RT_THROW("out of memory");
                    }
                } else if (js_is_string(src)) {
                    JsString *s = js_value_string(src);
                    for (uint32_t i = from; i < s->length; i++) {
                        JsString *c = js_string_cell_new(vm, s->units + i, 1);
                        if (!c || !js_array_append(vm, out, js_value_from_cell(&c->gc)))
                            RT_THROW("out of memory");
                    }
                }
                fb->sp -= 2;
                PUSH(js_value_from_cell(&out->gc));
                break;
            }
            case JS_OP_OBJ_SPREAD: {
                if (!js_is_object(PEEK(1)))
                    RT_THROW("internal error: object builder on non-object");
                JsObject *dst = js_value_object(PEEK(1));
                JsString *uninit;
                if (!js_spread_into_object(ctx, dst, PEEK(0), &uninit)) {
                    if (!uninit)
                        RT_THROW("out of memory");
                    fiber_throw_value(fb, js_error_fmt(ctx, "ReferenceError: %S is not defined", uninit));
                    RT_RAISE();
                }
                fb->sp--;
                break;
            }
            case JS_OP_DEFINE_PROP: {
                const char *msg;
                JsPropStatus st = set_property(ctx, PEEK(2), PEEK(1), PEEK(0), &msg);
                if (st == JS_PROP_OOM)
                    RT_THROW("out of memory");
                if (st == JS_PROP_TYPE_ERROR) {
                    fiber_throw_value(fb, set_error(ctx, msg, PEEK(2), PEEK(1)));
                    RT_RAISE();
                }
                fb->sp -= 2;
                break;
            }
            case JS_OP_GET_PROP: {
                JsValue out;
                JsPropStatus st = get_property(ctx, PEEK(1), PEEK(0), &out);
                if (st == JS_PROP_OOM)
                    RT_THROW("out of memory");
                if (JS_UNLIKELY(out.bits == JS_SPECIAL_TDZ)) {
                    fiber_throw_uninit(ctx, fb, PEEK(0));
                    RT_RAISE();
                }
                if (st == JS_PROP_TYPE_ERROR) {
                    fiber_throw_read(ctx, fb, fn, op_ip, PEEK(1), PEEK(0));
                    RT_RAISE();
                }
                fb->sp--;
                PEEK(0) = out;
                break;
            }
            case JS_OP_GET_PROP_ATOM: {
                JsValue key = K[READ_U16()];
                JsValue out;
                JsPropStatus st = get_property(ctx, PEEK(0), key, &out);
                if (st == JS_PROP_OOM)
                    RT_THROW("out of memory");
                if (JS_UNLIKELY(out.bits == JS_SPECIAL_TDZ)) {
                    fiber_throw_uninit(ctx, fb, key);
                    RT_RAISE();
                }
                if (st == JS_PROP_TYPE_ERROR) {
                    fiber_throw_read(ctx, fb, fn, op_ip, PEEK(0), key);
                    RT_RAISE();
                }
                PEEK(0) = out;
                break;
            }
            case JS_OP_SET_PROP: {
                const char *msg;
                JsPropStatus st = set_property(ctx, PEEK(2), PEEK(1), PEEK(0), &msg);
                if (st == JS_PROP_OOM)
                    RT_THROW("out of memory");
                if (st == JS_PROP_TYPE_ERROR) {
                    fiber_throw_value(fb, set_error(ctx, msg, PEEK(2), PEEK(1)));
                    RT_RAISE();
                }
                JsValue v = PEEK(0);
                fb->sp -= 2;
                PEEK(0) = v;
                break;
            }
            case JS_OP_SET_PROP_ATOM: {
                JsValue key = K[READ_U16()];
                const char *msg;
                JsPropStatus st = set_property(ctx, PEEK(1), key, PEEK(0), &msg);
                if (st == JS_PROP_OOM)
                    RT_THROW("out of memory");
                if (st == JS_PROP_TYPE_ERROR) {
                    fiber_throw_value(fb, set_error(ctx, msg, PEEK(1), key));
                    RT_RAISE();
                }
                JsValue v = PEEK(0);
                fb->sp--;
                PEEK(0) = v;
                break;
            }
            case JS_OP_DELETE_PROP: {
                JsValue key = PEEK(0), b = PEEK(1);
                if (js_is_object(b)) {
                    JsObject *o = js_value_object(b);
                    /* Sealing exists precisely to stop the shape changing, so
                     * delete is refused on both sealed and frozen objects. */
                    if (o->obj_flags) {
                        fiber_throw_value(fb, js_error_fmt(ctx, "TypeError: Cannot delete property '%v' of %v", key, b));
                        RT_RAISE();
                    }
                    if (!delete_object_prop(ctx, o, key))
                        RT_THROW("out of memory");
                } else if (js_is_function(b)) {
                    JsValue derr;
                    int dst = delete_function_prop(ctx, b, key, &derr);
                    if (dst == 1) {
                        fiber_throw_value(fb, derr);
                        RT_RAISE();
                    }
                    if (dst == 2)
                        RT_THROW("out of memory");
                } else if (is_nullish(b)) {
                    RT_THROW("TypeError: Cannot convert undefined or null to object");
                }
                fb->sp -= 2;
                PUSH(js_bool(true));
                break;
            }
            case JS_OP_ITER_NEW: {
                JsValue it = PEEK(0);
                if (js_mapobj_is(it) || js_setobj_is(it)) {
                    /* no lazy MapIterator/SetIterator: substitute a real
                     * (and thus array-iterable) snapshot right here — a
                     * Map's default iterator is its entries, a Set's is
                     * its values */
                    fr->ip = ip; /* GC-safe point before allocating it */
                    JsObject *snap = js_mapobj_is(it)
                        ? js_mapobj_entries_array(ctx, js_value_object(it))
                        : js_setobj_values_array(ctx, js_value_object(it));
                    if (!snap)
                        RT_THROW("out of memory");
                    it = js_value_from_cell(&snap->gc);
                    PEEK(0) = it;
                }
                bool ok = js_is_string(it) ||
                          (js_is_object(it) && js_value_object(it)->obj_kind == JS_OBJ_ARRAY);
                if (!ok) {
                    fiber_throw_operand(ctx, fb, fn, op_ip, it, " is not iterable");
                    RT_RAISE();
                }
                PUSH(js_number(0));
                break;
            }
            case JS_OP_ITER_NEXT: {
                uint32_t done_target = READ_U32();
                JsValue it = PEEK(1);
                /* PEEK(0) is the iterator index the compiler always keeps a
                 * small non-negative integer; guard the cast against
                 * hostile bytecode the same way as ARRAY_REST above. */
                uint32_t i;
                if (!value_to_index(PEEK(0), &i))
                    RT_THROW("internal error: bad iterator index operand");
                if (js_is_string(it)) {
                    JsString *s = js_value_string(it);
                    if (i >= s->length) {
                        fb->sp -= 2;
                        ip = done_target;
                        break;
                    }
                    uint32_t n = 1;
                    if (s->units[i] >= 0xD800 && s->units[i] <= 0xDBFF &&
                        i + 1 < s->length && s->units[i + 1] >= 0xDC00 &&
                        s->units[i + 1] <= 0xDFFF)
                        n = 2;
                    JsString *c = js_string_cell_new(vm, s->units + i, n);
                    if (!c)
                        RT_THROW("out of memory");
                    PEEK(0) = js_number((double)(i + n));
                    PUSH(js_value_from_cell(&c->gc));
                } else {
                    /* ITER_NEW only pushes string/array iterators; guard the
                     * cast against hostile bytecode that reaches ITER_NEXT
                     * with some other value in the iterator slot */
                    if (!is_array(it))
                        RT_THROW("TypeError: value is not iterable");
                    JsObject *a = js_value_object(it);
                    if (i >= a->elem_count) {
                        fb->sp -= 2;
                        ip = done_target;
                        break;
                    }
                    PEEK(0) = js_number((double)(i + 1));
                    PUSH(a->elems[i]);
                }
                break;
            }
            case JS_OP_CLOSURE: {
                uint16_t cidx = READ_U16();
                uint8_t flags = code[ip++];
                JsFunctionCell *proto = js_value_function(K[cidx]);
                fr->ip = ip; /* GC-safe point before allocations */
                JsClosure *ncl = closure_new(vm, proto); /* upvals[] are NULL */
                if (!ncl)
                    RT_THROW("out of memory");
                /* Root ncl on the operand stack before capturing upvalues:
                 * upvalue_capture is a GC safe point, and ncl is otherwise
                 * unreachable. (Its NULL upvals are skipped by the tracer.) */
                PUSH(js_value_from_cell(&ncl->gc));
                if (flags & 1) { /* arrow: capture lexical this */
                    ncl->has_this = true;
                    ncl->this_val = cl->has_this ? cl->this_val : S[base - 1];
                }
                for (uint16_t i = 0; i < proto->n_upvals; i++) {
                    if (proto->upvals[i].from_local) {
                        JsUpvalue *uv = upvalue_capture(vm, fb, base + proto->upvals[i].idx);
                        if (!uv)
                            RT_THROW("out of memory");
                        ncl->upvals[i] = uv;
                    } else {
                        ncl->upvals[i] = cl->upvals[proto->upvals[i].idx];
                    }
                }
                break;
            }
            case JS_OP_CALL:
            case JS_OP_CALL_VARARGS: {
                uint32_t argc;
                if (op == JS_OP_CALL) {
                    argc = code[ip++];
                } else {
                    JsValue arrv = PEEK(0);
                    /* the args array is compiler-built; guard for hostile
                     * bytecode that reaches CALL_VARARGS without one */
                    if (!is_array(arrv))
                        RT_THROW("internal error: varargs call on non-array");
                    JsObject *arr = js_value_object(arrv);
                    argc = arr->elem_count;
                    if (argc > JS_MAX_CALL_ARGS)
                        RT_THROW("RangeError: too many function arguments");
                    fb->sp--; /* drop the array */
                    if (!stack_ensure(vm, fb, fb->sp + argc + 8))
                        RT_THROW("out of memory");
                    for (uint32_t i = 0; i < argc; i++)
                        S[fb->sp++] = arr->elems[i];
                }
                uint32_t nb = fb->sp - argc; /* new base: callee=nb-2, this=nb-1 */
                JsValue callee = S[nb - 2];
                if (js_is_function(callee)) {
                    JsGcCell *cc = js_value_cell(callee);
                    if (cc->kind == JS_KIND_NATIVE) {
                        JsNative *nf = (JsNative *)cc;
                        fr->ip = ip;
                        JsValue res;
                        bool ok = call_native(ctx, fb, nf, S[nb - 1], &S[nb], (int)argc, false, &res);
                        fb->sp = nb - 2;
                        if (!ok) {
                            fb->error = res;
                            fb->failed = true;
                            RT_RAISE();
                        }
                        PUSH(res);
                        break;
                    }
                    if (cc->kind == JS_KIND_CLOSURE) {
                        JsClosure *callee_cl = (JsClosure *)cc;
                        fr->ip = ip;
                        if (callee_cl->fn->fn_flags & JS_FN_ASYNC) {
                            /* async call: run on its own fiber, return a
                             * promise, keep executing this frame */
                            JsValue promise;
                            if (!call_async(ctx, callee_cl, S[nb - 1], &S[nb],
                                            (int)argc, &promise))
                                RT_THROW("out of memory");
                            fb->sp = nb - 2;
                            PUSH(promise);
                            break;
                        }
                        int rc = frame_setup(ctx, fb, callee_cl, nb, argc, false);
                        if (rc == -1)
                            RT_THROW("out of memory");
                        if (rc == -2)
                            RT_THROW("RangeError: Maximum call stack size exceeded");
                        goto run;
                    }
                }
                fiber_throw_callee(ctx, fb, fn, op_ip, callee, " is not a function");
                RT_RAISE();
            }
            case JS_OP_NEW:
            case JS_OP_NEW_VARARGS: {
                uint32_t argc;
                if (op == JS_OP_NEW) {
                    argc = code[ip++];
                } else {
                    JsValue arrv = PEEK(0);
                    if (!is_array(arrv))
                        RT_THROW("internal error: varargs new on non-array");
                    JsObject *arr = js_value_object(arrv);
                    argc = arr->elem_count;
                    if (argc > JS_MAX_CALL_ARGS)
                        RT_THROW("RangeError: too many function arguments");
                    fb->sp--; /* drop the array */
                    if (!stack_ensure(vm, fb, fb->sp + argc + 8))
                        RT_THROW("out of memory");
                    for (uint32_t i = 0; i < argc; i++)
                        S[fb->sp++] = arr->elems[i];
                }
                uint32_t nb = fb->sp - argc; /* callee=nb-2, this(placeholder)=nb-1 */
                JsValue callee = S[nb - 2];
                /* `new` on a function from bind() constructs its target with the preset arguments
                 * first; the bound this is ignored, as the language specifies. Bound twice: unwrap
                 * again (each layer's presets go in front of the next one's). */
                while (js_is_function(callee) && js_value_cell(callee)->kind == JS_KIND_NATIVE &&
                       ((JsNative *)js_value_cell(callee))->is_bound &&
                       ((JsNative *)js_value_cell(callee))->bound_fn == js_bind_call) {
                    JsObject *b = js_value_object(((JsNative *)js_value_cell(callee))->bound);
                    uint32_t extra = b->elem_count - 2;
                    if (argc + extra > JS_MAX_CALL_ARGS)
                        RT_THROW("RangeError: too many function arguments");
                    if (!stack_ensure(vm, fb, fb->sp + extra + 8))
                        RT_THROW("out of memory");
                    for (uint32_t i = argc; i-- > 0;)
                        S[nb + extra + i] = S[nb + i];
                    for (uint32_t i = 0; i < extra; i++)
                        S[nb + i] = b->elems[2 + i];
                    fb->sp += extra;
                    argc += extra;
                    callee = b->elems[0];
                    S[nb - 2] = callee;
                }
                if (!js_is_function(callee))
                    { fiber_throw_callee(ctx, fb, fn, op_ip, callee, " is not a constructor"); RT_RAISE(); }
                JsGcCell *cc = js_value_cell(callee);
                if (cc->kind == JS_KIND_NATIVE) {
                    if (((JsNative *)cc)->ctor == JS_CTOR_NONE)
                        { fiber_throw_callee(ctx, fb, fn, op_ip, callee, " is not a constructor"); RT_RAISE(); }
                    fr->ip = ip;
                    JsValue res;
                    /* A builtin makes its own object: called straight from here. (Routing it
                     * through construct_native too cost 15-20% on every benchmark, the loop's
                     * register allocation being sensitive to its call sites.) */
                    bool ok = ((JsNative *)cc)->ctor == JS_CTOR_ORDINARY
                        ? construct_native(ctx, fb, (JsNative *)cc, nb, argc, &res)
                        : call_native(ctx, fb, (JsNative *)cc, S[nb - 1], &S[nb], (int)argc, true, &res);
                    fb->sp = nb - 2;
                    if (!ok) {
                        fb->error = res;
                        fb->failed = true;
                        RT_RAISE();
                    }
                    PUSH(res);
                    break;
                }
                if (cc->kind != JS_KIND_CLOSURE)
                    { fiber_throw_callee(ctx, fb, fn, op_ip, callee, " is not a constructor"); RT_RAISE(); }
                JsClosure *ctor = (JsClosure *)cc;
                if (ctor->fn->fn_flags & (JS_FN_ARROW | JS_FN_ASYNC | JS_FN_METHOD))
                    { fiber_throw_callee(ctx, fb, fn, op_ip, callee, " is not a constructor"); RT_RAISE(); }
                fr->ip = ip; /* GC-safe point before allocations below */
                JsValue instv = construct_this(ctx, cc);
                if (!js_is_object(instv))
                    RT_THROW("out of memory");
                S[nb - 1] = instv; /* this */
                int rc = frame_setup(ctx, fb, ctor, nb, argc, true);
                if (rc == -1)
                    RT_THROW("out of memory");
                if (rc == -2)
                    RT_THROW("RangeError: Maximum call stack size exceeded");
                goto run;
            }
            case JS_OP_TRY_PUSH: {
                uint32_t target = READ_U32();
                if (fb->try_count == fb->try_cap) {
                    uint32_t ncap = fb->try_cap ? fb->try_cap * 2 : 8;
                    JsTryEntry *nt = js_realloc_raw(vm, fb->trys,
                                                    (size_t)fb->try_cap * sizeof(JsTryEntry),
                                                    (size_t)ncap * sizeof(JsTryEntry));
                    if (!nt)
                        RT_THROW("out of memory");
                    fb->trys = nt;
                    fb->try_cap = ncap;
                }
                fb->trys[fb->try_count].frame_count = fb->frame_count;
                fb->trys[fb->try_count].sp = fb->sp;
                fb->trys[fb->try_count].target = target;
                fb->try_count++;
                break;
            }
            case JS_OP_TRY_POP:
                /* The verifier proves try-stack balance, so this can only fire
                 * on a bug in it — but the cost of being wrong is an unsigned
                 * underflow that turns fb->trys[] into a wild pointer whose
                 * contents become `sp` and `ip`, so it is checked anyway. */
                if (fb->try_count == 0)
                    RT_THROW("internal error: try stack underflow");
                fb->try_count--;
                break;
            case JS_OP_GOSUB: {
                uint32_t target = READ_U32();
                if (!stack_ensure(vm, fb, fb->sp + 2))
                    RT_THROW("out of memory");
                PUSH(js_number((double)ip)); /* return address */
                ip = target;
                break;
            }
            case JS_OP_RET_SUB: {
                /* The popped value is the return address a matching GOSUB
                 * pushed. The loader's structural verifier proves stack
                 * *shape* around GOSUB/RET_SUB's subroutine polymorphism,
                 * not that this runtime value is actually a legitimate
                 * previously-pushed ip — so both the cast and the jump
                 * target need guarding, unlike GOSUB's own target (a static
                 * bytecode field, already validated at load time).
                 *
                 * "On an instruction boundary" is not enough. It admits a jump
                 * to any instruction at any operand depth — including code the
                 * verifier's fixpoint never reached, which was therefore never
                 * depth-checked and never counted towards max_stack. Landing
                 * high also leaves stale JsValues below the new top: slots the
                 * GC stopped tracing at the old sp, so they may name cells it
                 * has already freed. Both are avoided by demanding the pair the
                 * verifier actually proved — a GOSUB resume point, reached at
                 * one of the depths recorded for it. */
                uint32_t ret;
                if (!value_to_index(POPV(), &ret) || ret >= fn->code_len)
                    RT_THROW("internal error: bad subroutine return address");
                if (fn->insn_boundary) {
                    if (!(fn->insn_boundary[ret] & JSBC_B_GOSUB_RET))
                        RT_THROW("internal error: bad subroutine return address");
                    uint32_t depth = fb->sp - base - fn->n_locals;
                    bool ok_depth = false;
                    for (uint32_t i = 0; i < fn->gosub_ret_count; i++)
                        if (fn->gosub_rets[2 * i] == ret &&
                            fn->gosub_rets[2 * i + 1] == depth) {
                            ok_depth = true;
                            break;
                        }
                    if (!ok_depth)
                        RT_THROW("internal error: bad subroutine return depth");
                }
                ip = ret;
                break;
            }
            case JS_OP_DYNAMIC_IMPORT: {
                /* Kick off (or join) a module load and push its promise —
                 * shaped like a native returning js_promise_new(), NOT like
                 * AWAIT: the current fiber keeps running. */
                fr->ip = ip;
                fb->err_pos = lookup_pos(fn, op_ip);
                JsValue promise = js_module_dynamic_import(ctx, fn->module, PEEK(0));
                if (!js_is_promise(promise))
                    RT_THROW("out of memory");
                fb->stack[fb->sp - 1] = promise; /* replace the specifier */
                break;
            }
            case JS_OP_AWAIT: {
                /* Leave the awaited value on the stack (keeps it rooted);
                 * resume replaces it with the settled value or throws. */
                JsValue awaited = PEEK(0);
                fr->ip = ip; /* resume just after AWAIT */
                fb->err_pos = lookup_pos(fn, op_ip);
                JsPromise *pr;
                if (js_is_promise(awaited)) {
                    pr = js_value_promise(awaited);
                } else {
                    pr = js_promise_alloc(ctx);
                    if (!pr)
                        RT_THROW("out of memory");
                    js_promise_fulfill(ctx, pr, awaited);
                }
                if (!js_promise_await(ctx, pr, fb))
                    RT_THROW("out of memory");
                return JS_RUN_SUSPENDED;
            }
            case JS_OP_GET_EXPORT: {
                JsString *name = js_value_string(K[READ_U16()]);
                bool found;
                JsValue v = js_map_get(&fn->module->exports->props, name, &found);
                if (v.bits == JS_SPECIAL_TDZ) {
                    fiber_throw_tdz_binding(ctx, fb, fn, op_ip, name);
                    RT_RAISE();
                }
                PUSH(found ? v : js_undefined());
                break;
            }
            case JS_OP_SET_EXPORT:
            case JS_OP_INIT_EXPORT: {
                JsString *name = js_value_string(K[READ_U16()]);
                if (op == JS_OP_SET_EXPORT) {
                    bool found;
                    JsValue old = js_map_get(&fn->module->exports->props, name, &found);
                    if (old.bits == JS_SPECIAL_TDZ) {
                        fiber_throw_tdz_binding(ctx, fb, fn, op_ip, name);
                        RT_RAISE();
                    }
                }
                if (!js_map_set(vm, &fn->module->exports->props, name, PEEK(0)))
                    RT_THROW("out of memory");
                break;
            }
            case JS_OP_SET_FUNCTION_NAME: {
                /* the compiler emits this only after an anonymous function definition, so the
                 * value is a fresh closure with no name of its own */
                JsValue f = PEEK(0);
                if (js_is_function(f) && js_value_cell(f)->kind == JS_KIND_CLOSURE) {
                    /* a computed key converts here, once: DEFINE_PROP then finds the string */
                    JsString *nm = js_op_to_string(ctx, PEEK(1));
                    if (!nm)
                        RT_THROW("out of memory");
                    PEEK(1) = js_value_from_cell(&nm->gc);
                    ((JsClosure *)js_value_cell(PEEK(0)))->name = nm;
                }
                break;
            }
            case JS_OP_MODULE_READY:
                /* the loader is instantiating this body: its functions exist, so park until
                 * every module in the graph has reached this point */
                if (fb->stop_at_ready && fb->frame_count == 1) {
                    fb->stop_at_ready = false;
                    fr->ip = ip;
                    return JS_RUN_READY;
                }
                break;
            case JS_OP_GET_IMPORT: {
                JsModuleImport *imp = &fn->module->imports[READ_U16()];
                /* imported_name is legitimately NULL for a side-effect-only or
                 * namespace import, which GET_IMPORT is never emitted against
                 * — but a tampered module buffer can pair the two, and
                 * js_map_get dereferences the key to hash it. The verifier
                 * bounds the import INDEX; the record's shape is checked here. */
                if (!imp->source || !imp->source->exports || !imp->imported_name)
                    RT_THROW("ReferenceError: module binding is not available");
                bool found;
                JsValue v = js_map_get(&imp->source->exports->props, imp->imported_name,
                                       &found);
                if (v.bits == JS_SPECIAL_TDZ) {
                    fiber_throw_tdz_binding(ctx, fb, fn, op_ip, imp->imported_name);
                    RT_RAISE();
                }
                PUSH(found ? v : js_undefined());
                break;
            }
            case JS_OP_IMPORT_NS: {
                JsModuleImport *imp = &fn->module->imports[READ_U16()];
                if (!imp->source || !imp->source->exports)
                    RT_THROW("ReferenceError: module namespace is not available");
                PUSH(js_value_from_cell(&imp->source->exports->gc));
                break;
            }
            case JS_OP_THROW_CONST_ASSIGN:
                RT_THROW("TypeError: Assignment to constant variable.");
            case JS_OP_THROW:
                fb->error = POPV();
                fb->failed = true;
                RT_RAISE();
            case JS_OP_RETURN: {
                JsValue ret = POPV();
                /* `new`: a return value that is not an object (functions and promises are)
                 * is discarded in favor of the constructed `this` (still live at base-1). */
                if (fr->is_construct && !is_object_type(ret))
                    ret = S[base - 1];
                upvalues_close_from(fb, base);
                /* drop try entries owned by this frame */
                while (fb->try_count && fb->trys[fb->try_count - 1].frame_count >= fb->frame_count)
                    fb->try_count--;
                fb->frame_count--;
                if (fb->frame_count == 0) {
                    *result = ret;
                    return JS_RUN_DONE;
                }
                S[base - 2] = ret;
                fb->sp = base - 1;
                goto run;
            }
            default:
                RT_THROW("internal error: bad opcode");
            }
        }
    throw_pending: /* RT_THROW_PENDING: only from an opcode without operands */
        fb->error = js_take_error(ctx);
        fb->failed = true;
        fb->err_pos = lookup_pos(fn, ip - 1);
        fb->frames[fb->frame_count - 1].ip = ip;
        goto on_error;
    }

on_error:
    /* Before any frame is popped: where the exception was raised, for
     * js_context_error_stack. A resume-with-rejection arrives here with the
     * top frame's ip just past its AWAIT, which reads the same way. */
    record_raise(ctx, fb);
    while (fb->try_count > 0) {
        JsTryEntry t = fb->trys[fb->try_count - 1];
        while (fb->frame_count > t.frame_count) {
            uint32_t b = fb->frames[fb->frame_count - 1].base;
            upvalues_close_from(fb, b);
            while (fb->try_count &&
                   fb->trys[fb->try_count - 1].frame_count >= fb->frame_count)
                fb->try_count--;
            fb->frame_count--;
        }
        /* pop the handler entry itself */
        fb->try_count--;
        if (!stack_ensure(vm, fb, t.sp + 1)) {
            /* cannot even install the handler; propagate */
            continue;
        }
        fb->sp = t.sp;
        S[fb->sp++] = fb->error;
        fb->failed = false;
        fb->frames[fb->frame_count - 1].ip = t.target;
        goto run;
    }
    *result = fb->error;
    ctx->error_pos = fb->err_pos;
    /* The same error value leaving a second fiber — an awaiting fiber resumed
     * with the rejection — extends the trace outward instead of replacing it,
     * so the frames read: where it was thrown, then who was waiting. */
    if (ctx->error_trace_count > 0 && js_same_value(ctx->error_trace_value, fb->error) &&
        !js_is_undefined(fb->error)) {
        uint32_t room = JS_TRACE_MAX - ctx->error_trace_count;
        uint32_t add = fb->trace_count < room ? fb->trace_count : room;
        memcpy(ctx->error_trace + ctx->error_trace_count, fb->trace, add * sizeof fb->trace[0]);
        ctx->error_trace_count += add;
    } else {
        ctx->error_trace_count = fb->trace_count;
        memcpy(ctx->error_trace, fb->trace, fb->trace_count * sizeof fb->trace[0]);
    }
    ctx->error_trace_value = fb->error;
    return JS_RUN_ERROR;

#undef S
#undef PUSH
#undef POPV
#undef PEEK
#undef READ_U16
#undef READ_U32
#undef RT_THROW
#undef RT_RAISE
}

/* ---- fiber entry ---- */

/* The fuel a fiber starts (or restarts) with: the caller's remainder, as spawn_fiber explains. */
static uint64_t fiber_start_fuel(JsContext *ctx) {
    if (ctx->fiber)
        return ctx->fiber->fuel;
    if (!ctx->fuel)
        return UINT64_MAX;
    return ctx->fuel_left ? ctx->fuel_left : 1;
}

/* Allocates a fiber and sets up frame 0 for a call; NULL + *err set on OOM. */
static JsFiber *spawn_fiber(JsContext *ctx, JsClosure *cl, JsValue this_val,
                            const JsValue *args, int argc, const char **err) {
    JsVm *vm = ctx->vm;
    *err = NULL;

    /* Root cl/this across the fiber allocation (a GC safe point); args are
     * kept alive by the caller's fiber, which is still ctx->fiber here. */
    JsValue cl_val = js_value_from_cell(&cl->gc);
    js_gc_protect(vm, &cl_val);
    js_gc_protect(vm, &this_val);
    JsGcCell *fc = js_gc_new_cell(vm, JS_KIND_FIBER, sizeof(JsFiber));
    js_gc_unprotect(vm, &this_val);
    js_gc_unprotect(vm, &cl_val);
    if (!fc) {
        *err = "out of memory";
        return NULL;
    }
    cl = (JsClosure *)js_value_cell(cl_val);
    JsFiber *fb = (JsFiber *)fc;
    memset((char *)fb + sizeof(JsGcCell), 0, sizeof *fb - sizeof(JsGcCell));
    fb->error = js_undefined();
    /* Fuel is a shared budget, not a per-fiber allowance: a fiber spawned by a
     * native re-entering the interpreter inherits the caller's *remaining*
     * fuel so a script cannot refill the budget by making nested calls.
     *
     * A top-level fiber (no current fiber) draws from ctx->fuel_left rather
     * than ctx->fuel, because every microtask the job queue runs arrives here
     * as a top-level fiber too. Charging those to ctx->fuel would hand each one
     * a full tank, and `function f(){ Promise.resolve().then(f); }` would then
     * run forever under any budget. A remainder of 0 means the turn is spent:
     * start at 1 so the very first instruction throws (0 would underflow into
     * an unlimited budget). */
    fb->fuel = fiber_start_fuel(ctx);

    /*
     * Root the new fiber for the rest of setup. Until the caller installs it as
     * ctx->fiber it is reachable from nothing, and the work below allocates:
     * stack_ensure can collect once a heap_limit is set, and frame_setup builds
     * the rest-parameter array outright. Collecting here frees the fiber whose
     * stack frame_setup is in the middle of filling in — `async function
     * f(...args) {}` was enough to turn that into a use-after-free.
     */
    JsValue fb_val = js_value_from_cell(fc);
    js_gc_protect(vm, &fb_val);
    JsFiber *result = NULL;
    if (!stack_ensure(vm, fb, (uint32_t)(2 + argc) + cl->fn->n_locals +
                                  cl->fn->max_stack + 8)) {
        *err = "out of memory";
        goto done;
    }
    fb->stack[0] = js_value_from_cell(&cl->gc);
    fb->stack[1] = this_val;
    for (int i = 0; i < argc; i++)
        fb->stack[2 + i] = args[i];
    fb->sp = (uint32_t)(2 + argc);
    int rc = frame_setup(ctx, fb, cl, 2, (uint32_t)argc, false);
    if (rc != 0) {
        *err = rc == -2 ? "RangeError: Maximum call stack size exceeded" : "out of memory";
        goto done;
    }
    result = fb;
done:
    js_gc_unprotect(vm, &fb_val);
    return result;
}

/*
 * Runs (or resumes) a fiber to its next yield point. Manages ctx->fiber and
 * settles the fiber's result promise when it finishes.
 */
static JsRunStatus advance_fiber(JsContext *ctx, JsFiber *fb, JsValue *out) {
    JsFiber *saved = ctx->fiber;
    fb->caller = saved;
    ctx->fiber = fb; /* roots fb across any allocation below */
    JsRunStatus st;
    /* Bound nested interpreter entries so native-mediated recursion (a native
     * calling back into JS) cannot exhaust the C stack — the per-fiber frame
     * limit can't see across the fresh fibers each re-entry spawns. */
    if (ctx->reentry_depth >= JS_MAX_REENTRY) {
        fb->error = js_error_from_ascii(ctx, "RangeError: Maximum call stack size exceeded");
        fb->failed = true;
        *out = fb->error;
        st = JS_RUN_ERROR;
    } else {
        ctx->reentry_depth++;
        st = run_fiber(ctx, fb, out);
        ctx->reentry_depth--;
    }
    ctx->fiber = saved;
    /* Return the fiber's remaining fuel to its caller so the shared budget
     * keeps decreasing across the whole call tree — or, at top level, back to
     * the context, so the microtasks this run queued inherit what is left
     * instead of restarting from the full budget. */
    if (saved)
        saved->fuel = fb->fuel;
    else if (ctx->fuel)
        ctx->fuel_left = fb->fuel;
    if (fb->result_promise) {
        if (st == JS_RUN_DONE)
            js_promise_resolve_with(ctx, fb->result_promise, *out);
        else if (st == JS_RUN_ERROR)
            js_promise_reject(ctx, fb->result_promise, fb->error);
    }
    return st;
}

static bool call_async(JsContext *ctx, JsClosure *cl, JsValue this_val,
                       const JsValue *args, int argc, JsValue *out) {
    JsVm *vm = ctx->vm;
    JsPromise *p = js_promise_alloc(ctx);
    if (!p)
        return false;
    JsValue pv = js_value_from_cell(&p->gc);
    js_gc_protect(vm, &pv); /* keep the result promise alive across spawn/run */
    const char *err;
    JsFiber *fb = spawn_fiber(ctx, cl, this_val, args, argc, &err);
    if (!fb) {
        js_gc_unprotect(vm, &pv);
        return false;
    }
    fb->result_promise = js_value_promise(pv);
    fb->is_async = true;
    JsValue tmp;
    advance_fiber(ctx, fb, &tmp); /* settles pv if it completes now; else parks */
    js_gc_unprotect(vm, &pv);
    *out = pv;
    return true;
}

void js_resume_fiber(JsContext *ctx, JsFiber *fb, JsValue value, bool is_throw) {
    if (is_throw) {
        fb->sp--; /* drop the awaited operand left on the stack by AWAIT */
        fb->error = value;
        fb->failed = true;
    } else {
        fb->stack[fb->sp - 1] = value; /* replace operand with the settled value */
    }
    JsValue tmp;
    advance_fiber(ctx, fb, &tmp);
}

/* Synchronous call: cl must not be async. Returns false with *result=error. */
static bool run_closure(JsContext *ctx, JsClosure *cl, JsValue this_val,
                        const JsValue *args, int argc, JsValue *result) {
    *result = js_undefined();
    const char *err;
    JsFiber *fb = spawn_fiber(ctx, cl, this_val, args, argc, &err);
    if (!fb) {
        JsString *s = js_ascii_cell(ctx->vm, err);
        *result = s ? js_value_from_cell(&s->gc) : js_undefined();
        return false;
    }
    JsRunStatus st = advance_fiber(ctx, fb, result);
    return st == JS_RUN_DONE;
}

bool js_interp_run(JsContext *ctx, JsFunctionCell *fn, JsValue *result) {
    JsClosure *cl = closure_new(ctx->vm, fn);
    if (!cl) {
        *result = js_undefined();
        return false;
    }
    return run_closure(ctx, cl, js_undefined(), NULL, 0, result);
}

bool js_call(JsContext *ctx, JsValue fn, JsValue this_val, const JsValue *args,
             int argc, JsValue *result) {
    if (!js_is_function(fn)) {
        JsString *s = js_ascii_cell(ctx->vm, "TypeError: value is not a function");
        *result = s ? js_value_from_cell(&s->gc) : js_undefined();
        return false;
    }
    JsGcCell *c = js_value_cell(fn);
    if (c->kind == JS_KIND_NATIVE) {
        JsNative *nf = (JsNative *)c;
        return call_native(ctx, ctx->fiber, nf, this_val, args, argc, false, result);
    }
    JsClosure *cl;
    if (c->kind == JS_KIND_CLOSURE) {
        cl = (JsClosure *)c;
    } else {
        cl = closure_new(ctx->vm, (JsFunctionCell *)c); /* bare prototype: wrap */
        if (!cl) {
            *result = js_undefined();
            return false;
        }
    }
    /* Calling an async function yields a promise, not a synchronous result. */
    if (cl->fn->fn_flags & JS_FN_ASYNC)
        return call_async(ctx, cl, this_val, args, argc, result);
    return run_closure(ctx, cl, this_val, args, argc, result);
}

/* ---- public API ---- */

/* js_compile_module / js_compile_module_repl are the FRONTEND's entry points
 * and live in src/frontend/js_compile_api.c — they were only ever here because
 * this file happened to own the public-API banner. Everything below runs
 * already-compiled code and needs no parser. */

JsFiber *js_module_instantiate_body(JsContext *ctx, JsFunctionCell *body, JsValue *error) {
    JsVm *vm = ctx->vm;
    *error = js_undefined();
    JsClosure *cl = closure_new(vm, body);
    JsFiber *fb = NULL;
    const char *err = "out of memory";
    if (cl)
        fb = spawn_fiber(ctx, cl, js_undefined(), NULL, 0, &err);
    if (!fb) {
        JsString *s = js_ascii_cell(vm, err);
        *error = s ? js_value_from_cell(&s->gc) : js_undefined();
        return NULL;
    }
    fb->is_async = (body->fn_flags & JS_FN_ASYNC) != 0; /* top-level await */
    fb->stop_at_ready = true;
    JsValue fbv = js_value_from_cell(&fb->gc);
    js_gc_protect(vm, &fbv);
    JsValue out;
    JsRunStatus st = advance_fiber(ctx, fb, &out);
    js_gc_unprotect(vm, &fbv);
    fb->caller = NULL; /* parked: nothing to return to until evaluation resumes it */
    if (st == JS_RUN_READY)
        return fb;
    /* the prologue only makes functions, so this is OOM or the frame limit; a body compiled
     * before JS_OP_MODULE_READY existed cannot load (the bytecode version is checked) */
    *error = st == JS_RUN_ERROR ? fb->error : js_undefined();
    if (st != JS_RUN_ERROR) {
        JsString *s = js_ascii_cell(vm, "internal error: module body ran past instantiation");
        *error = s ? js_value_from_cell(&s->gc) : js_undefined();
    }
    return NULL;
}

JsValue js_module_finish_body(JsContext *ctx, JsFiber *fb) {
    JsVm *vm = ctx->vm;
    JsValue fbv = js_value_from_cell(&fb->gc);
    js_gc_protect(vm, &fbv);
    JsPromise *p = js_promise_alloc(ctx);
    if (!p) {
        js_gc_unprotect(vm, &fbv);
        return js_undefined();
    }
    JsValue pv = js_value_from_cell(&p->gc);
    js_gc_protect(vm, &pv);
    /* the time since it parked belongs to whoever runs it now */
    fb->fuel = fiber_start_fuel(ctx);
    JsValue out;
    if (fb->is_async) {
        /* TLA module: as js_run_module — the promise may still be pending, the suspended fiber
         * kept alive by its AWAIT reaction */
        fb->result_promise = p;
        advance_fiber(ctx, fb, &out);
        js_run_jobs(ctx);
    } else {
        /* sync module: run to completion, drain the microtasks it scheduled, settle */
        JsRunStatus st = advance_fiber(ctx, fb, &out);
        js_gc_protect(vm, &out);
        js_run_jobs(ctx);
        js_gc_unprotect(vm, &out);
        if (st == JS_RUN_DONE)
            js_promise_fulfill(ctx, js_value_promise(pv), out);
        else
            js_promise_reject(ctx, js_value_promise(pv), fb->error);
    }
    JsValue result = pv;
    js_gc_unprotect(vm, &pv);
    js_gc_unprotect(vm, &fbv);
    return result;
}

JsValue js_run_module(JsContext *ctx, JsValue fnv) {
    JsVm *vm = ctx->vm;
    if (!js_is_function(fnv))
        return js_undefined();
    JsFunctionCell *fn = js_value_function(fnv);
    JsClosure *cl = closure_new(vm, fn);
    if (!cl)
        return js_undefined();
    bool is_async = (fn->fn_flags & JS_FN_ASYNC) != 0; /* top-level await */

    if (!is_async) {
        /* Sync module: run to completion, drain microtasks it scheduled
         * (e.g. Promise.resolve().then(...) at top level), and wrap the
         * outcome in an already-settled promise. */
        JsValue result;
        bool ok = run_closure(ctx, cl, js_undefined(), NULL, 0, &result);
        js_run_jobs(ctx);
        js_gc_protect(vm, &result);
        JsPromise *p = js_promise_alloc(ctx);
        js_gc_unprotect(vm, &result);
        if (!p)
            return js_undefined();
        if (ok)
            js_promise_fulfill(ctx, p, result);
        else
            js_promise_reject(ctx, p, result);
        return js_value_from_cell(&p->gc);
    }

    /* TLA module: drive as an async fiber, drain the jobs available now, and
     * return the result promise in whatever state it reached — a promise
     * still pending here is genuinely waiting on the host, which keeps the
     * suspended fiber alive through this promise's AWAIT reaction and
     * settles it on a later js_run_jobs() turn. */
    JsValue clv = js_value_from_cell(&cl->gc);
    js_gc_protect(vm, &clv); /* root cl before the promise allocation GCs */
    JsPromise *p = js_promise_alloc(ctx);
    if (!p) {
        js_gc_unprotect(vm, &clv);
        return js_undefined();
    }
    JsValue pv = js_value_from_cell(&p->gc);
    js_gc_protect(vm, &pv);
    const char *err;
    JsFiber *fb = spawn_fiber(ctx, (JsClosure *)js_value_cell(clv), js_undefined(),
                              NULL, 0, &err);
    if (!fb) {
        js_gc_unprotect(vm, &clv);
        JsString *s = js_ascii_cell(vm, err);
        js_promise_reject(ctx, js_value_promise(pv),
                          s ? js_value_from_cell(&s->gc) : js_undefined());
        JsValue out = pv;
        js_gc_unprotect(vm, &pv);
        return out;
    }
    fb->result_promise = js_value_promise(pv);
    fb->is_async = true;
    JsValue tmp;
    advance_fiber(ctx, fb, &tmp);
    js_run_jobs(ctx);
    js_gc_unprotect(vm, &clv);
    JsValue out = pv;
    js_gc_unprotect(vm, &pv);
    return out;
}

JsValue js_to_string(JsContext *ctx, JsValue v) {
    JsString *s = js_op_to_string(ctx, v);
    if (!s)
        ctx->pending_error = js_undefined(); /* the host asked for display: drop what it threw */
    return s ? js_value_from_cell(&s->gc) : js_undefined();
}

/* ---- builtin lengths ---- */

#include "js_builtin_lengths.h"

/* The object a builtin's owner names: a global, a global's static, or a prototype — the method
 * table a primitive's methods come from, or the constructor's real prototype object. */
static bool lengths_owner(JsContext *ctx, const char *owner, JsValue *out) {
    *out = js_value_from_cell(&ctx->globals->gc);
    if (!*owner)
        return true;
    static const struct { const char *name; size_t off; } tables[] = {
        {"String.prototype", offsetof(JsContext, string_methods)},
        {"Number.prototype", offsetof(JsContext, number_methods)},
        {"Boolean.prototype", offsetof(JsContext, boolean_methods)},
        {"Function.prototype", offsetof(JsContext, function_methods)},
        {"Promise.prototype", offsetof(JsContext, promise_methods)},
        {"Object.prototype", offsetof(JsContext, object_proto)},
    };
    for (size_t i = 0; i < sizeof tables / sizeof tables[0]; i++) {
        if (strcmp(owner, tables[i].name) == 0) {
            JsObject *o = *(JsObject **)((char *)ctx + tables[i].off);
            *out = o ? js_value_from_cell(&o->gc) : js_undefined();
            return true;
        }
    }
    const char *dot = strchr(owner, '.');
    size_t n = dot ? (size_t)(dot - owner) : strlen(owner);
    uint16_t u[32];
    if (n > 32)
        return true;
    for (size_t i = 0; i < n; i++)
        u[i] = (uint16_t)owner[i];
    JsValue k = js_atom(ctx->vm, u, n);
    if (!js_is_string(k))
        return false;
    if (get_property(ctx, *out, k, out) == JS_PROP_OOM)
        return false;
    if (dot) {
        JsValue pk = js_atom(ctx->vm, (const uint16_t[]){'p','r','o','t','o','t','y','p','e'}, 9);
        if (!js_is_string(pk) || get_property(ctx, *out, pk, out) == JS_PROP_OOM)
            return false;
    }
    return true;
}

bool js_builtin_lengths_apply(JsContext *ctx) {
    for (size_t i = 0; i < sizeof js_builtin_lengths / sizeof js_builtin_lengths[0]; i++) {
        const JsBuiltinLength *e = &js_builtin_lengths[i];
        JsValue owner;
        if (!lengths_owner(ctx, e->owner, &owner))
            return false;
        if (js_is_undefined(owner))
            continue;
        uint16_t u[32];
        size_t n = strlen(e->name);
        if (n > 32)
            continue;
        for (size_t j = 0; j < n; j++)
            u[j] = (uint16_t)e->name[j];
        JsValue k = js_atom(ctx->vm, u, n);
        if (!js_is_string(k))
            return false;
        JsValue f;
        if (get_property(ctx, owner, k, &f) == JS_PROP_OOM)
            return false;
        if (js_is_function(f) && js_value_cell(f)->kind == JS_KIND_NATIVE)
            ((JsNative *)js_value_cell(f))->length = e->length;
    }
    return true;
}

/* What a constructor inherits from the one that is its [[Prototype]] (TypeError from Error): that
 * one's own properties and statics, up its chain, then Function.prototype's and Object.prototype's. Out of line: it is
 * the cold end of a function property read. */
__attribute__((noinline)) static JsPropStatus parent_ctor_lookup(JsContext *ctx, JsNative *p, JsValue key,
                                                               JsValue *out) {
    for (int depth = 0; p && depth < JS_PROTO_CHAIN_LIMIT; p = p->parent, depth++) {
        if (p->props) {
            JsPropStatus st = method_lookup(ctx, p->props, key, out);
            if (st != JS_PROP_OK || !js_is_undefined(*out))
                return st;
        }
        if (p->statics) {
            JsPropStatus st = method_lookup(ctx, p->statics, key, out);
            if (st != JS_PROP_OK || !js_is_undefined(*out))
                return st;
        }
    }
    return proto_table_lookup(ctx, ctx->function_methods, key, out);
}
