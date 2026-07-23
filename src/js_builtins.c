/*
 * Standard library. Array instance methods live on a real, script-visible
 * Array.prototype (ctx->array_proto is a construction-time cache of it, not
 * a lookup shortcut — see lamassu_internal.h). String/Number don't have a
 * [[Prototype]] slot to hang a real chain off (they're primitives, not
 * JS_KIND_OBJECT), so their methods still live on a hidden per-context
 * table (ctx->string_methods / number_methods) that property lookup falls
 * back to directly. Statics live on global objects (Math, JSON, Object,
 * ...). Everything is a JS_KIND_NATIVE.
 *
 * GC discipline: a native runs with the calling fiber rooted, so `args`
 * and `this` survive. Any JS cell we create and then hold across a further
 * allocation (or a js_call into user code) is protected; scratch is built
 * in C buffers and turned into cells once.
 *
 * Known deviations (documented): object key iteration follows hash order,
 * not insertion order (resolved when shapes land); case mapping is ASCII
 * only; sort is a stable O(n^2) binary insertion sort.
 */
#include "js_bytecode.h"
#include "lamassu_internal.h"
#include "js_date.h"
#include "js_mapobj.h"
#include "js_setobj.h"
#ifdef LAMASSU_HAS_REGEX
#include "js_regexp.h"
#endif

#define ARG(i) ((i) < argc ? args[i] : js_undefined())

/* ---- small numeric helpers ---- */

static double to_int(double d) {
    if (d != d)
        return 0;
    if (d == __builtin_inf() || d == -__builtin_inf())
        return d;
    return __builtin_trunc(d);
}

/*
 * ES ToInteger, then saturate into [lo, hi]. The result is guaranteed to lie
 * in [lo, hi], so the caller may cast it to a bounded integer type without the
 * undefined behavior of casting a NaN/Infinity/out-of-range double directly.
 * NaN maps to lo; ±Infinity and out-of-range finite values clamp to the nearer
 * bound. Every radix/count/space cast routes through here.
 */
static double to_integer_clamped(double d, double lo, double hi) {
    if (d != d)
        return lo; /* NaN */
    if (d == __builtin_inf())
        return hi;
    if (d == -__builtin_inf())
        return lo;
    d = __builtin_trunc(d);
    if (d < lo)
        return lo;
    if (d > hi)
        return hi;
    return d;
}

/* clamp a relative index (negative counts from end) into [0, len] */
static uint32_t clamp_index(double d, uint32_t len) {
    d = to_int(d);
    if (d < 0)
        d += len;
    if (d < 0)
        return 0;
    if (d > len)
        return len;
    return (uint32_t)d;
}

/* ECMAScript ToUint16: truncate, then reduce modulo 2^16 — never a plain
 * (uint16_t)(uint32_t)d cast, which is UB for d outside [0, UINT32_MAX]
 * (e.g. String.fromCharCode(1e20)). NaN/+-Infinity go to 0 first. */
static uint16_t to_uint16(double d) {
    d = to_int(d);
    if (d == __builtin_inf() || d == -__builtin_inf())
        return 0;
    double m = __builtin_fmod(d, 65536.0);
    if (m < 0)
        m += 65536.0;
    return (uint16_t)m; /* m is now in [0, 65536) — safe */
}

/* ---- string builder (C-side scratch, no GC cells until finish) ---- */

typedef struct {
    JsVm *vm;
    uint16_t *u;
    uint32_t len, cap;
    bool oom;
    bool circular; /* set by JSON.stringify on a cyclic structure */
} StrBuf;

static void sb_init(StrBuf *sb, JsVm *vm) {
    sb->vm = vm;
    sb->u = NULL;
    sb->len = sb->cap = 0;
    sb->oom = false;
    sb->circular = false;
}

static bool sb_reserve(StrBuf *sb, uint32_t extra) {
    if (sb->oom)
        return false;
    if (extra > UINT32_MAX - sb->len) { /* len + extra would wrap uint32 */
        sb->oom = true;
        return false;
    }
    uint32_t need = sb->len + extra;
    if (need <= sb->cap)
        return true;
    uint32_t ncap = sb->cap ? sb->cap * 2 : 32;
    while (ncap < need) {
        if (ncap > UINT32_MAX / 2) { /* next doubling would overflow */
            ncap = need;
            break;
        }
        ncap *= 2;
    }
    uint16_t *nu = js_realloc_raw(sb->vm, sb->u, (size_t)sb->cap * sizeof(uint16_t),
                                  (size_t)ncap * sizeof(uint16_t));
    if (!nu) {
        sb->oom = true;
        return false;
    }
    sb->u = nu;
    sb->cap = ncap;
    return true;
}

static void sb_unit(StrBuf *sb, uint16_t c) {
    if (sb_reserve(sb, 1))
        sb->u[sb->len++] = c;
}

static void sb_units(StrBuf *sb, const uint16_t *u, uint32_t n) {
    if (n && sb_reserve(sb, n)) {
        memcpy(sb->u + sb->len, u, (size_t)n * sizeof(uint16_t));
        sb->len += n;
    }
}

static void sb_ascii(StrBuf *sb, const char *s) {
    while (*s)
        sb_unit(sb, (uint16_t)(unsigned char)*s++);
}

static void sb_free(StrBuf *sb) {
    js_realloc_raw(sb->vm, sb->u, (size_t)sb->cap * sizeof(uint16_t), 0);
    sb->u = NULL;
    sb->cap = sb->len = 0;
}

/* Turns the buffer into a JS string value and frees it. undefined on OOM. */
static JsValue sb_finish(StrBuf *sb) {
    if (sb->oom) {
        sb_free(sb);
        return js_undefined();
    }
    JsString *s = js_string_cell_new(sb->vm, sb->u, sb->len);
    sb_free(sb);
    return s ? js_value_from_cell(&s->gc) : js_undefined();
}

/* ---- throwing from a native ---- */

static bool native_throw(JsContext *ctx, JsValue *result, const char *msg) {
    JsString *s = js_ascii_cell(ctx->vm, msg);
    *result = s ? js_value_from_cell(&s->gc) : js_undefined();
    return false;
}

static bool oom(JsContext *ctx, JsValue *result) {
    return native_throw(ctx, result, "out of memory");
}

/* ---- ToString of an argument (rooted) ---- */

static JsString *arg_string(JsContext *ctx, JsValue v) {
    return js_to_string_cell(ctx, v, 0);
}

/* substring search: returns index in hay of first needle at/after `from`, or -1 */
static long units_find(const uint16_t *hay, uint32_t hlen, const uint16_t *ndl,
                       uint32_t nlen, uint32_t from) {
    if (nlen == 0)
        return from <= hlen ? (long)from : (long)hlen;
    if (nlen > hlen)
        return -1;
    for (uint32_t i = from; i + nlen <= hlen; i++) {
        uint32_t j = 0;
        while (j < nlen && hay[i + j] == ndl[j])
            j++;
        if (j == nlen)
            return (long)i;
    }
    return -1;
}

/* =====================================================================
 * String methods (this_val = a string; coerced via ToString otherwise)
 * ===================================================================== */

static bool str_this(JsContext *ctx, JsValue this_val, JsString **out, JsValue *result) {
    JsString *s = js_to_string_cell(ctx, this_val, 0);
    if (!s)
        return oom(ctx, result);
    *out = s;
    return true;
}

static bool sm_charAt(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    double d = to_int(js_to_number_value(ctx, ARG(0)));
    if (d < 0 || d >= s->length) {
        JsString *e = js_string_cell_new(ctx->vm, NULL, 0);
        if (!e)
            return oom(ctx, r);
        *r = js_value_from_cell(&e->gc);
        return true;
    }
    JsString *c = js_string_cell_new(ctx->vm, s->units + (uint32_t)d, 1);
    if (!c)
        return oom(ctx, r);
    *r = js_value_from_cell(&c->gc);
    return true;
}

static bool sm_charCodeAt(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    double d = to_int(js_to_number_value(ctx, ARG(0)));
    if (d < 0 || d >= s->length) {
        *r = js_number(__builtin_nan(""));
        return true;
    }
    *r = js_number(s->units[(uint32_t)d]);
    return true;
}

static bool sm_codePointAt(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    double d = to_int(js_to_number_value(ctx, ARG(0)));
    if (d < 0 || d >= s->length) {
        *r = js_undefined();
        return true;
    }
    uint32_t i = (uint32_t)d;
    uint32_t c = s->units[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s->length &&
        s->units[i + 1] >= 0xDC00 && s->units[i + 1] <= 0xDFFF)
        c = 0x10000 + ((c - 0xD800) << 10) + (s->units[i + 1] - 0xDC00);
    *r = js_number(c);
    return true;
}

static bool sm_at(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    double d = to_int(js_to_number_value(ctx, ARG(0)));
    if (d < 0)
        d += s->length;
    if (d < 0 || d >= s->length) {
        *r = js_undefined();
        return true;
    }
    JsString *c = js_string_cell_new(ctx->vm, s->units + (uint32_t)d, 1);
    if (!c)
        return oom(ctx, r);
    *r = js_value_from_cell(&c->gc);
    return true;
}

static bool sm_indexOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    JsString *n = arg_string(ctx, ARG(0));
    if (!n)
        return oom(ctx, r);
    uint32_t from = argc > 1 ? clamp_index(js_to_number_value(ctx, ARG(1)), s->length) : 0;
    *r = js_number((double)units_find(s->units, s->length, n->units, n->length, from));
    return true;
}

static bool sm_lastIndexOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    JsString *n = arg_string(ctx, ARG(0));
    if (!n)
        return oom(ctx, r);
    /* fromIndex: the last match whose start index is <= fromIndex. Defaults to
     * +Infinity (whole string); NaN is also treated as +Infinity per spec. */
    double posd = argc > 1 ? js_to_number_value(ctx, ARG(1)) : __builtin_inf();
    uint32_t limit;
    if (posd != posd || posd >= (double)s->length)
        limit = s->length;
    else if (posd < 0)
        limit = 0;
    else
        limit = (uint32_t)posd;
    long found = -1;
    for (long i = 0; i + (long)n->length <= (long)s->length && i <= (long)limit; i++) {
        uint32_t j = 0;
        while (j < n->length && s->units[i + j] == n->units[j])
            j++;
        if (j == n->length)
            found = i;
    }
    *r = js_number((double)found);
    return true;
}

static bool sm_includes(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    JsString *n = arg_string(ctx, ARG(0));
    if (!n)
        return oom(ctx, r);
    *r = js_bool(units_find(s->units, s->length, n->units, n->length, 0) >= 0);
    return true;
}

static bool sm_startsWith(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    JsString *n = arg_string(ctx, ARG(0));
    if (!n)
        return oom(ctx, r);
    uint32_t from = argc > 1 ? clamp_index(js_to_number_value(ctx, ARG(1)), s->length) : 0;
    bool ok = from + n->length <= s->length;
    for (uint32_t j = 0; ok && j < n->length; j++)
        ok = s->units[from + j] == n->units[j];
    *r = js_bool(ok);
    return true;
}

static bool sm_endsWith(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    JsString *n = arg_string(ctx, ARG(0));
    if (!n)
        return oom(ctx, r);
    uint32_t end = argc > 1 ? clamp_index(js_to_number_value(ctx, ARG(1)), s->length) : s->length;
    bool ok = n->length <= end;
    for (uint32_t j = 0; ok && j < n->length; j++)
        ok = s->units[end - n->length + j] == n->units[j];
    *r = js_bool(ok);
    return true;
}

static bool make_substr(JsContext *ctx, JsString *s, uint32_t start, uint32_t end, JsValue *r) {
    if (end < start)
        end = start;
    JsString *c = js_string_cell_new(ctx->vm, s->units + start, end - start);
    if (!c)
        return oom(ctx, r);
    *r = js_value_from_cell(&c->gc);
    return true;
}

static bool sm_slice(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    uint32_t start = clamp_index(js_to_number_value(ctx, ARG(0)), s->length);
    uint32_t end = argc > 1 && !js_is_undefined(ARG(1))
                       ? clamp_index(js_to_number_value(ctx, ARG(1)), s->length)
                       : s->length;
    return make_substr(ctx, s, start, end, r);
}

static bool sm_substring(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    uint32_t a = clamp_index(js_to_number_value(ctx, ARG(0)), s->length);
    uint32_t b = argc > 1 && !js_is_undefined(ARG(1))
                     ? clamp_index(js_to_number_value(ctx, ARG(1)), s->length)
                     : s->length;
    if (a > b) {
        uint32_t t = a;
        a = b;
        b = t;
    }
    return make_substr(ctx, s, a, b, r);
}

