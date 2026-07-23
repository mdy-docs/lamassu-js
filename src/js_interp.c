#include "js_bytecode.h"
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
 * - ToPrimitive(object) is ToString: arrays join with ',', others are
 *   "[object Object]".
 * - % is a - trunc(a/b)*b (may differ in the last ulp for extreme ratios).
 * - ** supports integer exponents and ±0.5 exactly; other fractional
 *   exponents yield NaN (TODO transcendental pow).
 * - delete on array elements writes undefined instead of making a hole.
 */

#define JS_MAX_CONCAT_UNITS (64u * 1024 * 1024)
#define JS_TOSTRING_MAX_DEPTH 8

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

JsString *js_to_string_cell(JsContext *ctx, JsValue v, int depth) {
    JsVm *vm = ctx->vm;
    if (js_is_string(v))
        return js_value_string(v);
    if (js_is_number(v))
        return js_number_to_string(vm, js_get_number(v));
    if (js_is_undefined(v))
        return js_ascii_cell(vm, "undefined");
    if (js_is_null(v))
        return js_ascii_cell(vm, "null");
    if (v.bits == JS_SPECIAL_TRUE)
        return js_ascii_cell(vm, "true");
    if (v.bits == JS_SPECIAL_FALSE)
        return js_ascii_cell(vm, "false");
    if (js_is_function(v))
        return js_ascii_cell(vm, "[function]");
    if (js_is_promise(v))
        return js_ascii_cell(vm, "[object Promise]");
    if (js_is_object(v)) {
        JsObject *o = js_value_object(v);
#ifdef LAMASSU_HAS_REGEX
        if (o->obj_kind == JS_OBJ_REGEXP)
            return js_regexp_repr(ctx, o);
#endif
        if (o->obj_kind == JS_OBJ_DATE)
            return js_date_repr(ctx, o);
        if (o->obj_kind == JS_OBJ_MAP)
            return js_ascii_cell(vm, "[object Map]");
        if (o->obj_kind == JS_OBJ_SET)
            return js_ascii_cell(vm, "[object Set]");
        if (o->obj_kind != JS_OBJ_ARRAY)
            return js_ascii_cell(vm, "[object Object]");
        if (depth >= JS_TOSTRING_MAX_DEPTH)
            return js_string_cell_new(vm, NULL, 0);
        /* Array.prototype.join(',') — undefined/null elements print empty. */
        uint16_t *buf = NULL;
        size_t cap = 0, len = 0;
        for (uint32_t i = 0; i < o->elem_count; i++) {
            JsString *part = NULL;
            JsValue el = o->elems[i];
            if (!is_nullish(el)) {
                part = js_to_string_cell(ctx, el, depth + 1);
                if (!part) {
                    js_realloc_raw(vm, buf, cap * sizeof(uint16_t), 0);
                    return NULL;
                }
            }
            size_t need = len + (part ? part->length : 0) + 1;
            if (need > JS_MAX_CONCAT_UNITS) {
                js_realloc_raw(vm, buf, cap * sizeof(uint16_t), 0);
                return NULL;
            }
            if (need > cap) {
                size_t ncap = cap ? cap * 2 : 64;
                while (ncap < need)
                    ncap *= 2;
                uint16_t *nb = js_realloc_raw(vm, buf, cap * sizeof(uint16_t),
                                              ncap * sizeof(uint16_t));
                if (!nb) {
                    js_realloc_raw(vm, buf, cap * sizeof(uint16_t), 0);
                    return NULL;
                }
                buf = nb;
                cap = ncap;
            }
            if (i)
                buf[len++] = ',';
            if (part) {
                memcpy(buf + len, part->units, (size_t)part->length * sizeof(uint16_t));
                len += part->length;
            }
        }
        JsString *s = js_string_cell_new(vm, buf, len);
        js_realloc_raw(vm, buf, cap * sizeof(uint16_t), 0);
        return s;
    }
    return js_ascii_cell(vm, "undefined");
}