static bool sm_toUpperCase(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    for (uint32_t i = 0; i < s->length; i++) {
        uint16_t c = s->units[i];
        sb_unit(&sb, c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    *r = sb_finish(&sb);
    return !js_is_undefined(*r) || s->length == 0 ? true : oom(ctx, r);
}

static bool sm_toLowerCase(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    for (uint32_t i = 0; i < s->length; i++) {
        uint16_t c = s->units[i];
        sb_unit(&sb, c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    *r = sb_finish(&sb);
    return !js_is_undefined(*r) || s->length == 0 ? true : oom(ctx, r);
}

static bool is_ws(uint16_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B ||
           c == 0x0C || c == 0xA0 || c == 0xFEFF;
}

static bool sm_trim_impl(JsContext *ctx, JsValue tv, JsValue *r, bool start, bool end) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    uint32_t a = 0, b = s->length;
    if (start)
        while (a < b && is_ws(s->units[a]))
            a++;
    if (end)
        while (b > a && is_ws(s->units[b - 1]))
            b--;
    return make_substr(ctx, s, a, b, r);
}

static bool sm_trim(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    (void)a; (void)c; return sm_trim_impl(ctx, tv, r, true, true);
}
static bool sm_trimStart(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    (void)a; (void)c; return sm_trim_impl(ctx, tv, r, true, false);
}
static bool sm_trimEnd(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    (void)a; (void)c; return sm_trim_impl(ctx, tv, r, false, true);
}

static bool sm_repeat(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    double d = to_int(js_to_number_value(ctx, ARG(0)));
    if (d < 0 || d == __builtin_inf())
        return native_throw(ctx, r, "RangeError: invalid count value");
    /* bounds-check before the cast: (uint32_t)d is UB above UINT32_MAX,
     * e.g. 'x'.repeat(1e20) — any count this large already fails the size
     * check below anyway, so throw the same error a step earlier. */
    if (d > 4294967295.0)
        return native_throw(ctx, r, "RangeError: repeat count too large");
    uint32_t n = (uint32_t)d;
    if ((uint64_t)n * s->length > (64u * 1024 * 1024))
        return native_throw(ctx, r, "RangeError: repeat count too large");
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    for (uint32_t i = 0; i < n; i++)
        sb_units(&sb, s->units, s->length);
    *r = sb_finish(&sb);
    return js_is_undefined(*r) && (n * s->length) ? oom(ctx, r) : true;
}

static bool sm_pad(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r, bool start) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    /* Clamp the target length into [0, UINT32_MAX] before the cast so an
     * Infinity/huge argument can't UB; an oversized target still means "pad a
     * lot" and is bounded by allocation limits downstream. */
    uint32_t target = (uint32_t)to_integer_clamped(js_to_number_value(ctx, ARG(0)), 0, 4294967295.0);
    if (target <= s->length)
        return make_substr(ctx, s, 0, s->length, r);
    JsString *pad;
    static const uint16_t space[] = {' '};
    const uint16_t *pu = space;
    uint32_t pn = 1;
    if (argc > 1 && !js_is_undefined(ARG(1))) {
        pad = arg_string(ctx, ARG(1));
        if (!pad)
            return oom(ctx, r);
        pu = pad->units;
        pn = pad->length;
    }
    if (pn == 0)
        return make_substr(ctx, s, 0, s->length, r);
    uint32_t need = target - s->length;
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    if (!start)
        sb_units(&sb, s->units, s->length);
    for (uint32_t i = 0; i < need; i++)
        sb_unit(&sb, pu[i % pn]);
    if (start)
        sb_units(&sb, s->units, s->length);
    *r = sb_finish(&sb);
    return js_is_undefined(*r) ? oom(ctx, r) : true;
}

static bool sm_padStart(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return sm_pad(ctx, tv, a, c, r, true);
}
static bool sm_padEnd(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return sm_pad(ctx, tv, a, c, r, false);
}

/* replacement with $$ and $& expansion */
static void append_replacement(StrBuf *sb, JsString *rep, const uint16_t *match,
                               uint32_t mlen) {
    for (uint32_t i = 0; i < rep->length; i++) {
        if (rep->units[i] == '$' && i + 1 < rep->length) {
            uint16_t n = rep->units[i + 1];
            if (n == '$') {
                sb_unit(sb, '$');
                i++;
                continue;
            }
            if (n == '&') {
                sb_units(sb, match, mlen);
                i++;
                continue;
            }
        }
        sb_unit(sb, rep->units[i]);
    }
}

static bool sm_replace_impl(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                            JsValue *r, bool all) {
#ifdef LAMASSU_HAS_REGEX
    if (js_regexp_is(ARG(0)))
        return js_re_str_replace(ctx, tv, args, argc, r, all);
#endif
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    JsString *pat = arg_string(ctx, ARG(0));
    if (!pat)
        return oom(ctx, r);
    JsString *rep = arg_string(ctx, ARG(1));
    if (!rep)
        return oom(ctx, r);
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    uint32_t pos = 0;
    for (;;) {
        long f = units_find(s->units, s->length, pat->units, pat->length, pos);
        if (f < 0) {
            sb_units(&sb, s->units + pos, s->length - pos);
            break;
        }
        sb_units(&sb, s->units + pos, (uint32_t)f - pos);
        append_replacement(&sb, rep, pat->units, pat->length);
        uint32_t adv = pat->length ? pat->length : 1;
        if (pat->length == 0 && (uint32_t)f < s->length)
            sb_unit(&sb, s->units[f]);
        pos = (uint32_t)f + adv;
        if (!all || pos > s->length) {
            if (pos <= s->length)
                sb_units(&sb, s->units + pos, s->length - pos);
            break;
        }
    }
    *r = sb_finish(&sb);
    return js_is_undefined(*r) ? oom(ctx, r) : true;
}

static bool sm_replace(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return sm_replace_impl(ctx, tv, a, c, r, false);
}
static bool sm_replaceAll(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return sm_replace_impl(ctx, tv, a, c, r, true);
}

static bool sm_split(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
#ifdef LAMASSU_HAS_REGEX
    if (js_regexp_is(ARG(0)))
        return js_re_str_split(ctx, tv, args, argc, r);
#endif
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    JsObject *out = js_array_new_cell(ctx, 0);
    if (!out)
        return oom(ctx, r);
    JsValue outv = js_value_from_cell(&out->gc);
    js_gc_protect(ctx->vm, &outv);

    bool ok = true;
    if (js_is_undefined(ARG(0))) {
        JsString *whole = js_string_cell_new(ctx->vm, s->units, s->length);
        ok = whole && js_array_append(ctx->vm, out, js_value_from_cell(&whole->gc));
    } else {
        JsString *sep = arg_string(ctx, ARG(0));
        if (!sep) {
            ok = false;
        } else if (sep->length == 0) {
            for (uint32_t i = 0; i < s->length && ok; i++) {
                JsString *c = js_string_cell_new(ctx->vm, s->units + i, 1);
                ok = c && js_array_append(ctx->vm, out, js_value_from_cell(&c->gc));
            }
        } else {
            uint32_t pos = 0;
            for (;;) {
                long f = units_find(s->units, s->length, sep->units, sep->length, pos);
                uint32_t end = f < 0 ? s->length : (uint32_t)f;
                JsString *piece = js_string_cell_new(ctx->vm, s->units + pos, end - pos);
                if (!piece || !js_array_append(ctx->vm, out, js_value_from_cell(&piece->gc))) {
                    ok = false;
                    break;
                }
                if (f < 0)
                    break;
                pos = (uint32_t)f + sep->length;
            }
        }
    }
    js_gc_unprotect(ctx->vm, &outv);
    if (!ok)
        return oom(ctx, r);
    *r = outv;
    return true;
}

static bool sm_concat(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    sb_units(&sb, s->units, s->length);
    for (int i = 0; i < argc; i++) {
        JsString *a = arg_string(ctx, args[i]);
        if (!a) {
            sb_free(&sb);
            return oom(ctx, r);
        }
        sb_units(&sb, a->units, a->length);
    }
    *r = sb_finish(&sb);
    return js_is_undefined(*r) && sb.len ? oom(ctx, r) : true;
}

static bool sm_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    *r = js_value_from_cell(&s->gc);
    return true;
}

/* =====================================================================
 * Array methods (this_val = array object)
 * ===================================================================== */

static bool array_this(JsContext *ctx, JsValue tv, JsObject **out, JsValue *r) {
    if (!js_is_object(tv) || js_value_object(tv)->obj_kind != JS_OBJ_ARRAY)
        return native_throw(ctx, r, "TypeError: not an array");
    *out = js_value_object(tv);
    return true;
}

static bool am_push(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    for (int i = 0; i < argc; i++) {
        if (!js_array_append(ctx->vm, a, args[i]))
            return oom(ctx, r);
    }
    *r = js_number(a->elem_count);
    return true;
}

static bool am_pop(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    if (a->elem_count == 0) {
        *r = js_undefined();
        return true;
    }
    *r = a->elems[--a->elem_count];
    return true;
}

static bool am_shift(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    if (a->elem_count == 0) {
        *r = js_undefined();
        return true;
    }
    JsValue first = a->elems[0];
    for (uint32_t i = 1; i < a->elem_count; i++)
        a->elems[i - 1] = a->elems[i];
    a->elem_count--;
    *r = first;
    return true;
}

static bool am_unshift(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    for (int i = 0; i < argc; i++) {
        if (!js_array_append(ctx->vm, a, js_undefined())) /* grow */
            return oom(ctx, r);
    }
    uint32_t n = (uint32_t)argc;
    for (uint32_t i = a->elem_count; i-- > n;)
        a->elems[i] = a->elems[i - n];
    for (int i = 0; i < argc; i++)
        a->elems[i] = args[i];
    *r = js_number(a->elem_count);
    return true;
}

static bool am_at(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    double d = to_int(js_to_number_value(ctx, ARG(0)));
    if (d < 0)
        d += a->elem_count;
    if (d < 0 || d >= a->elem_count) {
        *r = js_undefined();
        return true;
    }
    *r = a->elems[(uint32_t)d];
    return true;
}

static bool am_indexOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    uint32_t from = argc > 1 ? clamp_index(js_to_number_value(ctx, ARG(1)), a->elem_count) : 0;
    for (uint32_t i = from; i < a->elem_count; i++) {
        if (js_strict_equals(a->elems[i], ARG(0))) {
            *r = js_number(i);
            return true;
        }
    }
    *r = js_number(-1);
    return true;
}

static bool am_lastIndexOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    for (uint32_t i = a->elem_count; i-- > 0;) {
        if (js_strict_equals(a->elems[i], ARG(0))) {
            *r = js_number(i);
            return true;
        }
    }
    *r = js_number(-1);
    return true;
}

static bool am_includes(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    for (uint32_t i = 0; i < a->elem_count; i++) {
        if (js_strict_equals(a->elems[i], ARG(0))) {
            *r = js_bool(true);
            return true;
        }
    }
    *r = js_bool(false);
    return true;
}

static bool am_slice(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    uint32_t start = clamp_index(js_to_number_value(ctx, ARG(0)), a->elem_count);
    uint32_t end = argc > 1 && !js_is_undefined(ARG(1))
                       ? clamp_index(js_to_number_value(ctx, ARG(1)), a->elem_count)
                       : a->elem_count;
    JsObject *out = js_array_new_cell(ctx, end > start ? end - start : 0);
    if (!out)
        return oom(ctx, r);
    for (uint32_t i = start; i < end; i++)
        out->elems[out->elem_count++] = a->elems[i];
    *r = js_value_from_cell(&out->gc);
    return true;
}

static bool am_join(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    JsValue av = tv;
    js_gc_protect(ctx->vm, &av);
    const uint16_t comma[] = {','};
    const uint16_t *su = comma;
    uint32_t sn = 1;
    JsString *sep = NULL;
    if (argc > 0 && !js_is_undefined(ARG(0))) {
        sep = arg_string(ctx, ARG(0));
        if (!sep) {
            js_gc_unprotect(ctx->vm, &av);
            return oom(ctx, r);
        }
        su = sep->units;
        sn = sep->length;
    }
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    bool ok = true;
    for (uint32_t i = 0; i < a->elem_count; i++) {
        if (i)
            sb_units(&sb, su, sn);
        JsValue el = a->elems[i];
        if (js_is_undefined(el) || js_is_null(el))
            continue;
        JsString *es = js_to_string_cell(ctx, el, 0);
        if (!es) {
            ok = false;
            break;
        }
        sb_units(&sb, es->units, es->length);
    }
    js_gc_unprotect(ctx->vm, &av);
    if (!ok) {
        sb_free(&sb);
        return oom(ctx, r);
    }
    *r = sb_finish(&sb);
    return js_is_undefined(*r) && sb.oom ? oom(ctx, r) : true;
}

static bool am_reverse(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    uint32_t n = a->elem_count;
    for (uint32_t i = 0; i < n / 2; i++) {
        JsValue t = a->elems[i];
        a->elems[i] = a->elems[n - 1 - i];
        a->elems[n - 1 - i] = t;
    }
    *r = tv;
    return true;
}

static bool am_fill(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    uint32_t start = argc > 1 ? clamp_index(js_to_number_value(ctx, ARG(1)), a->elem_count) : 0;
    uint32_t end = argc > 2 && !js_is_undefined(ARG(2))
                       ? clamp_index(js_to_number_value(ctx, ARG(2)), a->elem_count)
                       : a->elem_count;
    for (uint32_t i = start; i < end; i++)
        a->elems[i] = ARG(0);
    *r = tv;
    return true;
}

static bool am_concat(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    JsObject *out = js_array_new_cell(ctx, a->elem_count);
    if (!out)
        return oom(ctx, r);
    JsValue outv = js_value_from_cell(&out->gc);
    js_gc_protect(ctx->vm, &outv);
    bool ok = true;
    for (uint32_t i = 0; i < a->elem_count && ok; i++)
        ok = js_array_append(ctx->vm, out, a->elems[i]);
    for (int i = 0; i < argc && ok; i++) {
        JsValue v = args[i];
        if (js_is_object(v) && js_value_object(v)->obj_kind == JS_OBJ_ARRAY) {
            JsObject *o = js_value_object(v);
            for (uint32_t k = 0; k < o->elem_count && ok; k++)
                ok = js_array_append(ctx->vm, out, o->elems[k]);
        } else {
            ok = js_array_append(ctx->vm, out, v);
        }
    }
    js_gc_unprotect(ctx->vm, &outv);
    if (!ok)
        return oom(ctx, r);
    *r = outv;
    return true;
}

/* Calls a callback(elem, index, array) and returns its result. */
static bool call_cb(JsContext *ctx, JsValue cb, JsValue thisArg, JsValue elem,
                    uint32_t idx, JsValue arr, JsValue *out) {
    JsValue argv[3] = {elem, js_number(idx), arr};
    return js_call(ctx, cb, thisArg, argv, 3, out);
}

typedef enum { IT_MAP, IT_FILTER, IT_FOREACH, IT_SOME, IT_EVERY, IT_FIND, IT_FINDINDEX } IterKind;

static bool array_iterate(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                          JsValue *r, IterKind kind) {
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    if (!js_is_function(ARG(0)))
        return native_throw(ctx, r, "TypeError: callback is not a function");
    JsValue cb = ARG(0);
    JsValue thisArg = ARG(1);
    JsValue arrv = tv;
    js_gc_protect(ctx->vm, &arrv);
    js_gc_protect(ctx->vm, &cb);
    js_gc_protect(ctx->vm, &thisArg);

    JsObject *out = NULL;
    JsValue outv = js_undefined();
    if (kind == IT_MAP || kind == IT_FILTER) {
        out = js_array_new_cell(ctx, kind == IT_MAP ? a->elem_count : 0);
        if (!out) {
            js_gc_unprotect(ctx->vm, &thisArg);
            js_gc_unprotect(ctx->vm, &cb);
            js_gc_unprotect(ctx->vm, &arrv);
            return oom(ctx, r);
        }
        outv = js_value_from_cell(&out->gc);
    }
    js_gc_protect(ctx->vm, &outv);

    bool ok = true, failed = false;
    JsValue result = js_undefined();
    switch (kind) {
    case IT_SOME: result = js_bool(false); break;
    case IT_EVERY: result = js_bool(true); break;
    case IT_FIND: result = js_undefined(); break;
    case IT_FINDINDEX: result = js_number(-1); break;
    default: break;
    }
    for (uint32_t i = 0; i < a->elem_count && ok; i++) {
        JsValue elem = a->elems[i];
        JsValue cbres;
        if (!call_cb(ctx, cb, thisArg, elem, i, arrv, &cbres)) {
            *r = cbres;
            failed = true;
            ok = false;
            break;
        }
        switch (kind) {
        case IT_MAP:
            ok = js_array_append(ctx->vm, out, cbres);
            break;
        case IT_FILTER:
            if (js_to_boolean(cbres))
                ok = js_array_append(ctx->vm, out, elem);
            break;
        case IT_FOREACH:
            break;
        case IT_SOME:
            if (js_to_boolean(cbres)) {
                result = js_bool(true);
                i = a->elem_count;
            }
            break;
        case IT_EVERY:
            if (!js_to_boolean(cbres)) {
                result = js_bool(false);
                i = a->elem_count;
            }
            break;
        case IT_FIND:
            if (js_to_boolean(cbres)) {
                result = elem;
                i = a->elem_count;
            }
            break;
        case IT_FINDINDEX:
            if (js_to_boolean(cbres)) {
                result = js_number(i);
                i = a->elem_count;
            }
            break;
        }
    }
    if (kind == IT_MAP || kind == IT_FILTER)
        result = outv;
    js_gc_unprotect(ctx->vm, &outv);
    js_gc_unprotect(ctx->vm, &thisArg);
    js_gc_unprotect(ctx->vm, &cb);
    js_gc_unprotect(ctx->vm, &arrv);
    if (failed)
        return false;
    if (!ok)
        return oom(ctx, r);
    *r = result;
    return true;
}

static bool am_map(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return array_iterate(ctx, tv, a, c, r, IT_MAP);
}
static bool am_filter(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return array_iterate(ctx, tv, a, c, r, IT_FILTER);
}
static bool am_forEach(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return array_iterate(ctx, tv, a, c, r, IT_FOREACH);
}
static bool am_some(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return array_iterate(ctx, tv, a, c, r, IT_SOME);
}
static bool am_every(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return array_iterate(ctx, tv, a, c, r, IT_EVERY);
}
static bool am_find(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return array_iterate(ctx, tv, a, c, r, IT_FIND);
}
static bool am_findIndex(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return array_iterate(ctx, tv, a, c, r, IT_FINDINDEX);
}

static bool am_reduce(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    if (!js_is_function(ARG(0)))
        return native_throw(ctx, r, "TypeError: callback is not a function");
    JsValue cb = ARG(0), arrv = tv, acc;
    uint32_t i = 0;
    if (argc > 1) {
        acc = ARG(1);
    } else {
        if (a->elem_count == 0)
            return native_throw(ctx, r, "TypeError: reduce of empty array with no initial value");
        acc = a->elems[0];
        i = 1;
    }
    js_gc_protect(ctx->vm, &arrv);
    js_gc_protect(ctx->vm, &cb);
    js_gc_protect(ctx->vm, &acc);
    bool failed = false;
    for (; i < a->elem_count; i++) {
        JsValue argv[4] = {acc, a->elems[i], js_number(i), arrv};
        JsValue res;
        if (!js_call(ctx, cb, js_undefined(), argv, 4, &res)) {
            *r = res;
            failed = true;
            break;
        }
        acc = res;
    }
    JsValue out = acc;
    js_gc_unprotect(ctx->vm, &acc);
    js_gc_unprotect(ctx->vm, &cb);
    js_gc_unprotect(ctx->vm, &arrv);
    if (failed)
        return false;
    *r = out;
    return true;
}

/* stable binary insertion sort; comparator optional */
static bool am_sort(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    JsValue cmp = ARG(0);
    bool has_cmp = js_is_function(cmp);
    if (argc > 0 && !has_cmp && !js_is_undefined(cmp))
        return native_throw(ctx, r, "TypeError: comparator is not a function");
    JsValue arrv = tv, key = js_undefined();
    js_gc_protect(ctx->vm, &arrv);
    js_gc_protect(ctx->vm, &cmp);
    js_gc_protect(ctx->vm, &key);
    bool failed = false;

    for (uint32_t i = 1; i < a->elem_count && !failed; i++) {
        key = a->elems[i];
        uint32_t lo = 0, hi = i;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            int c; /* c>0 means a->elems[mid] should come after key */
            /* A user comparator (or a user toString) may mutate — even shrink —
             * the array, freeing the slot `mid` used to name. Re-read against
             * the current elem_count so we never touch a freed element. */
            JsValue midv = mid < a->elem_count ? a->elems[mid] : js_undefined();
            if (has_cmp) {
                JsValue argv[2] = {midv, key};
                JsValue res;
                if (!js_call(ctx, cmp, js_undefined(), argv, 2, &res)) {
                    *r = res;
                    failed = true;
                    break;
                }
                double d = js_to_number_value(ctx, res);
                c = d > 0 ? 1 : (d < 0 ? -1 : 0);
            } else {
                JsString *sa = js_to_string_cell(ctx, midv, 0);
                JsString *sk = js_to_string_cell(ctx, key, 0);
                if (!sa || !sk) {
                    failed = true;
                    oom(ctx, r);
                    break;
                }
                uint32_t n = sa->length < sk->length ? sa->length : sk->length, k = 0;
                c = 0;
                for (; k < n; k++)
                    if (sa->units[k] != sk->units[k]) {
                        c = sa->units[k] < sk->units[k] ? -1 : 1;
                        break;
                    }
                if (c == 0 && sa->length != sk->length)
                    c = sa->length < sk->length ? -1 : 1;
            }
            if (c > 0)
                hi = mid;
            else
                lo = mid + 1;
        }
        if (failed)
            break;
        /* If the comparator shrank the array below i, the insertion slot no
         * longer exists — skip placing this key rather than writing OOB. */
        if (i >= a->elem_count)
            continue;
        for (uint32_t j = i; j > lo; j--)
            a->elems[j] = a->elems[j - 1];
        a->elems[lo] = key;
    }
    js_gc_unprotect(ctx->vm, &key);
    js_gc_unprotect(ctx->vm, &cmp);
    js_gc_unprotect(ctx->vm, &arrv);
    if (failed)
        return false;
    *r = tv;
    return true;
}

static bool am_flat(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    double depth = argc > 0 && !js_is_undefined(ARG(0)) ? to_int(js_to_number_value(ctx, ARG(0))) : 1;
    JsObject *out = js_array_new_cell(ctx, 0);
    if (!out)
        return oom(ctx, r);
    JsValue outv = js_value_from_cell(&out->gc);
    js_gc_protect(ctx->vm, &outv);
    /* single-level or fixed-depth flatten via explicit recursion over arrays */
    JsObject *stack_arr[16];
    uint32_t stack_idx[16];
    double stack_depth[16];
    int sp = 0;
    stack_arr[0] = a;
    stack_idx[0] = 0;
    stack_depth[0] = depth;
    bool ok = true;
    while (sp >= 0 && ok) {
        JsObject *cur = stack_arr[sp];
        if (stack_idx[sp] >= cur->elem_count) {
            sp--;
            continue;
        }
        JsValue el = cur->elems[stack_idx[sp]++];
        double dd = stack_depth[sp];
        if (dd > 0 && js_is_object(el) && js_value_object(el)->obj_kind == JS_OBJ_ARRAY &&
            sp < 15) {
            sp++;
            stack_arr[sp] = js_value_object(el);
            stack_idx[sp] = 0;
            stack_depth[sp] = dd - 1;
        } else {
            ok = js_array_append(ctx->vm, out, el);
        }
    }
    js_gc_unprotect(ctx->vm, &outv);
    if (!ok)
        return oom(ctx, r);
    *r = outv;
    return true;
}

static bool am_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
    JsString *s = js_to_string_cell(ctx, tv, 0);
    if (!s)
        return oom(ctx, r);
    *r = js_value_from_cell(&s->gc);
    return true;
}

/* =====================================================================
 * Number methods
 * ===================================================================== */