double js_to_number_value(JsContext *ctx, JsValue v) {
    if (js_is_number(v))
        return js_get_number(v);
    if (js_is_undefined(v))
        return __builtin_nan("");
    if (js_is_null(v))
        return 0.0;
    if (v.bits == JS_SPECIAL_TRUE)
        return 1.0;
    if (v.bits == JS_SPECIAL_FALSE)
        return 0.0;
    if (js_is_string(v)) {
        JsString *s = js_value_string(v);
        return js_units_to_number(s->units, s->length);
    }
    /* object -> ToPrimitive (string) -> number */
    JsString *s = js_to_string_cell(ctx, v, 0);
    if (!s)
        return __builtin_nan("");
    return js_units_to_number(s->units, s->length);
}

bool js_strict_equals(JsValue a, JsValue b) {
    if (js_is_number(a) && js_is_number(b))
        return js_get_number(a) == js_get_number(b);
    if (js_is_string(a) && js_is_string(b))
        return js_string_equals(a, b);
    return a.bits == b.bits;
}

static bool loose_eq_values(JsContext *ctx, JsValue a, JsValue b) {
    for (int guard = 0; guard < 4; guard++) {
        if (is_nullish(a) || is_nullish(b))
            return is_nullish(a) && is_nullish(b);
        bool an = js_is_number(a), bn = js_is_number(b);
        bool as = js_is_string(a), bs = js_is_string(b);
        if (an && bn)
            return js_get_number(a) == js_get_number(b);
        if (as && bs)
            return js_string_equals(a, b);
        if (js_is_bool(a)) {
            a = js_number(js_get_bool(a) ? 1 : 0);
            continue;
        }
        if (js_is_bool(b)) {
            b = js_number(js_get_bool(b) ? 1 : 0);
            continue;
        }
        if (an && bs)
            return js_get_number(a) == js_to_number_value(ctx, b);
        if (as && bn)
            return js_to_number_value(ctx, a) == js_get_number(b);
        bool acell = js_is_object(a) || js_is_function(a) || js_is_promise(a);
        bool bcell = js_is_object(b) || js_is_function(b) || js_is_promise(b);
        if (acell && bcell)
            return a.bits == b.bits;
        if (acell && (bn || bs)) {
            JsString *s = js_to_string_cell(ctx, a, 0);
            if (!s)
                return false;
            a = js_value_from_cell(&s->gc);
            continue;
        }
        if (bcell && (an || as)) {
            JsString *s = js_to_string_cell(ctx, b, 0);
            if (!s)
                return false;
            b = js_value_from_cell(&s->gc);
            continue;
        }
        return false;
    }
    return false;
}