static bool nm_toFixed(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    double x = js_to_number_value(ctx, tv);
    double dd = to_int(js_to_number_value(ctx, ARG(0)));
    if (dd < 0 || dd > 100)
        return native_throw(ctx, r, "RangeError: toFixed() digits out of range");
    int d = (int)dd;
    /* x < 1e21 gives <=21 integer digits; plus up to 100 fraction digits, a
     * sign, a '.', and a NUL. Size both scratch buffers for the worst case so
     * neither the digit extraction nor the assembly can overrun. */
    char buf[160];
    if (x != x) {
        *r = js_value_from_cell(&js_ascii_cell(ctx->vm, "NaN")->gc);
        return true;
    }
    bool neg = x < 0;
    if (neg)
        x = -x;
    if (x >= 1e21) {
        JsString *s = js_number_to_string(ctx->vm, neg ? -x : x);
        if (!s)
            return oom(ctx, r);
        *r = js_value_from_cell(&s->gc);
        return true;
    }
    double scale = 1.0;
    for (int i = 0; i < d; i++)
        scale *= 10.0;
    double scaled = __builtin_floor(x * scale + 0.5);
    /* build digits of `scaled` (an integer) */
    char digs[160];
    int nd = 0;
    if (scaled == 0)
        digs[nd++] = '0';
    while (scaled >= 1 && nd < (int)sizeof(digs) - 1) {
        double q = __builtin_floor(scaled / 10.0);
        int dig = (int)(scaled - q * 10.0);
        digs[nd++] = (char)('0' + dig);
        scaled = q;
    }
    while (nd <= d)
        digs[nd++] = '0'; /* ensure enough digits for the fraction */
    int bi = 0;
    if (neg)
        buf[bi++] = '-';
    int int_digits = nd - d;
    for (int i = 0; i < int_digits; i++)
        buf[bi++] = digs[nd - 1 - i];
    if (d > 0) {
        buf[bi++] = '.';
        for (int i = 0; i < d; i++)
            buf[bi++] = digs[d - 1 - i];
    }
    buf[bi] = 0;
    JsString *s = js_ascii_cell(ctx->vm, buf);
    if (!s)
        return oom(ctx, r);
    *r = js_value_from_cell(&s->gc);
    return true;
}

static bool nm_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    double x = js_to_number_value(ctx, tv);
    /* Clamp to [0,37] before the cast (avoids UB on Infinity/NaN/huge); an
     * out-of-range radix lands outside 2..36 and is rejected just below. */
    int radix = argc > 0 && !js_is_undefined(ARG(0))
                    ? (int)to_integer_clamped(js_to_number_value(ctx, ARG(0)), 0, 37)
                    : 10;
    if (radix < 2 || radix > 36)
        return native_throw(ctx, r, "RangeError: toString() radix must be 2..36");
    if (radix == 10 || x != x || x == __builtin_inf() || x == -__builtin_inf()) {
        JsString *s = js_number_to_string(ctx->vm, x);
        if (!s)
            return oom(ctx, r);
        *r = js_value_from_cell(&s->gc);
        return true;
    }
    static const char *digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    /* A finite double is < 2^1024, so its integer part is at most 1024 binary
     * digits (radix 2 is the worst case); size the scratch/output for that plus
     * a sign, '.', the ~20 fraction digits, and a NUL. */
    char buf[1152];
    int bi = 0;
    bool neg = x < 0;
    if (neg)
        x = -x;
    double ip = __builtin_floor(x);
    double fp = x - ip;
    char tmp[1088];
    int ti = 0;
    if (ip == 0)
        tmp[ti++] = '0';
    while (ip >= 1 && ti < (int)sizeof(tmp) - 1) {
        double q = __builtin_floor(ip / radix);
        int dig = (int)(ip - q * radix);
        tmp[ti++] = digits[dig];
        ip = q;
    }
    if (neg)
        buf[bi++] = '-';
    for (int i = 0; i < ti; i++)
        buf[bi++] = tmp[ti - 1 - i];
    if (fp > 0) {
        buf[bi++] = '.';
        for (int i = 0; i < 20 && fp > 0 && bi < (int)sizeof(buf) - 8; i++) {
            fp *= radix;
            int dig = (int)__builtin_floor(fp);
            buf[bi++] = digits[dig];
            fp -= dig;
        }
    }
    buf[bi] = 0;
    JsString *s = js_ascii_cell(ctx->vm, buf);
    if (!s)
        return oom(ctx, r);
    *r = js_value_from_cell(&s->gc);
    return true;
}

/* =====================================================================
 * Math
 * ===================================================================== */

static double m_abs(double x) { return x < 0 ? -x : x; }
static double m_sign(double x) { return x != x ? x : (x > 0 ? 1 : (x < 0 ? -1 : x)); }
static double m_cbrt(double x) {
    if (x == 0.0 || x != x || x == __builtin_inf() || x == -__builtin_inf())
        return x;
    double a = x < 0 ? -x : x;
    double y = js_k_pow(a, 1.0 / 3);
    /* two Newton steps nail perfect cubes to full precision */
    y = y - (y * y * y - a) / (3.0 * y * y);
    y = y - (y * y * y - a) / (3.0 * y * y);
    return x < 0 ? -y : y;
}

static bool math_min(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    double best = __builtin_inf();
    for (int i = 0; i < argc; i++) {
        double v = js_to_number_value(ctx, args[i]);
        if (v != v) {
            *r = js_number(v);
            return true;
        }
        if (v < best || (v == 0 && best == 0 && 1.0 / v < 0))
            best = v;
    }
    *r = js_number(best);
    return true;
}

static bool math_max(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    double best = -__builtin_inf();
    for (int i = 0; i < argc; i++) {
        double v = js_to_number_value(ctx, args[i]);
        if (v != v) {
            *r = js_number(v);
            return true;
        }
        if (v > best || (v == 0 && best == 0 && 1.0 / v > 0))
            best = v;
    }
    *r = js_number(best);
    return true;
}

static bool math_hypot(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    /* Spec order: any ±Infinity argument yields +Infinity (even alongside a
     * NaN); otherwise any NaN yields NaN. Coerce every argument first (each is
     * an observable ToNumber). */
    double inf = __builtin_inf();
    bool has_inf = false, has_nan = false;
    double max = 0;
    /* two passes need the coerced values, but ToNumber may run user code, so
     * coerce once into a small buffer growing on demand */
    double stackv[8];
    double *vals = stackv;
    if (argc > 8) {
        vals = js_realloc_raw(ctx->vm, NULL, 0, (size_t)argc * sizeof(double));
        if (!vals)
            return oom(ctx, r);
    }
    for (int i = 0; i < argc; i++) {
        double v = js_to_number_value(ctx, args[i]);
        vals[i] = v;
        if (v == inf || v == -inf)
            has_inf = true;
        else if (v != v)
            has_nan = true;
        else {
            double av = v < 0 ? -v : v;
            if (av > max)
                max = av;
        }
    }
    double result;
    if (has_inf)
        result = inf;
    else if (has_nan)
        result = __builtin_nan("");
    else if (max == 0)
        result = 0;
    else {
        /* Scale by the largest magnitude to avoid overflow/underflow in the
         * squares (hypot(1e200, 1e200) must not overflow to Infinity). */
        double sum = 0;
        for (int i = 0; i < argc; i++) {
            double s = vals[i] / max;
            sum += s * s;
        }
        result = max * __builtin_sqrt(sum);
    }
    if (vals != stackv)
        js_realloc_raw(ctx->vm, vals, (size_t)argc * sizeof(double), 0);
    *r = js_number(result);
    return true;
}

static bool math_pow(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_number(js_k_pow(js_to_number_value(ctx, ARG(0)), js_to_number_value(ctx, ARG(1))));
    return true;
}

static bool math_atan2(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_number(js_k_atan2(js_to_number_value(ctx, ARG(0)), js_to_number_value(ctx, ARG(1))));
    return true;
}

static bool math_random(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    (void)args;
    (void)argc;
    uint64_t x = ctx->vm->rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    ctx->vm->rng_state = x;
    *r = js_number((double)(x >> 11) * (1.0 / 9007199254740992.0));
    return true;
}

/* =====================================================================
 * JSON
 * ===================================================================== */

static void json_quote(StrBuf *sb, const uint16_t *u, uint32_t n) {
    sb_unit(sb, '"');
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = u[i];
        switch (c) {
        case '"': sb_ascii(sb, "\\\""); break;
        case '\\': sb_ascii(sb, "\\\\"); break;
        case '\n': sb_ascii(sb, "\\n"); break;
        case '\r': sb_ascii(sb, "\\r"); break;
        case '\t': sb_ascii(sb, "\\t"); break;
        case '\b': sb_ascii(sb, "\\b"); break;
        case '\f': sb_ascii(sb, "\\f"); break;
        default:
            if (c < 0x20) {
                char h[7];
                static const char *hex = "0123456789abcdef";
                h[0] = '\\'; h[1] = 'u'; h[2] = '0'; h[3] = '0';
                h[4] = hex[(c >> 4) & 0xF]; h[5] = hex[c & 0xF]; h[6] = 0;
                sb_ascii(sb, h);
            } else {
                sb_unit(sb, c);
            }
        }
    }
    sb_unit(sb, '"');
}

/* Ancestor chain of objects/arrays currently being serialized, threaded on the
 * C stack so JSON.stringify can detect a cycle and throw instead of recursing. */
typedef struct JsonSeen {
    const JsObject *o;
    const struct JsonSeen *prev;
} JsonSeen;

static bool json_str(JsContext *ctx, StrBuf *sb, JsValue v, const uint16_t *indent,
                     uint32_t indent_len, uint32_t depth, const JsonSeen *seen,
                     bool *emitted);

static void json_newline_indent(StrBuf *sb, const uint16_t *indent, uint32_t indent_len,
                                uint32_t depth) {
    if (indent_len == 0)
        return;
    sb_unit(sb, '\n');
    for (uint32_t i = 0; i < depth; i++)
        sb_units(sb, indent, indent_len);
}

static bool json_str(JsContext *ctx, StrBuf *sb, JsValue v, const uint16_t *indent,
                     uint32_t indent_len, uint32_t depth, const JsonSeen *seen,
                     bool *emitted) {
    *emitted = true;
    if (depth > 200) {
        sb->oom = true;
        return false;
    }
    if (js_is_undefined(v) || js_is_function(v)) {
        *emitted = false;
        return true;
    }
    if (js_is_null(v)) {
        sb_ascii(sb, "null");
        return true;
    }
    if (js_is_bool(v)) {
        sb_ascii(sb, js_get_bool(v) ? "true" : "false");
        return true;
    }
    if (js_is_number(v)) {
        double d = js_get_number(v);
        if (d != d || d == __builtin_inf() || d == -__builtin_inf())
            sb_ascii(sb, "null");
        else {
            JsString *s = js_number_to_string(ctx->vm, d);
            if (!s) {
                sb->oom = true;
                return false;
            }
            sb_units(sb, s->units, s->length);
        }
        return true;
    }
    if (js_is_string(v)) {
        JsString *s = js_value_string(v);
        json_quote(sb, s->units, s->length);
        return true;
    }
    JsObject *o = js_value_object(v);
    /* Cycle detection: a container that is already an ancestor on the current
     * serialization path makes the structure circular — throw, as real JS does,
     * rather than recurse to the depth cap. */
    for (const JsonSeen *s = seen; s; s = s->prev) {
        if (s->o == o) {
            sb->circular = true;
            return false;
        }
    }
    JsonSeen node = {o, seen};
    if (o->obj_kind == JS_OBJ_ARRAY) {
        sb_unit(sb, '[');
        for (uint32_t i = 0; i < o->elem_count; i++) {
            if (i)
                sb_unit(sb, ',');
            json_newline_indent(sb, indent, indent_len, depth + 1);
            bool em;
            if (!json_str(ctx, sb, o->elems[i], indent, indent_len, depth + 1, &node, &em))
                return false;
            if (!em)
                sb_ascii(sb, "null");
        }
        if (o->elem_count)
            json_newline_indent(sb, indent, indent_len, depth);
        sb_unit(sb, ']');
        return true;
    }
    /* plain object: own string keys (hash order — documented deviation).
     * Values are written straight into `sb`; a property whose value emits
     * nothing (undefined/function) is undone by rewinding sb->len, so no
     * throwaway per-property buffer is needed. */
    sb_unit(sb, '{');
    bool first = true;
    for (uint32_t i = 0; i < o->props.capacity; i++) {
        JsMapEntry *e = &o->props.entries[i];
        if (!e->key || e->key == JS_MAP_TOMBSTONE)
            continue;
        uint32_t mark = sb->len; /* rewind point if the value emits nothing */
        if (!first)
            sb_unit(sb, ',');
        json_newline_indent(sb, indent, indent_len, depth + 1);
        json_quote(sb, e->key->units, e->key->length);
        sb_unit(sb, ':');
        if (indent_len)
            sb_unit(sb, ' ');
        bool em;
        if (!json_str(ctx, sb, e->value, indent, indent_len, depth + 1, &node, &em))
            return false;
        if (!em) {
            sb->len = mark; /* drop the comma/key/colon we just wrote */
            continue;
        }
        first = false;
    }
    if (!first)
        json_newline_indent(sb, indent, indent_len, depth);
    sb_unit(sb, '}');
    return true;
}

static bool json_stringify(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    uint16_t indent[10];
    uint32_t indent_len = 0;
    JsValue space = ARG(2);
    if (js_is_number(space)) {
        /* space count is clamped to [0,10]; route through to_integer_clamped so
         * an Infinity/NaN/huge argument can't UB on the cast to int */
        int n = (int)to_integer_clamped(js_to_number_value(ctx, space), 0, 10);
        for (int i = 0; i < n; i++)
            indent[indent_len++] = ' ';
    } else if (js_is_string(space)) {
        JsString *s = js_value_string(space);
        for (uint32_t i = 0; i < s->length && i < 10; i++)
            indent[indent_len++] = s->units[i];
    }
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    bool em;
    if (!json_str(ctx, &sb, ARG(0), indent, indent_len, 0, NULL, &em)) {
        bool circular = sb.circular;
        sb_free(&sb);
        if (circular)
            return native_throw(ctx, r, "TypeError: Converting circular structure to JSON");
        return oom(ctx, r);
    }
    if (!em) {
        sb_free(&sb);
        *r = js_undefined();
        return true;
    }
    *r = sb_finish(&sb);
    return js_is_undefined(*r) ? oom(ctx, r) : true;
}

/* --- JSON.parse --- */

typedef struct {
    JsContext *ctx;
    const uint16_t *u;
    uint32_t len, pos;
    bool error;
    int depth;
} JsonP;

/* Nesting cap for JSON.parse — bounds C-stack recursion on hostile input like
 * `[`.repeat(N). Mirrors JSON.stringify's own depth limit. */
#define JSON_PARSE_MAX_DEPTH 200

static void json_ws(JsonP *p) {
    while (p->pos < p->len) {
        uint16_t c = p->u[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->pos++;
        else
            break;
    }
}

static bool json_value(JsonP *p, JsValue *out);
static bool json_value_body(JsonP *p, JsValue *out);

static bool json_lit(JsonP *p, const char *word, JsValue v, JsValue *out) {
    for (uint32_t i = 0; word[i]; i++) {
        if (p->pos + i >= p->len || p->u[p->pos + i] != (uint16_t)word[i]) {
            p->error = true;
            return false;
        }
    }
    uint32_t wl = 0;
    while (word[wl])
        wl++;
    p->pos += wl;
    *out = v;
    return true;
}

/* Strict JSON number grammar (RFC 8259):
 *   number = [ "-" ] int [ frac ] [ exp ]
 *   int    = "0" / (digit1-9 *digit)     -- no leading zeros
 *   frac   = "." 1*digit                 -- no bare trailing dot
 *   exp    = ("e"/"E") ["+"/"-"] 1*digit -- no dangling exponent
 * Rejects the lenient forms the old greedy scan let through: 01, 1., 1e,
 * --5, 1.2.3 (the trailing ".3" is left for the caller to reject). */
static bool json_number(JsonP *p, JsValue *out) {
    uint32_t start = p->pos;
#define JN_DIGIT(pos) ((pos) < p->len && p->u[(pos)] >= '0' && p->u[(pos)] <= '9')
    if (p->pos < p->len && p->u[p->pos] == '-')
        p->pos++;
    if (!JN_DIGIT(p->pos)) {
        p->error = true;
        return false;
    }
    if (p->u[p->pos] == '0') {
        p->pos++; /* a leading zero stands alone — no further integer digits */
    } else {
        while (JN_DIGIT(p->pos))
            p->pos++;
    }
    if (p->pos < p->len && p->u[p->pos] == '.') {
        p->pos++;
        if (!JN_DIGIT(p->pos)) {
            p->error = true;
            return false;
        }
        while (JN_DIGIT(p->pos))
            p->pos++;
    }
    if (p->pos < p->len && (p->u[p->pos] == 'e' || p->u[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->u[p->pos] == '+' || p->u[p->pos] == '-'))
            p->pos++;
        if (!JN_DIGIT(p->pos)) {
            p->error = true;
            return false;
        }
        while (JN_DIGIT(p->pos))
            p->pos++;
    }
#undef JN_DIGIT
    *out = js_number(js_units_to_number(p->u + start, p->pos - start));
    return true;
}

static bool json_string_raw(JsonP *p, JsString **out) {
    p->pos++; /* opening quote */
    StrBuf sb;
    sb_init(&sb, p->ctx->vm);
    while (p->pos < p->len) {
        uint16_t c = p->u[p->pos++];
        if (c == '"') {
            JsString *s = js_string_cell_new(p->ctx->vm, sb.u, sb.len);
            sb_free(&sb);
            if (!s) {
                p->error = true;
                return false;
            }
            *out = s;
            return true;
        }
        if (c == '\\' && p->pos < p->len) {
            uint16_t e = p->u[p->pos++];
            switch (e) {
            case '"': sb_unit(&sb, '"'); break;
            case '\\': sb_unit(&sb, '\\'); break;
            case '/': sb_unit(&sb, '/'); break;
            case 'n': sb_unit(&sb, '\n'); break;
            case 't': sb_unit(&sb, '\t'); break;
            case 'r': sb_unit(&sb, '\r'); break;
            case 'b': sb_unit(&sb, '\b'); break;
            case 'f': sb_unit(&sb, '\f'); break;
            case 'u': {
                uint32_t cp = 0;
                for (int i = 0; i < 4 && p->pos < p->len; i++) {
                    uint16_t h = p->u[p->pos++];
                    int d = h >= '0' && h <= '9' ? h - '0'
                            : h >= 'a' && h <= 'f' ? h - 'a' + 10
                            : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                    if (d < 0) {
                        p->error = true;
                        sb_free(&sb);
                        return false;
                    }
                    cp = cp * 16 + (uint32_t)d;
                }
                sb_unit(&sb, (uint16_t)cp);
                break;
            }
            default:
                p->error = true;
                sb_free(&sb);
                return false;
            }
        } else if (c == '\\') {
            p->error = true;
            sb_free(&sb);
            return false;
        } else {
            sb_unit(&sb, c);
        }
    }
    p->error = true;
    sb_free(&sb);
    return false;
}

/* Depth-guarded entry: bounds recursion before descending into json_value_body,
 * which recurses back through json_value for nested arrays/objects. */
static bool json_value(JsonP *p, JsValue *out) {
    if (++p->depth > JSON_PARSE_MAX_DEPTH) {
        p->error = true;
        p->depth--;
        return false;
    }
    bool ok = json_value_body(p, out);
    p->depth--;
    return ok;
}

static bool json_value_body(JsonP *p, JsValue *out) {
    json_ws(p);
    if (p->pos >= p->len) {
        p->error = true;
        return false;
    }
    uint16_t c = p->u[p->pos];
    if (c == '{') {
        p->pos++;
        JsValue ov = js_object_new(p->ctx);
        if (!js_is_object(ov)) {
            p->error = true;
            return false;
        }
        JsObject *o = js_value_object(ov);
        js_gc_protect(p->ctx->vm, &ov);
        json_ws(p);
        bool okc = true;
        if (p->pos < p->len && p->u[p->pos] == '}') {
            p->pos++;
        } else {
            for (;;) {
                json_ws(p);
                if (p->pos >= p->len || p->u[p->pos] != '"') {
                    p->error = true;
                    okc = false;
                    break;
                }
                JsString *key;
                if (!json_string_raw(p, &key)) {
                    okc = false;
                    break;
                }
                JsString *ikey = js_intern_cell(p->ctx->vm, key);
                if (!ikey) {
                    p->error = true;
                    okc = false;
                    break;
                }
                /* root the key across the value parse (a GC safe point) */
                JsValue keyv = js_value_from_cell(&ikey->gc);
                js_gc_protect(p->ctx->vm, &keyv);
                json_ws(p);
                if (p->pos >= p->len || p->u[p->pos] != ':') {
                    js_gc_unprotect(p->ctx->vm, &keyv);
                    p->error = true;
                    okc = false;
                    break;
                }
                p->pos++;
                JsValue val;
                if (!json_value(p, &val)) {
                    js_gc_unprotect(p->ctx->vm, &keyv);
                    okc = false;
                    break;
                }
                bool set_ok = js_map_set(p->ctx->vm, &o->props,
                                         js_value_string(keyv), val);
                js_gc_unprotect(p->ctx->vm, &keyv);
                if (!set_ok) {
                    p->error = true;
                    okc = false;
                    break;
                }
                json_ws(p);
                if (p->pos < p->len && p->u[p->pos] == ',') {
                    p->pos++;
                    continue;
                }
                if (p->pos < p->len && p->u[p->pos] == '}') {
                    p->pos++;
                    break;
                }
                p->error = true;
                okc = false;
                break;
            }
        }
        js_gc_unprotect(p->ctx->vm, &ov);
        if (!okc)
            return false;
        *out = ov;
        return true;
    }
    if (c == '[') {
        p->pos++;
        JsObject *a = js_array_new_cell(p->ctx, 0);
        if (!a) {
            p->error = true;
            return false;
        }
        JsValue av = js_value_from_cell(&a->gc);
        js_gc_protect(p->ctx->vm, &av);
        json_ws(p);
        bool okc = true;
        if (p->pos < p->len && p->u[p->pos] == ']') {
            p->pos++;
        } else {
            for (;;) {
                JsValue el;
                if (!json_value(p, &el)) {
                    okc = false;
                    break;
                }
                if (!js_array_append(p->ctx->vm, a, el)) {
                    p->error = true;
                    okc = false;
                    break;
                }
                json_ws(p);
                if (p->pos < p->len && p->u[p->pos] == ',') {
                    p->pos++;
                    continue;
                }
                if (p->pos < p->len && p->u[p->pos] == ']') {
                    p->pos++;
                    break;
                }
                p->error = true;
                okc = false;
                break;
            }
        }
        js_gc_unprotect(p->ctx->vm, &av);
        if (!okc)
            return false;
        *out = av;
        return true;
    }
    if (c == '"') {
        JsString *s;
        if (!json_string_raw(p, &s))
            return false;
        *out = js_value_from_cell(&s->gc);
        return true;
    }
    if (c == 't')
        return json_lit(p, "true", js_bool(true), out);
    if (c == 'f')
        return json_lit(p, "false", js_bool(false), out);
    if (c == 'n')
        return json_lit(p, "null", js_null(), out);
    if (c == '-' || (c >= '0' && c <= '9'))
        return json_number(p, out);
    p->error = true;
    return false;
}

static bool json_parse(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsString *s = arg_string(ctx, ARG(0));
    if (!s)
        return oom(ctx, r);
    JsValue sv = js_value_from_cell(&s->gc);
    js_gc_protect(ctx->vm, &sv);
    JsonP p = {ctx, s->units, s->length, 0, false, 0};
    JsValue out;
    bool ok = json_value(&p, &out);
    if (ok) {
        json_ws(&p);
        if (p.pos != p.len)
            ok = false;
    }
    js_gc_unprotect(ctx->vm, &sv);
    if (!ok || p.error)
        return native_throw(ctx, r, "SyntaxError: invalid JSON");
    *r = out;
    return true;
}

/* =====================================================================
 * Object / Array / Number / String statics + globals
 * ===================================================================== */

static bool obj_keys_impl(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                          JsValue *r, int mode /*0 keys,1 values,2 entries*/) {
    (void)tv;
    JsValue v = ARG(0);
    JsObject *out = js_array_new_cell(ctx, 0);
    if (!out)
        return oom(ctx, r);
    JsValue outv = js_value_from_cell(&out->gc);
    js_gc_protect(ctx->vm, &outv);
    bool ok = true;
    if (js_is_object(v)) {
        JsObject *o = js_value_object(v);
        if (o->obj_kind == JS_OBJ_ARRAY) {
            for (uint32_t i = 0; i < o->elem_count && ok; i++) {
                JsValue entry;
                if (mode == 0) {
                    JsString *k = js_number_to_string(ctx->vm, i);
                    entry = k ? js_value_from_cell(&k->gc) : js_undefined();
                    ok = k != NULL;
                } else if (mode == 1) {
                    entry = o->elems[i];
                } else {
                    JsObject *pair = js_array_new_cell(ctx, 2);
                    if (!pair) { ok = false; break; }
                    /* Root pair across the key allocation before its slots are
                     * written — js_number_to_string can otherwise collect it. */
                    JsValue pairv = js_value_from_cell(&pair->gc);
                    js_gc_protect(ctx->vm, &pairv);
                    JsString *k = js_number_to_string(ctx->vm, i);
                    if (!k) { js_gc_unprotect(ctx->vm, &pairv); ok = false; break; }
                    pair->elems[0] = js_value_from_cell(&k->gc);
                    pair->elems[1] = o->elems[i];
                    pair->elem_count = 2;
                    js_gc_unprotect(ctx->vm, &pairv);
                    entry = pairv;
                }
                if (ok)
                    ok = js_array_append(ctx->vm, out, entry);
            }
        } else {
            for (uint32_t i = 0; i < o->props.capacity && ok; i++) {
                JsMapEntry *e = &o->props.entries[i];
                if (!e->key || e->key == JS_MAP_TOMBSTONE)
                    continue;
                JsValue entry;
                if (mode == 0) {
                    entry = js_value_from_cell(&e->key->gc);
                } else if (mode == 1) {
                    entry = e->value;
                } else {
                    JsObject *pair = js_array_new_cell(ctx, 2);
                    if (!pair) { ok = false; break; }
                    pair->elems[0] = js_value_from_cell(&e->key->gc);
                    pair->elems[1] = e->value;
                    pair->elem_count = 2;
                    entry = js_value_from_cell(&pair->gc);
                }
                ok = js_array_append(ctx->vm, out, entry);
            }
        }
    }
    js_gc_unprotect(ctx->vm, &outv);
    if (!ok)
        return oom(ctx, r);
    *r = outv;
    return true;
}

static bool obj_keys(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return obj_keys_impl(ctx, tv, a, c, r, 0);
}
static bool obj_values(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return obj_keys_impl(ctx, tv, a, c, r, 1);
}
static bool obj_entries(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return obj_keys_impl(ctx, tv, a, c, r, 2);
}

static bool obj_assign(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue target = ARG(0);
    if (!js_is_object(target))
        return native_throw(ctx, r, "TypeError: Object.assign target must be an object");
    js_gc_protect(ctx->vm, &target);
    bool ok = true;
    for (int i = 1; i < argc && ok; i++) {
        if (js_is_object(args[i]))
            ok = js_spread_into_object(ctx, js_value_object(target), args[i]);
    }
    js_gc_unprotect(ctx->vm, &target);
    if (!ok)
        return oom(ctx, r);
    *r = target;
    return true;
}

static bool obj_freeze(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)ctx;
    (void)tv;
    (void)argc;
    *r = ARG(0); /* freeze semantics not enforced (documented) */
    return true;
}