/* -1: a<b, 0: equal, 1: a>b, 2: unordered (NaN) */
static int js_compare_values(JsContext *ctx, JsValue a, JsValue b) {
    if (js_is_string(a) && js_is_string(b)) {
        JsString *x = js_value_string(a), *y = js_value_string(b);
        uint32_t n = x->length < y->length ? x->length : y->length;
        for (uint32_t i = 0; i < n; i++) {
            if (x->units[i] != y->units[i])
                return x->units[i] < y->units[i] ? -1 : 1;
        }
        return x->length == y->length ? 0 : (x->length < y->length ? -1 : 1);
    }
    double da = js_to_number_value(ctx, a);
    double db = js_to_number_value(ctx, b);
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
        s = js_to_string_cell(ctx, key, 0);
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

/* Lazily creates cl's `.prototype` object (the constructor-function pattern
 * `new` relies on). NULL on OOM. */
static JsObject *closure_prototype(JsContext *ctx, JsClosure *cl) {
    if (cl->prototype_obj)
        return cl->prototype_obj;
    JsValue pv = js_object_new(ctx);
    if (!js_is_object(pv))
        return NULL;
    cl->prototype_obj = js_value_object(pv); /* reachable via cl from here */
    if (!js_object_set_ascii(ctx, cl->prototype_obj, "constructor",
                             js_value_from_cell(&cl->gc))) {
        cl->prototype_obj = NULL;
        return NULL;
    }
    return cl->prototype_obj;
}

/* Same lazy-create-on-first-access shape as closure_prototype, for natives
 * that don't already have one set up eagerly (js_mapobj_builtins_init and
 * friends set RegExp/Date/Map/Set's up front, since instance allocation
 * needs it before any script could have touched `.prototype` — this path
 * only fires for some other native someone registered by hand). */
static JsObject *native_prototype(JsContext *ctx, JsNative *nf) {
    if (nf->prototype)
        return nf->prototype;
    JsValue pv = js_object_new(ctx);
    if (!js_is_object(pv))
        return NULL;
    nf->prototype = js_value_object(pv);
    if (!js_object_set_ascii(ctx, nf->prototype, "constructor", js_value_from_cell(&nf->gc))) {
        nf->prototype = NULL;
        return NULL;
    }
    return nf->prototype;
}

typedef enum { JS_PROP_OK, JS_PROP_OOM, JS_PROP_TYPE_ERROR } JsPropStatus;

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

static JsPropStatus get_property(JsContext *ctx, JsValue base, JsValue key,
                                 JsValue *out) {
    JsVm *vm = ctx->vm;
    *out = js_undefined();
    if (is_nullish(base))
        return JS_PROP_TYPE_ERROR;

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
        return method_lookup(ctx, ctx->string_methods, key, out);
    }
    if (js_is_object(base)) {
        JsObject *o = js_value_object(base);
#ifdef LAMASSU_HAS_REGEX
        if (o->obj_kind == JS_OBJ_REGEXP) {
            /* synthesized props (source, lastIndex, ...) shadow everything */
            bool handled = false;
            if (!js_regexp_prop_get(ctx, o, key, out, &handled))
                return JS_PROP_OOM;
            if (handled)
                return JS_PROP_OK;
        }
#endif
        if (o->obj_kind == JS_OBJ_MAP) {
            /* synthesized `size` shadows everything, like RegExp's props */
            bool handled = false;
            js_mapobj_prop_get(o, key, out, &handled);
            if (handled)
                return JS_PROP_OK;
        }
        if (o->obj_kind == JS_OBJ_SET) {
            bool handled = false;
            js_setobj_prop_get(o, key, out, &handled);
            if (handled)
                return JS_PROP_OK;
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
        return method_lookup(ctx, ctx->number_methods, key, out);
    if (js_is_promise(base))
        return method_lookup(ctx, ctx->promise_methods, key, out);
    if (js_is_function(base)) {
        JsGcCell *c = js_value_cell(base);
        bool is_proto_key = js_is_string(key) && units_is_ascii_str(js_value_string(key), "prototype");
        if (is_proto_key) {
            JsObject *proto = c->kind == JS_KIND_CLOSURE
                ? closure_prototype(ctx, (JsClosure *)c)
                : c->kind == JS_KIND_NATIVE ? native_prototype(ctx, (JsNative *)c) : NULL;
            if (proto) {
                *out = js_value_from_cell(&proto->gc);
                return JS_PROP_OK;
            }
            if (c->kind == JS_KIND_CLOSURE || c->kind == JS_KIND_NATIVE)
                return JS_PROP_OOM;
        }
        if (c->kind == JS_KIND_NATIVE && ((JsNative *)c)->statics)
            return method_lookup(ctx, ((JsNative *)c)->statics, key, out);
    }
    /* booleans: no properties */
    return JS_PROP_OK;
}

static JsPropStatus set_property(JsContext *ctx, JsValue base, JsValue key,
                                 JsValue val, const char **errmsg) {
    JsVm *vm = ctx->vm;
    if (is_nullish(base)) {
        *errmsg = "TypeError: cannot set properties of undefined or null";
        return JS_PROP_TYPE_ERROR;
    }
    if (js_is_function(base)) {
        JsGcCell *c = js_value_cell(base);
        if ((c->kind == JS_KIND_CLOSURE || c->kind == JS_KIND_NATIVE) && js_is_string(key) &&
            units_is_ascii_str(js_value_string(key), "prototype")) {
            /* assigning a non-object leaves the auto-created prototype in
             * place, matching real JS's "F.prototype must be an Object". */
            if (js_is_object(val)) {
                if (c->kind == JS_KIND_CLOSURE)
                    ((JsClosure *)c)->prototype_obj = js_value_object(val);
                else
                    ((JsNative *)c)->prototype = js_value_object(val);
            }
            return JS_PROP_OK;
        }
        *errmsg = "TypeError: cannot create property on a primitive value";
        return JS_PROP_TYPE_ERROR;
    }
    if (!js_is_object(base)) {
        *errmsg = "TypeError: cannot create property on a primitive value";
        return JS_PROP_TYPE_ERROR;
    }
    JsObject *o = js_value_object(base);
#ifdef LAMASSU_HAS_REGEX
    if (o->obj_kind == JS_OBJ_REGEXP) {
        bool handled = false;
        if (!js_regexp_prop_set(ctx, o, key, val, &handled, errmsg))
            return JS_PROP_TYPE_ERROR;
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
            double d = js_to_number_value(ctx, val);
            /* bounds-check before the cast: (uint32_t)d is UB for d outside
             * [0, UINT32_MAX], e.g. arr.length = -1 or arr.length = 1e20 */
            if (!(d >= 0 && d < 4294967296.0)) {
                *errmsg = "RangeError: invalid array length";
                return JS_PROP_TYPE_ERROR;
            }
            uint32_t n = (uint32_t)d;
            if ((double)n != d) {
                *errmsg = "RangeError: invalid array length";
                return JS_PROP_TYPE_ERROR;
            }
            if (n <= o->elem_count) {
                o->elem_count = n;
                return JS_PROP_OK;
            }
            if (n - o->elem_count > JS_MAX_ARRAY_GAP) {
                *errmsg = "RangeError: array length growth too large";
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
    if (!js_is_object(base))
        return JS_PROP_TYPE_ERROR;
    JsObject *o = js_value_object(base);
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

bool js_spread_into_object(JsContext *ctx, JsObject *dst, JsValue src) {
    JsVm *vm = ctx->vm;
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
    for (uint32_t i = 0; i < o->props.capacity; i++) {
        JsMapEntry *e = &o->props.entries[i];
        if (e->key && e->key != JS_MAP_TOMBSTONE) {
            if (!js_map_set(vm, &dst->props, e->key, e->value))
                return false;
        }
    }
    return true;
}

/* ---- string ops ---- */

JsString *js_concat_cells(JsVm *vm, const JsString *a, const JsString *b) {
    if ((uint64_t)a->length + b->length > JS_MAX_CONCAT_UNITS)
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
    s->hash = js_units_hash(s->units, len);
    return s;
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

static void fiber_throw(JsContext *ctx, JsFiber *fb, const char *ascii_msg) {
    JsString *s = js_ascii_cell(ctx->vm, ascii_msg);
    fb->error = s ? js_value_from_cell(&s->gc) : js_undefined();
    fb->failed = true;
}

/*
 * Builds "<prefix><name><suffix>" as one string. The message is assembled in
 * a heap buffer (only the final js_string_cell_new allocates a GC cell), so
 * there are no unrooted intermediate strings to be freed by a GC that fires
 * mid-build — a bug that only surfaces once the heap is large enough to
 * collect during error formatting.
 */
static void fiber_throw_name(JsContext *ctx, JsFiber *fb, const char *prefix,
                             const JsString *name, const char *suffix) {
    JsVm *vm = ctx->vm;
    size_t plen = 0, slen = 0;
    while (prefix[plen])
        plen++;
    while (suffix[slen])
        slen++;
    size_t total = plen + name->length + slen;
    uint16_t *buf = js_realloc_raw(vm, NULL, 0, total * sizeof(uint16_t));
    if (!buf) {
        fiber_throw(ctx, fb, prefix);
        return;
    }
    size_t n = 0;
    for (size_t i = 0; i < plen; i++)
        buf[n++] = (uint16_t)(unsigned char)prefix[i];
    for (uint32_t i = 0; i < name->length; i++)
        buf[n++] = name->units[i];
    for (size_t i = 0; i < slen; i++)
        buf[n++] = (uint16_t)(unsigned char)suffix[i];
    JsString *s = js_string_cell_new(vm, buf, (uint32_t)total);
    js_realloc_raw(vm, buf, total * sizeof(uint16_t), 0);
    fb->error = s ? js_value_from_cell(&s->gc) : js_undefined();
    fb->failed = true;
}

/* ---- closures, upvalues, natives ---- */

#define JS_MAX_FRAMES 2000

/* Bound on nested interpreter entries. A native (e.g. Array.prototype.forEach)
 * that calls back into a JS closure re-enters run_fiber on the C stack with a
 * fresh fiber, so the per-fiber JS_MAX_FRAMES limit cannot see the depth. This
 * caps the real C-stack recursion; kept well under JS_MAX_FRAMES since each
 * level here also nests a full native + interpreter C frame. */
#define JS_MAX_REENTRY 200

static JsClosure *closure_new(JsVm *vm, JsFunctionCell *fn) {
    size_t sz = sizeof(JsClosure) + (size_t)fn->n_upvals * sizeof(JsUpvalue *);
    JsGcCell *c = js_gc_new_cell(vm, JS_KIND_CLOSURE, sz);
    if (!c)
        return NULL;
    JsClosure *cl = (JsClosure *)c;
    cl->fn = fn;
    cl->this_val = js_undefined();
    cl->has_this = false;
    cl->prototype_obj = NULL;
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
    nf->prototype = NULL;
    nf->bound = js_undefined();
    nf->is_bound = false;
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
    nf->prototype = NULL;
    nf->bound = bound;
    nf->is_bound = true;
    return js_value_from_cell(c);
}

bool js_object_set_ascii(JsContext *ctx, JsObject *obj, const char *key, JsValue v) {
    JsVm *vm = ctx->vm;
    js_gc_protect(vm, &v);
    JsString *k = js_ascii_cell(vm, key);
    JsString *ik = k ? js_intern_cell(vm, k) : NULL;
    js_gc_unprotect(vm, &v);
    if (!ik)
        return false;
    return js_map_set(vm, &obj->props, ik, v);
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
    nf->prototype = NULL;
    nf->name = js_value_string(key);
    nf->bound = js_undefined();
    nf->is_bound = false;
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
    uint32_t fixed = has_rest ? (uint32_t)fn->n_params - 1 : fn->n_params;

    JsObject *rest = NULL;
    if (has_rest) {
        uint32_t rn = argc > fixed ? argc - fixed : 0;
        rest = js_array_new_cell(ctx, rn);
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
    /* clear declared (non-param) locals */
    for (uint32_t i = fn->n_params; i < fn->n_locals; i++)
        fb->stack[base + i] = js_undefined();

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

static JsRunStatus run_fiber(JsContext *ctx, JsFiber *fb, JsValue *result) {
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
        goto on_error;                                                          \
    } while (0)
#define RT_RAISE()                                                              \
    do {                                                                        \
        fb->err_pos = lookup_pos(fn, op_ip);                                    \
        goto on_error;                                                          \
    } while (0)

        for (;;) {
            if (--fb->fuel == 0) {
                fb->fuel = 1;
                RT_THROW("RangeError: execution budget exhausted");
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
                if (v.bits == JS_SPECIAL_TDZ)
                    RT_THROW("ReferenceError: cannot access variable before initialization");
                PUSH(v);
                break;
            }
            case JS_OP_SET_LOCAL:
                S[base + READ_U16()] = PEEK(0);
                break;
            case JS_OP_TDZ:
                S[base + READ_U16()].bits = JS_SPECIAL_TDZ;
                break;
            case JS_OP_GET_UPVAL: {
                JsUpvalue *uv = cl->upvals[READ_U16()];
                /* an open upvalue aliases the stack of the fiber that owns
                 * it, which may differ from fb during a nested call */
                JsValue v = uv->open ? uv->fiber->stack[uv->slot] : uv->closed;
                if (v.bits == JS_SPECIAL_TDZ)
                    RT_THROW("ReferenceError: cannot access variable before initialization");
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
                    if (v.bits == JS_SPECIAL_TDZ)
                        RT_THROW("ReferenceError: cannot access variable before initialization");
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
                        RT_THROW("TypeError: assignment to constant variable");
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
                if (js_is_object(a) || js_is_function(a) || js_is_promise(a)) {
                    JsString *s = js_to_string_cell(ctx, a, 0);
                    if (!s)
                        RT_THROW("out of memory");
                    PEEK(1) = a = js_value_from_cell(&s->gc);
                }
                if (js_is_object(b) || js_is_function(b) || js_is_promise(b)) {
                    JsString *s = js_to_string_cell(ctx, b, 0);
                    if (!s)
                        RT_THROW("out of memory");
                    PEEK(0) = b = js_value_from_cell(&s->gc);
                }
                if (js_is_string(a) || js_is_string(b)) {
                    JsString *sa = js_to_string_cell(ctx, a, 0);
                    if (!sa)
                        RT_THROW("out of memory");
                    PEEK(1) = js_value_from_cell(&sa->gc);
                    JsString *sb = js_to_string_cell(ctx, b, 0);
                    if (!sb)
                        RT_THROW("out of memory");
                    PEEK(0) = js_value_from_cell(&sb->gc);
                    JsString *r = js_concat_cells(vm, sa, sb);
                    if (!r)
                        RT_THROW("RangeError: string too long");
                    fb->sp--;
                    PEEK(0) = js_value_from_cell(&r->gc);
                    break;
                }
                double x = js_to_number_value(ctx, a), y = js_to_number_value(ctx, b);
                fb->sp--;
                PEEK(0) = js_number(x + y);
                break;
            }
            case JS_OP_SUB: case JS_OP_MUL: case JS_OP_DIV:
            case JS_OP_MOD: case JS_OP_POW: {
                double a = js_to_number_value(ctx, PEEK(1));
                double b = js_to_number_value(ctx, PEEK(0));
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
            case JS_OP_NEG:
                PEEK(0) = js_number(-js_to_number_value(ctx, PEEK(0)));
                break;
            case JS_OP_POS:
                PEEK(0) = js_number(js_to_number_value(ctx, PEEK(0)));
                break;
            case JS_OP_NOT:
                PEEK(0) = js_bool(!js_to_boolean(PEEK(0)));
                break;
            case JS_OP_BITNOT:
                PEEK(0) = js_number(~js_to_int32(js_to_number_value(ctx, PEEK(0))));
                break;
            case JS_OP_BITAND: case JS_OP_BITOR: case JS_OP_BITXOR:
            case JS_OP_SHL: case JS_OP_SHR: case JS_OP_USHR: {
                double da = js_to_number_value(ctx, PEEK(1));
                double db = js_to_number_value(ctx, PEEK(0));
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
                bool eq = loose_eq_values(ctx, PEEK(1), PEEK(0));
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
                int c = js_compare_values(ctx, PEEK(1), PEEK(0));
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
                if (st == JS_PROP_TYPE_ERROR)
                    RT_THROW("TypeError: 'in' requires an object");
                if (st == JS_PROP_OOM)
                    RT_THROW("out of memory");
                fb->sp--;
                PEEK(0) = js_bool(has);
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
                JsString *s = js_to_string_cell(ctx, PEEK(0), 0);
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
                if (!js_is_object(re))
                    RT_THROW(em ? em : "out of memory");
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
                    RT_THROW("TypeError: value is not iterable");
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
                if (!js_spread_into_object(ctx, dst, PEEK(0)))
                    RT_THROW("out of memory");
                fb->sp--;
                break;
            }
            case JS_OP_DEFINE_PROP: {
                const char *msg;
                JsPropStatus st = set_property(ctx, PEEK(2), PEEK(1), PEEK(0), &msg);
                if (st == JS_PROP_OOM)
                    RT_THROW("out of memory");
                if (st == JS_PROP_TYPE_ERROR)
                    RT_THROW(msg);
                fb->sp -= 2;
                break;
            }
            case JS_OP_GET_PROP: {
                JsValue out;
                JsPropStatus st = get_property(ctx, PEEK(1), PEEK(0), &out);
                if (st == JS_PROP_OOM)
                    RT_THROW("out of memory");
                if (st == JS_PROP_TYPE_ERROR)
                    RT_THROW("TypeError: cannot read properties of undefined or null");
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
                if (st == JS_PROP_TYPE_ERROR) {
                    fiber_throw_name(ctx, fb,
                                     "TypeError: cannot read properties of undefined or null (reading '",
                                     js_value_string(key), "')");
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
                if (st == JS_PROP_TYPE_ERROR)
                    RT_THROW(msg);
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
                if (st == JS_PROP_TYPE_ERROR)
                    RT_THROW(msg);
                JsValue v = PEEK(0);
                fb->sp--;
                PEEK(0) = v;
                break;
            }
            case JS_OP_DELETE_PROP: {
                JsValue key = PEEK(0), b = PEEK(1);
                if (js_is_object(b)) {
                    JsObject *o = js_value_object(b);
                    uint32_t idx;
                    if (o->obj_kind == JS_OBJ_ARRAY && value_to_index(key, &idx)) {
                        if (idx < o->elem_count)
                            o->elems[idx] = js_undefined();
                    } else {
                        JsString *k = to_prop_key(ctx, key);
                        if (!k)
                            RT_THROW("out of memory");
                        js_map_delete(&o->props, k);
                    }
                } else if (is_nullish(b)) {
                    RT_THROW("TypeError: cannot convert undefined or null to object");
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
                if (!ok)
                    RT_THROW("TypeError: value is not iterable");
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
                        bool ok = nf->is_bound
                            ? nf->bound_fn(ctx, nf->bound, S[nb - 1], &S[nb], (int)argc, &res)
                            : nf->fn(ctx, S[nb - 1], &S[nb], (int)argc, &res);
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
                            RT_THROW("RangeError: maximum call stack size exceeded");
                        goto run;
                    }
                }
                RT_THROW("TypeError: value is not a function");
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
                    fb->sp--; /* drop the array */
                    if (!stack_ensure(vm, fb, fb->sp + argc + 8))
                        RT_THROW("out of memory");
                    for (uint32_t i = 0; i < argc; i++)
                        S[fb->sp++] = arr->elems[i];
                }
                uint32_t nb = fb->sp - argc; /* callee=nb-2, this(placeholder)=nb-1 */
                JsValue callee = S[nb - 2];
                if (!js_is_function(callee))
                    RT_THROW("TypeError: value is not a constructor");
                JsGcCell *cc = js_value_cell(callee);
                if (cc->kind == JS_KIND_NATIVE) {
                    /* Builtins (Array, Object, RegExp, ...) are factory
                     * functions that already return a suitable object;
                     * `new` on them just calls them. String/Number/Boolean
                     * return their primitive form rather than a wrapper
                     * object — this engine has no boxed-primitive type. */
                    JsNative *nf = (JsNative *)cc;
                    fr->ip = ip;
                    JsValue res;
                    bool ok = nf->is_bound
                        ? nf->bound_fn(ctx, nf->bound, S[nb - 1], &S[nb], (int)argc, &res)
                        : nf->fn(ctx, S[nb - 1], &S[nb], (int)argc, &res);
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
                    RT_THROW("TypeError: value is not a constructor");
                JsClosure *ctor = (JsClosure *)cc;
                if (ctor->fn->fn_flags & (JS_FN_ARROW | JS_FN_ASYNC))
                    RT_THROW("TypeError: value is not a constructor");
                fr->ip = ip; /* GC-safe point before allocations below */
                JsObject *proto = closure_prototype(ctx, ctor);
                if (!proto)
                    RT_THROW("out of memory");
                JsValue instv = js_object_new(ctx);
                if (!js_is_object(instv))
                    RT_THROW("out of memory");
                js_value_object(instv)->proto = js_value_from_cell(&proto->gc);
                S[nb - 1] = instv; /* this */
                int rc = frame_setup(ctx, fb, ctor, nb, argc, true);
                if (rc == -1)
                    RT_THROW("out of memory");
                if (rc == -2)
                    RT_THROW("RangeError: maximum call stack size exceeded");
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
                 * bytecode field, already validated at load time). */
                uint32_t ret;
                if (!value_to_index(POPV(), &ret) || ret >= fn->code_len ||
                    (fn->insn_boundary && !fn->insn_boundary[ret]))
                    RT_THROW("internal error: bad subroutine return address");
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
                PUSH(found ? v : js_undefined());
                break;
            }
            case JS_OP_SET_EXPORT: {
                JsString *name = js_value_string(K[READ_U16()]);
                if (!js_map_set(vm, &fn->module->exports->props, name, PEEK(0)))
                    RT_THROW("out of memory");
                break;
            }
            case JS_OP_GET_IMPORT: {
                JsModuleImport *imp = &fn->module->imports[READ_U16()];
                if (!imp->source || !imp->source->exports)
                    RT_THROW("ReferenceError: module binding is not available");
                bool found;
                JsValue v = js_map_get(&imp->source->exports->props, imp->imported_name,
                                       &found);
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
            case JS_OP_THROW:
                fb->error = POPV();
                fb->failed = true;
                RT_RAISE();
            case JS_OP_RETURN: {
                JsValue ret = POPV();
                /* `new`: a non-object return value is discarded in favor of
                 * the constructed `this` (still live at base-1). */
                if (fr->is_construct && !js_is_object(ret))
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
    }

on_error:
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
     * fuel so a script cannot refill the budget by making nested calls. Only a
     * fresh top-level run (no current fiber) starts from ctx->fuel. */
    fb->fuel = ctx->fiber ? ctx->fiber->fuel : (ctx->fuel ? ctx->fuel : UINT64_MAX);

    if (!stack_ensure(vm, fb, (uint32_t)(2 + argc) + cl->fn->n_locals +
                                  cl->fn->max_stack + 8)) {
        *err = "out of memory";
        return NULL;
    }
    fb->stack[0] = js_value_from_cell(&cl->gc);
    fb->stack[1] = this_val;
    for (int i = 0; i < argc; i++)
        fb->stack[2 + i] = args[i];
    fb->sp = (uint32_t)(2 + argc);
    int rc = frame_setup(ctx, fb, cl, 2, (uint32_t)argc, false);
    if (rc != 0) {
        *err = rc == -2 ? "RangeError: maximum call stack size exceeded" : "out of memory";
        return NULL;
    }
    return fb;
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
        JsString *s = js_ascii_cell(ctx->vm, "RangeError: maximum call stack size exceeded");
        fb->error = s ? js_value_from_cell(&s->gc) : js_undefined();
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
     * keeps decreasing across the whole call tree. */
    if (saved)
        saved->fuel = fb->fuel;
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
        return nf->is_bound
            ? nf->bound_fn(ctx, nf->bound, this_val, args, argc, result)
            : nf->fn(ctx, this_val, args, argc, result);
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

static JsValue compile_module_impl(JsContext *ctx, const uint16_t *src, size_t len,
                                   bool repl, const char **err_msg, uint32_t *err_pos) {
    *err_msg = NULL;
    *err_pos = 0;
    JsArena arena;
    js_arena_init(&arena, ctx->vm);
    JsParseResult pr;
    if (!js_parse_module(&arena, src, len, &pr)) {
        *err_msg = pr.err_msg;
        *err_pos = pr.err_pos;
        js_arena_free(&arena);
        return js_undefined();
    }
    JsCompileError ce;
    JsFunctionCell *fn = js_compile_ast(ctx, pr.module, repl, &ce);
    js_arena_free(&arena);
    if (!fn) {
        *err_msg = ce.msg;
        *err_pos = ce.pos;
        return js_undefined();
    }
    return js_value_from_cell(&fn->gc);
}

JsValue js_compile_module(JsContext *ctx, const uint16_t *src, size_t len,
                          const char **err_msg, uint32_t *err_pos) {
    return compile_module_impl(ctx, src, len, false, err_msg, err_pos);
}

JsValue js_compile_module_repl(JsContext *ctx, const uint16_t *src, size_t len,
                               const char **err_msg, uint32_t *err_pos) {
    return compile_module_impl(ctx, src, len, true, err_msg, err_pos);
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
    JsString *s = js_to_string_cell(ctx, v, 0);
    return s ? js_value_from_cell(&s->gc) : js_undefined();
}