static bool obj_fromEntries(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue src = ARG(0);
    if (!js_is_object(src) || js_value_object(src)->obj_kind != JS_OBJ_ARRAY)
        return native_throw(ctx, r, "TypeError: Object.fromEntries expects an array of pairs");
    JsValue outv = js_object_new(ctx);
    if (!js_is_object(outv))
        return oom(ctx, r);
    JsObject *out = js_value_object(outv);
    js_gc_protect(ctx->vm, &outv);
    JsObject *arr = js_value_object(src);
    bool ok = true;
    for (uint32_t i = 0; i < arr->elem_count && ok; i++) {
        JsValue pair = arr->elems[i];
        if (!js_is_object(pair) || js_value_object(pair)->obj_kind != JS_OBJ_ARRAY) {
            ok = false;
            break;
        }
        JsObject *pa = js_value_object(pair);
        JsValue k = pa->elem_count > 0 ? pa->elems[0] : js_undefined();
        JsValue val = pa->elem_count > 1 ? pa->elems[1] : js_undefined();
        JsString *ks = js_to_string_cell(ctx, k, 0);
        JsString *iks = ks ? js_intern_cell(ctx->vm, ks) : NULL;
        ok = iks && js_map_set(ctx->vm, &out->props, iks, val);
    }
    js_gc_unprotect(ctx->vm, &outv);
    if (!ok)
        return native_throw(ctx, r, "TypeError: invalid entry in Object.fromEntries");
    *r = outv;
    return true;
}

/* Shared by Object.hasOwn(obj, key) and Object.prototype.hasOwnProperty
 * (this.hasOwnProperty(key)) below. *out is only meaningful when this
 * returns true; false means OOM (caller throws). */
static bool object_has_own_key(JsContext *ctx, JsObject *ob, JsValue key, bool *out) {
    uint32_t idx;
    JsString *ks = js_to_string_cell(ctx, key, 0);
    if (!ks)
        return false;
    if (ob->obj_kind == JS_OBJ_ARRAY) {
        /* index check; bounds-check before the cast: (uint32_t)d is UB for
         * d outside [0, UINT32_MAX], e.g. Object.hasOwn(arr, "1e20") */
        double d = js_units_to_number(ks->units, ks->length);
        if (d >= 0 && d < 4294967296.0 && d == __builtin_floor(d)) {
            idx = (uint32_t)d;
            if (idx < ob->elem_count) {
                *out = true;
                return true;
            }
        }
    }
    JsString *ik = js_intern_cell(ctx->vm, ks);
    if (!ik)
        return false;
    js_map_get(&ob->props, ik, out);
    return true;
}

static bool obj_hasOwn(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue o = ARG(0);
    if (!js_is_object(o)) {
        *r = js_bool(false);
        return true;
    }
    bool found;
    if (!object_has_own_key(ctx, js_value_object(o), ARG(1), &found))
        return oom(ctx, r);
    *r = js_bool(found);
    return true;
}

/* ---- Object.prototype: hasOwnProperty/toString/valueOf ----
 * Reached via the normal own-prop-miss -> [[Prototype]] walk (see
 * ctx->object_proto in lamassu_internal.h) for any object whose own chain
 * doesn't shadow these first — Array/Map/Set/Date/RegExp all define their
 * own `toString`, so this generic one is only ever actually reached for
 * plain objects (and anything else that doesn't override it). */

static bool objp_hasOwnProperty(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                                JsValue *r) {
    if (!js_is_object(tv)) {
        *r = js_bool(false);
        return true;
    }
    bool found;
    if (!object_has_own_key(ctx, js_value_object(tv), ARG(0), &found))
        return oom(ctx, r);
    *r = js_bool(found);
    return true;
}

static bool objp_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    (void)args;
    (void)argc;
    JsString *s = js_ascii_cell(ctx->vm, "[object Object]");
    if (!s)
        return oom(ctx, r);
    *r = js_value_from_cell(&s->gc);
    return true;
}

static bool objp_valueOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)ctx;
    (void)args;
    (void)argc;
    *r = tv;
    return true;
}

/* Functions (closures/natives) aren't JS_KIND_OBJECT and so have no [[Prototype]]
 * slot of their own in this engine (no Function.prototype either) — reported
 * as null rather than throwing, a pragmatic fallback rather than a full
 * per-kind spec match. */
static bool obj_getPrototypeOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                               JsValue *r) {
    (void)tv;
    JsValue v = ARG(0);
    if (js_is_undefined(v) || js_is_null(v))
        return native_throw(ctx, r, "TypeError: Object.getPrototypeOf called on null or undefined");
    if (js_is_object(v)) {
        JsValue proto = js_value_object(v)->proto;
        *r = js_is_object(proto) ? proto : js_null();
        return true;
    }
    *r = js_null();
    return true;
}

static bool obj_setPrototypeOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                               JsValue *r) {
    (void)tv;
    JsValue v = ARG(0), proto = ARG(1);
    if (js_is_undefined(v) || js_is_null(v))
        return native_throw(ctx, r, "TypeError: Object.setPrototypeOf called on null or undefined");
    if (!js_is_object(proto) && !js_is_null(proto))
        return native_throw(ctx, r, "TypeError: prototype must be an object or null");
    if (js_is_object(v))
        js_value_object(v)->proto = js_is_object(proto) ? proto : js_undefined();
    *r = v;
    return true;
}

/* Object(...): callable with or without `new` (see JS_OP_NEW's native-
 * constructor dispatch), like Array below. No arg, or undefined/null, means
 * "new empty object"; an object/function/promise argument is returned as-is
 * (identity, matching spec). A primitive argument is also returned as-is —
 * real JS would box it (`new String(x)`-style), but this engine has no
 * boxed-primitive type at all (see String()/Number() in the built-ins
 * reference), so there is nothing to box it into. */
static bool g_Object(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue v = ARG(0);
    if (argc == 0 || js_is_undefined(v) || js_is_null(v)) {
        JsValue o = js_object_new(ctx);
        if (!js_is_object(o))
            return oom(ctx, r);
        *r = o;
        return true;
    }
    *r = v;
    return true;
}

static bool arr_isArray(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)ctx;
    (void)tv;
    (void)argc;
    JsValue v = ARG(0);
    *r = js_bool(js_is_object(v) && js_value_object(v)->obj_kind == JS_OBJ_ARRAY);
    return true;
}

static bool arr_of(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsObject *out = js_array_new_cell(ctx, (uint32_t)argc);
    if (!out)
        return oom(ctx, r);
    for (int i = 0; i < argc; i++)
        out->elems[out->elem_count++] = args[i];
    *r = js_value_from_cell(&out->gc);
    return true;
}

static bool arr_from(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue src = ARG(0);
    JsValue mapfn = ARG(1);
    bool has_map = js_is_function(mapfn);
    JsObject *out = js_array_new_cell(ctx, 0);
    if (!out)
        return oom(ctx, r);
    JsValue outv = js_value_from_cell(&out->gc);
    js_gc_protect(ctx->vm, &outv);
    js_gc_protect(ctx->vm, &mapfn);
    JsValue srcv = src;
    js_gc_protect(ctx->vm, &srcv);
    bool ok = true, failed = false;

#define FROM_EMIT(val, idx)                                                    \
    do {                                                                       \
        JsValue e = (val);                                                     \
        if (has_map) {                                                         \
            JsValue argv2[2] = {e, js_number(idx)};                            \
            JsValue mapped;                                                    \
            if (!js_call(ctx, mapfn, js_undefined(), argv2, 2, &mapped)) {     \
                *r = mapped; failed = true; ok = false; break;                 \
            }                                                                  \
            e = mapped;                                                        \
        }                                                                      \
        ok = js_array_append(ctx->vm, out, e);                                 \
    } while (0)

    if (js_is_string(src)) {
        JsString *s = js_value_string(src);
        for (uint32_t i = 0; i < s->length && ok;) {
            uint32_t n = 1;
            if (s->units[i] >= 0xD800 && s->units[i] <= 0xDBFF && i + 1 < s->length &&
                s->units[i + 1] >= 0xDC00 && s->units[i + 1] <= 0xDFFF)
                n = 2;
            JsString *c = js_string_cell_new(ctx->vm, s->units + i, n);
            if (!c) { ok = false; break; }
            FROM_EMIT(js_value_from_cell(&c->gc), i);
            i += n;
        }
    } else if (js_is_object(src) && js_value_object(src)->obj_kind == JS_OBJ_ARRAY) {
        JsObject *o = js_value_object(src);
        for (uint32_t i = 0; i < o->elem_count && ok; i++)
            FROM_EMIT(o->elems[i], i);
    }
    /* other array-likes are not supported (documented) */
#undef FROM_EMIT
    js_gc_unprotect(ctx->vm, &srcv);
    js_gc_unprotect(ctx->vm, &mapfn);
    js_gc_unprotect(ctx->vm, &outv);
    if (failed)
        return false;
    if (!ok)
        return oom(ctx, r);
    *r = outv;
    return true;
}

/* Array(...): callable with or without `new` (see JS_OP_NEW's native-constructor
 * dispatch — both forms call this the same way, matching real JS's Array).
 * One numeric argument means "empty array of that length" (spec's special
 * case); anything else — zero args, or one non-numeric, or several — means
 * "array of these elements", like Array.of. No holes here (arrays are a
 * flat JsValue vector — see "Property storage" in docs/plan.md), so a
 * length past JS_MAX_ARRAY_GAP is refused rather than eagerly filled: same
 * memory-exhaustion guard as growing an array via index assignment. */
static bool g_Array(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    if (argc == 1 && js_is_number(args[0])) {
        double d = js_get_number(args[0]);
        /* bounds-check before the cast: (uint32_t)d is UB for d outside
         * [0, UINT32_MAX], e.g. Array(-1) or Array(1e20) */
        if (!(d >= 0 && d < 4294967296.0))
            return native_throw(ctx, r, "RangeError: Invalid array length");
        uint32_t n = (uint32_t)d;
        if ((double)n != d)
            return native_throw(ctx, r, "RangeError: Invalid array length");
        if (n > JS_MAX_ARRAY_GAP)
            return native_throw(ctx, r, "RangeError: array length too large");
        JsObject *out = js_array_new_cell(ctx, n);
        if (!out)
            return oom(ctx, r);
        if (n > 0 && !js_array_set_index(ctx->vm, out, n - 1, js_undefined()))
            return oom(ctx, r);
        *r = js_value_from_cell(&out->gc);
        return true;
    }
    JsObject *out = js_array_new_cell(ctx, (uint32_t)argc);
    if (!out)
        return oom(ctx, r);
    for (int i = 0; i < argc; i++)
        out->elems[out->elem_count++] = args[i];
    *r = js_value_from_cell(&out->gc);
    return true;
}

static bool num_isInteger(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)ctx; (void)tv; (void)argc;
    JsValue v = ARG(0);
    if (!js_is_number(v)) { *r = js_bool(false); return true; }
    double d = js_get_number(v);
    *r = js_bool(d == d && d != __builtin_inf() && d != -__builtin_inf() && d == __builtin_trunc(d));
    return true;
}

static bool num_isFinite(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)ctx; (void)tv; (void)argc;
    JsValue v = ARG(0);
    *r = js_bool(js_is_number(v) && js_get_number(v) == js_get_number(v) &&
                 js_get_number(v) != __builtin_inf() && js_get_number(v) != -__builtin_inf());
    return true;
}

static bool num_isNaN(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)ctx; (void)tv; (void)argc;
    JsValue v = ARG(0);
    *r = js_bool(js_is_number(v) && js_get_number(v) != js_get_number(v));
    return true;
}

static bool num_isSafeInteger(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)ctx; (void)tv; (void)argc;
    JsValue v = ARG(0);
    if (!js_is_number(v)) { *r = js_bool(false); return true; }
    double d = js_get_number(v);
    *r = js_bool(d == __builtin_trunc(d) && d >= -9007199254740991.0 && d <= 9007199254740991.0);
    return true;
}

static bool g_parseInt(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsString *s = arg_string(ctx, ARG(0));
    if (!s)
        return oom(ctx, r);
    /* Spec: radix = ToInt32(arg). js_to_int32 is UB-free (unlike a raw
     * (int)-cast of an Infinity/huge double) and wraps mod 2^32 as ToInt32
     * requires; an out-of-2..36 result is rejected below. */
    int radix = argc > 1 && !js_is_undefined(ARG(1)) ? (int)js_to_int32(js_to_number_value(ctx, ARG(1))) : 0;
    uint32_t i = 0;
    while (i < s->length && is_ws(s->units[i]))
        i++;
    int sign = 1;
    if (i < s->length && (s->units[i] == '+' || s->units[i] == '-')) {
        if (s->units[i] == '-')
            sign = -1;
        i++;
    }
    if ((radix == 0 || radix == 16) && i + 1 < s->length && s->units[i] == '0' &&
        (s->units[i + 1] == 'x' || s->units[i + 1] == 'X')) {
        i += 2;
        radix = 16;
    }
    if (radix == 0)
        radix = 10;
    if (radix < 2 || radix > 36) {
        *r = js_number(__builtin_nan(""));
        return true;
    }
    double val = 0;
    bool any = false;
    for (; i < s->length; i++) {
        uint16_t c = s->units[i];
        int d = c >= '0' && c <= '9' ? c - '0'
                : c >= 'a' && c <= 'z' ? c - 'a' + 10
                : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
        if (d >= radix)
            break;
        val = val * radix + d;
        any = true;
    }
    *r = any ? js_number(sign * val) : js_number(__builtin_nan(""));
    return true;
}

static bool g_parseFloat(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    (void)argc;
    JsString *s = arg_string(ctx, ARG(0));
    if (!s)
        return oom(ctx, r);
    uint32_t i = 0;
    while (i < s->length && is_ws(s->units[i]))
        i++;
    uint32_t start = i;
    if (i < s->length && (s->units[i] == '+' || s->units[i] == '-'))
        i++;
    /* Infinity */
    static const char inf[] = "Infinity";
    bool is_inf = i + 8 <= s->length;
    for (int k = 0; is_inf && k < 8; k++)
        if (s->units[i + (uint32_t)k] != (uint16_t)inf[k])
            is_inf = false;
    if (is_inf) {
        *r = js_number(s->units[start] == '-' ? -__builtin_inf() : __builtin_inf());
        return true;
    }
    bool dot = false, exp = false, any = false;
    while (i < s->length) {
        uint16_t c = s->units[i];
        if (c >= '0' && c <= '9') {
            any = true;
            i++;
        } else if (c == '.' && !dot && !exp) {
            dot = true;
            i++;
        } else if ((c == 'e' || c == 'E') && any && !exp) {
            /* Only absorb the exponent if it is well-formed (e[+/-]?digit).
             * A dangling 'e' as in "5e" is not part of the number — parseFloat
             * takes the longest valid prefix, so it stops before the 'e'. */
            uint32_t j = i + 1;
            if (j < s->length && (s->units[j] == '+' || s->units[j] == '-'))
                j++;
            if (j < s->length && s->units[j] >= '0' && s->units[j] <= '9') {
                exp = true;
                i = j; /* the digit is consumed by the digit branch next */
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if (!any) {
        *r = js_number(__builtin_nan(""));
        return true;
    }
    *r = js_number(js_units_to_number(s->units + start, i - start));
    return true;
}

static bool g_isNaN(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    (void)argc;
    double d = js_to_number_value(ctx, ARG(0));
    *r = js_bool(d != d);
    return true;
}

static bool g_isFinite(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    (void)argc;
    double d = js_to_number_value(ctx, ARG(0));
    *r = js_bool(d == d && d != __builtin_inf() && d != -__builtin_inf());
    return true;
}

static bool g_String(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    if (argc == 0) {
        JsString *e = js_string_cell_new(ctx->vm, NULL, 0);
        *r = e ? js_value_from_cell(&e->gc) : js_undefined();
        return e ? true : oom(ctx, r);
    }
    JsString *s = js_to_string_cell(ctx, ARG(0), 0);
    if (!s)
        return oom(ctx, r);
    *r = js_value_from_cell(&s->gc);
    return true;
}

static bool g_Number(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_number(argc == 0 ? 0 : js_to_number_value(ctx, ARG(0)));
    return true;
}

static bool g_Boolean(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)ctx;
    (void)tv;
    (void)argc;
    *r = js_bool(js_to_boolean(ARG(0)));
    return true;
}

static bool str_fromCharCode(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    for (int i = 0; i < argc; i++)
        sb_unit(&sb, to_uint16(js_to_number_value(ctx, args[i])));
    *r = sb_finish(&sb);
    return js_is_undefined(*r) && argc ? oom(ctx, r) : true;
}

static bool str_fromCodePoint(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    for (int i = 0; i < argc; i++) {
        double d = js_to_number_value(ctx, args[i]);
        /* real RangeError, not a garbage cast, for an invalid code point —
         * String.fromCodePoint(-1) / (1e20) previously cast an out-of-range
         * double straight to uint32_t (UB) */
        if (!(d >= 0 && d <= 0x10FFFF) || d != __builtin_floor(d)) {
            sb_free(&sb);
            return native_throw(ctx, r, "RangeError: invalid code point");
        }
        uint32_t cp = (uint32_t)d;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            sb_unit(&sb, (uint16_t)(0xD800 + (cp >> 10)));
            sb_unit(&sb, (uint16_t)(0xDC00 + (cp & 0x3FF)));
        } else {
            sb_unit(&sb, (uint16_t)cp);
        }
    }
    *r = sb_finish(&sb);
    return js_is_undefined(*r) && argc ? oom(ctx, r) : true;
}

/* =====================================================================
 * Registration
 * ===================================================================== */

typedef struct {
    const char *name;
    JsNativeFn fn;
} MethodDef;

static bool install_methods(JsContext *ctx, JsObject *table, const MethodDef *defs) {
    for (const MethodDef *d = defs; d->name; d++) {
        JsValue nf = js_native_new(ctx, d->name, d->fn, NULL);
        if (!js_is_function(nf) || !js_object_set_ascii(ctx, table, d->name, nf))
            return false;
    }
    return true;
}

/* Math unary trampolines (one per function; keeps ud unused and avoids
 * function-pointer-in-void* casts). */
#define MATH1(fnname, kernel)                                                  \
    static bool fnname(JsContext *ctx, JsValue tv, const JsValue *args,        \
                       int argc, JsValue *r) {                                 \
        (void)tv;                                                              \
        (void)argc;                                                           \
        *r = js_number(kernel(js_to_number_value(ctx, ARG(0))));              \
        return true;                                                          \
    }
MATH1(math_abs, m_abs)
MATH1(math_floor, __builtin_floor)
MATH1(math_ceil, __builtin_ceil)
MATH1(math_trunc, __builtin_trunc)
MATH1(math_sign, m_sign)
MATH1(math_sqrt, __builtin_sqrt)
MATH1(math_cbrt, m_cbrt)
MATH1(math_exp, js_k_exp)
MATH1(math_log, js_k_log)
MATH1(math_sin, js_k_sin)
MATH1(math_cos, js_k_cos)
MATH1(math_tan, js_k_tan)
MATH1(math_asin, js_k_asin)
MATH1(math_acos, js_k_acos)
MATH1(math_atan, js_k_atan)
#undef MATH1

static bool math_round(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    (void)argc;
    double x = js_to_number_value(ctx, ARG(0));
    double result;
    if (x != x || x == __builtin_inf() || x == -__builtin_inf()) {
        result = x;
    } else {
        /* JS rounds half toward +Infinity (not away from zero). Compute from
         * floor(x) + a half test rather than floor(x + 0.5): the latter is
         * wrong at 0.49999999999999994, where x + 0.5 rounds up to 1.0. */
        double f = __builtin_floor(x);
        result = (x - f >= 0.5) ? f + 1.0 : f;
        /* Preserve the sign of zero: inputs in [-0.5, -0] round to -0. */
        if (result == 0.0 && __builtin_signbit(x))
            result = -0.0;
    }
    *r = js_number(result);
    return true;
}

static bool math_log2(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv; (void)argc;
    *r = js_number(js_k_log(js_to_number_value(ctx, ARG(0))) / 0.69314718055994530942);
    return true;
}

static bool math_log10(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv; (void)argc;
    *r = js_number(js_k_log(js_to_number_value(ctx, ARG(0))) / 2.30258509299404568402);
    return true;
}

static bool def_num(JsContext *ctx, JsObject *o, const char *k, double v) {
    return js_object_set_ascii(ctx, o, k, js_number(v));
}

static bool def_fn(JsContext *ctx, JsObject *o, const char *k, JsNativeFn fn) {
    JsValue nf = js_native_new(ctx, k, fn, NULL);
    return js_is_function(nf) && js_object_set_ascii(ctx, o, k, nf);
}

/* Creates a callable global (native) with an attached statics object. */
static JsObject *def_ctor(JsContext *ctx, const char *name, JsNativeFn fn) {
    JsValue nf = js_native_new(ctx, name, fn, NULL);
    if (!js_is_function(nf))
        return NULL;
    js_gc_protect(ctx->vm, &nf); /* keep rooted through statics + global set */
    JsValue statics = js_object_new(ctx);
    if (!js_is_object(statics)) {
        js_gc_unprotect(ctx->vm, &nf);
        return NULL;
    }
    JsNative *native = (JsNative *)js_value_cell(nf);
    native->statics = js_value_object(statics);
    bool ok = js_object_set_ascii(ctx, ctx->globals, name, nf);
    js_gc_unprotect(ctx->vm, &nf);
    return ok ? native->statics : NULL;
}

bool js_builtins_init(JsContext *ctx) {
    JsVm *vm = ctx->vm;

    /* Object.prototype: the root of every [[Prototype]] chain in the engine
     * (js_object_new(ctx) chains everything created after this point to it
     * automatically — see js_object_new's bootstrap comment in js_object.c).
     * Must be first: array_proto etc. below are themselves created via
     * js_object_new, and this is the one call where ctx->object_proto is
     * still NULL (the correct, spec-matching bootstrap). */
    JsValue op = js_object_new(ctx);
    if (!js_is_object(op))
        return false;
    ctx->object_proto = js_value_object(op);
    static const MethodDef object_proto_methods[] = {
        {"hasOwnProperty", objp_hasOwnProperty},
        {"toString", objp_toString},
        {"valueOf", objp_valueOf},
        {NULL, NULL}};
    if (!install_methods(ctx, ctx->object_proto, object_proto_methods))
        return false;

    /* method tables — assign each into ctx before the next allocation so it
     * is a GC root immediately (gc_stress collects at every allocation) */
    JsValue sm = js_object_new(ctx);
    if (!js_is_object(sm))
        return false;
    ctx->string_methods = js_value_object(sm);
    /* Array.prototype: a real object, exposed below as Array's own
     * "prototype" property; ctx->array_proto is just a fast-path cache so
     * js_array_new_cell doesn't need a property lookup on every array it
     * creates — property access on array instances never consults it
     * directly, it goes through the normal [[Prototype]] walk. Chains to
     * Object.prototype automatically (js_object_new, above). */
    JsValue am = js_object_new(ctx);
    if (!js_is_object(am))
        return false;
    ctx->array_proto = js_value_object(am);
    JsValue num = js_object_new(ctx);
    if (!js_is_object(num))
        return false;
    ctx->number_methods = js_value_object(num);

    static const MethodDef string_methods[] = {
        {"charAt", sm_charAt}, {"charCodeAt", sm_charCodeAt},
        {"codePointAt", sm_codePointAt}, {"at", sm_at}, {"indexOf", sm_indexOf},
        {"lastIndexOf", sm_lastIndexOf}, {"includes", sm_includes},
        {"startsWith", sm_startsWith}, {"endsWith", sm_endsWith}, {"slice", sm_slice},
        {"substring", sm_substring}, {"substr", sm_substring},
        {"toUpperCase", sm_toUpperCase}, {"toLowerCase", sm_toLowerCase},
        {"trim", sm_trim}, {"trimStart", sm_trimStart}, {"trimEnd", sm_trimEnd},
        {"repeat", sm_repeat}, {"padStart", sm_padStart}, {"padEnd", sm_padEnd},
        {"replace", sm_replace}, {"replaceAll", sm_replaceAll}, {"split", sm_split},
        {"concat", sm_concat}, {"toString", sm_toString}, {"valueOf", sm_toString},
#ifdef LAMASSU_HAS_REGEX
        {"match", js_re_str_match}, {"matchAll", js_re_str_matchAll},
        {"search", js_re_str_search},
#endif
        {NULL, NULL}};
    static const MethodDef array_methods[] = {
        {"push", am_push}, {"pop", am_pop}, {"shift", am_shift}, {"unshift", am_unshift},
        {"at", am_at}, {"indexOf", am_indexOf}, {"lastIndexOf", am_lastIndexOf},
        {"includes", am_includes}, {"slice", am_slice}, {"join", am_join},
        {"reverse", am_reverse}, {"fill", am_fill}, {"concat", am_concat},
        {"map", am_map}, {"filter", am_filter}, {"forEach", am_forEach},
        {"some", am_some}, {"every", am_every}, {"find", am_find},
        {"findIndex", am_findIndex}, {"reduce", am_reduce}, {"sort", am_sort},
        {"flat", am_flat}, {"toString", am_toString},
        {NULL, NULL}};
    static const MethodDef number_methods[] = {
        {"toFixed", nm_toFixed}, {"toString", nm_toString}, {"valueOf", nm_toString},
        {NULL, NULL}};
    if (!install_methods(ctx, ctx->string_methods, string_methods) ||
        !install_methods(ctx, ctx->array_proto, array_methods) ||
        !install_methods(ctx, ctx->number_methods, number_methods))
        return false;

    /* Math */
    JsValue mathv = js_object_new(ctx);
    if (!js_is_object(mathv) || !js_object_set_ascii(ctx, ctx->globals, "Math", mathv))
        return false;
    JsObject *math = js_value_object(mathv);
    if (!def_fn(ctx, math, "abs", math_abs) || !def_fn(ctx, math, "floor", math_floor) ||
        !def_fn(ctx, math, "ceil", math_ceil) || !def_fn(ctx, math, "round", math_round) ||
        !def_fn(ctx, math, "trunc", math_trunc) || !def_fn(ctx, math, "sign", math_sign) ||
        !def_fn(ctx, math, "sqrt", math_sqrt) || !def_fn(ctx, math, "cbrt", math_cbrt) ||
        !def_fn(ctx, math, "exp", math_exp) || !def_fn(ctx, math, "log", math_log) ||
        !def_fn(ctx, math, "log2", math_log2) || !def_fn(ctx, math, "log10", math_log10) ||
        !def_fn(ctx, math, "sin", math_sin) || !def_fn(ctx, math, "cos", math_cos) ||
        !def_fn(ctx, math, "tan", math_tan) || !def_fn(ctx, math, "asin", math_asin) ||
        !def_fn(ctx, math, "acos", math_acos) || !def_fn(ctx, math, "atan", math_atan) ||
        !def_fn(ctx, math, "atan2", math_atan2) || !def_fn(ctx, math, "pow", math_pow) ||
        !def_fn(ctx, math, "min", math_min) || !def_fn(ctx, math, "max", math_max) ||
        !def_fn(ctx, math, "hypot", math_hypot) || !def_fn(ctx, math, "random", math_random))
        return false;
    if (!def_num(ctx, math, "PI", 3.141592653589793) ||
        !def_num(ctx, math, "E", 2.718281828459045) ||
        !def_num(ctx, math, "LN2", 0.6931471805599453) ||
        !def_num(ctx, math, "LN10", 2.302585092994046) ||
        !def_num(ctx, math, "LOG2E", 1.4426950408889634) ||
        !def_num(ctx, math, "LOG10E", 0.4342944819032518) ||
        !def_num(ctx, math, "SQRT2", 1.4142135623730951) ||
        !def_num(ctx, math, "SQRT1_2", 0.7071067811865476))
        return false;

    /* JSON */
    JsValue jsonv = js_object_new(ctx);
    if (!js_is_object(jsonv) || !js_object_set_ascii(ctx, ctx->globals, "JSON", jsonv))
        return false;
    JsObject *json = js_value_object(jsonv);
    if (!def_fn(ctx, json, "stringify", json_stringify) ||
        !def_fn(ctx, json, "parse", json_parse))
        return false;

    /* Object: a callable native (Object() / new Object() behave identically —
     * see JS_OP_NEW's native-constructor dispatch), same "real prototype
     * object" wiring as Date/Map/Set/RegExp below — Object.prototype is
     * ctx->object_proto itself, not a separately lazily-created object. */
    JsValue objv = js_native_new(ctx, "Object", g_Object, NULL);
    if (!js_is_function(objv))
        return false;
    js_gc_protect(vm, &objv); /* keep rooted through statics + global set */
    ((JsNative *)js_value_cell(objv))->prototype = ctx->object_proto;
    JsValue objectv = js_object_new(ctx);
    bool obj_ok = js_is_object(objectv);
    JsObject *object = obj_ok ? js_value_object(objectv) : NULL;
    if (obj_ok) {
        ((JsNative *)js_value_cell(objv))->statics = object;
        obj_ok = js_object_set_ascii(ctx, ctx->object_proto, "constructor", objv) &&
                js_object_set_ascii(ctx, ctx->globals, "Object", objv);
    }
    js_gc_unprotect(vm, &objv);
    if (!obj_ok)
        return false;
    if (!def_fn(ctx, object, "keys", obj_keys) || !def_fn(ctx, object, "values", obj_values) ||
        !def_fn(ctx, object, "entries", obj_entries) || !def_fn(ctx, object, "assign", obj_assign) ||
        !def_fn(ctx, object, "freeze", obj_freeze) || !def_fn(ctx, object, "fromEntries", obj_fromEntries) ||
        !def_fn(ctx, object, "hasOwn", obj_hasOwn) ||
        !def_fn(ctx, object, "getPrototypeOf", obj_getPrototypeOf) ||
        !def_fn(ctx, object, "setPrototypeOf", obj_setPrototypeOf))
        return false;

    /* Array: a callable native (Array(n) / new Array(n) behave identically —
     * see JS_OP_NEW's native-constructor dispatch), same "real prototype
     * object" wiring as Date/Map/Set/RegExp below. */
    JsValue arrv = js_native_new(ctx, "Array", g_Array, NULL);
    if (!js_is_function(arrv))
        return false;
    js_gc_protect(vm, &arrv); /* keep rooted through statics + global set */
    ((JsNative *)js_value_cell(arrv))->prototype = ctx->array_proto;
    JsValue arrayv = js_object_new(ctx);
    bool arr_ok = js_is_object(arrayv);
    JsObject *array = arr_ok ? js_value_object(arrayv) : NULL;
    if (arr_ok) {
        ((JsNative *)js_value_cell(arrv))->statics = array;
        arr_ok = def_fn(ctx, array, "isArray", arr_isArray) && def_fn(ctx, array, "of", arr_of) &&
                def_fn(ctx, array, "from", arr_from) &&
                js_object_set_ascii(ctx, ctx->array_proto, "constructor", arrv) &&
                js_object_set_ascii(ctx, ctx->globals, "Array", arrv);
    }
    js_gc_unprotect(vm, &arrv);
    if (!arr_ok)
        return false;

    /* Number: callable conversion + statics */
    JsObject *number = def_ctor(ctx, "Number", g_Number);
    if (!number)
        return false;
    if (!def_fn(ctx, number, "isInteger", num_isInteger) ||
        !def_fn(ctx, number, "isFinite", num_isFinite) ||
        !def_fn(ctx, number, "isNaN", num_isNaN) ||
        !def_fn(ctx, number, "isSafeInteger", num_isSafeInteger) ||
        !def_fn(ctx, number, "parseInt", g_parseInt) ||
        !def_fn(ctx, number, "parseFloat", g_parseFloat))
        return false;
    if (!def_num(ctx, number, "MAX_SAFE_INTEGER", 9007199254740991.0) ||
        !def_num(ctx, number, "MIN_SAFE_INTEGER", -9007199254740991.0) ||
        !def_num(ctx, number, "MAX_VALUE", 1.7976931348623157e308) ||
        !def_num(ctx, number, "MIN_VALUE", 5e-324) ||
        !def_num(ctx, number, "EPSILON", 2.220446049250313e-16) ||
        !def_num(ctx, number, "POSITIVE_INFINITY", __builtin_inf()) ||
        !def_num(ctx, number, "NEGATIVE_INFINITY", -__builtin_inf()) ||
        !def_num(ctx, number, "NaN", __builtin_nan("")))
        return false;

    /* String: callable conversion + statics */
    JsObject *stro = def_ctor(ctx, "String", g_String);
    if (!stro)
        return false;
    if (!def_fn(ctx, stro, "fromCharCode", str_fromCharCode) ||
        !def_fn(ctx, stro, "fromCodePoint", str_fromCodePoint))
        return false;

    /* Boolean: callable conversion */
    if (!def_ctor(ctx, "Boolean", g_Boolean))
        return false;

    /* promises (js_promise.c) */
    if (!js_promise_builtins_init(ctx))
        return false;

#ifdef LAMASSU_HAS_REGEX
    /* RegExp global + real RegExp.prototype (js_regexp.c) */
    if (!js_regexp_builtins_init(ctx))
        return false;
#endif

    /* Date global + real Date.prototype (js_date.c) */
    if (!js_date_builtins_init(ctx))
        return false;

    /* Map global + real Map.prototype (js_mapobj.c) */
    if (!js_mapobj_builtins_init(ctx))
        return false;

    /* Set global + real Set.prototype (js_setobj.c) */
    if (!js_setobj_builtins_init(ctx))
        return false;

    /* global functions */
    static const uint16_t n_parseInt[] = {'p','a','r','s','e','I','n','t'};
    static const uint16_t n_parseFloat[] = {'p','a','r','s','e','F','l','o','a','t'};
    static const uint16_t n_isNaN[] = {'i','s','N','a','N'};
    static const uint16_t n_isFinite[] = {'i','s','F','i','n','i','t','e'};
    if (!js_register_native(ctx, n_parseInt, 8, g_parseInt, NULL) ||
        !js_register_native(ctx, n_parseFloat, 10, g_parseFloat, NULL) ||
        !js_register_native(ctx, n_isNaN, 5, g_isNaN, NULL) ||
        !js_register_native(ctx, n_isFinite, 8, g_isFinite, NULL))
        return false;
    return true;
}
