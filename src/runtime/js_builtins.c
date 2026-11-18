#include <stdio.h>
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
 * only.
 */
#include "js_bytecode.h"
#include "js_error.h"
#include "lamassu_internal.h"
#include "js_date.h"
#include "js_mapobj.h"
#include "js_weak.h"
#include "js_bigint.h"
#include "js_setobj.h"
#include "js_iter.h"
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
} StrBuf;

static void sb_init(StrBuf *sb, JsVm *vm) {
    sb->vm = vm;
    sb->u = NULL;
    sb->len = sb->cap = 0;
    sb->oom = false;
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
    *result = js_error_from_ascii(ctx, msg); /* Error object; string on OOM */
    return false;
}

/* A formatted engine error (js_error_fmt: V8's wording with the values in it). */
#define native_throw_fmt(ctx, result, ...) (*(result) = js_error_fmt((ctx), __VA_ARGS__), false)

/* "a number", "an object", "undefined": how V8 names a value's type in a sentence */
static const char *type_phrase(JsValue v) {
    if (js_is_undefined(v)) return "undefined";
    if (js_is_null(v)) return "null";
    if (js_is_number(v)) return "a number";
    if (js_is_string(v)) return "a string";
    if (js_is_bool(v)) return "a boolean";
    return "an object";
}

/* A failure's error: what a conversion threw (valueOf/toString can run guest code), else out of
 * memory. Every path that gives up after a failed js_op_to_string/js_op_to_number reports this. */
static bool oom(JsContext *ctx, JsValue *result) {
    if (!js_is_undefined(ctx->pending_error)) {
        *result = js_take_error(ctx);
        return false;
    }
    return native_throw(ctx, result, "out of memory");
}

/* ToNumber of V into D, or the native returns what it threw. For natives whose result is r and
 * which hold nothing to release. */
#define TO_NUM(d, v)                                                            \
    do {                                                                        \
        if (!js_op_to_number(ctx, (v), &(d)))                                   \
            return oom(ctx, r);                                                 \
    } while (0)

/* ---- ToString of an argument, kept rooted until the native returns ---- */

static JsString *arg_string(JsContext *ctx, JsValue v) {
    return js_native_string(ctx, v);
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

/* RequireObjectCoercible(this), worded as V8 words it with the running method's name */
bool js_str_this_check(JsContext *ctx, JsValue this_val, JsValue *result) {
    if (!js_is_undefined(this_val) && !js_is_null(this_val))
        return true;
    const JsNativeAct *act = ctx->native_act;
    if (act && act->nf && act->nf->name)
        *result = js_error_fmt(ctx, "TypeError: String.prototype.%S called on null or undefined", act->nf->name);
    else
        *result = js_error_fmt(ctx, "TypeError: String.prototype method called on null or undefined");
    return false;
}

static bool str_this(JsContext *ctx, JsValue this_val, JsString **out, JsValue *result) {
    if (!js_str_this_check(ctx, this_val, result))
        return false;
    JsString *s = js_native_string(ctx, this_val);
    if (!s)
        return oom(ctx, result);
    *out = s;
    return true;
}

static bool sm_charAt(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    double d;
    TO_NUM(d, ARG(0));
    d = to_int(d);
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
    double d;
    TO_NUM(d, ARG(0));
    d = to_int(d);
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
    double d;
    TO_NUM(d, ARG(0));
    d = to_int(d);
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
    double d;
    TO_NUM(d, ARG(0));
    d = to_int(d);
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
    double fromd = 0;
    if (argc > 1)
        TO_NUM(fromd, ARG(1));
    uint32_t from = clamp_index(fromd, s->length);
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
    double posd = __builtin_inf();
    if (argc > 1)
        TO_NUM(posd, ARG(1));
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
#ifdef LAMASSU_HAS_REGEX
    if (js_regexp_isregexp(ctx, ARG(0)))
        return native_throw(ctx, r, "TypeError: First argument to String.prototype.includes must not be a regular expression");
#endif
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
#ifdef LAMASSU_HAS_REGEX
    if (js_regexp_isregexp(ctx, ARG(0)))
        return native_throw(ctx, r, "TypeError: First argument to String.prototype.startsWith must not be a regular expression");
#endif
    JsString *n = arg_string(ctx, ARG(0));
    if (!n)
        return oom(ctx, r);
    double fromd = 0;
    if (argc > 1)
        TO_NUM(fromd, ARG(1));
    uint32_t from = clamp_index(fromd, s->length);
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
#ifdef LAMASSU_HAS_REGEX
    if (js_regexp_isregexp(ctx, ARG(0)))
        return native_throw(ctx, r, "TypeError: First argument to String.prototype.endsWith must not be a regular expression");
#endif
    JsString *n = arg_string(ctx, ARG(0));
    if (!n)
        return oom(ctx, r);
    uint32_t end = s->length;
    if (argc > 1) {
        double ed;
        TO_NUM(ed, ARG(1));
        end = clamp_index(ed, s->length);
    }
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
    double sd, ed = s->length;
    TO_NUM(sd, ARG(0));
    if (argc > 1 && !js_is_undefined(ARG(1)))
        TO_NUM(ed, ARG(1));
    uint32_t start = clamp_index(sd, s->length);
    uint32_t end = clamp_index(ed, s->length);
    return make_substr(ctx, s, start, end, r);
}

static bool sm_substring(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    double ad, bd = s->length;
    TO_NUM(ad, ARG(0));
    if (argc > 1 && !js_is_undefined(ARG(1)))
        TO_NUM(bd, ARG(1));
    uint32_t a = clamp_index(ad, s->length);
    uint32_t b = clamp_index(bd, s->length);
    if (a > b) {
        uint32_t t = a;
        a = b;
        b = t;
    }
    return make_substr(ctx, s, a, b, r);
}

/*
 * Case conversion is Unicode's, not ASCII's.
 *
 * These both used to add or subtract 32 over A-Z, which is wrong for every
 * language that is not English and wrong quietly: `'Ä'.toLowerCase()` came
 * back unchanged, so anything lowercasing a string before comparing it stopped
 * matching. The mapping tables live in js_case.c; what these do is walk the
 * string as CHARACTERS rather than as code units, which is the other half of
 * being right.
 */

/** One code point at `i`, and how many units it took — a surrogate pair is one
 * character, and mapping its halves separately maps neither. */
static uint32_t str_codepoint_at(const JsString *s, uint32_t i, uint32_t *width) {
    uint16_t hi = s->units[i];
    if (hi >= 0xD800 && hi <= 0xDBFF && i + 1 < s->length) {
        uint16_t lo = s->units[i + 1];
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
            *width = 2;
            return 0x10000u + (((uint32_t)(hi - 0xD800)) << 10) + (uint32_t)(lo - 0xDC00);
        }
    }
    *width = 1;
    return hi;
}

static void sb_codepoint(StrBuf *sb, uint32_t cp) {
    if (cp > 0xFFFF) {
        cp -= 0x10000;
        sb_unit(sb, (uint16_t)(0xD800 + (cp >> 10)));
        sb_unit(sb, (uint16_t)(0xDC00 + (cp & 0x3FF)));
    } else {
        sb_unit(sb, (uint16_t)cp);
    }
}

/*
 * Final_Sigma, the one context-dependent rule in the language-independent
 * path: a capital sigma lowercases to `ς` when it ends a word and `σ` when it
 * does not, so `'ΟΔΟΣ ΣΤΟ'` is `'οδος στο'`.
 *
 * "Ends a word" means preceded by a cased character and not followed by one,
 * skipping case-ignorable characters (accents, apostrophes) on both sides.
 */
static bool final_sigma_at(const JsString *s, uint32_t index, uint32_t width) {
    bool before = false;
    for (uint32_t i = index; i > 0;) {
        uint32_t w = 1;
        /* Step back one character; a low surrogate means the pair started one
         * unit earlier. */
        uint16_t unit = s->units[i - 1];
        uint32_t start = i - 1;
        if (unit >= 0xDC00 && unit <= 0xDFFF && i >= 2 &&
            s->units[i - 2] >= 0xD800 && s->units[i - 2] <= 0xDBFF) {
            start = i - 2;
        }
        uint32_t cp = str_codepoint_at(s, start, &w);
        i = start;
        if (js_case_is_ignorable(cp)) continue;
        before = js_case_is_cased(cp);
        break;
    }
    if (!before) return false;

    for (uint32_t i = index + width; i < s->length;) {
        uint32_t w = 1;
        uint32_t cp = str_codepoint_at(s, i, &w);
        i += w;
        if (js_case_is_ignorable(cp)) continue;
        return !js_case_is_cased(cp);
    }
    return true;   /* nothing after it at all — still final */
}

static bool str_case_convert(JsContext *ctx, JsValue tv, JsValue *r, bool to_upper) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    for (uint32_t i = 0; i < s->length;) {
        uint32_t width = 1;
        uint32_t cp = str_codepoint_at(s, i, &width);
        bool final_sigma = !to_upper && cp == 0x03A3 && final_sigma_at(s, i, width);

        uint32_t mapped[3];
        int n = js_case_map(cp, to_upper, final_sigma, mapped);
        for (int k = 0; k < n; k++) sb_codepoint(&sb, mapped[k]);
        i += width;
    }
    *r = sb_finish(&sb);
    return !js_is_undefined(*r) || s->length == 0 ? true : oom(ctx, r);
}

static bool sm_toUpperCase(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    return str_case_convert(ctx, tv, r, true);
}

static bool sm_toLowerCase(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    return str_case_convert(ctx, tv, r, false);
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

/* isWellFormed / toWellFormed: whether every surrogate is paired; the copy with each lone one
 * replaced by U+FFFD */
static bool lone_surrogate(const JsString *s, uint32_t i) {
    uint16_t c = s->units[i];
    if (c >= 0xD800 && c <= 0xDBFF)
        return !(i + 1 < s->length && s->units[i + 1] >= 0xDC00 && s->units[i + 1] <= 0xDFFF);
    if (c >= 0xDC00 && c <= 0xDFFF)
        return !(i > 0 && s->units[i - 1] >= 0xD800 && s->units[i - 1] <= 0xDBFF);
    return false;
}

static bool sm_isWellFormed(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    *r = js_bool(true);
    for (uint32_t i = 0; i < s->length; i++)
        if (lone_surrogate(s, i)) {
            *r = js_bool(false);
            break;
        }
    return true;
}

static bool sm_toWellFormed(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    uint32_t i = 0;
    while (i < s->length && !lone_surrogate(s, i))
        i++;
    if (i == s->length) {
        *r = js_value_from_cell(&s->gc);
        return true;
    }
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    sb_units(&sb, s->units, i);
    for (; i < s->length; i++)
        sb_unit(&sb, lone_surrogate(s, i) ? 0xFFFD : s->units[i]);
    *r = sb_finish(&sb);
    return js_is_undefined(*r) ? oom(ctx, r) : true;
}

static bool str_is_ascii(JsValue key, const char *a);

/* s.normalize(form): NFC (the default), NFD, NFKC or NFKD */
static bool sm_normalize(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    int form = JS_NF_NFC;
    if (!js_is_undefined(ARG(0))) {
        JsString *f = arg_string(ctx, ARG(0));
        if (!f)
            return oom(ctx, r);
        static const char *const names[] = {"NFC", "NFD", "NFKC", "NFKD"};
        form = -1;
        for (int i = 0; i < 4; i++)
            if (str_is_ascii(js_value_from_cell(&f->gc), names[i]))
                form = i;
        if (form < 0)
            return native_throw(ctx, r, "RangeError: The normalization form should be one of NFC, NFD, NFKC, NFKD.");
    }
    JsString *n = js_normalize(ctx->vm, s, form);
    if (!n)
        return oom(ctx, r);
    *r = js_value_from_cell(&n->gc);
    return true;
}

/* The string HTML methods (Annex B CreateHTML): "<tag attr=\"value\">" + this + "</tag>", the
 * value's quotes written &quot; */
static bool create_html(JsContext *ctx, JsValue tv, JsValue *r, const char *tag, const char *attr, JsValue value) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    JsString *v = NULL;
    if (attr && !(v = arg_string(ctx, value)))
        return oom(ctx, r);
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    sb_unit(&sb, '<');
    for (const char *c = tag; *c; c++)
        sb_unit(&sb, (uint16_t)*c);
    if (attr) {
        sb_unit(&sb, ' ');
        for (const char *c = attr; *c; c++)
            sb_unit(&sb, (uint16_t)*c);
        sb_unit(&sb, '=');
        sb_unit(&sb, '"');
        for (uint32_t i = 0; i < v->length; i++) {
            if (v->units[i] == '"') {
                for (const char *c = "&quot;"; *c; c++)
                    sb_unit(&sb, (uint16_t)*c);
            } else {
                sb_unit(&sb, v->units[i]);
            }
        }
        sb_unit(&sb, '"');
    }
    sb_unit(&sb, '>');
    sb_units(&sb, s->units, s->length);
    sb_unit(&sb, '<');
    sb_unit(&sb, '/');
    for (const char *c = tag; *c; c++)
        sb_unit(&sb, (uint16_t)*c);
    sb_unit(&sb, '>');
    *r = sb_finish(&sb);
    return js_is_undefined(*r) ? oom(ctx, r) : true;
}

#define HTML_METHOD(fn, tag, attr)                                                    \
    static bool fn(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) { \
        (void)args;                                                                   \
        (void)argc;                                                                   \
        return create_html(ctx, tv, r, tag, attr, ARG(0));                            \
    }
HTML_METHOD(sm_anchor, "a", "name")
HTML_METHOD(sm_big, "big", NULL)
HTML_METHOD(sm_blink, "blink", NULL)
HTML_METHOD(sm_bold, "b", NULL)
HTML_METHOD(sm_fixed, "tt", NULL)
HTML_METHOD(sm_fontcolor, "font", "color")
HTML_METHOD(sm_fontsize, "font", "size")
HTML_METHOD(sm_italics, "i", NULL)
HTML_METHOD(sm_link, "a", "href")
HTML_METHOD(sm_small, "small", NULL)
HTML_METHOD(sm_strike, "strike", NULL)
HTML_METHOD(sm_sub, "sub", NULL)
HTML_METHOD(sm_sup, "sup", NULL)
#undef HTML_METHOD

static bool sm_repeat(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    double d;
    TO_NUM(d, ARG(0));
    d = to_int(d);
    if (d < 0 || d == __builtin_inf())
        return native_throw_fmt(ctx, r, "RangeError: Invalid count value: %v", ARG(0));
    /* bounds-check before the cast: (uint32_t)d is UB above UINT32_MAX,
     * e.g. 'x'.repeat(1e20) — any count this large already fails the size
     * check below anyway, so throw the same error a step earlier. */
    if (d > 4294967295.0)
        return native_throw(ctx, r, "RangeError: Invalid string length");
    uint32_t n = (uint32_t)d;
    if ((uint64_t)n * s->length > JS_MAX_STRING_UNITS)
        return native_throw(ctx, r, "RangeError: Invalid string length");
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    for (uint32_t i = 0; i < n; i++)
        sb_units(&sb, s->units, s->length);
    *r = sb_finish(&sb);
    return js_is_undefined(*r) && n * s->length != 0 ? oom(ctx, r) : true;
}

static bool sm_pad(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r, bool start) {
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    /* Clamp the target length into [0, UINT32_MAX] before the cast so an
     * Infinity/huge argument can't UB; an oversized target still means "pad a
     * lot" and is bounded by allocation limits downstream. */
    double td;
    TO_NUM(td, ARG(0));
    uint32_t target = (uint32_t)to_integer_clamped(td, 0, 4294967295.0);
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
    /* Reject an impossible length up front, the way repeat() does. sb_unit is
     * a no-op once the builder has hit OOM, but the loop itself is not: a bare
     * `"x".padStart(4294967295)` would still spin four billion times inside
     * this one native, where no fuel is charged. */
    if ((uint64_t)need + s->length > JS_MAX_STRING_UNITS)
        return native_throw(ctx, r, "RangeError: Invalid string length");
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

/* GetSubstitution for a string pattern: $$ $& $` $'; with no captures, $n and
 * $<name> stay literal, as does anything else after a $ */
static void append_replacement(StrBuf *sb, const JsString *rep, const JsString *s,
                               uint32_t pos, uint32_t mlen) {
    for (uint32_t i = 0; i < rep->length; i++) {
        if (rep->units[i] == '$' && i + 1 < rep->length) {
            uint16_t n = rep->units[i + 1];
            if (n == '$') {
                sb_unit(sb, '$');
                i++;
                continue;
            }
            if (n == '&') {
                sb_units(sb, s->units + pos, mlen);
                i++;
                continue;
            }
            if (n == '`') {
                sb_units(sb, s->units, pos);
                i++;
                continue;
            }
            if (n == '\'') {
                sb_units(sb, s->units + pos + mlen, s->length - pos - mlen);
                i++;
                continue;
            }
        }
        sb_unit(sb, rep->units[i]);
    }
}

/* String.prototype.replace/replaceAll with a string pattern. The replacement is
 * a string (with GetSubstitution) or a function called as fn(match, position,
 * string) for each match, its result converted to a string. replaceAll finds
 * every match first, advancing by the pattern's length (1 for ""). */
static bool sm_replace_impl(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                            JsValue *r, bool all) {
    if (!js_str_this_check(ctx, tv, r))
        return false;
#ifdef LAMASSU_HAS_REGEX
    /* a regexp, or anything that inherits from RegExp.prototype: its @@replace does the work */
    if (all && !js_re_global_check(ctx, ARG(0), false, r))
        return false;
    if (js_regexp_like(ctx, ARG(0)))
        return js_re_str_replace(ctx, tv, args, argc, r);
#endif
    JsVm *vm = ctx->vm;
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    JsValue sv = js_value_from_cell(&s->gc);
    js_gc_protect(vm, &sv);
    JsString *pat = arg_string(ctx, ARG(0));
    if (!pat) {
        js_gc_unprotect(vm, &sv);
        return oom(ctx, r);
    }
    JsValue patv = js_value_from_cell(&pat->gc);
    js_gc_protect(vm, &patv);
    JsValue fn = ARG(1);
    bool is_fn = js_is_function(fn);
    JsValue repv = js_undefined();
    js_gc_protect(vm, &repv);
    bool ok = true, thrown = false;
    if (is_fn) {
        repv = fn;
    } else {
        JsString *rep = arg_string(ctx, fn);
        if (rep)
            repv = js_value_from_cell(&rep->gc);
        else
            ok = false;
    }
    StrBuf sb;
    sb_init(&sb, vm);
    uint32_t pos = 0, cursor = 0;
    while (ok) {
        long f = units_find(s->units, s->length, pat->units, pat->length, pos);
        if (f < 0)
            break;
        uint32_t at = (uint32_t)f;
        sb_units(&sb, s->units + cursor, at - cursor);
        if (is_fn) {
            JsValue argv[3] = {patv, js_number(at), sv};
            JsValue res;
            if (!js_call(ctx, repv, js_undefined(), argv, 3, &res)) {
                *r = res;
                ok = false;
                thrown = true;
                break;
            }
            js_gc_protect(vm, &res);
            JsString *rs = js_op_to_string(ctx, res);
            js_gc_unprotect(vm, &res);
            if (!rs) {
                ok = false;
                break;
            }
            sb_units(&sb, rs->units, rs->length);
        } else {
            append_replacement(&sb, js_value_string(repv), s, at, pat->length);
        }
        cursor = at + pat->length;
        pos = at + (pat->length ? pat->length : 1);
        if (!all || pos > s->length)
            break;
    }
    if (ok) {
        sb_units(&sb, s->units + cursor, s->length - cursor);
        *r = sb_finish(&sb);
        ok = !js_is_undefined(*r);
    } else {
        sb_free(&sb);
    }
    js_gc_unprotect(vm, &repv);
    js_gc_unprotect(vm, &patv);
    js_gc_unprotect(vm, &sv);
    if (thrown)
        return false;
    return ok ? true : oom(ctx, r);
}

static bool sm_replace(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return sm_replace_impl(ctx, tv, a, c, r, false);
}
static bool sm_replaceAll(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return sm_replace_impl(ctx, tv, a, c, r, true);
}

/* ToUint32(limit), the most pieces split returns; undefined is no limit (2^32 - 1). False
 * thrown (its valueOf may throw), the error pending. */
bool js_split_limit(JsContext *ctx, JsValue limit, uint32_t *lim) {
    *lim = 4294967295u;
    if (js_is_undefined(limit))
        return true;
    double d;
    if (!js_op_to_number(ctx, limit, &d))
        return false;
    *lim = js_to_uint32(d);
    return true;
}

static bool sm_split(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (!js_str_this_check(ctx, tv, r))
        return false;
#ifdef LAMASSU_HAS_REGEX
    if (js_regexp_like(ctx, ARG(0)))
        return js_re_str_split(ctx, tv, args, argc, r);
#endif
    /* this, then the limit, then the separator, as the spec orders the conversions; the
     * strings stay rooted for the call (js_native_string) */
    JsString *s;
    if (!str_this(ctx, tv, &s, r))
        return false;
    uint32_t lim;
    if (!js_split_limit(ctx, ARG(1), &lim))
        return (*r = js_take_error(ctx)), false;
    JsString *sep = NULL;
    if (!js_is_undefined(ARG(0)) && !(sep = arg_string(ctx, ARG(0))))
        return oom(ctx, r);
    JsObject *out = js_array_new_cell(ctx, 0);
    JsValue outv = out ? js_value_from_cell(&out->gc) : js_undefined();
    js_gc_protect(ctx->vm, &outv);

    bool ok = out != NULL;
    if (!ok || lim == 0) {
        /* nothing more */
    } else if (!sep) {
        JsString *whole = js_string_cell_new(ctx->vm, s->units, s->length);
        ok = whole && js_array_append(ctx->vm, out, js_value_from_cell(&whole->gc));
    } else if (sep->length == 0) {
        for (uint32_t i = 0; i < s->length && ok && out->elem_count < lim; i++) {
            JsString *c = js_string_cell_new(ctx->vm, s->units + i, 1);
            ok = c && js_array_append(ctx->vm, out, js_value_from_cell(&c->gc));
        }
    } else {
        uint32_t pos = 0;
        while (out->elem_count < lim) {
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
        JsString *a = js_op_to_string(ctx, args[i]); /* copied out before the next converts */
        if (!a) {
            sb_free(&sb);
            return oom(ctx, r);
        }
        sb_units(&sb, a->units, a->length);
    }
    *r = sb_finish(&sb);
    return js_is_undefined(*r) && sb.len ? oom(ctx, r) : true;
}

/* thisNumberValue / thisStringValue / thisBooleanValue: the receiver must be that primitive (there
 * are no wrapper objects), or the prototype itself, which in V8 is a wrapper of 0, "" or false */
static JsValue this_primitive(JsContext *ctx, JsValue tv) {
    if (js_is_object(tv)) {
        JsObject *o = js_value_object(tv);
        if (o == ctx->number_methods)
            return js_number(0);
        if (o == ctx->boolean_methods)
            return js_bool(false);
        if (o == ctx->string_methods) {
            JsString *e = js_string_cell_new(ctx->vm, NULL, 0);
            return e ? js_value_from_cell(&e->gc) : js_undefined(); /* undefined: fails the check */
        }
    }
    return tv;
}

/* thisStringValue: the receiver must be a string (there are no String objects) */
static bool sm_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    tv = this_primitive(ctx, tv);
    if (!js_is_string(tv))
        return native_throw(ctx, r, "TypeError: String.prototype.toString requires that 'this' be a String");
    *r = tv;
    return true;
}

static bool sm_valueOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    tv = this_primitive(ctx, tv);
    if (!js_is_string(tv))
        return native_throw(ctx, r, "TypeError: String.prototype.valueOf requires that 'this' be a String");
    *r = tv;
    return true;
}

/* Boolean.prototype: thisBooleanValue, as the others (no Boolean objects) */
static bool bm_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    tv = this_primitive(ctx, tv);
    if (!js_is_bool(tv))
        return native_throw(ctx, r, "TypeError: Boolean.prototype.toString requires that 'this' be a Boolean");
    JsString *s = js_ascii_cell(ctx->vm, js_get_bool(tv) ? "true" : "false");
    if (!s)
        return oom(ctx, r);
    *r = js_value_from_cell(&s->gc);
    return true;
}

static bool bm_valueOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    tv = this_primitive(ctx, tv);
    if (!js_is_bool(tv))
        return native_throw(ctx, r, "TypeError: Boolean.prototype.valueOf requires that 'this' be a Boolean");
    *r = tv;
    return true;
}

/* =====================================================================
 * Array methods
 *
 * Every one is generic (ECMA-262 23.1.3): `this` may be anything with a `length` and indices. An
 * array, which is nearly every receiver, takes the method's own path over its element vector;
 * anything else leaves through GENERIC for the spec's steps, a property at a time (gen_run,
 * after the methods).
 * ===================================================================== */

typedef struct Gen Gen;
typedef bool (*GenBody)(Gen *g, const JsValue *args, int argc);
static bool gen_run(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r,
                    const char *name, GenBody body, int kind);
static bool gen_push(Gen *g, const JsValue *args, int argc);
static bool gen_pop(Gen *g, const JsValue *args, int argc);
static bool gen_shift(Gen *g, const JsValue *args, int argc);
static bool gen_unshift(Gen *g, const JsValue *args, int argc);
static bool gen_at(Gen *g, const JsValue *args, int argc);
static bool gen_indexOf(Gen *g, const JsValue *args, int argc);
static bool gen_lastIndexOf(Gen *g, const JsValue *args, int argc);
static bool gen_slice(Gen *g, const JsValue *args, int argc);
static bool gen_join(Gen *g, const JsValue *args, int argc);
static bool gen_reverse(Gen *g, const JsValue *args, int argc);
static bool gen_fill(Gen *g, const JsValue *args, int argc);
static bool gen_iterate(Gen *g, const JsValue *args, int argc);
static bool gen_reduce(Gen *g, const JsValue *args, int argc);
static bool gen_sort(Gen *g, const JsValue *args, int argc);
static bool gen_flat(Gen *g, const JsValue *args, int argc);
static bool gen_splice(Gen *g, const JsValue *args, int argc);
static bool gen_copyWithin(Gen *g, const JsValue *args, int argc);
static bool gen_copy(Gen *g, const JsValue *args, int argc);
static bool gen_concat(Gen *g, const JsValue *args, int argc);

static inline bool is_array(JsValue v) {
    return js_is_object(v) && js_value_object(v)->obj_kind == JS_OBJ_ARRAY;
}

/* TV is an array whose `constructor` is the Array the engine gave Array.prototype, found there:
 * what ArraySpeciesCreate makes for it is a plain array, and reading that has no effect a script
 * could see. Any other array (an own `constructor`, another prototype, Array.prototype's
 * replaced) takes the generic steps, which ask (gen_out_new). */
static inline bool array_plain_species(JsContext *ctx, JsValue tv) {
    if (!is_array(tv))
        return false;
    const JsObject *a = js_value_object(tv);
    return js_is_object(a->proto) && js_value_object(a->proto) == ctx->array_proto &&
           js_intact(ctx, JS_INTACT_ARRAY_CONSTRUCTOR) &&
           (a->props.used == 0 ||
            js_map_position(&a->props, ctx->intact[JS_INTACT_ARRAY_CONSTRUCTOR].key) == JS_MAP_EMPTY);
}

/* A receiver that is not an array: BODY's steps on it, and that is the method's result. NAME is
 * the method V8 names when the receiver is undefined or null ("Array.prototype.NAME called on
 * null or undefined"), or NULL where it says only that they do not convert to an object; KIND
 * tells a body shared by several methods which one it is. */
#define GENERIC(name, body, kind)                                                                  \
    do {                                                                                           \
        if (JS_UNLIKELY(!is_array(tv)))                                                            \
            return gen_run(ctx, tv, args, argc, r, name, body, kind);                              \
    } while (0)
/* The same for a method whose result ArraySpeciesCreate makes: an array takes the direct path
 * only when that is a plain array */
#define GENERIC_SPECIES(name, body, kind)                                                          \
    do {                                                                                           \
        if (JS_UNLIKELY(!array_plain_species(ctx, tv)))                                            \
            return gen_run(ctx, tv, args, argc, r, name, body, kind);                              \
    } while (0)

/*
 * The mutating array methods write a->elems and a->elem_count directly, so
 * they never reach set_property's freeze check and have to make it themselves.
 * `changes_length` separates the two states: sealing forbids adding and
 * removing elements, freezing forbids touching them at all, so reverse and fill
 * are legal on a sealed array but not a frozen one. (sort writes its result
 * back after the comparator has run, and checks there: sort_array.)
 */
typedef enum {
    MUT_ADD,          /* push, unshift: new elements at the end */
    MUT_REMOVE_END,   /* pop: deletes the last element */
    MUT_REMOVE_FRONT, /* shift: moves the elements down, then deletes the last */
    MUT_WRITE,        /* fill, copyWithin: writes elements in place */
    MUT_REORDER,      /* reverse: as MUT_WRITE, but fewer than two elements stay put */
} ArrayMut;

/* The refusal is worded as V8 words the first write such a method makes. */
static bool array_mut_ok(JsContext *ctx, JsObject *a, JsValue *r, ArrayMut how) {
    uint8_t f = a->obj_flags & JS_OBJ_NO_ADD; /* named properties' attributes do not matter */
    if (!f)
        return true;
    bool frozen = (f & JS_OBJ_FROZEN) != 0;
    if (how != MUT_ADD && !(f & (JS_OBJ_SEALED | JS_OBJ_FROZEN)))
        return true; /* not extensible only: elements may still be removed and moved */
    double n = (double)a->elem_count;
    const char *read_only = "TypeError: Cannot assign to read only property '%s' of object '[object Array]'";
    switch (how) {
    case MUT_ADD:
        *r = js_error_fmt(ctx, "TypeError: Cannot add property %v, object is not extensible", js_number(n));
        return false;
    case MUT_REMOVE_END:
    case MUT_REMOVE_FRONT:
        if (n == 0) {
            if (!frozen)
                return true;
            *r = js_error_fmt(ctx, read_only, "length");
            return false;
        }
        if (frozen && how == MUT_REMOVE_FRONT && n > 1) {
            *r = js_error_fmt(ctx, read_only, "0");
            return false;
        }
        *r = js_error_fmt(ctx, "TypeError: Cannot delete property '%v' of [object Array]", js_number(n - 1));
        return false;
    case MUT_WRITE:
    case MUT_REORDER:
        if (!frozen || n == 0 || (how == MUT_REORDER && n < 2))
            return true;
        *r = js_error_fmt(ctx, read_only, "0");
        return false;
    }
    return true;
}

static bool am_push(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    GENERIC(NULL, gen_push, 0);
    JsObject *a = js_value_object(tv);
    if (!array_mut_ok(ctx, a, r, MUT_ADD))
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
    GENERIC(NULL, gen_pop, 0);
    JsObject *a = js_value_object(tv);
    if (!array_mut_ok(ctx, a, r, MUT_REMOVE_END))
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
    GENERIC(NULL, gen_shift, 0);
    JsObject *a = js_value_object(tv);
    if (!array_mut_ok(ctx, a, r, MUT_REMOVE_FRONT))
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
    GENERIC(NULL, gen_unshift, 0);
    JsObject *a = js_value_object(tv);
    if (!array_mut_ok(ctx, a, r, MUT_ADD))
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
    GENERIC(NULL, gen_at, 0);
    JsObject *a = js_value_object(tv);
    double d;
    TO_NUM(d, ARG(0));
    d = to_int(d);
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
    GENERIC("indexOf", gen_indexOf, 0);
    JsObject *a = js_value_object(tv);
    /* the length first, then fromIndex, as V8: a valueOf that shrinks the array ends the search */
    uint32_t len = a->elem_count;
    double fd = 0;
    if (argc > 1)
        TO_NUM(fd, ARG(1));
    uint32_t from = clamp_index(fd, len);
    for (uint32_t i = from; i < len && i < a->elem_count; i++) {
        if (js_strict_equals(a->elems[i], ARG(0))) {
            *r = js_number(i);
            return true;
        }
    }
    *r = js_number(-1);
    return true;
}

/* Array.prototype.lastIndexOf (23.1.3.20): the length, then fromIndex (default length - 1) */
static bool am_lastIndexOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    GENERIC(NULL, gen_lastIndexOf, 0);
    JsObject *a = js_value_object(tv);
    uint32_t len = a->elem_count;
    *r = js_number(-1);
    if (len == 0)
        return true;
    double n = (double)len - 1;
    if (argc > 1) {
        TO_NUM(n, ARG(1));
        n = to_int(n);
    }
    double k = n >= 0 ? (n < (double)len - 1 ? n : (double)len - 1) : (double)len + n;
    for (; k >= 0; k--) {
        uint32_t i = (uint32_t)k;
        if (i < a->elem_count && js_strict_equals(a->elems[i], ARG(0))) {
            *r = js_number(i);
            return true;
        }
    }
    *r = js_number(-1);
    return true;
}

/* SameValueZero: strict equality, except that NaN equals NaN */
static bool same_value_zero(JsValue x, JsValue y) {
    if (js_is_number(x) && js_is_number(y)) {
        double dx = js_get_number(x), dy = js_get_number(y);
        return dx == dy || (dx != dx && dy != dy);
    }
    return js_strict_equals(x, y);
}

/* Array.prototype.includes (23.1.3.16): SameValueZero from fromIndex; an index a valueOf removed
 * reads undefined */
static bool am_includes(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    GENERIC(NULL, gen_indexOf, 1);
    JsObject *a = js_value_object(tv);
    uint32_t len = a->elem_count;
    *r = js_bool(false);
    if (len == 0)
        return true;
    double fd = 0;
    if (argc > 1)
        TO_NUM(fd, ARG(1));
    for (uint32_t i = clamp_index(fd, len); i < len; i++) {
        if (same_value_zero(i < a->elem_count ? a->elems[i] : js_undefined(), ARG(0))) {
            *r = js_bool(true);
            return true;
        }
    }
    return true;
}

static bool am_slice(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    GENERIC_SPECIES(NULL, gen_slice, 0);
    JsObject *a = js_value_object(tv);
    /* the length first, then start and end, as V8; an element a valueOf removed reads undefined */
    uint32_t len = a->elem_count;
    double sd, ed = len;
    TO_NUM(sd, ARG(0));
    if (argc > 1 && !js_is_undefined(ARG(1)))
        TO_NUM(ed, ARG(1));
    uint32_t start = clamp_index(sd, len);
    uint32_t end = clamp_index(ed, len);
    JsObject *out = js_array_new_cell(ctx, end > start ? end - start : 0);
    if (!out)
        return oom(ctx, r);
    for (uint32_t i = start; i < end; i++)
        out->elems[out->elem_count++] = i < a->elem_count ? a->elems[i] : js_undefined();
    *r = js_value_from_cell(&out->gc);
    return true;
}

/* Array.prototype.join's cycle check, as V8's: an array already being joined further out joins as
 * the empty string. False on OOM. */
static bool join_enter(JsContext *ctx, JsObject *a, bool *cyclic) {
    for (uint32_t i = 0; i < ctx->join_len; i++) {
        if (ctx->join_stack[i] == a) {
            *cyclic = true;
            return true;
        }
    }
    *cyclic = false;
    if (ctx->join_len == ctx->join_cap) {
        uint32_t ncap = ctx->join_cap ? ctx->join_cap * 2 : 8;
        JsObject **ns = js_realloc_raw(ctx->vm, ctx->join_stack, ctx->join_cap * sizeof *ns,
                                       ncap * sizeof *ns);
        if (!ns)
            return false;
        ctx->join_stack = ns;
        ctx->join_cap = ncap;
    }
    ctx->join_stack[ctx->join_len++] = a;
    return true;
}

/* Array.prototype.join (23.1.3.18): the length, then the separator, then each element's ToString
 * (an element's own toString may run, and may change the array: one it removed reads undefined). */
static bool am_join(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    GENERIC(NULL, gen_join, 0);
    JsObject *a = js_value_object(tv);
    bool cyclic;
    if (!join_enter(ctx, a, &cyclic))
        return oom(ctx, r);
    if (cyclic) {
        JsString *e = js_string_cell_new(ctx->vm, NULL, 0);
        if (!e)
            return oom(ctx, r);
        *r = js_value_from_cell(&e->gc);
        return true;
    }
    uint32_t len = a->elem_count;
    const uint16_t comma[] = {','};
    JsString *sep = NULL;
    bool ok = true;
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    if (argc > 0 && !js_is_undefined(ARG(0))) {
        sep = arg_string(ctx, ARG(0));
        ok = sep != NULL;
    }
    for (uint32_t i = 0; ok && i < len; i++) {
        if (i) {
            if (sep)
                sb_units(&sb, sep->units, sep->length);
            else
                sb_units(&sb, comma, 1);
        }
        JsValue el = i < a->elem_count ? a->elems[i] : js_undefined();
        if (js_is_undefined(el) || js_is_null(el))
            continue;
        JsString *es = js_op_to_string(ctx, el); /* copied out before the next converts */
        if (!es) {
            ok = false;
            break;
        }
        sb_units(&sb, es->units, es->length);
    }
    ctx->join_len--;
    if (!ok) {
        sb_free(&sb);
        return oom(ctx, r);
    }
    if (!sb.oom && sb.len > JS_MAX_STRING_UNITS) {
        sb_free(&sb);
        return native_throw(ctx, r, "RangeError: Invalid string length");
    }
    *r = sb_finish(&sb);
    return js_is_undefined(*r) && sb.oom ? oom(ctx, r) : true;
}

static bool am_reverse(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    GENERIC(NULL, gen_reverse, 0);
    JsObject *a = js_value_object(tv);
    if (!array_mut_ok(ctx, a, r, MUT_REORDER))
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
    GENERIC(NULL, gen_fill, 0);
    JsObject *a = js_value_object(tv);
    uint32_t len = a->elem_count;
    double sd = 0, ed = len;
    if (argc > 1)
        TO_NUM(sd, ARG(1));
    if (argc > 2 && !js_is_undefined(ARG(2)))
        TO_NUM(ed, ARG(2));
    /* refused at the first write, as V8 (none for an empty range); a valueOf may have frozen or
     * shortened it, and an index past the end is written as a property set would */
    uint32_t start = clamp_index(sd, len), end = clamp_index(ed, len);
    if (start < end && (a->obj_flags & JS_OBJ_FROZEN))
        return native_throw_fmt(ctx, r, "TypeError: Cannot assign to read only property '%v' of object '[object Array]'",
                                js_number(start));
    for (uint32_t i = start; i < end; i++) {
        if (i < a->elem_count)
            a->elems[i] = ARG(0);
        else if (!js_array_set_index(ctx->vm, a, i, ARG(0)))
            return oom(ctx, r);
    }
    *r = tv;
    return true;
}

/* Array.prototype.concat (23.1.3.2): the receiver, then each argument; an array's elements, and
 * anything else as itself (there is no Symbol.isConcatSpreadable, so only an array spreads). A
 * primitive receiver is the first element as it is: V8 has its wrapper there. */
static bool am_concat(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (js_is_undefined(tv) || js_is_null(tv))
        return native_throw(ctx, r, "TypeError: Array.prototype.concat called on null or undefined");
    if (is_array(tv) && !array_plain_species(ctx, tv))
        return gen_run(ctx, tv, args, argc, r, NULL, gen_concat, 0);
    JsObject *a = is_array(tv) ? js_value_object(tv) : NULL;
    JsObject *out = js_array_new_cell(ctx, a ? a->elem_count : 1);
    if (!out)
        return oom(ctx, r);
    JsValue outv = js_value_from_cell(&out->gc);
    js_gc_protect(ctx->vm, &outv);
    bool ok = a ? true : js_array_append(ctx->vm, out, tv);
    for (uint32_t i = 0; a && i < a->elem_count && ok; i++)
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
static const char *const iter_names[] = {"map", "filter", "forEach", "some", "every", "find", "findIndex"};
/* the two more the generic steps tell apart (gen_iterate) */
enum { IT_FINDLAST = IT_FINDINDEX + 1, IT_FINDLASTINDEX };

static bool array_iterate(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                          JsValue *r, IterKind kind) {
    if (kind == IT_MAP || kind == IT_FILTER)
        GENERIC_SPECIES(iter_names[kind], gen_iterate, kind);
    GENERIC(iter_names[kind], gen_iterate, kind);
    JsObject *a = js_value_object(tv);
    if (!js_is_function(ARG(0)))
        return native_throw_fmt(ctx, r, "TypeError: %t is not a function", ARG(0));
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
    /* the length is read once, as V8: what the callback appends is not visited, and an index it
     * removed is skipped (map leaves undefined there, V8 a hole) except by find and findIndex,
     * which see undefined */
    uint32_t len = a->elem_count;
    for (uint32_t i = 0; i < len && ok; i++) {
        if (i >= a->elem_count && kind != IT_FIND && kind != IT_FINDINDEX) {
            if (kind == IT_MAP)
                ok = js_array_append(ctx->vm, out, js_undefined());
            continue;
        }
        JsValue elem = i < a->elem_count ? a->elems[i] : js_undefined();
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
                i = len;
            }
            break;
        case IT_EVERY:
            if (!js_to_boolean(cbres)) {
                result = js_bool(false);
                i = len;
            }
            break;
        case IT_FIND:
            if (js_to_boolean(cbres)) {
                result = elem;
                i = len;
            }
            break;
        case IT_FINDINDEX:
            if (js_to_boolean(cbres)) {
                result = js_number(i);
                i = len;
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
    GENERIC("reduce", gen_reduce, 0);
    JsObject *a = js_value_object(tv);
    if (!js_is_function(ARG(0)))
        return native_throw_fmt(ctx, r, "TypeError: %t is not a function", ARG(0));
    JsValue cb = ARG(0), arrv = tv, acc;
    uint32_t i = 0;
    if (argc > 1) {
        acc = ARG(1);
    } else {
        if (a->elem_count == 0)
            return native_throw(ctx, r, "TypeError: Reduce of empty array with no initial value");
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

/* ---------------------------------------------------------------------
 * Array.prototype.sort and toSorted: V8's TimSort
 *
 * A port of V8's third_party/v8/builtins/array-sort.tq as V8 14.6 has it
 * (itself CPython's listsort), statement for statement, so that the comparator
 * is called with the same pairs in the same order as in V8. The order is
 * observable: in what a comparator logs, and in the result when the comparator
 * is not a consistent ordering (`(a, b) => a > b`, a random one).
 *
 * As there, the elements that are not undefined are sorted in a work array the
 * script cannot reach and written back afterwards, the undefined ones after
 * them: whatever the comparator does to the array meanwhile neither disturbs
 * the sort nor survives it. Only `order < 0` is ever asked of a comparison, so
 * one is carried here as "less".
 * --------------------------------------------------------------------- */

enum { SORT_MAX_RUNS = 85, SORT_MIN_GALLOP = 7, SORT_TEMP_MIN = 32 };

typedef struct {
    JsContext *ctx;
    JsValue *r;           /* where the error of a failed sort goes */
    JsValue cmp;          /* the comparator, or undefined (rooted, as the three below are) */
    JsValue workv, tempv; /* the work array and the merge buffer: hidden array cells */
    JsValue str;          /* the default comparison: x's string while y is converted */
    JsObject *work;
    int64_t min_gallop;
    int runs; /* the pending runs, a stack */
    int64_t run_base[SORT_MAX_RUNS], run_len[SORT_MAX_RUNS];
} SortState;

/* An array cell of N undefined elements, for the collector to trace what is put in it */
static JsObject *sort_buffer(JsContext *ctx, int64_t n) {
    JsObject *o = js_array_new_cell(ctx, (uint32_t)(n ? n : 1));
    if (!o)
        return NULL;
    for (int64_t i = 0; i < n; i++)
        o->elems[i] = js_undefined();
    o->elem_count = (uint32_t)n;
    return o;
}

/* The merge buffer, at least N long (GetTempArray); NULL thrown */
static JsValue *sort_temp(SortState *st, int64_t n) {
    JsObject *t = js_is_undefined(st->tempv) ? NULL : js_value_object(st->tempv);
    if (!t || t->elem_count < n) {
        t = sort_buffer(st->ctx, n < SORT_TEMP_MIN ? SORT_TEMP_MIN : n);
        if (!t) {
            oom(st->ctx, st->r);
            return NULL;
        }
        st->tempv = js_value_from_cell(&t->gc);
    }
    return t->elems;
}

/* Compare(x, y) < 0: the comparator's result through ToNumber (NaN is 0), or without one
 * ToString(x) against ToString(y) by code unit. False thrown. */
static bool sort_less(SortState *st, JsValue x, JsValue y, bool *less) {
    JsContext *ctx = st->ctx;
    /* A native costs one unit of fuel however long it runs, and a large sort is the longest a
     * guest can stay inside one: poll the stop flag so an interrupt does not wait for it. */
    if (ctx->vm->interrupt)
        return native_throw(ctx, st->r, JS_MSG_INTERRUPT);
    if (!js_is_undefined(st->cmp)) {
        JsValue argv[2] = {x, y};
        JsValue res;
        if (!js_call(ctx, st->cmp, js_undefined(), argv, 2, &res)) {
            *st->r = res;
            return false;
        }
        double d;
        if (!js_op_to_number(ctx, res, &d))
            return oom(ctx, st->r);
        *less = d < 0;
        return true;
    }
    JsString *sx = js_op_to_string(ctx, x);
    if (!sx)
        return oom(ctx, st->r);
    st->str = js_value_from_cell(&sx->gc); /* converting y allocates, and may run a toString */
    JsString *sy = js_op_to_string(ctx, y);
    if (!sy)
        return oom(ctx, st->r);
    uint32_t n = sx->length < sy->length ? sx->length : sy->length;
    for (uint32_t k = 0; k < n; k++)
        if (sx->units[k] != sy->units[k]) {
            *less = sx->units[k] < sy->units[k];
            return true;
        }
    *less = sx->length < sy->length;
    return true;
}

/* Sorts [low, high) of the work array, of which [low, start) is sorted already */
static bool sort_binary_insertion(SortState *st, int64_t low, int64_t start, int64_t high) {
    JsValue *w = st->work->elems;
    if (start == low)
        start++;
    for (; start < high; start++) {
        int64_t left = low, right = start;
        JsValue pivot = w[right];
        while (left < right) {
            int64_t mid = left + ((right - left) >> 1);
            bool less;
            if (!sort_less(st, pivot, w[mid], &less))
                return false;
            if (less)
                right = mid;
            else
                left = mid + 1;
        }
        for (int64_t p = start; p > left; p--)
            w[p] = w[p - 1];
        w[left] = pivot;
    }
    return true;
}

/* The length of the run that starts at LOW (an ascending one, or a strictly descending one,
 * which is reversed in place) */
static bool sort_count_run(SortState *st, int64_t low_arg, int64_t high, int64_t *run) {
    JsValue *w = st->work->elems;
    int64_t low = low_arg + 1;
    *run = 1;
    if (low == high)
        return true;
    int64_t len = 2;
    bool less;
    if (!sort_less(st, w[low], w[low - 1], &less))
        return false;
    bool descending = less;
    for (int64_t i = low + 1; i < high; i++) {
        if (!sort_less(st, w[i], w[i - 1], &less))
            return false;
        if (less != descending)
            break;
        len++;
    }
    if (descending)
        for (int64_t a = low_arg, b = low_arg + len - 1; a < b; a++, b--) {
            JsValue t = w[a];
            w[a] = w[b];
            w[b] = t;
        }
    *run = len;
    return true;
}

/* Where KEY belongs in the sorted A[base, base + length), before any equal to it, searching
 * outwards from HINT: the count of elements less than KEY */
static bool sort_gallop_left(SortState *st, const JsValue *a, JsValue key, int64_t base,
                             int64_t length, int64_t hint, int64_t *out) {
    int64_t last = 0, ofs = 1;
    bool less;
    if (!sort_less(st, a[base + hint], key, &less))
        return false;
    if (less) {
        int64_t max = length - hint;
        while (ofs < max) {
            if (!sort_less(st, a[base + hint + ofs], key, &less))
                return false;
            if (!less)
                break;
            last = ofs;
            ofs = (ofs << 1) + 1;
        }
        if (ofs > max)
            ofs = max;
        last += hint;
        ofs += hint;
    } else {
        int64_t max = hint + 1;
        while (ofs < max) {
            if (!sort_less(st, a[base + hint - ofs], key, &less))
                return false;
            if (less)
                break;
            last = ofs;
            ofs = (ofs << 1) + 1;
        }
        if (ofs > max)
            ofs = max;
        int64_t tmp = last;
        last = hint - ofs;
        ofs = hint - tmp;
    }
    last++;
    while (last < ofs) {
        int64_t m = last + ((ofs - last) >> 1);
        if (!sort_less(st, a[base + m], key, &less))
            return false;
        if (less)
            last = m + 1;
        else
            ofs = m;
    }
    *out = ofs;
    return true;
}

/* As sort_gallop_left, but after any equal to KEY: the count of elements not greater */
static bool sort_gallop_right(SortState *st, const JsValue *a, JsValue key, int64_t base,
                              int64_t length, int64_t hint, int64_t *out) {
    int64_t last = 0, ofs = 1;
    bool less;
    if (!sort_less(st, key, a[base + hint], &less))
        return false;
    if (less) {
        int64_t max = hint + 1;
        while (ofs < max) {
            if (!sort_less(st, key, a[base + hint - ofs], &less))
                return false;
            if (!less)
                break;
            last = ofs;
            ofs = (ofs << 1) + 1;
        }
        if (ofs > max)
            ofs = max;
        int64_t tmp = last;
        last = hint - ofs;
        ofs = hint - tmp;
    } else {
        int64_t max = length - hint;
        while (ofs < max) {
            if (!sort_less(st, key, a[base + hint + ofs], &less))
                return false;
            if (less)
                break;
            last = ofs;
            ofs = (ofs << 1) + 1;
        }
        if (ofs > max)
            ofs = max;
        last += hint;
        ofs += hint;
    }
    last++;
    while (last < ofs) {
        int64_t m = last + ((ofs - last) >> 1);
        if (!sort_less(st, key, a[base + m], &less))
            return false;
        if (less)
            ofs = m;
        else
            last = m + 1;
    }
    *out = ofs;
    return true;
}

static void sort_copy(JsValue *dst, const JsValue *src, int64_t n) {
    memmove(dst, src, (size_t)n * sizeof *dst);
}

/* Merges the adjacent runs A and B of the work array, A no longer than B: A goes to the buffer
 * and the merge fills from the left */
static bool sort_merge_low(SortState *st, int64_t base_a, int64_t len_a, int64_t base_b, int64_t len_b) {
    JsValue *t = sort_temp(st, len_a);
    if (!t)
        return false;
    JsValue *w = st->work->elems;
    sort_copy(t, w + base_a, len_a);
    int64_t dest = base_a, ct = 0, cb = base_b;
    w[dest++] = w[cb++];
    if (--len_b == 0)
        goto succeed;
    if (len_a == 1)
        goto copy_b;
    int64_t min_gallop = st->min_gallop;
    for (;;) {
        int64_t wins_a = 0, wins_b = 0; /* how many times in a row each run has won */
        for (;;) {
            bool less;
            if (!sort_less(st, w[cb], t[ct], &less))
                return false;
            if (less) {
                w[dest++] = w[cb++];
                wins_b++;
                len_b--;
                wins_a = 0;
                if (len_b == 0)
                    goto succeed;
                if (wins_b >= min_gallop)
                    break;
            } else {
                w[dest++] = t[ct++];
                wins_a++;
                len_a--;
                wins_b = 0;
                if (len_a == 1)
                    goto copy_b;
                if (wins_a >= min_gallop)
                    break;
            }
        }
        /* one run is winning consistently: gallop, until neither does */
        min_gallop++;
        for (bool first = true; wins_a >= SORT_MIN_GALLOP || wins_b >= SORT_MIN_GALLOP || first;) {
            first = false;
            min_gallop = min_gallop > 2 ? min_gallop - 1 : 1;
            st->min_gallop = min_gallop;
            if (!sort_gallop_right(st, t, w[cb], ct, len_a, 0, &wins_a))
                return false;
            if (wins_a > 0) {
                sort_copy(w + dest, t + ct, wins_a);
                dest += wins_a;
                ct += wins_a;
                len_a -= wins_a;
                if (len_a == 1)
                    goto copy_b;
                if (len_a == 0) /* only with a comparator that is not consistent */
                    goto succeed;
            }
            w[dest++] = w[cb++];
            if (--len_b == 0)
                goto succeed;
            if (!sort_gallop_left(st, w, t[ct], cb, len_b, 0, &wins_b))
                return false;
            if (wins_b > 0) {
                sort_copy(w + dest, w + cb, wins_b);
                dest += wins_b;
                cb += wins_b;
                len_b -= wins_b;
                if (len_b == 0)
                    goto succeed;
            }
            w[dest++] = t[ct++];
            if (--len_a == 1)
                goto copy_b;
        }
        min_gallop++; /* the price of leaving galloping mode */
        st->min_gallop = min_gallop;
    }
succeed:
    if (len_a > 0)
        sort_copy(w + dest, t + ct, len_a);
    return true;
copy_b: /* the last of A goes after all that is left of B */
    sort_copy(w + dest, w + cb, len_b);
    w[dest + len_b] = t[ct];
    return true;
}

/* As sort_merge_low with B no longer than A: B goes to the buffer, filling from the right */
static bool sort_merge_high(SortState *st, int64_t base_a, int64_t len_a, int64_t base_b, int64_t len_b) {
    JsValue *t = sort_temp(st, len_b);
    if (!t)
        return false;
    JsValue *w = st->work->elems;
    sort_copy(t, w + base_b, len_b);
    int64_t dest = base_b + len_b - 1, ct = len_b - 1, ca = base_a + len_a - 1;
    w[dest--] = w[ca--];
    if (--len_a == 0)
        goto succeed;
    if (len_b == 1)
        goto copy_a;
    int64_t min_gallop = st->min_gallop;
    for (;;) {
        int64_t wins_a = 0, wins_b = 0;
        for (;;) {
            bool less;
            if (!sort_less(st, t[ct], w[ca], &less))
                return false;
            if (less) {
                w[dest--] = w[ca--];
                wins_a++;
                len_a--;
                wins_b = 0;
                if (len_a == 0)
                    goto succeed;
                if (wins_a >= min_gallop)
                    break;
            } else {
                w[dest--] = t[ct--];
                wins_b++;
                len_b--;
                wins_a = 0;
                if (len_b == 1)
                    goto copy_a;
                if (wins_b >= min_gallop)
                    break;
            }
        }
        min_gallop++;
        for (bool first = true; wins_a >= SORT_MIN_GALLOP || wins_b >= SORT_MIN_GALLOP || first;) {
            first = false;
            min_gallop = min_gallop > 2 ? min_gallop - 1 : 1;
            st->min_gallop = min_gallop;
            int64_t k;
            if (!sort_gallop_right(st, w, t[ct], base_a, len_a, len_a - 1, &k))
                return false;
            wins_a = len_a - k;
            if (wins_a > 0) {
                dest -= wins_a;
                ca -= wins_a;
                sort_copy(w + dest + 1, w + ca + 1, wins_a);
                len_a -= wins_a;
                if (len_a == 0)
                    goto succeed;
            }
            w[dest--] = t[ct--];
            if (--len_b == 1)
                goto copy_a;
            if (!sort_gallop_left(st, t, w[ca], 0, len_b, len_b - 1, &k))
                return false;
            wins_b = len_b - k;
            if (wins_b > 0) {
                dest -= wins_b;
                ct -= wins_b;
                sort_copy(w + dest + 1, t + ct + 1, wins_b);
                len_b -= wins_b;
                if (len_b == 1)
                    goto copy_a;
                if (len_b == 0) /* only with a comparator that is not consistent */
                    goto succeed;
            }
            w[dest--] = w[ca--];
            if (--len_a == 0)
                goto succeed;
        }
        min_gallop++;
        st->min_gallop = min_gallop;
    }
succeed:
    if (len_b > 0)
        sort_copy(w + dest - (len_b - 1), t, len_b);
    return true;
copy_a: /* the first of B goes before all that is left of A */
    dest -= len_a;
    ca -= len_a;
    sort_copy(w + dest + 1, w + ca + 1, len_a);
    w[dest] = t[ct];
    return true;
}

/* Merges pending runs I and I + 1 (I is the second or the third from the top) */
static bool sort_merge_at(SortState *st, int i) {
    JsValue *w = st->work->elems;
    int64_t base_a = st->run_base[i], len_a = st->run_len[i];
    int64_t base_b = st->run_base[i + 1], len_b = st->run_len[i + 1];
    st->run_len[i] = len_a + len_b;
    if (i == st->runs - 3) {
        st->run_base[i + 1] = st->run_base[i + 2];
        st->run_len[i + 1] = st->run_len[i + 2];
    }
    st->runs--;
    /* what of A is already in place before B's first, and of B after A's last, needs no merging */
    int64_t k;
    if (!sort_gallop_right(st, w, w[base_b], base_a, len_a, 0, &k))
        return false;
    base_a += k;
    len_a -= k;
    if (len_a == 0)
        return true;
    if (!sort_gallop_left(st, w, w[base_a + len_a - 1], base_b, len_b, len_b - 1, &len_b))
        return false;
    if (len_b == 0)
        return true;
    return len_a <= len_b ? sort_merge_low(st, base_a, len_a, base_b, len_b)
                          : sort_merge_high(st, base_a, len_a, base_b, len_b);
}

/* run N and the two below it: len[N - 2] > len[N - 1] + len[N] */
static bool sort_run_invariant(const SortState *st, int n) {
    return n < 2 || st->run_len[n - 2] > st->run_len[n - 1] + st->run_len[n];
}

/* Merges runs until the stack's invariants hold again (MergeCollapse) */
static bool sort_merge_collapse(SortState *st) {
    while (st->runs > 1) {
        int n = st->runs - 2;
        if (!sort_run_invariant(st, n + 1) || !sort_run_invariant(st, n)) {
            if (st->run_len[n - 1] < st->run_len[n + 1])
                n--;
        } else if (st->run_len[n] > st->run_len[n + 1]) {
            break;
        }
        if (!sort_merge_at(st, n))
            return false;
    }
    return true;
}

/* Sorts the work array. Array.prototype.sort inserts fewer than 8 elements one by one from the
 * first (ArrayTimSort); toSorted does not have that shortcut (ArrayTimSortIntoCopy), and the
 * comparator can tell. */
static bool sort_work(SortState *st, bool to_sorted) {
    int64_t length = st->work->elem_count;
    if (!to_sorted && length < 8)
        return sort_binary_insertion(st, 0, 0, length);
    if (length < 2)
        return true;
    int64_t min_run = length, bit = 0; /* ComputeMinRunLength */
    while (min_run >= 64) {
        bit |= min_run & 1;
        min_run >>= 1;
    }
    min_run += bit;
    for (int64_t low = 0, remaining = length; remaining != 0;) {
        int64_t run;
        if (!sort_count_run(st, low, low + remaining, &run))
            return false;
        if (run < min_run) {
            int64_t forced = min_run < remaining ? min_run : remaining;
            if (!sort_binary_insertion(st, low, low + run, low + forced))
                return false;
            run = forced;
        }
        if (st->runs == SORT_MAX_RUNS) /* cannot be: the invariants bound the stack far below */
            return native_throw(st->ctx, st->r, "internal: sort run stack");
        st->run_base[st->runs] = low;
        st->run_len[st->runs++] = run;
        if (!sort_merge_collapse(st))
            return false;
        low += run;
        remaining -= run;
    }
    while (st->runs > 1) { /* MergeForceCollapse */
        int n = st->runs - 2;
        if (n > 0 && st->run_len[n - 1] < st->run_len[n + 1])
            n--;
        if (!sort_merge_at(st, n))
            return false;
    }
    return true;
}

/* sort (in place, the result ARRV) or toSorted (the result a new array) of the array ARRV under
 * CMP, a function or undefined */
static bool sort_array(JsContext *ctx, JsValue arrv, JsValue cmp, bool to_sorted, JsValue *r) {
    SortState st;
    st.ctx = ctx;
    st.r = r;
    st.cmp = cmp;
    st.workv = st.tempv = st.str = js_undefined();
    st.min_gallop = SORT_MIN_GALLOP;
    st.runs = 0;
    js_gc_protect(ctx->vm, &arrv);
    js_gc_protect(ctx->vm, &st.cmp);
    js_gc_protect(ctx->vm, &st.workv);
    js_gc_protect(ctx->vm, &st.tempv);
    js_gc_protect(ctx->vm, &st.str);
    JsObject *a = js_value_object(arrv);
    uint32_t n = a->elem_count; /* the length the sort is of, whatever the comparator makes of it */
    bool ok = false;
    JsObject *work = st.work = js_array_new_cell(ctx, n ? n : 1);
    if (!work) {
        oom(ctx, r);
    } else {
        st.workv = js_value_from_cell(&work->gc);
        for (uint32_t i = 0; i < n; i++)
            if (!js_is_undefined(a->elems[i]))
                work->elems[work->elem_count++] = a->elems[i];
        ok = sort_work(&st, to_sorted);
    }
    if (ok && to_sorted) { /* the work array is the result, once it has the undefined ones */
        while (work->elem_count < n)
            work->elems[work->elem_count++] = js_undefined();
        *r = st.workv;
    } else if (ok) {
        /* Written back as V8 does, an element at a time and as an assignment would: to an array
         * the comparator froze that is an error, one it shortened grows again, and what it
         * added past the end stays. */
        for (uint32_t i = 0; i < n && ok; i++) {
            JsValue v = i < work->elem_count ? work->elems[i] : js_undefined();
            if (i < a->elem_count) {
                if (a->obj_flags & JS_OBJ_FROZEN) {
                    *r = js_error_fmt(ctx, "TypeError: Cannot assign to read only property '%s' of object "
                                           "'[object Array]'", "0");
                    ok = false;
                } else {
                    a->elems[i] = v;
                }
            } else if (a->obj_flags & JS_OBJ_NO_ADD) {
                *r = js_error_fmt(ctx, "TypeError: Cannot add property %v, object is not extensible",
                                  js_number((double)i));
                ok = false;
            } else if (!js_array_set_index(ctx->vm, a, i, v)) {
                ok = oom(ctx, r);
            }
        }
        if (ok)
            *r = arrv;
    }
    js_gc_unprotect(ctx->vm, &st.str);
    js_gc_unprotect(ctx->vm, &st.tempv);
    js_gc_unprotect(ctx->vm, &st.workv);
    js_gc_unprotect(ctx->vm, &st.cmp);
    js_gc_unprotect(ctx->vm, &arrv);
    return ok;
}

static bool am_sort(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsValue cmp = ARG(0); /* checked before the receiver is looked at, as V8 */
    if (!js_is_undefined(cmp) && !js_is_function(cmp))
        return native_throw_fmt(ctx, r, "TypeError: The comparison function must be either a function or undefined: %v", cmp);
    GENERIC(NULL, gen_sort, 0);
    JsObject *a = js_value_object(tv);
    if (a->elem_count < 2) { /* nothing to do, to a frozen array either */
        *r = tv;
        return true;
    }
    return sort_array(ctx, tv, cmp, false, r);
}

/* FlattenIntoArray (23.1.3.13.1): SRC's elements onto OUT, the arrays among them DEPTH levels
 * down. The levels are an explicit stack, so nesting costs heap and not C stack; V8 recurses, and
 * an array that holds itself overflows its stack: past FLAT_MAX_NEST levels (as deep as calls
 * go here, JS_MAX_FRAMES) that is V8's RangeError. Nothing here runs a script, so SRC and the
 * arrays in it stay as the caller roots them. False with the error in *r. */
#define FLAT_MAX_NEST 2000u

typedef struct {
    JsObject *arr;
    uint32_t idx;
} FlatLevel;

static bool flat_into(JsContext *ctx, JsObject *out, JsObject *src, double depth, JsValue *r) {
    FlatLevel fixed[16], *st = fixed;
    uint32_t sp = 0, cap = 16;
    bool ok = true;
    st[0].arr = src;
    st[0].idx = 0;
    for (;;) {
        JsObject *cur = st[sp].arr;
        if (st[sp].idx >= cur->elem_count) {
            if (sp-- == 0)
                break;
            continue;
        }
        JsValue el = cur->elems[st[sp].idx++];
        if (!(depth - sp > 0 && is_array(el))) {
            if (!js_array_append(ctx->vm, out, el)) {
                ok = oom(ctx, r);
                break;
            }
            continue;
        }
        if (sp + 1 == FLAT_MAX_NEST) {
            ok = native_throw(ctx, r, "RangeError: Maximum call stack size exceeded");
            break;
        }
        if (sp + 1 == cap) {
            FlatLevel *grown = js_realloc_raw(ctx->vm, st == fixed ? NULL : st, st == fixed ? 0 : cap * sizeof *st,
                                              (size_t)cap * 2 * sizeof *st);
            if (!grown) {
                ok = oom(ctx, r);
                break;
            }
            if (st == fixed)
                memcpy(grown, fixed, sizeof fixed);
            st = grown;
            cap *= 2;
        }
        st[++sp].arr = js_value_object(el);
        st[sp].idx = 0;
    }
    if (st != fixed)
        js_realloc_raw(ctx->vm, st, cap * sizeof *st, 0);
    return ok;
}

static bool am_flat(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    GENERIC_SPECIES(NULL, gen_flat, 0);
    JsObject *a = js_value_object(tv);
    double depth = 1;
    if (argc > 0 && !js_is_undefined(ARG(0))) {
        TO_NUM(depth, ARG(0));
        depth = to_int(depth);
    }
    JsObject *out = js_array_new_cell(ctx, 0);
    if (!out)
        return oom(ctx, r);
    JsValue outv = js_value_from_cell(&out->gc);
    js_gc_protect(ctx->vm, &outv);
    bool ok = flat_into(ctx, out, a, depth, r);
    js_gc_unprotect(ctx->vm, &outv);
    if (ok)
        *r = outv;
    return ok;
}

/* ---- findLast, findLastIndex, reduceRight, flatMap ---- */

/* Array.prototype.findLast / findLastIndex (23.1.3.11-12): from the end, over the length read
 * once; an index the predicate removed reads undefined */
static bool find_last(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r,
                      bool want_index) {
    GENERIC(want_index ? "findLastIndex" : "findLast", gen_iterate, want_index ? IT_FINDLASTINDEX : IT_FINDLAST);
    JsObject *a = js_value_object(tv);
    if (!js_is_function(ARG(0)))
        return native_throw_fmt(ctx, r, "TypeError: %t is not a function", ARG(0));
    JsValue cb = ARG(0), thisArg = ARG(1), arrv = tv;
    js_gc_protect(ctx->vm, &arrv);
    js_gc_protect(ctx->vm, &cb);
    js_gc_protect(ctx->vm, &thisArg);
    bool ok = true;
    JsValue result = want_index ? js_number(-1) : js_undefined();
    for (uint32_t k = a->elem_count; k-- > 0;) {
        JsValue elem = k < a->elem_count ? a->elems[k] : js_undefined(), res;
        if (!call_cb(ctx, cb, thisArg, elem, k, arrv, &res)) {
            *r = res;
            ok = false;
            break;
        }
        if (js_to_boolean(res)) {
            result = want_index ? js_number(k) : elem;
            break;
        }
    }
    js_gc_unprotect(ctx->vm, &thisArg);
    js_gc_unprotect(ctx->vm, &cb);
    js_gc_unprotect(ctx->vm, &arrv);
    if (ok)
        *r = result;
    return ok;
}

static bool am_findLast(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return find_last(ctx, tv, a, c, r, false);
}
static bool am_findLastIndex(JsContext *ctx, JsValue tv, const JsValue *a, int c, JsValue *r) {
    return find_last(ctx, tv, a, c, r, true);
}

/* Array.prototype.reduceRight (23.1.3.25): from the end, skipping an index the callback removed */
static bool am_reduceRight(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    GENERIC("reduceRight", gen_reduce, 1);
    JsObject *a = js_value_object(tv);
    if (!js_is_function(ARG(0)))
        return native_throw_fmt(ctx, r, "TypeError: %t is not a function", ARG(0));
    JsValue cb = ARG(0), arrv = tv, acc;
    uint32_t k = a->elem_count;
    if (argc > 1) {
        acc = ARG(1);
    } else {
        if (k == 0)
            return native_throw(ctx, r, "TypeError: Reduce of empty array with no initial value");
        acc = a->elems[--k];
    }
    js_gc_protect(ctx->vm, &arrv);
    js_gc_protect(ctx->vm, &cb);
    js_gc_protect(ctx->vm, &acc);
    bool ok = true;
    while (k-- > 0) {
        if (k >= a->elem_count)
            continue;
        JsValue argv[4] = {acc, a->elems[k], js_number(k), arrv}, res;
        if (!js_call(ctx, cb, js_undefined(), argv, 4, &res)) {
            *r = res;
            ok = false;
            break;
        }
        acc = res;
    }
    JsValue out = acc;
    js_gc_unprotect(ctx->vm, &acc);
    js_gc_unprotect(ctx->vm, &cb);
    js_gc_unprotect(ctx->vm, &arrv);
    if (ok)
        *r = out;
    return ok;
}

/* Array.prototype.flatMap (23.1.3.14): map, then flatten what the mapper returns one level (an
 * array's elements; anything else as itself) */
static bool am_flatMap(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    GENERIC_SPECIES(NULL, gen_flat, 1);
    JsObject *a = js_value_object(tv);
    if (!js_is_function(ARG(0)))
        return native_throw_fmt(ctx, r, "TypeError: %t is not a function", ARG(0));
    JsObject *out = js_array_new_cell(ctx, 0);
    if (!out)
        return oom(ctx, r);
    JsValue cb = ARG(0), thisArg = ARG(1), arrv = tv, outv = js_value_from_cell(&out->gc);
    js_gc_protect(ctx->vm, &arrv);
    js_gc_protect(ctx->vm, &cb);
    js_gc_protect(ctx->vm, &thisArg);
    js_gc_protect(ctx->vm, &outv);
    bool ok = true, failed = false;
    uint32_t len = a->elem_count;
    for (uint32_t k = 0; k < len && ok; k++) {
        if (k >= a->elem_count)
            continue;
        JsValue res;
        if (!call_cb(ctx, cb, thisArg, a->elems[k], k, arrv, &res)) {
            *r = res;
            failed = true;
            break;
        }
        if (js_is_object(res) && js_value_object(res)->obj_kind == JS_OBJ_ARRAY) {
            JsObject *m = js_value_object(res);
            for (uint32_t j = 0; j < m->elem_count && ok; j++)
                ok = js_array_append(ctx->vm, out, m->elems[j]);
        } else {
            ok = js_array_append(ctx->vm, out, res);
        }
    }
    js_gc_unprotect(ctx->vm, &outv);
    js_gc_unprotect(ctx->vm, &thisArg);
    js_gc_unprotect(ctx->vm, &cb);
    js_gc_unprotect(ctx->vm, &arrv);
    if (failed)
        return false;
    if (!ok)
        return oom(ctx, r);
    *r = outv;
    return true;
}

/* ---- splice, copyWithin, toReversed, toSorted, toSpliced, with ---- */

/* ToIntegerOrInfinity of an argument, then a relative index into [0, len] (-Infinity is 0) */
static bool relative_index(JsContext *ctx, JsValue v, uint32_t len, uint32_t *out, JsValue *r) {
    double d;
    TO_NUM(d, v);
    *out = clamp_index(d, len);
    return true;
}

/* A count argument: ToIntegerOrInfinity clamped to [0, max] */
static bool count_arg(JsContext *ctx, JsValue v, uint32_t max, uint32_t *out, JsValue *r) {
    double d;
    TO_NUM(d, v);
    d = to_int(d);
    *out = d <= 0 ? 0 : (d >= max ? max : (uint32_t)d);
    return true;
}

/* The property writes of splice and copyWithin, refused as V8 refuses each on a sealed or frozen
 * array (the array is left as the writes before it made it). Storage has no holes: deleting an
 * index removes it only at the end, and an index past the end is written as a property set
 * would. False with the error in *r, or OOM. */
static bool arr_put(JsContext *ctx, JsObject *a, uint32_t i, JsValue v, JsValue *r) {
    if (i >= a->elem_count) {
        if (a->obj_flags & JS_OBJ_NO_ADD)
            return native_throw_fmt(ctx, r, "TypeError: Cannot add property %v, object is not extensible", js_number(i));
        return js_array_set_index(ctx->vm, a, i, v) ? true : oom(ctx, r);
    }
    if (a->obj_flags & JS_OBJ_FROZEN)
        return native_throw_fmt(ctx, r, "TypeError: Cannot assign to read only property '%v' of object '[object Array]'",
                                js_number(i));
    a->elems[i] = v;
    return true;
}

static bool arr_delete(JsContext *ctx, JsObject *a, uint32_t i, JsValue *r) {
    if (i >= a->elem_count)
        return true;
    if (a->obj_flags & (JS_OBJ_SEALED | JS_OBJ_FROZEN))
        return native_throw_fmt(ctx, r, "TypeError: Cannot delete property '%v' of [object Array]", js_number(i));
    if (i + 1 == a->elem_count)
        a->elem_count--;
    else
        a->elems[i] = js_undefined();
    return true;
}

/* Array.prototype.splice (23.1.3.31), step by step as the spec moves and deletes. */
static bool am_splice(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    GENERIC_SPECIES(NULL, gen_splice, 0);
    JsObject *a = js_value_object(tv);
    uint32_t len = a->elem_count, start = 0, del = 0;
    if (argc > 0 && !relative_index(ctx, ARG(0), len, &start, r))
        return false;
    if (argc == 1)
        del = len - start;
    else if (argc > 1 && !count_arg(ctx, ARG(1), len - start, &del, r))
        return false;
    uint32_t items = argc > 2 ? (uint32_t)argc - 2 : 0;
    JsObject *removed = js_array_new_cell(ctx, del);
    if (!removed)
        return oom(ctx, r);
    for (uint32_t k = 0; k < del; k++) {
        uint32_t from = start + k;
        removed->elems[removed->elem_count++] = from < a->elem_count ? a->elems[from] : js_undefined();
    }
    JsValue removedv = js_value_from_cell(&removed->gc);
    js_gc_protect(ctx->vm, &removedv);
    bool ok = true;
    if (items < del) {
        for (uint32_t k = start; ok && k < len - del; k++) {
            uint32_t from = k + del, to = k + items;
            ok = from < a->elem_count ? arr_put(ctx, a, to, a->elems[from], r) : arr_delete(ctx, a, to, r);
        }
        for (uint32_t k = len; ok && k > len - del + items; k--)
            ok = arr_delete(ctx, a, k - 1, r);
    } else if (items > del) {
        for (uint32_t k = len - del; ok && k > start; k--) {
            uint32_t from = k + del - 1, to = k + items - 1;
            ok = from < a->elem_count ? arr_put(ctx, a, to, a->elems[from], r) : arr_delete(ctx, a, to, r);
        }
    }
    for (uint32_t i = 0; ok && i < items; i++)
        ok = arr_put(ctx, a, start + i, args[2 + i], r);
    /* the length is set last; frozen, it is read-only even when unchanged */
    if (ok && (a->obj_flags & JS_OBJ_FROZEN))
        ok = native_throw(ctx, r, "TypeError: Cannot assign to read only property 'length' of object '[object Array]'");
    js_gc_unprotect(ctx->vm, &removedv);
    if (ok)
        *r = removedv;
    return ok;
}

/* Array.prototype.copyWithin (23.1.3.4): backwards when the ranges overlap that way, as the spec */
static bool am_copyWithin(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    GENERIC(NULL, gen_copyWithin, 0);
    JsObject *a = js_value_object(tv);
    uint32_t len = a->elem_count, to, from, fin = len;
    if (!relative_index(ctx, ARG(0), len, &to, r) || !relative_index(ctx, ARG(1), len, &from, r))
        return false;
    if (argc > 2 && !js_is_undefined(ARG(2)) && !relative_index(ctx, ARG(2), len, &fin, r))
        return false;
    uint32_t count = fin > from ? fin - from : 0;
    if (len - to < count)
        count = len - to;
    bool back = from < to && to < from + count;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t f = back ? from + count - 1 - i : from + i, t = back ? to + count - 1 - i : to + i;
        bool ok = f < a->elem_count ? arr_put(ctx, a, t, a->elems[f], r) : arr_delete(ctx, a, t, r);
        if (!ok)
            return false;
    }
    *r = tv;
    return true;
}

/* A copy of the first len elements (undefined where a valueOf removed one) */
static JsObject *array_copy(JsContext *ctx, JsObject *a, uint32_t len) {
    JsObject *out = js_array_new_cell(ctx, len);
    if (!out)
        return NULL;
    for (uint32_t i = 0; i < len; i++)
        out->elems[out->elem_count++] = i < a->elem_count ? a->elems[i] : js_undefined();
    return out;
}

static bool am_toReversed(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    GENERIC(NULL, gen_copy, 0);
    JsObject *a = js_value_object(tv);
    uint32_t len = a->elem_count;
    JsObject *out = js_array_new_cell(ctx, len);
    if (!out)
        return oom(ctx, r);
    for (uint32_t i = 0; i < len; i++)
        out->elems[out->elem_count++] = a->elems[len - 1 - i];
    *r = js_value_from_cell(&out->gc);
    return true;
}

static bool am_toSorted(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsValue cmp = ARG(0);
    if (!js_is_undefined(cmp) && !js_is_function(cmp))
        return native_throw_fmt(ctx, r, "TypeError: The comparison function must be either a function or undefined: %v", cmp);
    GENERIC(NULL, gen_sort, 1);
    return sort_array(ctx, tv, cmp, true, r);
}

static bool am_toSpliced(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    GENERIC(NULL, gen_copy, 1);
    JsObject *a = js_value_object(tv);
    uint32_t len = a->elem_count, start = 0, skip = 0;
    if (argc > 0 && !relative_index(ctx, ARG(0), len, &start, r))
        return false;
    if (argc == 1)
        skip = len - start;
    else if (argc > 1 && !count_arg(ctx, ARG(1), len - start, &skip, r))
        return false;
    uint32_t items = argc > 2 ? (uint32_t)argc - 2 : 0;
    uint64_t nlen = (uint64_t)len - skip + items;
    if (nlen > UINT32_MAX)
        return native_throw(ctx, r, "RangeError: Invalid array length");
    JsObject *out = js_array_new_cell(ctx, (uint32_t)nlen);
    if (!out)
        return oom(ctx, r);
    for (uint32_t i = 0; i < start; i++)
        out->elems[out->elem_count++] = i < a->elem_count ? a->elems[i] : js_undefined();
    for (uint32_t i = 0; i < items; i++)
        out->elems[out->elem_count++] = args[2 + i];
    for (uint32_t i = start + skip; i < len; i++)
        out->elems[out->elem_count++] = i < a->elem_count ? a->elems[i] : js_undefined();
    *r = js_value_from_cell(&out->gc);
    return true;
}

static bool am_with(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    GENERIC(NULL, gen_copy, 2);
    JsObject *a = js_value_object(tv);
    uint32_t len = a->elem_count;
    double rel;
    TO_NUM(rel, ARG(0));
    rel = to_int(rel);
    double at = rel >= 0 ? rel : (double)len + rel;
    if (at < 0 || at >= len)
        return native_throw_fmt(ctx, r, "RangeError: Invalid index : %v", ARG(0));
    JsObject *out = array_copy(ctx, a, len);
    if (!out)
        return oom(ctx, r);
    out->elems[(uint32_t)at] = ARG(1);
    *r = js_value_from_cell(&out->gc);
    return true;
}

/* ---------------------------------------------------------------------
 * The array methods on a receiver that is not an array
 *
 * Every Array.prototype method is generic (ECMA-262 23.1.3): it reads `length` and the indices of
 * whatever `this` is, through [[Get]], [[HasProperty]], [[Set]] and [[Delete]]. An array takes
 * the paths above, which work on its element vector; anything else comes here, where each method
 * is the spec's steps in the spec's order, so the property a refused write names, and what the
 * writes before it left behind, are V8's. Three things differ, all of them the object model's:
 *   - there are no wrapper objects, so a primitive receiver stands for the wrapper V8 makes of
 *     it: a string's indices and length are read, and refuse a write as a String's do; a write
 *     to a number or a boolean goes nowhere; and a method that returns its receiver, or hands it
 *     to a callback, gives the primitive;
 *   - a result array has no holes: an index the receiver lacks is undefined in it, and a run of
 *     more than JS_MAX_ARRAY_GAP of them is the RangeError a sparse write is;
 *   - `length` may be anything up to 2**53 - 1, so every step is charged to the fuel budget and
 *     polls the interrupt flag (js_native_charge): a native otherwise costs one unit.
 * --------------------------------------------------------------------- */

struct Gen {
    JsContext *ctx;
    JsValue *r;             /* the native's result slot: on failure, what was thrown */
    JsValue o;              /* the receiver */
    JsValue lenkey;         /* "length" */
    JsValue v, w, acc, out; /* rooted: two elements, an accumulator, the result array */
    int kind;               /* which of a family of methods */
    bool made;              /* OUT is what a species constructor made (gen_out_species): any object */
};

#define GEN_MAX_LEN 9007199254740991.0 /* 2**53 - 1, the longest array-like */
#define GEN_MAX_ARRAY 4294967295.0     /* 2**32 - 1, the longest array */

static const char GEN_BAD_LENGTH[] = "RangeError: Invalid array length";

/* What a failed conversion or property operation left pending, into the result slot */
static bool gen_fail(Gen *g) { return oom(g->ctx, g->r); }

static bool gen_step(Gen *g) { return js_native_charge(g->ctx) || gen_fail(g); }

static bool gen_primitive(JsValue v) { return !js_is_object(v) && !js_has_cell_props(v); }

/* Get(O, key) */
static bool gen_get(Gen *g, JsValue key, JsValue *out) {
    if (js_get_property(g->ctx, g->o, key, out))
        return true;
    *out = js_undefined();
    return gen_fail(g);
}

static bool gen_elem(Gen *g, double i, JsValue *out) { return gen_get(g, js_number(i), out); }

/* HasProperty(O, i) */
static bool gen_has(Gen *g, double i, bool *has) {
    JsValue base = g->o;
    *has = false;
    if (gen_primitive(base)) { /* the wrapper's own indices (a String's), then its prototype's */
        if (js_is_string(base) && i < js_value_string(base)->length) {
            *has = true;
            return true;
        }
        base = js_get_proto(g->ctx, base);
        if (gen_primitive(base))
            return true;
    }
    return js_has_property(g->ctx, base, js_number(i), has) || gen_fail(g);
}

/* Set(O, key, val, true); KEY an index or "length", VAL rooted by the caller */
static bool gen_put(Gen *g, JsValue key, JsValue val) {
    if (gen_primitive(g->o)) {
        if (js_is_string(g->o) &&
            (!js_is_number(key) || js_get_number(key) < js_value_string(g->o)->length))
            return native_throw_fmt(g->ctx, g->r,
                                    "TypeError: Cannot assign to read only property '%v' of object '[object String]'", key);
        return true; /* onto the wrapper, which nothing else sees */
    }
    return js_set_property(g->ctx, g->o, key, val) || gen_fail(g);
}

static bool gen_set(Gen *g, double i, JsValue val) { return gen_put(g, js_number(i), val); }

static bool gen_set_len(Gen *g, double n) { return gen_put(g, g->lenkey, js_number(n)); }

/* DeletePropertyOrThrow(O, i) */
static bool gen_del(Gen *g, double i) {
    if (gen_primitive(g->o)) {
        if (js_is_string(g->o) && i < js_value_string(g->o)->length)
            return native_throw_fmt(g->ctx, g->r, "TypeError: Cannot delete property '%v' of [object String]",
                                    js_number(i));
        return true;
    }
    return js_delete_property(g->ctx, g->o, js_number(i)) || gen_fail(g);
}

/* O[to] = O[from] when O has `from`, else delete O[to]: how the spec moves an element */
static bool gen_move(Gen *g, double from, double to) {
    bool has;
    if (!gen_has(g, from, &has))
        return false;
    if (!has)
        return gen_del(g, to);
    return gen_elem(g, from, &g->v) && gen_set(g, to, g->v);
}

/* LengthOfArrayLike(O) */
static bool gen_len(Gen *g, double *len) {
    JsValue lv;
    double d;
    if (!gen_get(g, g->lenkey, &lv))
        return false;
    if (!js_op_to_number(g->ctx, lv, &d))
        return gen_fail(g);
    d = to_int(d);
    *len = d <= 0 ? 0 : d > GEN_MAX_LEN ? GEN_MAX_LEN : d;
    return true;
}

/* ToIntegerOrInfinity(v) */
static bool gen_int(Gen *g, JsValue v, double *out) {
    double d;
    if (!js_op_to_number(g->ctx, v, &d))
        return gen_fail(g);
    *out = to_int(d);
    return true;
}

/* ToIntegerOrInfinity(v) as a relative index into [0, len] */
static bool gen_rel(Gen *g, JsValue v, double len, double *out) {
    double d;
    if (!gen_int(g, v, &d))
        return false;
    if (d < 0)
        *out = len + d > 0 ? len + d : 0;
    else
        *out = d < len ? d : len;
    return true;
}

/* The result array, empty */
static bool gen_out_new(Gen *g) {
    JsObject *out = js_array_new_cell(g->ctx, 0);
    if (!out)
        return gen_fail(g);
    g->out = js_value_from_cell(&out->gc);
    return true;
}

/* ArraySpeciesCreate(O, len) (23.1.3.x): the result is a plain array unless O is an array whose
 * `constructor` says otherwise, when it is whatever `new constructor(len)` gives, and the
 * elements are defined on it as properties (gen_out_put) */
static bool gen_out_species(Gen *g, double len) {
    JsValue ctor = js_undefined();
    int sp = is_array(g->o) ? js_array_species(g->ctx, g->o, &ctor) : 0;
    if (sp < 0)
        return (*g->r = ctor), false;
    if (sp == 0)
        return gen_out_new(g);
    JsVm *vm = g->ctx->vm;
    JsValue arg = js_number(len);
    js_gc_protect(vm, &ctor);
    bool ok = js_construct(g->ctx, ctor, &arg, 1, &g->out);
    js_gc_unprotect(vm, &ctor);
    if (!ok) {
        *g->r = g->out;
        g->out = js_undefined();
        return false;
    }
    g->made = true;
    return true;
}

/* CreateDataPropertyOrThrow(a, key, v): A and V rooted by the caller. False thrown (pending).
 * A is what a constructor made, which here can be a primitive (`new Object(1)` is 1: there are
 * no wrapper objects), and that takes no property: the assignment's TypeError. */
static bool create_data_property(JsContext *ctx, JsValue a, JsValue key, JsValue v) {
    JsVm *vm = ctx->vm;
    if (gen_primitive(a))
        return js_set_property(ctx, a, key, v);
    if (js_is_number(key)) {
        JsString *ks = js_op_to_string(ctx, key);
        if (!ks)
            return false;
        key = js_value_from_cell(&ks->gc);
    }
    JsPropDesc d = {JS_DESC_VALUE | JS_DESC_WRITABLE | JS_DESC_ENUMERABLE | JS_DESC_CONFIGURABLE, true, true, true, v};
    js_gc_protect(vm, &key);
    bool ok = js_define_own_property(ctx, a, key, &d);
    js_gc_unprotect(vm, &key);
    return ok;
}

/* out[n] = v, n at or past its end; the indices skipped are undefined (holes, in V8). On what a
 * species constructor made, CreateDataPropertyOrThrow(out, n, v). */
static bool gen_out_put(Gen *g, double n, JsValue v) {
    bool ok;
    js_gc_protect(g->ctx->vm, &v);
    if (g->made) {
        ok = create_data_property(g->ctx, g->out, js_number(n), v);
    } else {
        JsObject *out = js_value_object(g->out);
        if (n >= GEN_MAX_ARRAY || n - out->elem_count > JS_MAX_ARRAY_GAP) {
            js_gc_unprotect(g->ctx->vm, &v);
            return native_throw(g->ctx, g->r, GEN_BAD_LENGTH);
        }
        ok = js_array_set_index(g->ctx->vm, out, (uint32_t)n, v);
    }
    js_gc_unprotect(g->ctx->vm, &v);
    return ok || gen_fail(g);
}

/* The array's indices up to n exist (they are holes in V8): n at least its length. Nothing, on
 * what a species constructor made. */
static bool gen_out_fill(Gen *g, double n) {
    return g->made || n <= js_value_object(g->out)->elem_count || gen_out_put(g, n - 1, js_undefined());
}

/* Set(out, "length", n, true), which a plain array's filling to n is */
static bool gen_out_len(Gen *g, double n) {
    if (!g->made)
        return gen_out_fill(g, n);
    return js_set_property(g->ctx, g->out, g->lenkey, js_number(n)) || gen_fail(g);
}

/* callback(v, i, O) with THISARG; the result into *RES. False: it threw. RES is not the slot
 * that roots V: a call clears its result slot before the callee has its arguments. */
static bool gen_call(Gen *g, JsValue cb, JsValue thisArg, JsValue v, double i, JsValue *res) {
    JsValue argv[3] = {v, js_number(i), g->o};
    if (js_call(g->ctx, cb, thisArg, argv, 3, res))
        return true;
    *g->r = *res;
    *res = js_undefined();
    return false;
}

static bool gen_run(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r,
                    const char *name, GenBody body, int kind) {
    if (js_is_undefined(tv) || js_is_null(tv)) {
        if (name)
            return native_throw_fmt(ctx, r, "TypeError: Array.prototype.%s called on null or undefined", name);
        return native_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
    }
    static const uint16_t klen[] = {'l', 'e', 'n', 'g', 't', 'h'};
    Gen g;
    g.ctx = ctx;
    g.r = r;
    g.o = tv;
    g.kind = kind;
    g.made = false;
    g.v = g.w = g.acc = g.out = js_undefined();
    g.lenkey = js_atom(ctx->vm, klen, 6);
    if (!js_is_string(g.lenkey))
        return oom(ctx, r);
    JsVm *vm = ctx->vm;
    js_gc_protect(vm, &g.o);
    js_gc_protect(vm, &g.lenkey);
    js_gc_protect(vm, &g.v);
    js_gc_protect(vm, &g.w);
    js_gc_protect(vm, &g.acc);
    js_gc_protect(vm, &g.out);
    bool ok = body(&g, args, argc);
    js_gc_unprotect(vm, &g.out);
    js_gc_unprotect(vm, &g.acc);
    js_gc_unprotect(vm, &g.w);
    js_gc_unprotect(vm, &g.v);
    js_gc_unprotect(vm, &g.lenkey);
    js_gc_unprotect(vm, &g.o);
    return ok;
}

/* 23.1.3.23 push */
static bool gen_push(Gen *g, const JsValue *args, int argc) {
    double len;
    if (!gen_len(g, &len))
        return false;
    if (len + argc > GEN_MAX_LEN)
        return native_throw_fmt(g->ctx, g->r,
                                "TypeError: Pushing %v elements on an array-like of length %v is disallowed, as the total surpasses 2**53-1",
                                js_number(argc), js_number(len));
    for (int i = 0; i < argc; i++, len++)
        if (!gen_set(g, len, args[i]))
            return false;
    if (!gen_set_len(g, len))
        return false;
    *g->r = js_number(len);
    return true;
}

/* 23.1.3.22 pop */
static bool gen_pop(Gen *g, const JsValue *args, int argc) {
    (void)args;
    (void)argc;
    double len;
    if (!gen_len(g, &len))
        return false;
    if (len < 1) {
        if (!gen_set_len(g, 0))
            return false;
        *g->r = js_undefined();
        return true;
    }
    if (!gen_elem(g, len - 1, &g->acc) || !gen_del(g, len - 1) || !gen_set_len(g, len - 1))
        return false;
    *g->r = g->acc;
    return true;
}

/* 23.1.3.27 shift */
static bool gen_shift(Gen *g, const JsValue *args, int argc) {
    (void)args;
    (void)argc;
    double len;
    if (!gen_len(g, &len))
        return false;
    if (len < 1) {
        if (!gen_set_len(g, 0))
            return false;
        *g->r = js_undefined();
        return true;
    }
    if (!gen_elem(g, 0, &g->acc))
        return false;
    for (double k = 1; k < len; k++)
        if (!gen_step(g) || !gen_move(g, k, k - 1))
            return false;
    if (!gen_del(g, len - 1) || !gen_set_len(g, len - 1))
        return false;
    *g->r = g->acc;
    return true;
}

/* 23.1.3.37 unshift */
static bool gen_unshift(Gen *g, const JsValue *args, int argc) {
    double len;
    if (!gen_len(g, &len))
        return false;
    if (argc > 0) {
        if (len + argc > GEN_MAX_LEN)
            return native_throw(g->ctx, g->r, "TypeError: Invalid array length");
        for (double k = len; k > 0; k--)
            if (!gen_step(g) || !gen_move(g, k - 1, k + argc - 1))
                return false;
        for (int j = 0; j < argc; j++)
            if (!gen_set(g, j, args[j]))
                return false;
    }
    if (!gen_set_len(g, len + argc))
        return false;
    *g->r = js_number(len + argc);
    return true;
}

/* 23.1.3.1 at */
static bool gen_at(Gen *g, const JsValue *args, int argc) {
    double len, rel;
    if (!gen_len(g, &len) || !gen_int(g, ARG(0), &rel))
        return false;
    double k = rel >= 0 ? rel : len + rel;
    *g->r = js_undefined();
    if (k < 0 || k >= len)
        return true;
    if (!gen_elem(g, k, &g->v))
        return false;
    *g->r = g->v;
    return true;
}

/* 23.1.3.17 indexOf (kind 0), 23.1.3.16 includes (kind 1): indexOf looks only at the indices the
 * receiver has, by strict equality; includes reads every one, by SameValueZero */
static bool gen_indexOf(Gen *g, const JsValue *args, int argc) {
    bool includes = g->kind == 1;
    double len, n = 0;
    *g->r = includes ? js_bool(false) : js_number(-1);
    if (!gen_len(g, &len))
        return false;
    if (len < 1)
        return true;
    if (!gen_int(g, ARG(1), &n))
        return false;
    for (double k = n >= 0 ? n : (len + n > 0 ? len + n : 0); k < len; k++) {
        bool has = true;
        if (!gen_step(g) || (!includes && !gen_has(g, k, &has)))
            return false;
        if (!has)
            continue;
        if (!gen_elem(g, k, &g->v))
            return false;
        if (includes ? same_value_zero(g->v, ARG(0)) : js_strict_equals(g->v, ARG(0))) {
            *g->r = includes ? js_bool(true) : js_number(k);
            return true;
        }
    }
    *g->r = includes ? js_bool(false) : js_number(-1);
    return true;
}

/* 23.1.3.20 lastIndexOf */
static bool gen_lastIndexOf(Gen *g, const JsValue *args, int argc) {
    double len, n;
    *g->r = js_number(-1);
    if (!gen_len(g, &len))
        return false;
    if (len < 1)
        return true;
    n = len - 1;
    if (argc > 1 && !gen_int(g, args[1], &n))
        return false;
    for (double k = n >= 0 ? (n < len - 1 ? n : len - 1) : len + n; k >= 0; k--) {
        bool has;
        if (!gen_step(g) || !gen_has(g, k, &has))
            return false;
        if (!has)
            continue;
        if (!gen_elem(g, k, &g->v))
            return false;
        if (js_strict_equals(g->v, ARG(0))) {
            *g->r = js_number(k);
            return true;
        }
    }
    *g->r = js_number(-1);
    return true;
}

/* 23.1.3.28 slice */
static bool gen_slice(Gen *g, const JsValue *args, int argc) {
    double len, k, fin;
    if (!gen_len(g, &len) || !gen_rel(g, ARG(0), len, &k))
        return false;
    fin = len;
    if (argc > 1 && !js_is_undefined(args[1]) && !gen_rel(g, args[1], len, &fin))
        return false;
    if (fin - k > GEN_MAX_ARRAY)
        return native_throw(g->ctx, g->r, GEN_BAD_LENGTH);
    if (!gen_out_species(g, fin > k ? fin - k : 0))
        return false;
    double n = 0;
    for (; k < fin; k++, n++) {
        bool has;
        if (!gen_step(g) || !gen_has(g, k, &has))
            return false;
        if (has && (!gen_elem(g, k, &g->v) || !gen_out_put(g, n, g->v)))
            return false;
    }
    if (!gen_out_len(g, n))
        return false;
    *g->r = g->out;
    return true;
}

/* 23.1.3.18 join, in V8's order: the length, the separator, then the cycle check */
static bool gen_join(Gen *g, const JsValue *args, int argc) {
    JsContext *ctx = g->ctx;
    double len;
    if (!gen_len(g, &len))
        return false;
    JsString *sep = NULL;
    if (argc > 0 && !js_is_undefined(args[0]) && !(sep = arg_string(ctx, args[0])))
        return gen_fail(g);
    /* identity is all the stack is for: a function or promise goes on it as an object does */
    JsObject *self = gen_primitive(g->o) ? NULL : (JsObject *)js_value_cell(g->o);
    bool cyclic = false, ok = true;
    if (len >= 1 && self && !join_enter(ctx, self, &cyclic))
        return gen_fail(g);
    const uint16_t comma[] = {','};
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    if (!cyclic) {
        for (double k = 0; k < len; k++) {
            if (!(ok = gen_step(g)))
                break;
            if (k > 0) {
                if (sep)
                    sb_units(&sb, sep->units, sep->length);
                else
                    sb_units(&sb, comma, 1);
            }
            if (!(ok = gen_elem(g, k, &g->v)))
                break;
            if (!js_is_undefined(g->v) && !js_is_null(g->v)) {
                JsString *es = js_op_to_string(ctx, g->v); /* copied out before the next converts */
                if (!es) {
                    ok = gen_fail(g);
                    break;
                }
                sb_units(&sb, es->units, es->length);
            }
            if (sb.oom || sb.len > JS_MAX_STRING_UNITS) {
                ok = sb.oom ? gen_fail(g) : native_throw(ctx, g->r, "RangeError: Invalid string length");
                break;
            }
        }
        if (len >= 1 && self)
            ctx->join_len--;
    }
    if (!ok) {
        sb_free(&sb);
        return false;
    }
    *g->r = sb_finish(&sb);
    return js_is_undefined(*g->r) ? gen_fail(g) : true;
}

/* 23.1.3.26 reverse */
static bool gen_reverse(Gen *g, const JsValue *args, int argc) {
    (void)args;
    (void)argc;
    double len;
    if (!gen_len(g, &len))
        return false;
    for (double lower = 0, upper = len - 1; lower < upper; lower++, upper--) {
        bool lo, up;
        if (!gen_step(g))
            return false;
        if (!gen_has(g, lower, &lo) || (lo && !gen_elem(g, lower, &g->v)))
            return false;
        if (!gen_has(g, upper, &up) || (up && !gen_elem(g, upper, &g->w)))
            return false;
        if (lo && up) {
            if (!gen_set(g, lower, g->w) || !gen_set(g, upper, g->v))
                return false;
        } else if (up) {
            if (!gen_set(g, lower, g->w) || !gen_del(g, upper))
                return false;
        } else if (lo) {
            if (!gen_del(g, lower) || !gen_set(g, upper, g->v))
                return false;
        }
    }
    *g->r = g->o;
    return true;
}

/* 23.1.3.7 fill */
static bool gen_fill(Gen *g, const JsValue *args, int argc) {
    double len, k, fin;
    if (!gen_len(g, &len) || !gen_rel(g, ARG(1), len, &k))
        return false;
    fin = len;
    if (argc > 2 && !js_is_undefined(args[2]) && !gen_rel(g, args[2], len, &fin))
        return false;
    for (; k < fin; k++)
        if (!gen_step(g) || !gen_set(g, k, ARG(0)))
            return false;
    *g->r = g->o;
    return true;
}

/* 23.1.3.21 map, 8 filter, 15 forEach, 29 some, 6 every, 9 find, 10 findIndex (kind an IterKind)
 * and 11 findLast, 12 findLastIndex (IT_FINDLAST, IT_FINDLASTINDEX): the length, then the
 * callback's check, as V8. The find family reads every index; the others skip what is absent. */
static bool gen_iterate(Gen *g, const JsValue *args, int argc) {
    int kind = g->kind;
    bool finds = kind == IT_FIND || kind == IT_FINDINDEX || kind == IT_FINDLAST || kind == IT_FINDLASTINDEX;
    bool back = kind == IT_FINDLAST || kind == IT_FINDLASTINDEX;
    double len, to = 0;
    if (!gen_len(g, &len))
        return false;
    if (!js_is_function(ARG(0)))
        return native_throw_fmt(g->ctx, g->r, "TypeError: %t is not a function", ARG(0));
    JsValue cb = args[0], thisArg = ARG(1); /* rooted as the native's arguments */
    if (kind == IT_MAP && len > GEN_MAX_ARRAY)
        return native_throw(g->ctx, g->r, GEN_BAD_LENGTH);
    if ((kind == IT_MAP || kind == IT_FILTER) && !gen_out_species(g, kind == IT_MAP ? len : 0))
        return false;
    JsValue result = js_undefined();
    switch (kind) {
    case IT_SOME: result = js_bool(false); break;
    case IT_EVERY: result = js_bool(true); break;
    case IT_FINDINDEX:
    case IT_FINDLASTINDEX: result = js_number(-1); break;
    default: break;
    }
    for (double i = 0; i < len; i++) {
        double k = back ? len - 1 - i : i;
        bool has = true;
        if (!gen_step(g) || (!finds && !gen_has(g, k, &has)))
            return false;
        if (!has)
            continue;
        if (!gen_elem(g, k, &g->v) || !gen_call(g, cb, thisArg, g->v, k, &g->w))
            return false;
        bool yes = js_to_boolean(g->w);
        if (kind == IT_MAP) {
            if (!gen_out_put(g, k, g->w))
                return false;
        } else if (kind == IT_FILTER) {
            if (yes && !gen_out_put(g, to++, g->v))
                return false;
        } else if (kind == IT_SOME ? yes : kind == IT_EVERY ? !yes : finds && yes) {
            *g->r = kind == IT_SOME ? js_bool(true) : kind == IT_EVERY ? js_bool(false)
                    : kind == IT_FIND || kind == IT_FINDLAST ? g->v : js_number(k);
            return true;
        }
    }
    if (kind == IT_MAP && !gen_out_fill(g, len))
        return false;
    *g->r = kind == IT_MAP || kind == IT_FILTER ? g->out : result;
    return true;
}

/* 23.1.3.24 reduce (kind 0), 25 reduceRight (kind 1): without an initial value, the first index
 * the receiver has is the seed */
static bool gen_reduce(Gen *g, const JsValue *args, int argc) {
    bool back = g->kind == 1, seeded = argc > 1;
    double len;
    if (!gen_len(g, &len))
        return false;
    if (!js_is_function(ARG(0)))
        return native_throw_fmt(g->ctx, g->r, "TypeError: %t is not a function", ARG(0));
    JsValue cb = args[0];
    if (seeded)
        g->acc = args[1];
    for (double i = 0; i < len; i++) {
        double k = back ? len - 1 - i : i;
        bool has;
        if (!gen_step(g) || !gen_has(g, k, &has))
            return false;
        if (!has)
            continue;
        if (!gen_elem(g, k, &g->v))
            return false;
        if (!seeded) {
            g->acc = g->v;
            seeded = true;
            continue;
        }
        JsValue argv[4] = {g->acc, g->v, js_number(k), g->o};
        if (!js_call(g->ctx, cb, js_undefined(), argv, 4, &g->w)) {
            *g->r = g->w;
            return false;
        }
        g->acc = g->w;
    }
    if (!seeded)
        return native_throw(g->ctx, g->r, "TypeError: Reduce of empty array with no initial value");
    *g->r = g->acc;
    return true;
}

/* 23.1.3.30 sort (kind 0), 34 toSorted (kind 1), as V8's TimSort has them: sort takes the
 * elements the receiver has, sorts them aside, writes them back from index 0 with the undefined
 * ones after them and deletes the rest; toSorted reads every index. The comparator was checked
 * by the caller. */
static bool gen_sort(Gen *g, const JsValue *args, int argc) {
    bool to_sorted = g->kind == 1;
    double len, undefs = 0;
    if (!gen_len(g, &len))
        return false;
    if (!to_sorted && len < 2) {
        *g->r = g->o;
        return true;
    }
    if (to_sorted && len > GEN_MAX_ARRAY)
        return native_throw(g->ctx, g->r, GEN_BAD_LENGTH);
    if (!gen_out_new(g))
        return false;
    JsObject *work = js_value_object(g->out);
    for (double k = 0; k < len; k++) {
        bool has = true;
        if (!gen_step(g) || (!to_sorted && !gen_has(g, k, &has)))
            return false;
        if (!has)
            continue;
        if (!gen_elem(g, k, &g->v))
            return false;
        if (!to_sorted && js_is_undefined(g->v))
            undefs++;
        else if (!js_array_append(g->ctx->vm, work, g->v))
            return gen_fail(g);
    }
    /* sorted into a new array, the undefined ones last: toSorted's result */
    if (!sort_array(g->ctx, g->out, ARG(0), true, &g->w))
        return (*g->r = g->w), false;
    if (to_sorted) {
        *g->r = g->w;
        return true;
    }
    g->out = g->w;
    JsObject *sorted = js_value_object(g->out);
    double n = sorted->elem_count, k = 0;
    for (; k < n; k++)
        if (!gen_step(g) || !gen_set(g, k, sorted->elems[(uint32_t)k]))
            return false;
    for (; k < n + undefs; k++)
        if (!gen_step(g) || !gen_set(g, k, js_undefined()))
            return false;
    for (; k < len; k++)
        if (!gen_step(g) || !gen_del(g, k))
            return false;
    *g->r = g->o;
    return true;
}

/* FlattenIntoArray onto what a species constructor made: flat_into, with each element defined on
 * it at *N on (gen_out_put). A define runs no script, but that result may be one of the arrays
 * being flattened (the constructor chose it), so a level's length is the one it had when it was
 * entered, as the spec has it, an index it has lost is skipped, and the levels are kept alive by
 * g->acc and not by SRC alone (which g->v roots). */
typedef struct {
    JsObject *arr;
    uint32_t idx, len;
} FlatMadeLevel;

static bool gen_flat_made(Gen *g, JsObject *src, double depth, double *n) {
    JsVm *vm = g->ctx->vm;
    FlatMadeLevel fixed[16], *st = fixed;
    uint32_t sp = 0, cap = 16;
    bool ok = true;
    JsObject *keep = js_array_new_cell(g->ctx, 0);
    if (!keep)
        return gen_fail(g);
    g->acc = js_value_from_cell(&keep->gc);
    st[0].arr = src;
    st[0].idx = 0;
    st[0].len = src->elem_count;
    for (;;) {
        JsObject *cur = st[sp].arr;
        if (st[sp].idx >= st[sp].len) {
            if (sp-- == 0)
                break;
            continue;
        }
        uint32_t i = st[sp].idx++;
        if (i >= cur->elem_count)
            continue;
        JsValue el = cur->elems[i];
        if (!(depth - sp > 0 && is_array(el))) {
            if (!gen_out_put(g, (*n)++, el)) {
                ok = false;
                break;
            }
            continue;
        }
        if (sp + 1 == FLAT_MAX_NEST) {
            ok = native_throw(g->ctx, g->r, "RangeError: Maximum call stack size exceeded");
            break;
        }
        if (sp + 1 == cap) {
            FlatMadeLevel *grown = js_realloc_raw(vm, st == fixed ? NULL : st, st == fixed ? 0 : cap * sizeof *st,
                                                  (size_t)cap * 2 * sizeof *st);
            if (!grown) {
                ok = gen_fail(g);
                break;
            }
            if (st == fixed)
                memcpy(grown, fixed, sizeof fixed);
            st = grown;
            cap *= 2;
        }
        st[++sp].arr = js_value_object(el); /* before the append, which may collect */
        st[sp].idx = 0;
        st[sp].len = st[sp].arr->elem_count;
        js_gc_protect(vm, &el);
        bool kept = js_array_append(vm, js_value_object(g->acc), el);
        js_gc_unprotect(vm, &el);
        if (!kept) {
            ok = gen_fail(g);
            break;
        }
    }
    if (st != fixed)
        js_realloc_raw(vm, st, cap * sizeof *st, 0);
    g->acc = js_undefined();
    return ok;
}

/* 23.1.3.2 concat, of an array whose result ArraySpeciesCreate has to be asked for (am_concat
 * does every other receiver): the receiver and each argument, an array's elements and anything
 * else as itself, defined on the result, whose length is then set */
static bool gen_concat(Gen *g, const JsValue *args, int argc) {
    if (!gen_out_species(g, 0))
        return false;
    double n = 0;
    for (int i = -1; i < argc; i++) {
        JsValue e = i < 0 ? g->o : args[i];
        if (!is_array(e)) {
            if (!gen_out_put(g, n++, e))
                return false;
            continue;
        }
        JsObject *a = js_value_object(e);
        uint32_t len = a->elem_count;
        for (uint32_t k = 0; k < len; k++)
            if (!gen_step(g) || (k < a->elem_count && !gen_out_put(g, n + k, a->elems[k])))
                return false;
        n += len;
    }
    if (!gen_out_len(g, n))
        return false;
    *g->r = g->out;
    return true;
}

/* 23.1.3.13 flat (kind 0), 14 flatMap (kind 1): the receiver's own level here, the arrays in it
 * by flat_into */
static bool gen_flat(Gen *g, const JsValue *args, int argc) {
    bool mapped = g->kind == 1;
    double len, depth = 1;
    if (!gen_len(g, &len))
        return false;
    if (mapped) {
        if (!js_is_function(ARG(0)))
            return native_throw_fmt(g->ctx, g->r, "TypeError: %t is not a function", ARG(0));
    } else if (argc > 0 && !js_is_undefined(args[0]) && !gen_int(g, args[0], &depth)) {
        return false;
    }
    if (!gen_out_species(g, 0))
        return false;
    double n = 0; /* the next index of what a species constructor made */
    for (double k = 0; k < len; k++) {
        bool has;
        if (!gen_step(g) || !gen_has(g, k, &has))
            return false;
        if (!has)
            continue;
        if (!gen_elem(g, k, &g->v))
            return false;
        if (mapped) { /* into its own slot: the call clears its result first, and V is the argument's root */
            if (!gen_call(g, args[0], ARG(1), g->v, k, &g->w))
                return false;
            g->v = g->w;
        }
        if (g->made) {
            if (!(depth > 0 && is_array(g->v))) {
                if (!gen_out_put(g, n++, g->v))
                    return false;
            } else if (!gen_flat_made(g, js_value_object(g->v), mapped ? 0 : depth - 1, &n)) {
                return false;
            }
            continue;
        }
        JsObject *out = js_value_object(g->out);
        if (depth > 0 && is_array(g->v)) {
            if (!flat_into(g->ctx, out, js_value_object(g->v), mapped ? 0 : depth - 1, g->r))
                return false;
        } else if (!js_array_append(g->ctx->vm, out, g->v)) {
            return gen_fail(g);
        }
    }
    *g->r = g->out;
    return true;
}

/* 23.1.3.31 splice */
static bool gen_splice(Gen *g, const JsValue *args, int argc) {
    double len, start = 0, del = 0, items = argc > 2 ? argc - 2 : 0;
    if (!gen_len(g, &len) || !gen_rel(g, ARG(0), len, &start))
        return false;
    if (argc == 1) {
        del = len - start;
    } else if (argc > 1) {
        if (!gen_int(g, args[1], &del))
            return false;
        del = del < 0 ? 0 : del > len - start ? len - start : del;
    }
    if (len + items - del > GEN_MAX_LEN)
        return native_throw(g->ctx, g->r, "TypeError: Invalid array length");
    if (del > GEN_MAX_ARRAY)
        return native_throw(g->ctx, g->r, GEN_BAD_LENGTH);
    if (!gen_out_species(g, del))
        return false;
    for (double k = 0; k < del; k++) {
        bool has;
        if (!gen_step(g) || !gen_has(g, start + k, &has))
            return false;
        if (has && (!gen_elem(g, start + k, &g->v) || !gen_out_put(g, k, g->v)))
            return false;
    }
    if (!gen_out_len(g, del))
        return false;
    if (items < del) {
        for (double k = start; k < len - del; k++)
            if (!gen_step(g) || !gen_move(g, k + del, k + items))
                return false;
        for (double k = len; k > len - del + items; k--)
            if (!gen_step(g) || !gen_del(g, k - 1))
                return false;
    } else if (items > del) {
        for (double k = len - del; k > start; k--)
            if (!gen_step(g) || !gen_move(g, k + del - 1, k + items - 1))
                return false;
    }
    for (int i = 2; i < argc; i++)
        if (!gen_set(g, start + (i - 2), args[i]))
            return false;
    if (!gen_set_len(g, len - del + items))
        return false;
    *g->r = g->out;
    return true;
}

/* 23.1.3.4 copyWithin */
static bool gen_copyWithin(Gen *g, const JsValue *args, int argc) {
    double len, to, from, fin;
    if (!gen_len(g, &len) || !gen_rel(g, ARG(0), len, &to) || !gen_rel(g, ARG(1), len, &from))
        return false;
    fin = len;
    if (argc > 2 && !js_is_undefined(args[2]) && !gen_rel(g, args[2], len, &fin))
        return false;
    double count = fin - from < len - to ? fin - from : len - to, dir = 1;
    if (from < to && to < from + count) {
        dir = -1;
        from += count - 1;
        to += count - 1;
    }
    for (; count > 0; count--, from += dir, to += dir)
        if (!gen_step(g) || !gen_move(g, from, to))
            return false;
    *g->r = g->o;
    return true;
}

/* 23.1.3.33 toReversed (kind 0), 35 toSpliced (kind 1), 39 with (kind 2): a new array of every
 * index read, absent or not */
static bool gen_copy(Gen *g, const JsValue *args, int argc) {
    double len, start = 0, skip = 0, items = 0, at = -1;
    if (!gen_len(g, &len))
        return false;
    if (g->kind == 1) {
        if (!gen_rel(g, ARG(0), len, &start))
            return false;
        items = argc > 2 ? argc - 2 : 0;
        if (argc == 1) {
            skip = len - start;
        } else if (argc > 1) {
            if (!gen_int(g, args[1], &skip))
                return false;
            skip = skip < 0 ? 0 : skip > len - start ? len - start : skip;
        }
        if (len + items - skip > GEN_MAX_LEN)
            return native_throw(g->ctx, g->r, "TypeError: Invalid array length");
    } else if (g->kind == 2) {
        if (!gen_int(g, ARG(0), &at))
            return false;
        if (at < 0)
            at += len;
        if (at < 0 || at >= len)
            return native_throw_fmt(g->ctx, g->r, "RangeError: Invalid index : %v", ARG(0));
    }
    double nlen = len + items - skip;
    if (nlen > GEN_MAX_ARRAY)
        return native_throw(g->ctx, g->r, GEN_BAD_LENGTH);
    if (!gen_out_new(g))
        return false;
    JsObject *out = js_value_object(g->out);
    for (double i = 0; i < nlen; i++) {
        if (!gen_step(g))
            return false;
        if (g->kind == 1 && i >= start && i < start + items) {
            g->v = args[2 + (int)(i - start)];
        } else if (g->kind == 2 && !(i < at) && !(i > at)) {
            g->v = ARG(1);
        } else {
            double from = g->kind == 0 ? len - 1 - i : i < start ? i : i - items + skip;
            if (!gen_elem(g, from, &g->v))
                return false;
        }
        if (!js_array_append(g->ctx->vm, out, g->v))
            return gen_fail(g);
    }
    *g->r = g->out;
    return true;
}

/* CreateListFromArrayLike (7.3.19): every index read, into an array: the argument list
 * Function.prototype.apply is given, when that is not an array */
static bool gen_list(Gen *g, const JsValue *args, int argc) {
    (void)args;
    (void)argc;
    double len;
    if (!gen_len(g, &len))
        return false;
    if (len > (double)(1u << 24))
        return native_throw(g->ctx, g->r, "RangeError: too many function arguments");
    if (!gen_out_new(g))
        return false;
    for (double k = 0; k < len; k++) {
        if (!gen_step(g) || !gen_elem(g, k, &g->v))
            return false;
        if (!js_array_append(g->ctx->vm, js_value_object(g->out), g->v))
            return gen_fail(g);
    }
    *g->r = g->out;
    return true;
}

static bool objp_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r);

/* Array.prototype.toString (23.1.3.36), generic as V8's: this.join() when join is a function,
 * else Object.prototype.toString. */
static bool am_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (js_is_undefined(tv) || js_is_null(tv))
        return native_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
    JsValue join = js_undefined();
    static const uint16_t kj[] = {'j', 'o', 'i', 'n'};
    JsValue key = js_atom(ctx->vm, kj, 4);
    if (!js_is_string(key) || !js_get_property(ctx, tv, key, &join))
        return oom(ctx, r);
    if (!js_is_function(join))
        return objp_toString(ctx, tv, args, argc, r);
    return js_call(ctx, join, tv, NULL, 0, r);
}

/* =====================================================================
 * Number methods
 * ===================================================================== */

/* Function.prototype.call(thisArg, ...args): calls the function with `this` = thisArg (as given:
 * module code is strict, so undefined and null are not replaced by the global object). */
static bool fm_call(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (!js_is_function(tv))
        return native_throw_fmt(ctx, r, "TypeError: Function.prototype.call was called on %v, which is %s and not a function",
                                tv, type_phrase(tv));
    JsValue self = argc > 0 ? args[0] : js_undefined();
    return js_call(ctx, tv, self, argc > 1 ? args + 1 : NULL, argc > 1 ? argc - 1 : 0, r);
}

/* Calls fn with `this` = self and the arguments `pre` then `post`, gathered in one buffer. The
 * buffer is no GC root of its own and the owners the values came from (the bound array, the list
 * given to apply) may let go of them while the callee runs: a function callee has them in its
 * frame's slots, and a native one in its activation, which the collector marks. */
static bool call_joined(JsContext *ctx, JsValue fn, JsValue self, const JsValue *pre, uint32_t npre,
                        const JsValue *post, uint32_t npost, JsValue *r) {
    size_t n = (size_t)npre + npost;
    if (n > (1u << 24))
        return native_throw(ctx, r, "RangeError: too many function arguments");
    if (!n)
        return js_call(ctx, fn, self, NULL, 0, r);
    JsValue *buf = js_realloc_raw(ctx->vm, NULL, 0, n * sizeof *buf);
    if (!buf) {
        *r = js_oom_value(ctx->vm);
        return false;
    }
    for (uint32_t i = 0; i < npre; i++)
        buf[i] = pre[i];
    for (uint32_t i = 0; i < npost; i++)
        buf[npre + i] = post[i];
    bool ok = js_call(ctx, fn, self, buf, (int)n, r);
    js_realloc_raw(ctx->vm, buf, n * sizeof *buf, 0);
    return ok;
}

/* Function.prototype.apply(thisArg, argsArray): call with the list's elements as arguments
 * (undefined or null for none). The list is an array, or anything else that is an object, read
 * as CreateListFromArrayLike reads it: its `length`, then each index. */
static bool fm_apply(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (!js_is_function(tv))
        return native_throw_fmt(ctx, r, "TypeError: Function.prototype.apply was called on %v, which is %s and not a function",
                                tv, type_phrase(tv));
    JsValue self = argc > 0 ? args[0] : js_undefined();
    JsValue list = argc > 1 ? args[1] : js_undefined();
    if (js_is_undefined(list) || js_is_null(list))
        return js_call(ctx, tv, self, NULL, 0, r);
    if (!js_is_array(list)) {
        if (!js_is_object(list) && !js_has_cell_props(list))
            return native_throw(ctx, r, "TypeError: CreateListFromArrayLike called on non-object");
        if (!gen_run(ctx, list, NULL, 0, r, NULL, gen_list, 0))
            return false;
        JsValue copy = *r;
        js_gc_protect(ctx->vm, &copy);
        JsObject *c = js_value_object(copy);
        bool ok = call_joined(ctx, tv, self, c->elems, c->elem_count, NULL, 0, r);
        js_gc_unprotect(ctx->vm, &copy);
        return ok;
    }
    JsObject *a = js_value_object(list);
    return call_joined(ctx, tv, self, a->elems, a->elem_count, NULL, 0, r);
}

/* A bound function's captured state: [target, thisArg, ...presetArgs], an array no script can reach. */
bool js_bind_call(JsContext *ctx, JsValue bound, JsValue this_val, const JsValue *args, int argc,
                  JsValue *r) {
    (void)this_val; /* a bound function ignores the this it is called with */
    JsObject *b = js_value_object(bound);
    return call_joined(ctx, b->elems[0], b->elems[1], b->elems + 2, b->elem_count - 2, args,
                       argc > 0 ? (uint32_t)argc : 0, r);
}

/* Function.prototype.bind(thisArg, ...args): a function that calls this one with that this and
 * those arguments first. */
static bool fm_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    if (!js_is_function(tv))
        return native_throw(ctx, r, "TypeError: Function.prototype.toString requires that 'this' be a Function");
    JsString *s = js_function_source(ctx, tv);
    if (!s)
        return oom(ctx, r);
    *r = js_value_from_cell(&s->gc);
    return true;
}

static bool fm_bind(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (!js_is_function(tv))
        return native_throw(ctx, r, "TypeError: Bind must be called on a function");
    JsVm *vm = ctx->vm;
    JsValue state = js_array_new(ctx, (uint32_t)(argc > 0 ? argc + 1 : 2));
    if (js_is_undefined(state)) {
        *r = js_oom_value(vm);
        return false;
    }
    js_gc_protect(vm, &state);
    bool ok = js_array_push(vm, state, tv) &&
              js_array_push(vm, state, argc > 0 ? args[0] : js_undefined());
    for (int i = 1; ok && i < argc; i++)
        ok = js_array_push(vm, state, args[i]);
    JsValue fn = ok ? js_bound_native_new(ctx, js_bind_call, state) : js_undefined();
    js_gc_unprotect(vm, &state);
    if (js_is_undefined(fn)) {
        *r = js_oom_value(vm);
        return false;
    }
    /* Its length is the target's own `length` less the arguments bound, and its name "bound "
     * and the target's `name`: both read as properties (20.2.3.2), so what a script defined
     * them as counts, and a length that is no number is 0, a name that is no string empty. */
    js_gc_protect(vm, &fn);
    JsValue kv = js_undefined(), lv = js_undefined(), nv = js_undefined(), pv = js_undefined();
    js_gc_protect(vm, &kv);
    js_gc_protect(vm, &nv);
    js_gc_protect(vm, &pv); /* the name's key, then the prefix: neither is held by anything else */
    uint8_t attrs;
    JsString *k = js_ascii_cell(vm, "length");
    k = k ? js_intern_cell(vm, k) : NULL;
    bool read = k != NULL;
    int has = 0;
    if (read) {
        kv = js_value_from_cell(&k->gc);
        has = js_own_property(ctx, tv, kv, &lv, &attrs);
        read = has >= 0 && (!has || js_get_property(ctx, tv, kv, &lv));
    }
    double bl = 0;
    if (read && has && js_is_number(lv)) {
        double tl = js_get_number(lv), bound = argc > 1 ? (double)(argc - 1) : 0;
        if (tl == tl) /* ToIntegerOrInfinity; an infinite length stays one */
            bl = tl - bound > 0 ? __builtin_trunc(tl) - bound : 0;
    }
    if (read) {
        k = js_ascii_cell(vm, "name");
        if (k)
            pv = js_value_from_cell(&k->gc);
        read = k && js_get_property(ctx, tv, pv, &nv);
    }
    JsString *nm = NULL;
    if (read) {
        JsString *prefix = js_ascii_cell(vm, "bound ");
        if (prefix)
            pv = js_value_from_cell(&prefix->gc);
        nm = prefix && js_is_string(nv) && js_value_string(nv)->length
                 ? js_concat_cells(vm, prefix, js_value_string(nv)) : prefix;
        read = nm != NULL;
    }
    if (read) {
        ((JsNative *)js_value_cell(fn))->name = nm;
        if (bl <= 255) {
            ((JsNative *)js_value_cell(fn))->length = (uint8_t)bl;
        } else { /* more than the function's own count holds: as a property defined on it */
            JsPropDesc d = {JS_DESC_VALUE | JS_DESC_WRITABLE | JS_DESC_ENUMERABLE | JS_DESC_CONFIGURABLE,
                            false, false, true, js_number(bl)};
            read = js_define_own_property(ctx, fn, kv, &d);
        }
    }
    js_gc_unprotect(vm, &pv);
    js_gc_unprotect(vm, &nv);
    js_gc_unprotect(vm, &kv);
    js_gc_unprotect(vm, &fn);
    if (!read) {
        *r = js_take_error(ctx);
        return false;
    }
    /* its [[Prototype]] is the target's, as it is now (BoundFunctionCreate) */
    JsValue tp = js_get_proto(ctx, tv);
    if (tp.bits != js_value_from_cell(&ctx->function_proto->gc).bits) {
        ((JsNative *)js_value_cell(fn))->own_proto = js_is_null(tp) ? js_undefined() : tp;
        ((JsNative *)js_value_cell(fn))->proto_set = true;
    }
    *r = fn;
    return true;
}

/* thisNumberValue: a Number method's receiver must be a number (there are no Number objects) */
#define NUMBER_THIS(name)                                                       \
    do {                                                                        \
        tv = this_primitive(ctx, tv);                                           \
        if (!js_is_number(tv))                                                  \
            return native_throw(ctx, r, "TypeError: Number.prototype." name     \
                                " requires that 'this' be a Number");           \
    } while (0)

static bool nm_valueOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    NUMBER_THIS("valueOf");
    *r = tv;
    return true;
}

/* ---- toFixed, toExponential, toPrecision: from the exact decimal value, rounded half up (of two
 * equally near, the larger), as the spec and V8 have them ---- */

/* The first KEEP (>= 1) digits of DIGS (N of them, zeros past the end) rounded half up by the
 * next one. True when that carried out of the first (999.. became 1000..: OUT is "1" and zeros
 * and the exponent grows by one). */
static bool round_half_up(const char *digs, int n, int keep, char *out) {
    for (int i = 0; i < keep; i++)
        out[i] = i < n ? digs[i] : '0';
    if (keep >= n || digs[keep] < '5')
        return false;
    int i = keep - 1;
    while (i >= 0 && out[i] == '9')
        out[i--] = '0';
    if (i >= 0) {
        out[i]++;
        return false;
    }
    out[0] = '1';
    return true;
}

static bool ascii_result(JsContext *ctx, const char *buf, JsValue *r) {
    JsString *s = js_ascii_cell(ctx->vm, buf);
    if (!s)
        return oom(ctx, r);
    *r = js_value_from_cell(&s->gc);
    return true;
}

/* d.ddd e±x */
static int put_exponential(char *buf, int bi, const char *digs, int nd, int e) {
    buf[bi++] = digs[0];
    if (nd > 1) {
        buf[bi++] = '.';
        for (int i = 1; i < nd; i++)
            buf[bi++] = digs[i];
    }
    bi += snprintf(buf + bi, 16, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    return bi;
}

static bool nm_toFixed(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    NUMBER_THIS("toFixed");
    double x = js_get_number(tv);
    double dd;
    TO_NUM(dd, ARG(0));
    dd = to_int(dd);
    if (dd < 0 || dd > 100)
        return native_throw(ctx, r, "RangeError: toFixed() digits argument must be between 0 and 100");
    int f = (int)dd;
    if (x != x || x >= 1e21 || x <= -1e21) {
        JsString *s = js_number_to_string(ctx->vm, x);
        if (!s)
            return oom(ctx, r);
        *r = js_value_from_cell(&s->gc);
        return true;
    }
    char buf[160], digs[800], n_digs[160];
    int bi = 0, nn;
    if (x < 0) {
        buf[bi++] = '-';
        x = -x;
    }
    /* n = x * 10^f rounded, as digits */
    if (x == 0) {
        n_digs[0] = '0';
        nn = 1;
    } else {
        int e10;
        int nd = js_number_exact(x, digs, &e10);
        int keep = e10 + 1 + f; /* the digits at or above 10^-f */
        if (keep <= 0) {
            n_digs[0] = keep == 0 && digs[0] >= '5' ? '1' : '0';
            nn = 1;
        } else if (round_half_up(digs, nd, keep, n_digs)) {
            n_digs[keep] = '0';
            nn = keep + 1;
        } else {
            nn = keep;
        }
    }
    /* at least f + 1 digits, then the point before the last f */
    int pad = f + 1 > nn ? f + 1 - nn : 0, total = nn + pad;
    for (int i = 0; i < total; i++) {
        if (f > 0 && i == total - f)
            buf[bi++] = '.';
        buf[bi++] = i < pad ? '0' : n_digs[i - pad];
    }
    buf[bi] = 0;
    return ascii_result(ctx, buf, r);
}

static bool nm_toExponential(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    NUMBER_THIS("toExponential");
    double x = js_get_number(tv);
    double dd;
    TO_NUM(dd, ARG(0));
    dd = to_int(dd);
    if (x != x || x == __builtin_inf() || x == -__builtin_inf()) {
        JsString *s = js_number_to_string(ctx->vm, x);
        if (!s)
            return oom(ctx, r);
        *r = js_value_from_cell(&s->gc);
        return true;
    }
    if (dd < 0 || dd > 100)
        return native_throw(ctx, r, "RangeError: toExponential() argument must be between 0 and 100");
    char buf[160], digs[800], out[110];
    int bi = 0, nd, e = 0;
    if (x < 0) {
        buf[bi++] = '-';
        x = -x;
    }
    bool shortest = js_is_undefined(ARG(0));
    if (x == 0) {
        nd = shortest ? 1 : (int)dd + 1;
        for (int i = 0; i < nd; i++)
            out[i] = '0';
    } else if (shortest) {
        nd = js_number_shortest(x, out, &e);
    } else {
        int n = js_number_exact(x, digs, &e);
        nd = (int)dd + 1;
        if (round_half_up(digs, n, nd, out))
            e++;
    }
    bi = put_exponential(buf, bi, out, nd, e);
    buf[bi] = 0;
    return ascii_result(ctx, buf, r);
}

static bool nm_toPrecision(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    NUMBER_THIS("toPrecision");
    double x = js_get_number(tv);
    if (js_is_undefined(ARG(0))) {
        JsString *s = js_number_to_string(ctx->vm, x);
        if (!s)
            return oom(ctx, r);
        *r = js_value_from_cell(&s->gc);
        return true;
    }
    double dd;
    TO_NUM(dd, ARG(0));
    dd = to_int(dd);
    if (x != x || x == __builtin_inf() || x == -__builtin_inf()) {
        JsString *s = js_number_to_string(ctx->vm, x);
        if (!s)
            return oom(ctx, r);
        *r = js_value_from_cell(&s->gc);
        return true;
    }
    if (dd < 1 || dd > 100)
        return native_throw(ctx, r, "RangeError: toPrecision() argument must be between 1 and 100");
    int p = (int)dd;
    char buf[160], digs[800], out[110];
    int bi = 0, e = 0;
    if (x < 0) {
        buf[bi++] = '-';
        x = -x;
    }
    if (x == 0) {
        for (int i = 0; i < p; i++)
            out[i] = '0';
    } else {
        int n = js_number_exact(x, digs, &e);
        if (round_half_up(digs, n, p, out))
            e++;
    }
    if (e < -6 || e >= p) {
        bi = put_exponential(buf, bi, out, p, e);
    } else if (e >= 0) {
        for (int i = 0; i < p; i++) {
            buf[bi++] = out[i];
            if (i == e && i < p - 1)
                buf[bi++] = '.';
        }
    } else {
        buf[bi++] = '0';
        buf[bi++] = '.';
        for (int i = 0; i < -(e + 1); i++)
            buf[bi++] = '0';
        for (int i = 0; i < p; i++)
            buf[bi++] = out[i];
    }
    buf[bi] = 0;
    return ascii_result(ctx, buf, r);
}

static bool nm_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    NUMBER_THIS("toString");
    double x = js_get_number(tv);
    /* Clamp to [0,37] before the cast (avoids UB on Infinity/NaN/huge); an
     * out-of-range radix lands outside 2..36 and is rejected just below. */
    int radix = 10;
    if (argc > 0 && !js_is_undefined(ARG(0))) {
        double rd;
        TO_NUM(rd, ARG(0));
        radix = (int)to_integer_clamped(rd, 0, 37);
    }
    if (radix < 2 || radix > 36)
        return native_throw(ctx, r, "RangeError: toString() radix argument must be between 2 and 36");
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
/* Math.fround: the nearest float32, as a double. Code that mirrors Float32Array arithmetic needs it. */
static double m_fround(double x) { return (double)(float)x; }

static double m_sign(double x) { return x != x ? x : (x > 0 ? 1 : (x < 0 ? -1 : x)); }

/* Math.f16round: the nearest IEEE binary16 value, ties to even, as a double. Scaling by the
 * half-precision quantum of x's binade is exact, so rint rounds once. */
static double m_f16round(double x) {
    double ax = x < 0 ? -x : x;
    if (x != x || ax == 0 || ax == __builtin_inf())
        return x;
    double q;
    if (ax < 0x1p-14) {
        q = 0x1p-24; /* half subnormals */
    } else {
        union { double d; uint64_t u; } b = {ax};
        int e = (int)((b.u >> 52) & 0x7ff) - 1023;
        if (e > 15)
            return x < 0 ? -__builtin_inf() : __builtin_inf();
        b.u = (uint64_t)(e - 10 + 1023) << 52;
        q = b.d;
    }
    double rv = __builtin_rint(ax / q) * q;
    if (rv > 65504)
        rv = __builtin_inf();
    return x < 0 ? -rv : rv;
}

static double m_clz32(double x) {
    uint32_t n = js_to_uint32(x);
    return n ? (double)__builtin_clz(n) : 32;
}

static bool math_imul(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    double a, b;
    TO_NUM(a, ARG(0));
    TO_NUM(b, ARG(1));
    *r = js_number((double)(int32_t)(js_to_uint32(a) * js_to_uint32(b)));
    return true;
}
static bool math_min(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    /* every argument converts (a valueOf runs) even after a NaN, as V8 */
    double best = __builtin_inf();
    bool nan = false;
    for (int i = 0; i < argc; i++) {
        double v;
        TO_NUM(v, args[i]);
        if (v != v)
            nan = true;
        else if (v < best || (v == 0 && best == 0 && 1.0 / v < 0))
            best = v;
    }
    if (nan)
        best = __builtin_nan("");
    *r = js_number(best);
    return true;
}

static bool math_max(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    /* every argument converts (a valueOf runs) even after a NaN, as V8 */
    double best = -__builtin_inf();
    bool nan = false;
    for (int i = 0; i < argc; i++) {
        double v;
        TO_NUM(v, args[i]);
        if (v != v)
            nan = true;
        else if (v > best || (v == 0 && best == 0 && 1.0 / v > 0))
            best = v;
    }
    if (nan)
        best = __builtin_nan("");
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
        double v;
        if (!js_op_to_number(ctx, args[i], &v)) {
            if (vals != stackv)
                js_realloc_raw(ctx->vm, vals, (size_t)argc * sizeof(double), 0);
            return oom(ctx, r);
        }
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
    double x, y;
    TO_NUM(x, ARG(0));
    TO_NUM(y, ARG(1));
    *r = js_number(js_k_pow(x, y));
    return true;
}

static bool math_atan2(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    double y, x;
    TO_NUM(y, ARG(0));
    TO_NUM(x, ARG(1));
    *r = js_number(js_k_atan2(y, x));
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
            if (c >= 0xD800 && c <= 0xDFFF) { /* well-formed JSON.stringify: a lone surrogate is escaped */
                bool paired = c <= 0xDBFF ? (i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF)
                                          : (i > 0 && u[i - 1] >= 0xD800 && u[i - 1] <= 0xDBFF);
                if (paired) {
                    sb_unit(sb, c);
                    break;
                }
            }
            if (c < 0x20 || (c >= 0xD800 && c <= 0xDFFF)) {
                char h[7];
                static const char *hex = "0123456789abcdef";
                h[0] = '\\'; h[1] = 'u'; h[2] = hex[(c >> 12) & 0xF]; h[3] = hex[(c >> 8) & 0xF];
                h[4] = hex[(c >> 4) & 0xF]; h[5] = hex[c & 0xF]; h[6] = 0;
                sb_ascii(sb, h);
            } else {
                sb_unit(sb, c);
            }
        }
    }
    sb_unit(sb, '"');
}

static void json_newline_indent(StrBuf *sb, const uint16_t *indent, uint32_t indent_len,
                                uint32_t depth) {
    if (indent_len == 0)
        return;
    sb_unit(sb, '\n');
    for (uint32_t i = 0; i < depth; i++)
        sb_units(sb, indent, indent_len);
}

/* JSON.stringify (25.5.2) as V8 has it: toJSON, a replacer function (called with the holder as
 * `this`, the wrapper {"": value} at the top) or property list, the gap, raw JSON
 * (JSON.rawJSON), and V8's circular-structure message. Values are read with [[Get]] in the
 * spec's order, so a toJSON or replacer that changes what is still to come is seen, and every
 * value held across a call is rooted. Errors are left pending (or the buffer marks OOM). */

/* Nesting bound: it guards the C stack, as the limit on nested interpreter entries does. */
#define JSON_MAX_DEPTH 200

typedef struct {
    JsContext *ctx;
    StrBuf sb;
    JsValue replacer; /* a function, or undefined */
    JsValue plist;    /* the property list (an array of strings), or undefined */
    JsValue stack;    /* the objects being serialized and the key each was reached by, in pairs */
    JsValue keys;     /* each open object's key snapshot, one after another (no array per object) */
    JsValue to_json;  /* the key "toJSON", looked up once */
    const uint16_t *gap;
    uint32_t gap_len;
    uint32_t depth;
} JsonS;

static bool json_prop(JsonS *s, JsValue holder, JsValue key, JsValue value, bool *emitted);

/* A key as the string toJSON and the replacer are given ("0" for an array's index). NULL on OOM. */
static JsString *json_key_string(JsContext *ctx, JsValue key) {
    return js_is_string(key) ? js_value_string(key) : js_number_to_string(ctx->vm, js_get_number(key));
}

static bool json_fail_pending(JsContext *ctx, JsValue err) {
    js_set_error(ctx, err);
    return false;
}

/* The name V8 gives an object in the circular-structure message: its constructor's, else Object */
static JsString *json_ctor_name(JsContext *ctx, JsValue o) {
    static const uint16_t kc[] = {'c', 'o', 'n', 's', 't', 'r', 'u', 'c', 't', 'o', 'r'};
    JsValue key = js_atom(ctx->vm, kc, 11), c = js_undefined();
    if (js_is_string(key) && js_get_property(ctx, o, key, &c) && js_is_function(c)) {
        JsString *n = js_function_name(c);
        if (n && n->length)
            return n;
    }
    js_take_error(ctx); /* a failed read just means no name */
    return js_ascii_cell(ctx->vm, "Object");
}

/* "property 'k'" or "index 3": how the message names the key an object was reached by */
static void json_key_phrase(JsContext *ctx, StrBuf *m, JsValue key) {
    if (js_is_number(key)) {
        JsString *k = js_number_to_string(ctx->vm, js_get_number(key));
        sb_ascii(m, "index ");
        if (k)
            sb_units(m, k->units, k->length);
        else
            m->oom = true;
    } else {
        JsString *k = js_value_string(key);
        sb_ascii(m, "property '");
        sb_units(m, k->units, k->length);
        sb_unit(m, '\'');
    }
}

static void json_ctor_phrase(JsContext *ctx, StrBuf *m, JsValue o) {
    JsString *n = json_ctor_name(ctx, o);
    sb_ascii(m, "object with constructor '");
    if (n)
        sb_units(m, n->units, n->length);
    else
        m->oom = true;
    sb_unit(m, '\'');
}

/* V8's TypeError for a cycle (JsonStringifier::ConstructCircularStructureErrorMessage): the object
 * it starts at, the first two steps and the last one of the path back to it, "..." for what lies
 * between, and the key that closes it. START is that object's index in the stack. */
static bool json_circular(JsonS *s, uint32_t start, JsValue closing_key) {
    JsContext *ctx = s->ctx;
    JsObject *st = js_value_object(s->stack);
    uint32_t n = st->elem_count / 2;
    StrBuf m;
    sb_init(&m, ctx->vm);
    sb_ascii(&m, "TypeError: Converting circular structure to JSON\n    --> starting at ");
    json_ctor_phrase(ctx, &m, st->elems[2 * start]);
    uint32_t i = start + 1, prefix_end = n < i + 2 ? n : i + 2;
    for (; i < prefix_end; i++) {
        sb_ascii(&m, "\n    |     ");
        json_key_phrase(ctx, &m, st->elems[2 * i + 1]);
        sb_ascii(&m, " -> ");
        json_ctor_phrase(ctx, &m, st->elems[2 * i]);
    }
    if (n > i + 1)
        sb_ascii(&m, "\n    |     ...");
    if (i < n - 1)
        i = n - 1;
    for (; i < n; i++) {
        sb_ascii(&m, "\n    |     ");
        json_key_phrase(ctx, &m, st->elems[2 * i + 1]);
        sb_ascii(&m, " -> ");
        json_ctor_phrase(ctx, &m, st->elems[2 * i]);
    }
    sb_ascii(&m, "\n    --- ");
    json_key_phrase(ctx, &m, closing_key);
    sb_ascii(&m, " closes the circle");
    JsValue msg = sb_finish(&m);
    if (!js_is_string(msg))
        return false;
    js_gc_protect(ctx->vm, &msg);
    JsValue err = js_error_from_cell(ctx, js_value_string(msg)); /* whole: js_error_fmt shortens a piece */
    js_gc_unprotect(ctx->vm, &msg);
    return json_fail_pending(ctx, err);
}

/* Enters O (reached by KEY): the cycle check, the depth bound, then onto the stack. */
static bool json_enter(JsonS *s, JsValue o, JsValue key) {
    JsObject *st = js_value_object(s->stack);
    for (uint32_t i = 0; i < st->elem_count; i += 2)
        if (st->elems[i].bits == o.bits)
            return json_circular(s, i / 2, key);
    if (s->depth >= JSON_MAX_DEPTH)
        return json_fail_pending(s->ctx, js_error_fmt(s->ctx, "RangeError: Maximum call stack size exceeded"));
    s->depth++;
    return js_array_append(s->ctx->vm, st, o) && js_array_append(s->ctx->vm, st, key);
}

static void json_leave(JsonS *s) {
    JsObject *st = js_value_object(s->stack);
    st->elem_count -= 2;
    s->depth--;
}

/* SerializeJSONArray: its length read once, each element read as it comes */
static bool json_array(JsonS *s, JsValue av, JsValue key) {
    if (!json_enter(s, av, key))
        return false;
    JsObject *a = js_value_object(av);
    uint32_t len = a->elem_count;
    sb_unit(&s->sb, '[');
    for (uint32_t i = 0; i < len; i++) {
        if (i)
            sb_unit(&s->sb, ',');
        json_newline_indent(&s->sb, s->gap, s->gap_len, s->depth);
        bool em;
        JsValue v = i < a->elem_count ? a->elems[i] : js_undefined();
        if (!json_prop(s, av, js_number(i), v, &em))
            return false;
        if (!em)
            sb_ascii(&s->sb, "null");
    }
    json_leave(s);
    if (len)
        json_newline_indent(&s->sb, s->gap, s->gap_len, s->depth);
    sb_unit(&s->sb, ']');
    return true;
}

/* The own enumerable string keys of O (a JsObject), in order, as an array. False on OOM. */
static bool json_own_keys(JsContext *ctx, JsObject *o, JsValue *out) {
    JsObject *keys = js_array_new_cell(ctx, 0);
    if (!keys)
        return false;
    *out = js_value_from_cell(&keys->gc);
    js_map_sort_keys(&o->props);
    for (uint32_t i = 0; i < o->props.capacity; i++) {
        JsMapEntry *e = &o->props.entries[i];
        if (!e->key || e->key == JS_MAP_TOMBSTONE || js_prop_hidden_at(o, i))
            continue;
        if (!js_array_append(ctx->vm, keys, js_value_from_cell(&e->key->gc)))
            return false; /* keys is the caller's to root: nothing here collects before append */
    }
    return true;
}

/* SerializeJSONObject: the property list, or a snapshot of the own enumerable keys (on the
 * shared key array, from FROM to its end while this object is open) */
static bool json_object(JsonS *s, JsValue ov, JsValue key) {
    JsContext *ctx = s->ctx;
    JsObject *o = js_is_promise(ov) ? js_cell_props(ctx, ov, false) : js_value_object(ov);
    if (!json_enter(s, ov, key))
        return false;
    JsObject *ka = js_value_object(js_is_undefined(s->plist) ? s->keys : s->plist);
    uint32_t from = 0, to = ka->elem_count;
    if (js_is_undefined(s->plist)) {
        from = ka->elem_count;
        if (o) {
            js_map_sort_keys(&o->props);
            for (uint32_t i = 0; i < o->props.capacity; i++) {
                JsMapEntry *e = &o->props.entries[i];
                if (!e->key || e->key == JS_MAP_TOMBSTONE || js_prop_hidden_at(o, i))
                    continue;
                if (!js_array_append(ctx->vm, ka, js_value_from_cell(&e->key->gc))) {
                    s->sb.oom = true;
                    return false;
                }
            }
        }
        to = ka->elem_count;
    }
    sb_unit(&s->sb, '{');
    bool first = true, ok = true;
    for (uint32_t i = from; i < to && ok; i++) {
        JsValue k = ka->elems[i];
        JsValue v;
        bool found = false;
        if (o) { /* an own data property is what [[Get]] reads; else (deleted meanwhile) the chain */
            JsValue raw = js_map_get(&o->props, js_value_string(k), &found);
            if (!js_prop_value(ctx, ov, raw, &v)) { /* an accessor's getter threw (pending) */
                ok = false;
                break;
            }
        }
        if (!found && !js_get_property(ctx, ov, k, &v)) {
            ok = false;
            break;
        }
        if (v.bits == JS_SPECIAL_TDZ) { /* a namespace's export not yet initialized */
            ok = json_fail_pending(ctx, js_error_fmt(ctx, "ReferenceError: Cannot access '%v' before initialization", k));
            break;
        }
        uint32_t mark = s->sb.len; /* rewind point if the value emits nothing */
        if (!first)
            sb_unit(&s->sb, ',');
        json_newline_indent(&s->sb, s->gap, s->gap_len, s->depth);
        json_quote(&s->sb, js_value_string(k)->units, js_value_string(k)->length);
        sb_unit(&s->sb, ':');
        if (s->gap_len)
            sb_unit(&s->sb, ' ');
        bool em;
        if (!json_prop(s, ov, k, v, &em)) {
            ok = false;
            break;
        }
        if (!em) {
            s->sb.len = mark;
            continue;
        }
        first = false;
    }
    if (js_is_undefined(s->plist))
        ka->elem_count = from; /* this object's snapshot is done */
    if (!ok)
        return false;
    json_leave(s);
    if (!first)
        json_newline_indent(&s->sb, s->gap, s->gap_len, s->depth);
    sb_unit(&s->sb, '}');
    return true;
}

/* Whether V is an object JSON.rawJSON made */
static bool json_is_raw(JsValue v) {
    return js_is_object(v) && (js_value_object(v)->obj_flags & JS_OBJ_RAW_JSON);
}

/* SerializeJSONProperty: VALUE is holder[key], already read. toJSON, then the replacer, then
 * the value's own form. *emitted is false for undefined, a function, or what becomes one. */
static bool json_prop(JsonS *s, JsValue holder, JsValue key, JsValue value, bool *emitted) {
    JsContext *ctx = s->ctx;
    *emitted = true;
    JsValue v = value, ks = js_undefined();
    js_gc_protect(ctx->vm, &v);
    js_gc_protect(ctx->vm, &ks);
    bool ok = true;
    if (js_is_object(v) || js_is_function(v) || js_is_promise(v) || js_is_bigint(v)) {
        JsValue tj = js_undefined();
        ok = js_get_property(ctx, v, s->to_json, &tj);
        if (ok && js_is_function(tj)) {
            JsString *k = json_key_string(ctx, key);
            ok = k != NULL;
            if (ok) {
                ks = js_value_from_cell(&k->gc);
                JsValue res;
                ok = js_call(ctx, tj, v, &ks, 1, &res) || json_fail_pending(ctx, res);
                if (ok)
                    v = res;
            }
        }
    }
    if (ok && js_is_function(s->replacer)) {
        if (js_is_undefined(ks)) {
            JsString *k = json_key_string(ctx, key);
            ok = k != NULL;
            if (ok)
                ks = js_value_from_cell(&k->gc);
        }
        if (ok) {
            JsValue argv[2] = {ks, v}, res;
            ok = js_call(ctx, s->replacer, holder, argv, 2, &res) || json_fail_pending(ctx, res);
            if (ok)
                v = res;
        }
    }
    if (ok) {
        if (js_is_null(v)) {
            sb_ascii(&s->sb, "null");
        } else if (js_is_bool(v)) {
            sb_ascii(&s->sb, js_get_bool(v) ? "true" : "false");
        } else if (js_is_string(v)) {
            json_quote(&s->sb, js_value_string(v)->units, js_value_string(v)->length);
        } else if (js_is_number(v)) {
            double d = js_get_number(v);
            if (d != d || d == __builtin_inf() || d == -__builtin_inf()) {
                sb_ascii(&s->sb, "null");
            } else {
                JsString *str = js_number_to_string(ctx->vm, d);
                if (str)
                    sb_units(&s->sb, str->units, str->length);
                else
                    s->sb.oom = true;
            }
        } else if (js_is_bigint(v)) {
            ok = json_fail_pending(ctx, js_error_fmt(ctx, "TypeError: Do not know how to serialize a BigInt"));
        } else if (json_is_raw(v)) {
            static const uint16_t kr[] = {'r', 'a', 'w', 'J', 'S', 'O', 'N'};
            JsValue rk = js_atom(ctx->vm, kr, 7), raw = js_undefined();
            ok = js_is_string(rk) && js_get_property(ctx, v, rk, &raw);
            if (ok && js_is_string(raw))
                sb_units(&s->sb, js_value_string(raw)->units, js_value_string(raw)->length);
        } else if (js_is_object(v) && js_value_object(v)->obj_kind == JS_OBJ_ARRAY) {
            ok = json_array(s, v, key);
        } else if (js_is_object(v) || js_is_promise(v)) {
            ok = json_object(s, v, key);
        } else {
            *emitted = false; /* undefined, a function */
        }
    }
    js_gc_unprotect(ctx->vm, &ks);
    js_gc_unprotect(ctx->vm, &v);
    return ok && !s->sb.oom;
}

/* A replacer array's property list: its strings and numbers as keys, in order, once each */
static bool json_property_list(JsContext *ctx, JsObject *ra, JsValue *out) {
    JsObject *list = js_array_new_cell(ctx, 0);
    if (!list)
        return false;
    *out = js_value_from_cell(&list->gc); /* the caller roots *out */
    for (uint32_t i = 0; i < ra->elem_count; i++) {
        JsValue e = ra->elems[i];
        JsString *k = js_is_string(e) ? js_value_string(e)
                      : js_is_number(e) ? js_number_to_string(ctx->vm, js_get_number(e)) : NULL;
        if (!k) {
            if (js_is_number(e))
                return false;
            continue;
        }
        bool dup = false;
        for (uint32_t j = 0; j < list->elem_count && !dup; j++) {
            const JsString *o = js_value_string(list->elems[j]);
            dup = o->length == k->length && memcmp(o->units, k->units, k->length * sizeof(uint16_t)) == 0;
        }
        if (!dup && !js_array_append(ctx->vm, list, js_value_from_cell(&k->gc)))
            return false;
    }
    return true;
}

static bool json_stringify(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    uint16_t gap[10];
    uint32_t gap_len = 0;
    JsValue space = ARG(2);
    if (js_is_number(space)) {
        /* space count is clamped to [0,10]; route through to_integer_clamped so
         * an Infinity/NaN/huge argument can't UB on the cast to int */
        int n = (int)to_integer_clamped(js_get_number(space), 0, 10);
        for (int i = 0; i < n; i++)
            gap[gap_len++] = ' ';
    } else if (js_is_string(space)) {
        JsString *sp = js_value_string(space);
        for (uint32_t i = 0; i < sp->length && i < 10; i++)
            gap[gap_len++] = sp->units[i];
    }
    JsonS s = {ctx, {0}, js_undefined(), js_undefined(), js_undefined(), js_undefined(),
               js_undefined(), gap, gap_len, 0};
    sb_init(&s.sb, ctx->vm);
    JsValue rep = ARG(1), wrapper = js_undefined();
    if (js_is_function(rep))
        s.replacer = rep; /* an argument: rooted */
    JsValue empty = js_undefined();
    js_gc_protect(ctx->vm, &s.plist);
    js_gc_protect(ctx->vm, &s.stack);
    js_gc_protect(ctx->vm, &s.keys);
    js_gc_protect(ctx->vm, &s.to_json);
    js_gc_protect(ctx->vm, &wrapper);
    js_gc_protect(ctx->vm, &empty);
    bool ok = true;
    if (js_is_object(rep) && js_value_object(rep)->obj_kind == JS_OBJ_ARRAY)
        ok = json_property_list(ctx, js_value_object(rep), &s.plist);
    JsObject *st = ok ? js_array_new_cell(ctx, 0) : NULL;
    ok = st != NULL;
    if (ok)
        s.stack = js_value_from_cell(&st->gc);
    JsObject *ka = ok ? js_array_new_cell(ctx, 0) : NULL;
    ok = ka != NULL;
    if (ok) {
        static const uint16_t kt[] = {'t', 'o', 'J', 'S', 'O', 'N'};
        s.keys = js_value_from_cell(&ka->gc);
        s.to_json = js_atom(ctx->vm, kt, 6);
        ok = js_is_string(s.to_json);
    }
    if (ok) {
        JsString *e = js_ascii_cell(ctx->vm, "");
        ok = e && (e = js_intern_cell(ctx->vm, e)) != NULL;
        if (ok)
            empty = js_value_from_cell(&e->gc);
    }
    if (ok && js_is_function(s.replacer)) { /* the replacer's first `this`: {"": value} */
        wrapper = js_object_new(ctx);
        ok = js_is_object(wrapper) &&
             js_map_set(ctx->vm, &js_value_object(wrapper)->props, js_value_string(empty), ARG(0));
    }
    bool em = false;
    if (ok)
        ok = json_prop(&s, wrapper, empty, ARG(0), &em);
    js_gc_unprotect(ctx->vm, &empty);
    js_gc_unprotect(ctx->vm, &wrapper);
    js_gc_unprotect(ctx->vm, &s.to_json);
    js_gc_unprotect(ctx->vm, &s.keys);
    js_gc_unprotect(ctx->vm, &s.stack);
    js_gc_unprotect(ctx->vm, &s.plist);
    if (!ok) {
        sb_free(&s.sb);
        return oom(ctx, r); /* the pending error, else out of memory */
    }
    if (!em) {
        sb_free(&s.sb);
        *r = js_undefined();
        return true;
    }
    *r = sb_finish(&s.sb);
    return js_is_undefined(*r) ? oom(ctx, r) : true;
}

/* --- JSON.parse --- */

typedef enum { JE_NONE, JE_END, JE_TOKEN, JE_AT, JE_OOM, JE_DEPTH } JsonErr;

typedef struct {
    JsContext *ctx;
    const uint16_t *u;
    uint32_t len, pos;
    bool error;
    int depth;
    JsonErr err;      /* what went wrong, for V8's wording */
    const char *why;  /* JE_AT: the message before " in JSON at position" */
    uint32_t at;      /* JE_TOKEN / JE_AT: the offending offset */
} JsonP;

/* Nesting cap for JSON.parse — bounds C-stack recursion on hostile input like
 * `[`.repeat(N). Mirrors JSON.stringify's own depth limit. */
#define JSON_PARSE_MAX_DEPTH 200

/* The first failure is the one reported. Each returns false. */
static bool json_fail(JsonP *p, JsonErr err, const char *why, uint32_t at) {
    if (!p->error) {
        p->error = true;
        p->err = err;
        p->why = why;
        p->at = at;
    }
    return false;
}
/* the input ran out; V8 says so unless it names what it expected there */
static bool json_end(JsonP *p) { return json_fail(p, JE_END, NULL, p->len); }
/* a character that cannot start or continue what is being read */
static bool json_token(JsonP *p, uint32_t at) {
    return at >= p->len ? json_end(p) : json_fail(p, JE_TOKEN, NULL, at);
}
static bool json_at(JsonP *p, const char *why, uint32_t at) { return json_fail(p, JE_AT, why, at); }
static bool json_oom(JsonP *p) { return json_fail(p, JE_OOM, NULL, 0); }

static void json_ws(JsonP *p) {
    while (p->pos < p->len) {
        uint16_t c = p->u[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->pos++;
        else
            break;
    }
}

static bool json_value(JsonP *p, JsValue *out, JsValue *snap);
static bool json_value_body(JsonP *p, JsValue *out, JsValue *snap);

/* With a reviver, JSON.parse keeps a snapshot of what it made, for the reviver's context argument
 * (V8's source text access): a primitive's node is [value, its source text], an object's or
 * array's [it, its members' nodes] (an object keyed as it is, an array in order). A and B are
 * rooted by the caller. False on OOM. */
static bool json_snap_node(JsonP *p, JsValue a, JsValue b, JsValue *snap) {
    JsObject *n = js_array_new_cell(p->ctx, 2);
    if (!n)
        return json_oom(p);
    n->elems[0] = a;
    n->elems[1] = b;
    n->elem_count = 2;
    *snap = js_value_from_cell(&n->gc);
    return true;
}

/* A primitive's node: VALUE and the source from START to the current position */
static bool json_snap_primitive(JsonP *p, uint32_t start, JsValue value, JsValue *snap) {
    js_gc_protect(p->ctx->vm, &value);
    JsString *src = js_string_cell_new(p->ctx->vm, p->u + start, p->pos - start);
    JsValue sv = src ? js_value_from_cell(&src->gc) : js_undefined();
    js_gc_protect(p->ctx->vm, &sv);
    bool ok = src ? json_snap_node(p, value, sv, snap) : json_oom(p);
    js_gc_unprotect(p->ctx->vm, &sv);
    js_gc_unprotect(p->ctx->vm, &value);
    return ok;
}

static bool json_lit(JsonP *p, const char *word, JsValue v, JsValue *out) {
    for (uint32_t i = 0; word[i]; i++) {
        if (p->pos + i >= p->len || p->u[p->pos + i] != (uint16_t)word[i])
            return json_token(p, p->pos + i);
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
    if (!JN_DIGIT(p->pos))
        return json_at(p, "No number after minus sign", p->pos);
    if (p->u[p->pos] == '0') {
        p->pos++; /* a leading zero stands alone — no further integer digits */
        if (JN_DIGIT(p->pos))
            return json_at(p, "Unexpected number", p->pos);
    } else {
        while (JN_DIGIT(p->pos))
            p->pos++;
    }
    if (p->pos < p->len && p->u[p->pos] == '.') {
        p->pos++;
        if (!JN_DIGIT(p->pos))
            return json_at(p, "Unterminated fractional number", p->pos);
        while (JN_DIGIT(p->pos))
            p->pos++;
    }
    if (p->pos < p->len && (p->u[p->pos] == 'e' || p->u[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->u[p->pos] == '+' || p->u[p->pos] == '-'))
            p->pos++;
        if (!JN_DIGIT(p->pos))
            return json_at(p, "Exponent part is missing a number", p->pos);
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
        uint32_t at = p->pos;
        uint16_t c = p->u[p->pos++];
        if (c == '"') {
            JsString *s = js_string_cell_new(p->ctx->vm, sb.u, sb.len);
            sb_free(&sb);
            if (!s)
                return json_oom(p);
            *out = s;
            return true;
        }
        if (c < 0x20) {
            sb_free(&sb);
            return json_at(p, "Bad control character in string literal", at);
        }
        if (c == '\\') {
            if (p->pos >= p->len) {
                sb_free(&sb);
                return json_end(p);
            }
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
                for (int i = 0; i < 4; i++) {
                    if (p->pos >= p->len) {
                        sb_free(&sb);
                        return json_end(p);
                    }
                    uint16_t h = p->u[p->pos];
                    int d = h >= '0' && h <= '9' ? h - '0'
                            : h >= 'a' && h <= 'f' ? h - 'a' + 10
                            : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                    if (d < 0) {
                        sb_free(&sb);
                        return json_at(p, "Bad Unicode escape", p->pos);
                    }
                    p->pos++;
                    cp = cp * 16 + (uint32_t)d;
                }
                sb_unit(&sb, (uint16_t)cp);
                break;
            }
            default:
                sb_free(&sb);
                return json_at(p, "Bad escaped character", p->pos - 1);
            }
        } else {
            sb_unit(&sb, c);
        }
    }
    sb_free(&sb);
    return json_at(p, "Unterminated string", p->len);
}

/* Depth-guarded entry: bounds recursion before descending into json_value_body,
 * which recurses back through json_value for nested arrays/objects. */
static bool json_value(JsonP *p, JsValue *out, JsValue *snap) {
    if (++p->depth > JSON_PARSE_MAX_DEPTH) {
        json_fail(p, JE_DEPTH, NULL, p->pos);
        p->depth--;
        return false;
    }
    bool ok = json_value_body(p, out, snap);
    p->depth--;
    return ok;
}

static bool json_value_body(JsonP *p, JsValue *out, JsValue *snap) {
    json_ws(p);
    if (p->pos >= p->len)
        return json_end(p);
    uint16_t c = p->u[p->pos];
    uint32_t start = p->pos;
    if (c == '{') {
        p->pos++;
        JsValue ov = js_object_new(p->ctx), kids = js_undefined();
        if (!js_is_object(ov))
            return json_oom(p);
        JsObject *o = js_value_object(ov);
        js_gc_protect(p->ctx->vm, &ov);
        js_gc_protect(p->ctx->vm, &kids);
        if (snap) {
            kids = js_object_new(p->ctx);
            if (!js_is_object(kids)) {
                js_gc_unprotect(p->ctx->vm, &kids);
                js_gc_unprotect(p->ctx->vm, &ov);
                return json_oom(p);
            }
        }
        json_ws(p);
        bool okc = true;
        if (p->pos < p->len && p->u[p->pos] == '}') {
            p->pos++;
        } else {
            bool after_comma = false;
            for (;;) {
                json_ws(p);
                if (p->pos >= p->len || p->u[p->pos] != '"') {
                    okc = json_at(p, after_comma ? "Expected double-quoted property name"
                                                 : "Expected property name or '}'", p->pos);
                    break;
                }
                JsString *key;
                if (!json_string_raw(p, &key)) {
                    okc = false;
                    break;
                }
                JsString *ikey = js_intern_cell(p->ctx->vm, key);
                if (!ikey) {
                    okc = json_oom(p);
                    break;
                }
                /* root the key across the value parse (a GC safe point) */
                JsValue keyv = js_value_from_cell(&ikey->gc);
                js_gc_protect(p->ctx->vm, &keyv);
                json_ws(p);
                if (p->pos >= p->len || p->u[p->pos] != ':') {
                    js_gc_unprotect(p->ctx->vm, &keyv);
                    okc = json_at(p, "Expected ':' after property name", p->pos);
                    break;
                }
                p->pos++;
                JsValue val, kid = js_undefined();
                if (!json_value(p, &val, snap ? &kid : NULL)) {
                    js_gc_unprotect(p->ctx->vm, &keyv);
                    okc = false;
                    break;
                }
                js_gc_protect(p->ctx->vm, &kid);
                bool set_ok = js_map_set(p->ctx->vm, &o->props,
                                         js_value_string(keyv), val) &&
                              (!snap || js_map_set(p->ctx->vm, &js_value_object(kids)->props,
                                                   js_value_string(keyv), kid));
                js_gc_unprotect(p->ctx->vm, &kid);
                js_gc_unprotect(p->ctx->vm, &keyv);
                if (!set_ok) {
                    okc = json_oom(p);
                    break;
                }
                json_ws(p);
                if (p->pos < p->len && p->u[p->pos] == ',') {
                    p->pos++;
                    after_comma = true;
                    continue;
                }
                if (p->pos < p->len && p->u[p->pos] == '}') {
                    p->pos++;
                    break;
                }
                okc = json_at(p, "Expected ',' or '}' after property value", p->pos);
                break;
            }
        }
        if (okc && snap)
            okc = json_snap_node(p, ov, kids, snap);
        js_gc_unprotect(p->ctx->vm, &kids);
        js_gc_unprotect(p->ctx->vm, &ov);
        if (!okc)
            return false;
        *out = ov;
        return true;
    }
    if (c == '[') {
        p->pos++;
        JsObject *a = js_array_new_cell(p->ctx, 0);
        if (!a)
            return json_oom(p);
        JsValue av = js_value_from_cell(&a->gc), kids = js_undefined();
        js_gc_protect(p->ctx->vm, &av);
        js_gc_protect(p->ctx->vm, &kids);
        if (snap) {
            JsObject *k = js_array_new_cell(p->ctx, 0);
            if (!k) {
                js_gc_unprotect(p->ctx->vm, &kids);
                js_gc_unprotect(p->ctx->vm, &av);
                return json_oom(p);
            }
            kids = js_value_from_cell(&k->gc);
        }
        json_ws(p);
        bool okc = true;
        if (p->pos < p->len && p->u[p->pos] == ']') {
            p->pos++;
        } else {
            for (;;) {
                JsValue el, kid = js_undefined();
                if (!json_value(p, &el, snap ? &kid : NULL)) {
                    okc = false;
                    break;
                }
                js_gc_protect(p->ctx->vm, &kid);
                bool app = js_array_append(p->ctx->vm, a, el) &&
                           (!snap || js_array_append(p->ctx->vm, js_value_object(kids), kid));
                js_gc_unprotect(p->ctx->vm, &kid);
                if (!app) {
                    okc = json_oom(p);
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
                okc = json_at(p, "Expected ',' or ']' after array element", p->pos);
                break;
            }
        }
        if (okc && snap)
            okc = json_snap_node(p, av, kids, snap);
        js_gc_unprotect(p->ctx->vm, &kids);
        js_gc_unprotect(p->ctx->vm, &av);
        if (!okc)
            return false;
        *out = av;
        return true;
    }
    bool ok;
    if (c == '"') {
        JsString *s;
        ok = json_string_raw(p, &s);
        if (ok)
            *out = js_value_from_cell(&s->gc);
    } else if (c == 't') {
        ok = json_lit(p, "true", js_bool(true), out);
    } else if (c == 'f') {
        ok = json_lit(p, "false", js_bool(false), out);
    } else if (c == 'n') {
        ok = json_lit(p, "null", js_null(), out);
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        ok = json_number(p, out);
    } else {
        return json_token(p, p->pos);
    }
    if (ok && snap)
        ok = json_snap_primitive(p, start, *out, snap);
    return ok;
}

/* V8's message for a failed parse: "Unexpected token 'x', "[1, x]" is not valid JSON" (the
 * source, or up to 10 characters either side of the token when it is longer than 20),
 * "Expected ':' after property name in JSON at position 4 (line 1 column 5)", "Unexpected end
 * of JSON input". */
static JsValue json_error(JsContext *ctx, const JsonP *p) {
    JsVm *vm = ctx->vm;
    if (p->err == JE_OOM)
        return js_oom_value(vm);
    if (p->err == JE_DEPTH)
        return js_error_from_ascii(ctx, "RangeError: Maximum call stack size exceeded");
    if (p->err == JE_END || p->err == JE_NONE)
        return js_error_from_ascii(ctx, "SyntaxError: Unexpected end of JSON input");
    if (p->err == JE_AT) {
        uint32_t line = 1, col = 1;
        for (uint32_t i = 0; i < p->at && i < p->len; i++) {
            if (p->u[i] == '\n') {
                line++;
                col = 1;
            } else {
                col++;
            }
        }
        char tail[96];
        size_t wl = strlen(p->why); /* "...after JSON" already says where */
        bool says_json = wl >= 4 && strcmp(p->why + wl - 4, "JSON") == 0;
        snprintf(tail, sizeof tail, "%s at position %u (line %u column %u)", says_json ? "" : " in JSON",
                 p->at, line, col);
        return js_error_fmt(ctx, "SyntaxError: %s%s", p->why, tail);
    }
    /* JE_TOKEN */
    uint32_t at = p->at, start = 0, end = p->len;
    bool before = false, after = false;
    if (p->len > 20) {
        start = at < 10 ? 0 : at - 10;
        end = at + 10 > p->len ? p->len : at + 10;
        before = start > 0;
        after = end < p->len;
    }
    JsString *tok = js_string_cell_new(vm, p->u + at, 1);
    JsValue tv = tok ? js_value_from_cell(&tok->gc) : js_undefined();
    js_gc_protect(vm, &tv);
    JsString *ctxs = js_string_cell_new(vm, p->u + start, end - start);
    JsValue cv = ctxs ? js_value_from_cell(&ctxs->gc) : js_undefined();
    js_gc_protect(vm, &cv);
    JsValue e = tok && ctxs ? js_error_fmt(ctx, "SyntaxError: Unexpected token '%v', %s\"%v\"%s is not valid JSON",
                                           tv, before ? "..." : "", cv, after ? "..." : "")
                            : js_oom_value(vm);
    js_gc_unprotect(vm, &cv);
    js_gc_unprotect(vm, &tv);
    return e;
}

/* InternalizeJSONProperty with V8's source text access: VAL is holder[name]; SNAP is the node
 * the parse made for what stood there, while the value is still that one (undefined once a
 * reviver has replaced it). The reviver is called with (name, val, context), context.source the
 * primitive's source text while it is unchanged. Each member is visited first, and replaced by
 * what the reviver returns (deleted for undefined) as CreateDataProperty and [[Delete]] would,
 * silently doing nothing on a frozen or sealed object. False with an error pending, or OOM. */
#define JSON_REVIVE_MAX_DEPTH 200

typedef struct {
    JsContext *ctx;
    JsValue reviver;
    uint32_t depth;
} JsonR;

static bool json_revive(JsonR *rv, JsValue holder, JsValue name, JsValue snap, JsValue *out);

/* CreateDataProperty(o, key, v) or, for undefined, [[Delete]](o, key), as internalize does them:
 * a refused one is no error. KEY is an interned string; for an array, INDEX is its index. */
static bool json_revive_put(JsContext *ctx, JsValue ov, JsValue key, int64_t index, JsValue v) {
    JsObject *o = js_has_cell_props(ov) ? js_cell_props(ctx, ov, !js_is_undefined(v))
                                        : js_value_object(ov);
    if (!o)
        return js_is_undefined(v); /* nothing to delete from; OOM making the props */
    if (o->obj_flags & JS_OBJ_FROZEN)
        return true;
    if (index >= 0 && o->obj_kind == JS_OBJ_ARRAY) {
        uint32_t i = (uint32_t)index;
        if (js_is_undefined(v)) { /* a hole here reads as undefined */
            if (i < o->elem_count && !(o->obj_flags & JS_OBJ_SEALED))
                o->elems[i] = js_undefined();
            return true;
        }
        if (i < o->elem_count) {
            o->elems[i] = v;
            return true;
        }
        if (o->obj_flags & JS_OBJ_NO_ADD)
            return true;
        js_gc_protect(ctx->vm, &v);
        bool ok = true;
        while (ok && o->elem_count < i)
            ok = js_array_append(ctx->vm, o, js_undefined());
        ok = ok && js_array_append(ctx->vm, o, v);
        js_gc_unprotect(ctx->vm, &v);
        return ok;
    }
    JsString *k = js_value_string(key);
    uint8_t attrs = js_obj_attrs(o, js_map_get_attrs(&o->props, k));
    if (js_is_undefined(v)) {
        if (!(attrs & JS_ATTR_FIXED))
            js_map_delete(&o->props, k);
        return true;
    }
    bool found;
    js_map_get(&o->props, k, &found);
    if (found ? (attrs & JS_ATTR_FIXED) != 0 : (o->obj_flags & JS_OBJ_NO_ADD) != 0)
        return true; /* CreateDataProperty refuses a non-configurable one or a new one here */
    if (!js_map_set(ctx->vm, &o->props, k, v))
        return false;
    js_map_put_attrs(&o->props, k, 0); /* a data property, enumerable, writable, configurable */
    return true;
}

static bool json_revive(JsonR *rv, JsValue holder, JsValue name, JsValue snap, JsValue *out) {
    JsContext *ctx = rv->ctx;
    if (rv->depth >= JSON_REVIVE_MAX_DEPTH) {
        js_set_error(ctx, js_error_fmt(ctx, "RangeError: Maximum call stack size exceeded"));
        return false;
    }
    JsValue val, kids = js_undefined(), keys = js_undefined(), cx = js_undefined();
    if (!js_get_property(ctx, holder, name, &val))
        return false;
    js_gc_protect(ctx->vm, &val);
    js_gc_protect(ctx->vm, &kids);
    js_gc_protect(ctx->vm, &keys);
    js_gc_protect(ctx->vm, &cx);
    JsObject *sn = js_is_object(snap) ? js_value_object(snap) : NULL;
    bool same = sn && sn->elems[0].bits == val.bits; /* still what the parse made */
    bool ok = true;
    rv->depth++;
    if (js_is_object(val) || js_is_function(val) || js_is_promise(val)) {
        if (same)
            kids = sn->elems[1];
        if (js_is_object(val) && js_value_object(val)->obj_kind == JS_OBJ_ARRAY) {
            uint32_t len = js_value_object(val)->elem_count;
            for (uint32_t i = 0; i < len && ok; i++) {
                JsString *k = js_number_to_string(ctx->vm, i);
                k = k ? js_intern_cell(ctx->vm, k) : NULL;
                ok = k != NULL;
                if (!ok)
                    break;
                JsValue kv = js_value_from_cell(&k->gc), nv;
                js_gc_protect(ctx->vm, &kv);
                JsObject *ka = js_is_object(kids) ? js_value_object(kids) : NULL;
                JsValue ks = ka && i < ka->elem_count ? ka->elems[i] : js_undefined();
                ok = json_revive(rv, val, kv, ks, &nv) && json_revive_put(ctx, val, kv, i, nv);
                js_gc_unprotect(ctx->vm, &kv);
            }
        } else {
            JsObject *o = js_has_cell_props(val) ? js_cell_props(ctx, val, false) : js_value_object(val);
            if (o)
                ok = json_own_keys(ctx, o, &keys);
            uint32_t n = ok && js_is_object(keys) ? js_value_object(keys)->elem_count : 0;
            for (uint32_t i = 0; i < n && ok; i++) {
                JsValue kv = js_value_object(keys)->elems[i], nv, ks = js_undefined();
                if (js_is_object(kids)) {
                    bool found;
                    ks = js_map_get(&js_value_object(kids)->props, js_value_string(kv), &found);
                    if (!found)
                        ks = js_undefined();
                }
                ok = json_revive(rv, val, kv, ks, &nv) && json_revive_put(ctx, val, kv, -1, nv);
            }
        }
    }
    rv->depth--;
    if (ok) { /* the context: { source } for a primitive still as parsed */
        cx = js_object_new(ctx);
        ok = js_is_object(cx);
        if (ok && same && !(js_is_object(val) || js_is_function(val) || js_is_promise(val)))
            ok = js_object_set_ascii(ctx, js_value_object(cx), "source", sn->elems[1]);
    }
    if (ok) {
        JsValue argv[3] = {name, val, cx}, res;
        ok = js_call(ctx, rv->reviver, holder, argv, 3, &res);
        if (ok)
            *out = res;
        else
            js_set_error(ctx, res);
    }
    js_gc_unprotect(ctx->vm, &cx);
    js_gc_unprotect(ctx->vm, &keys);
    js_gc_unprotect(ctx->vm, &kids);
    js_gc_unprotect(ctx->vm, &val);
    return ok;
}

static bool json_parse(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsString *s = arg_string(ctx, ARG(0));
    if (!s)
        return oom(ctx, r);
    JsValue sv = js_value_from_cell(&s->gc), snap = js_undefined();
    js_gc_protect(ctx->vm, &sv);
    js_gc_protect(ctx->vm, &snap);
    bool revive = js_is_function(ARG(1));
    JsonP p = {ctx, s->units, s->length, 0, false, 0, JE_NONE, NULL, 0};
    JsValue out;
    bool ok = json_value(&p, &out, revive ? &snap : NULL);
    if (ok) {
        json_ws(&p);
        if (p.pos != p.len)
            ok = json_at(&p, "Unexpected non-whitespace character after JSON", p.pos);
    }
    if (!ok || p.error) {
        *r = json_error(ctx, &p); /* s is still rooted: the message quotes it */
        js_gc_unprotect(ctx->vm, &snap);
        js_gc_unprotect(ctx->vm, &sv);
        return false;
    }
    js_gc_unprotect(ctx->vm, &sv);
    if (!revive) {
        js_gc_unprotect(ctx->vm, &snap);
        *r = out;
        return true;
    }
    /* the reviver starts at a wrapper {"": result} */
    js_gc_protect(ctx->vm, &out);
    JsValue root = js_object_new(ctx), empty = js_undefined();
    js_gc_protect(ctx->vm, &root);
    js_gc_protect(ctx->vm, &empty);
    JsString *e = js_is_object(root) ? js_ascii_cell(ctx->vm, "") : NULL;
    e = e ? js_intern_cell(ctx->vm, e) : NULL;
    if (e)
        empty = js_value_from_cell(&e->gc);
    ok = e && js_map_set(ctx->vm, &js_value_object(root)->props, e, out);
    JsonR rv = {ctx, ARG(1), 0};
    JsValue res = js_undefined();
    ok = ok && json_revive(&rv, root, empty, snap, &res);
    js_gc_unprotect(ctx->vm, &empty);
    js_gc_unprotect(ctx->vm, &root);
    js_gc_unprotect(ctx->vm, &out);
    js_gc_unprotect(ctx->vm, &snap);
    if (!ok)
        return oom(ctx, r);
    *r = res;
    return true;
}

/* JSON.parse(TEXT) for the runtime (a JSON module): false with the error in *OUT */
bool js_json_parse_text(JsContext *ctx, JsValue text, JsValue *out) {
    return json_parse(ctx, js_undefined(), &text, 1, out);
}

/* JSON.rawJSON(text): a frozen, prototype-less object whose `rawJSON` is TEXT, which must be one
 * JSON primitive with nothing around it; JSON.stringify writes it as it is. V8's errors: "Invalid
 * value for JSON.rawJSON" when it is empty or something follows, else the parser's own. */
static bool json_rawJSON(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsString *s = arg_string(ctx, ARG(0));
    if (!s)
        return oom(ctx, r);
    JsValue sv = js_value_from_cell(&s->gc);
    js_gc_protect(ctx->vm, &sv);
    JsonP p = {ctx, s->units, s->length, 0, false, 0, JE_NONE, NULL, 0};
    bool ok = s->length > 0, invalid = !ok;
    if (ok) {
        uint16_t c = s->units[0];
        JsValue v;
        if (c == '"' || c == 't' || c == 'f' || c == 'n' || c == '-' || (c >= '0' && c <= '9'))
            ok = json_value_body(&p, &v, NULL);
        else
            ok = json_token(&p, 0);
        if (ok && p.pos != p.len)
            ok = false, invalid = true;
    }
    if (!ok) {
        *r = invalid ? js_error_fmt(ctx, "SyntaxError: Invalid value for JSON.rawJSON")
                     : json_error(ctx, &p);
        js_gc_unprotect(ctx->vm, &sv);
        return false;
    }
    JsValue o = js_object_new(ctx);
    js_gc_protect(ctx->vm, &o); /* the key's allocation below may collect */
    bool made = js_is_object(o) && js_object_set_ascii(ctx, js_value_object(o), "rawJSON", sv);
    js_gc_unprotect(ctx->vm, &o);
    js_gc_unprotect(ctx->vm, &sv);
    if (!made)
        return oom(ctx, r);
    JsObject *ob = js_value_object(o);
    ob->proto = js_undefined(); /* null prototype */
    ob->obj_flags |= JS_OBJ_SEALED | JS_OBJ_FROZEN | JS_OBJ_RAW_JSON;
    *r = o;
    return true;
}

/* Function.prototype, called: it accepts any arguments and returns undefined */
static bool fp_call(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)ctx;
    (void)tv;
    (void)args;
    (void)argc;
    *r = js_undefined();
    return true;
}

static bool json_isRawJSON(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)ctx;
    (void)tv;
    *r = js_bool(json_is_raw(ARG(0)));
    return true;
}

/* =====================================================================
 * Object / Array / Number / String statics + globals
 * ===================================================================== */

/* Object.keys/values/entries of a string primitive: its indices, its one-unit strings, or both */
static bool string_keys(JsContext *ctx, JsString *str, JsValue *r, int mode) {
    JsValue sv = js_value_from_cell(&str->gc);
    js_gc_protect(ctx->vm, &sv);
    JsObject *out = js_array_new_cell(ctx, 0);
    JsValue outv = out ? js_value_from_cell(&out->gc) : js_undefined();
    js_gc_protect(ctx->vm, &outv);
    bool ok = out != NULL;
    for (uint32_t i = 0; ok && i < str->length; i++) {
        JsValue k = js_undefined(), c = js_undefined();
        js_gc_protect(ctx->vm, &k);
        if (mode != 1) {
            JsString *ks = js_number_to_string(ctx->vm, i);
            ok = ks != NULL;
            if (ok)
                k = js_value_from_cell(&ks->gc);
        }
        if (ok && mode != 0) {
            JsString *cs = js_string_cell_new(ctx->vm, str->units + i, 1);
            ok = cs != NULL;
            if (ok)
                c = js_value_from_cell(&cs->gc);
        }
        if (ok && mode == 2) {
            js_gc_protect(ctx->vm, &c);
            JsObject *pair = js_array_new_cell(ctx, 2);
            js_gc_unprotect(ctx->vm, &c);
            ok = pair != NULL;
            if (ok) {
                pair->elems[0] = k;
                pair->elems[1] = c;
                pair->elem_count = 2;
                ok = js_array_append(ctx->vm, out, js_value_from_cell(&pair->gc));
            }
        } else if (ok) {
            ok = js_array_append(ctx->vm, out, mode == 0 ? k : c);
        }
        js_gc_unprotect(ctx->vm, &k);
    }
    js_gc_unprotect(ctx->vm, &outv);
    js_gc_unprotect(ctx->vm, &sv);
    if (!ok)
        return oom(ctx, r);
    *r = outv;
    return true;
}

static bool obj_keys_impl(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                          JsValue *r, int mode /*0 keys,1 values,2 entries*/) {
    (void)tv;
    JsValue v = ARG(0);
    if (js_is_undefined(v) || js_is_null(v)) /* ToObject */
        return native_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
    if (js_is_string(v)) /* a string's own enumerable keys are its indices */
        return string_keys(ctx, js_value_string(v), r, mode);
    if (js_has_cell_props(v)) { /* a function's or promise's own properties (none until written) */
        JsObject *own = js_cell_props(ctx, v, false);
        v = own ? js_value_from_cell(&own->gc) : js_undefined();
    }
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
        }
        /* Named properties, after an array's elements (index keys first, then insertion order). */
        js_map_sort_keys(&o->props);
        {
            for (uint32_t i = 0; i < o->props.capacity && ok; i++) {
                JsMapEntry *e = &o->props.entries[i];
                if (!e->key || e->key == JS_MAP_TOMBSTONE || js_prop_hidden_at(o, i))
                    continue;
                if (e->value.bits == JS_SPECIAL_TDZ) { /* a namespace's uninitialized export */
                    js_gc_unprotect(ctx->vm, &outv);
                    return mode == 0
                        ? native_throw_fmt(ctx, r, "ReferenceError: Cannot access '%S' before initialization", e->key)
                        : native_throw_fmt(ctx, r, "ReferenceError: %S is not defined", e->key);
                }
                JsValue entry, pval = js_undefined();
                /* an accessor's getter runs here (a builtin's: it leaves the map as it is) */
                if (mode != 0 && !js_prop_value(ctx, v, e->value, &pval)) {
                    ok = false;
                    break;
                }
                if (mode == 0) {
                    entry = js_value_from_cell(&e->key->gc);
                } else if (mode == 1) {
                    entry = pval;
                } else {
                    js_gc_protect(ctx->vm, &pval);
                    JsObject *pair = js_array_new_cell(ctx, 2);
                    js_gc_unprotect(ctx->vm, &pval);
                    if (!pair) { ok = false; break; }
                    pair->elems[0] = js_value_from_cell(&e->key->gc);
                    pair->elems[1] = pval;
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

/* Appends the key named by the ASCII string A to OUT (rooted). False on OOM. */
static bool append_ascii_key(JsContext *ctx, JsObject *out, const char *a) {
    JsString *k = js_ascii_cell(ctx->vm, a);
    return k && js_array_append(ctx->vm, out, js_value_from_cell(&k->gc));
}

/* Appends the indices 0..N-1 to OUT (rooted) as strings. False on OOM. */
static bool append_index_keys(JsContext *ctx, JsObject *out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        JsString *k = js_number_to_string(ctx->vm, i);
        if (!k || !js_array_append(ctx->vm, out, js_value_from_cell(&k->gc)))
            return false;
    }
    return true;
}

static bool str_is_ascii(JsValue key, const char *a);
static inline bool is_object_type_arg(JsValue v);

static bool is_index_below(const JsString *s, uint32_t len);

/* Appends the keys in O's property map, enumerable or not, in enumeration order (integer keys
 * ascending, then insertion order), to OUT: all of them, or for a function (FN_PART, FN_INDICES
 * or FN_NAMED) its integer keys, which come before the "length", "name" and "prototype" it has
 * by nature, or the others, without the "name" (SKIP_NAME) and "length" (SKIP_LENGTH) listed
 * already. O and OUT are rooted. False on OOM. */
enum { FN_ALL_KEYS, FN_INDICES, FN_NAMED };
static bool append_map_keys_of(JsContext *ctx, JsObject *out, JsObject *o, int fn_part,
                               bool skip_name, bool skip_length) {
    js_map_sort_keys(&o->props);
    for (uint32_t i = 0; i < o->props.capacity; i++) {
        JsMapEntry *e = &o->props.entries[i];
        if (!e->key || e->key == JS_MAP_TOMBSTONE)
            continue;
        if (fn_part != FN_ALL_KEYS && is_index_below(e->key, UINT32_MAX) != (fn_part == FN_INDICES))
            continue;
        if ((skip_name && str_is_ascii(js_value_from_cell(&e->key->gc), "name")) ||
            (skip_length && str_is_ascii(js_value_from_cell(&e->key->gc), "length")))
            continue;
        if (!js_array_append(ctx->vm, out, js_value_from_cell(&e->key->gc)))
            return false;
    }
    return true;
}

static bool append_map_keys(JsContext *ctx, JsObject *out, JsObject *o) {
    return append_map_keys_of(ctx, out, o, FN_ALL_KEYS, false, false);
}

/* Object.getOwnPropertyNames(o): every own string key, enumerable or not, in V8's order. Its
 * [[OwnPropertyKeys]] reads no values, so a namespace's uninitialized export is listed, not thrown.
 *   string      its indices, then "length"; a number or boolean has none
 *   array       its indices, then "length", then its named properties
 *   function    the integer keys a script gave it, then "length", "name", "prototype" when it
 *               has one, then a builtin's statics and what else a script wrote onto it; a "name"
 *               or "length" defined again after it was deleted is one of those, and so comes
 *               where it was defined
 *   error       its map's order: "stack" (made with the error), "message", "cause" and the rest
 *   regexp      "lastIndex", then the rest */
static bool obj_getOwnPropertyNames(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                                    JsValue *r) {
    (void)tv;
    JsValue v = ARG(0);
    if (js_is_undefined(v) || js_is_null(v)) /* ToObject */
        return native_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
    JsObject *out = js_array_new_cell(ctx, 0);
    if (!out)
        return oom(ctx, r);
    JsValue outv = js_value_from_cell(&out->gc);
    js_gc_protect(ctx->vm, &outv);
    bool ok = true;
    if (js_is_string(v)) {
        ok = append_index_keys(ctx, out, js_value_string(v)->length) &&
             append_ascii_key(ctx, out, "length");
    } else if (js_is_function(v)) {
        JsGcCell *c = js_value_cell(v);
        /* the two it has by nature keep their place while they are not deleted, whatever
         * Object.defineProperty has made of them since */
        bool has_length = js_function_has_own_special(v, false);
        bool has_name = js_function_has_own_special(v, true);
        JsObject *own = js_cell_props(ctx, v, false);
        ok = (!own || append_map_keys_of(ctx, out, own, FN_INDICES, false, false)) &&
             (!has_length || append_ascii_key(ctx, out, "length")) &&
             (!has_name || append_ascii_key(ctx, out, "name")) &&
             (!js_function_has_own_prototype(v) || append_ascii_key(ctx, out, "prototype"));
        if (ok && c->kind == JS_KIND_NATIVE && ((JsNative *)c)->statics)
            ok = append_map_keys(ctx, out, ((JsNative *)c)->statics);
        if (ok && own)
            ok = append_map_keys_of(ctx, out, own, FN_NAMED, has_name, has_length);
    } else if (js_is_promise(v)) {
        JsObject *own = js_cell_props(ctx, v, false);
        if (own)
            ok = append_map_keys(ctx, out, own);
    } else if (js_is_object(v)) {
        JsObject *o = js_value_object(v);
        if (o->obj_kind == JS_OBJ_ARRAY)
            ok = append_index_keys(ctx, out, o->elem_count) && append_ascii_key(ctx, out, "length");
#ifdef LAMASSU_HAS_REGEX
        else if (o->obj_kind == JS_OBJ_REGEXP)
            ok = append_ascii_key(ctx, out, "lastIndex");
#endif
        ok = ok && append_map_keys(ctx, out, o);
    }
    js_gc_unprotect(ctx->vm, &outv);
    if (!ok)
        return oom(ctx, r);
    *r = outv;
    return true;
}

/* The own enumerable keys of SRC as an array of strings, in V8's order, for Object.assign: a
 * string's indices; an array's indices, then its named properties; a function's or promise's own
 * properties. False on OOM. */
static bool assign_source_keys(JsContext *ctx, JsValue src, JsValue *out) {
    JsObject *keys = js_array_new_cell(ctx, 0);
    if (!keys)
        return false;
    *out = js_value_from_cell(&keys->gc);
    js_gc_protect(ctx->vm, out);
    bool ok = true;
    if (js_is_string(src)) {
        ok = append_index_keys(ctx, keys, js_value_string(src)->length);
    } else {
        JsObject *o = js_has_cell_props(src) ? js_cell_props(ctx, src, false)
                      : js_is_object(src) ? js_value_object(src) : NULL;
        if (o && o->obj_kind == JS_OBJ_ARRAY)
            ok = append_index_keys(ctx, keys, o->elem_count);
        if (o && ok) {
            js_map_sort_keys(&o->props);
            for (uint32_t i = 0; ok && i < o->props.capacity; i++) {
                JsString *k = o->props.entries[i].key;
                if (k && k != JS_MAP_TOMBSTONE && !js_prop_hidden_at(o, i))
                    ok = js_array_append(ctx->vm, keys, js_value_from_cell(&k->gc));
            }
        }
    }
    js_gc_unprotect(ctx->vm, out);
    return ok;
}

/* Object.assign(target, ...sources): ToObject(target), then for each source's own enumerable keys,
 * [[Get]] from the source and [[Set]] on the target, as V8: a read-only or not-extensible target
 * refuses at the first write it cannot make. */
static bool obj_assign(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue target = ARG(0);
    if (js_is_undefined(target) || js_is_null(target))
        return native_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
    if (!js_is_object(target) && !js_has_cell_props(target))
        return native_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
    JsValue keys = js_undefined(), key = js_undefined(), val = js_undefined();
    js_gc_protect(ctx->vm, &target);
    js_gc_protect(ctx->vm, &keys);
    js_gc_protect(ctx->vm, &key);
    js_gc_protect(ctx->vm, &val);
    bool ok = true;
    for (int i = 1; i < argc && ok; i++) {
        JsValue src = args[i];
        if (js_is_undefined(src) || js_is_null(src))
            continue;
        if (!assign_source_keys(ctx, src, &keys)) {
            ok = oom(ctx, r);
            break;
        }
        JsObject *ko = js_value_object(keys);
        for (uint32_t k = 0; ok && k < ko->elem_count; k++) {
            key = ko->elems[k];
            if (!js_get_own_property(ctx, src, key, &val)) { /* a namespace's uninitialized export throws */
                ok = oom(ctx, r);
                break;
            }
            if (!js_set_property(ctx, target, key, val))
                ok = oom(ctx, r);
        }
    }
    js_gc_unprotect(ctx->vm, &val);
    js_gc_unprotect(ctx->vm, &key);
    js_gc_unprotect(ctx->vm, &keys);
    js_gc_unprotect(ctx->vm, &target);
    if (ok)
        *r = target;
    return ok;
}

/*
 * Per spec these are no-ops on a primitive rather than an error, and both
 * return their argument so `const o = Object.freeze({...})` reads naturally.
 * Idempotent: freezing a frozen object, or a sealed one, just re-asserts it.
 */
static bool obj_set_integrity(JsContext *ctx, const JsValue *args, int argc,
                              JsValue *r, uint8_t flags) {
    (void)argc;
    JsValue v = ARG(0);
    if (js_is_object(v)) {
        JsObject *o = js_value_object(v);
        /* a namespace's exports stay writable by their module: V8 refuses to freeze one */
        if ((o->obj_flags & JS_OBJ_NAMESPACE) && (flags & JS_OBJ_FROZEN) && js_object_size(v) > 0)
            return native_throw_fmt(ctx, r, "TypeError: Cannot redefine property: %v", js_object_key_at(v, 0));
        o->obj_flags |= flags;
        if (o == ctx->globals && (flags & JS_OBJ_FROZEN)) /* global assignment reads the bytes */
            for (uint32_t i = 0; i < o->props.used; i++)
                JS_MAP_ATTRS(&o->props)[i] |= JS_ATTR_READONLY | JS_ATTR_FIXED;
    }
    if (js_has_cell_props(v)) { /* freezes its own properties (made now, so it sticks) */
        JsObject *own = js_cell_props(ctx, v, true);
        if (!own)
            return oom(ctx, r);
        own->obj_flags |= flags;
    }
    *r = v;
    return true;
}

static bool obj_freeze(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return obj_set_integrity(ctx, args, argc, r, JS_OBJ_SEALED | JS_OBJ_FROZEN);
}

static bool obj_seal(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return obj_set_integrity(ctx, args, argc, r, JS_OBJ_SEALED);
}

static bool obj_getOwnPropertyNames(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                                    JsValue *r);

/* TestIntegrityLevel: a non-object is vacuously frozen and sealed; otherwise it must not be
 * extensible, and every own property must be not configurable (and, frozen, read-only). The
 * flags freeze and seal set answer at once; anything else is looked at property by property. */
static bool test_integrity(JsContext *ctx, const JsValue *args, int argc, JsValue *r, bool frozen) {
    JsValue v = ARG(0);
    if (!is_object_type_arg(v)) {
        *r = js_bool(true);
        return true;
    }
    JsObject *o = js_is_object(v) ? js_value_object(v) : js_cell_props(ctx, v, false);
    if (o && (o->obj_flags & (frozen ? JS_OBJ_FROZEN : JS_OBJ_SEALED))) {
        *r = js_bool(true);
        return true;
    }
    if (js_is_extensible(ctx, v)) {
        *r = js_bool(false);
        return true;
    }
    JsValue names, value = js_undefined();
    if (!obj_getOwnPropertyNames(ctx, js_undefined(), args, argc, &names)) {
        *r = names;
        return false;
    }
    js_gc_protect(ctx->vm, &names);
    js_gc_protect(ctx->vm, &value);
    bool ok = true, result = true;
    JsObject *no = js_value_object(names);
    for (uint32_t i = 0; result && i < no->elem_count; i++) {
        uint8_t attrs;
        int st = js_own_property(ctx, v, no->elems[i], &value, &attrs);
        if (st < 0) {
            ok = false;
            break;
        }
        /* (the __proto__ accessor, st 2, has no [[Writable]] to ask about) */
        if (st > 0 && (!(attrs & JS_ATTR_FIXED) || (frozen && st != 2 && !(attrs & JS_ATTR_READONLY))))
            result = false;
    }
    js_gc_unprotect(ctx->vm, &value);
    js_gc_unprotect(ctx->vm, &names);
    if (!ok)
        return oom(ctx, r);
    *r = js_bool(result);
    return true;
}

static bool obj_isFrozen(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return test_integrity(ctx, args, argc, r, true);
}

static bool obj_isSealed(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return test_integrity(ctx, args, argc, r, false);
}

static bool obj_fromEntries(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsVm *vm = ctx->vm;
    JsValue src = ARG(0);
    if (js_is_undefined(src) || js_is_null(src)) /* V8 says undefined for null too */
        return native_throw_fmt(ctx, r, "TypeError: undefined is not iterable");
    JsIterCursor cur;
    int open = js_iter_open(ctx, src, &cur);
    if (open <= 0)
        return open < 0 ? oom(ctx, r) : (*r = js_iter_not_iterable(ctx, src), false);
    js_gc_protect(vm, &cur.src);
    JsValue outv = js_object_new(ctx);
    js_gc_protect(vm, &outv);
    JsValue item = js_undefined(), k = js_undefined(), val = js_undefined();
    js_gc_protect(vm, &item);
    js_gc_protect(vm, &k);
    js_gc_protect(vm, &val);
    bool ok = js_is_object(outv), thrown = false;
    int rc;
    while (ok && (rc = js_iter_next(ctx, &cur, &item)) != 0) {
        if (rc < 0 || !js_iter_entry(ctx, item, &k, &val, &item)) {
            *r = item;
            ok = false;
            thrown = true;
            break;
        }
        /* ToPropertyKey may run k's toString, which may run anything; val stays rooted */
        JsString *ks = js_op_to_string(ctx, k);
        if (!ks && !js_is_undefined(ctx->pending_error)) {
            *r = js_take_error(ctx);
            ok = false;
            thrown = true;
            break;
        }
        JsString *iks = ks ? js_intern_cell(vm, ks) : NULL;
        ok = iks && js_map_set(vm, &js_value_object(outv)->props, iks, val);
    }
    if (thrown) {
        /* an abrupt completion closes the iterator (unless it was its own next that threw) */
        js_gc_protect(vm, r);
        JsValue ignored;
        (void)js_iter_close(ctx, &cur, true, false, &ignored);
        js_gc_unprotect(vm, r);
    }
    js_gc_unprotect(vm, &val);
    js_gc_unprotect(vm, &k);
    js_gc_unprotect(vm, &item);
    js_gc_unprotect(vm, &outv);
    js_gc_unprotect(vm, &cur.src);
    if (thrown)
        return false;
    if (!ok)
        return oom(ctx, r);
    *r = outv;
    return true;
}

static bool str_is_ascii(JsValue key, const char *a) {
    if (!js_is_string(key))
        return false;
    const JsString *s = js_value_string(key);
    size_t n = strlen(a);
    if (s->length != n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (s->units[i] != (uint16_t)(unsigned char)a[i])
            return false;
    return true;
}

/* Whether S is an array index below LEN in canonical form: "1", not "01", "1.0" or "+1" */
static bool is_index_below(const JsString *s, uint32_t len) {
    if (s->length == 0 || s->length > 10 || (s->length > 1 && s->units[0] == '0'))
        return false;
    uint64_t n = 0;
    for (uint32_t i = 0; i < s->length; i++) {
        uint16_t c = s->units[i];
        if (c < '0' || c > '9')
            return false;
        n = n * 10 + (uint64_t)(c - '0');
    }
    return n < len;
}

/* Whether V has the own property KS (a property key, rooted), for Object.hasOwn and
 * hasOwnProperty: exactly the keys Object.getOwnPropertyNames lists. V is neither undefined nor
 * null. *out is only meaningful when this returns true; false leaves an error pending (an
 * uninitialized export of a module namespace) or means OOM. */
static bool value_has_own(JsContext *ctx, JsValue v, JsString *ks, bool *out) {
    JsValue kv = js_value_from_cell(&ks->gc);
    JsObject *maps[2] = {NULL, NULL};
    *out = false;
    if (js_is_string(v)) {
        *out = is_index_below(ks, js_value_string(v)->length) || str_is_ascii(kv, "length");
        return true;
    }
    if (js_is_function(v)) {
        if ((str_is_ascii(kv, "name") && js_function_has_own_special(v, true)) ||
            (str_is_ascii(kv, "length") && js_function_has_own_special(v, false)) ||
            (str_is_ascii(kv, "prototype") && js_function_has_own_prototype(v))) {
            *out = true;
            return true;
        }
        JsGcCell *c = js_value_cell(v);
        maps[0] = js_cell_props(ctx, v, false);
        if (c->kind == JS_KIND_NATIVE)
            maps[1] = ((JsNative *)c)->statics;
    } else if (js_is_promise(v)) {
        maps[0] = js_cell_props(ctx, v, false);
    } else if (js_is_object(v)) {
        JsObject *o = js_value_object(v);
        bool special = o->obj_kind == JS_OBJ_ARRAY &&
                       (is_index_below(ks, o->elem_count) || str_is_ascii(kv, "length"));
#ifdef LAMASSU_HAS_REGEX
        special = special || (o->obj_kind == JS_OBJ_REGEXP && str_is_ascii(kv, "lastIndex"));
#endif
        if (special) {
            *out = true;
            return true;
        }
        maps[0] = o;
    }
    if (!maps[0] && !maps[1])
        return true; /* a number, boolean or promise: none */
    JsString *ik = js_intern_cell(ctx->vm, ks);
    if (!ik)
        return false;
    for (int i = 0; i < 2 && !*out; i++) {
        if (!maps[i])
            continue;
        JsValue val = js_map_get(&maps[i]->props, ik, out);
        if (*out && val.bits == JS_SPECIAL_TDZ) { /* a namespace's uninitialized export: V8 reads it */
            js_set_error(ctx, js_error_fmt(ctx, "ReferenceError: %S is not defined", ik));
            return false;
        }
    }
    return true;
}

/* Object.hasOwn(o, key): ToObject(o), then ToPropertyKey(key), as V8 */
static bool obj_hasOwn(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue o = ARG(0);
    if (js_is_undefined(o) || js_is_null(o))
        return native_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
    JsString *ks = js_native_string(ctx, ARG(1)); /* kept: interning it allocates */
    bool found;
    if (!ks || !value_has_own(ctx, o, ks, &found))
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

/* o.hasOwnProperty(key): ToPropertyKey(key) first, then ToObject(this), as V8 */
static bool objp_hasOwnProperty(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                                JsValue *r) {
    JsString *ks = js_native_string(ctx, ARG(0)); /* kept: interning it allocates */
    if (!ks)
        return oom(ctx, r);
    if (js_is_undefined(tv) || js_is_null(tv))
        return native_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
    bool found;
    if (!value_has_own(ctx, tv, ks, &found))
        return oom(ctx, r);
    *r = js_bool(found);
    return true;
}

/* o.propertyIsEnumerable(key): ToPropertyKey(key), then ToObject(this); an own property that
 * enumeration lists (a string's or array's index, not its length; not a function's name, length
 * or prototype, an error's stack, a regexp's lastIndex, or a builtin's hidden properties) */
static bool objp_propertyIsEnumerable(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                                      JsValue *r) {
    JsString *ks = js_native_string(ctx, ARG(0));
    if (!ks)
        return oom(ctx, r);
    if (js_is_undefined(tv) || js_is_null(tv))
        return native_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
    bool found;
    if (!value_has_own(ctx, tv, ks, &found))
        return oom(ctx, r);
    if (found) {
        JsValue kv = js_value_from_cell(&ks->gc);
        if (js_is_string(tv) || (js_is_object(tv) && js_value_object(tv)->obj_kind == JS_OBJ_ARRAY)) {
            found = !str_is_ascii(kv, "length");
        } else if (js_is_function(tv) && (str_is_ascii(kv, "name") || str_is_ascii(kv, "length") ||
                                          str_is_ascii(kv, "prototype"))) {
            found = false;
#ifdef LAMASSU_HAS_REGEX
        } else if (js_is_object(tv) && js_value_object(tv)->obj_kind == JS_OBJ_REGEXP &&
                   str_is_ascii(kv, "lastIndex")) {
            found = false;
#endif
        } else {
            JsString *ik = js_intern_cell(ctx->vm, ks);
            if (!ik)
                return oom(ctx, r);
            JsObject *maps[2] = {NULL, NULL};
            if (js_is_function(tv) || js_is_promise(tv)) {
                maps[0] = js_cell_props(ctx, tv, false);
                if (js_is_function(tv) && js_value_cell(tv)->kind == JS_KIND_NATIVE)
                    maps[1] = ((JsNative *)js_value_cell(tv))->statics;
            } else if (js_is_object(tv)) {
                maps[0] = js_value_object(tv);
            }
            found = false;
            for (int i = 0; i < 2; i++) {
                bool has = false;
                if (maps[i])
                    (void)js_map_get(&maps[i]->props, ik, &has);
                if (has) { /* a builtin's statics are not enumerable */
                    found = i == 0 && !(js_map_get_attrs(&maps[i]->props, ik) & JS_ATTR_HIDDEN);
                    break;
                }
            }
        }
    }
    *r = js_bool(found);
    return true;
}

/* o.isPrototypeOf(v): false for a primitive v (before this is checked, as V8), else whether this
 * is on v's [[Prototype]] chain */
static bool objp_isPrototypeOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsValue v = ARG(0);
    *r = js_bool(false);
    if (!js_is_object(v) && !js_is_function(v) && !js_is_promise(v))
        return true;
    if (js_is_undefined(tv) || js_is_null(tv))
        return native_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
    for (JsValue p = js_get_proto(ctx, v); !js_is_undefined(p) && !js_is_null(p); p = js_get_proto(ctx, p)) {
        if (p.bits == tv.bits) {
            *r = js_bool(true);
            break;
        }
    }
    return true;
}

/* Object.groupBy's ADD: the group array under the property key KEY (a string) */
static bool group_add_prop(JsContext *ctx, JsValue groups, JsValue key, JsValue value) {
    JsVm *vm = ctx->vm;
    JsObject *o = js_value_object(groups);
    JsString *ik = js_intern_cell(vm, js_value_string(key)); /* key is rooted by the caller */
    if (!ik)
        return false;
    bool found;
    JsValue arr = js_map_get(&o->props, ik, &found);
    if (!found) {
        js_gc_protect(vm, &value);
        JsObject *a = js_array_new_cell(ctx, 1);
        js_gc_unprotect(vm, &value);
        ik = a ? js_intern_cell(vm, js_value_string(key)) : NULL; /* the interned cell may have gone */
        if (!a || !ik)
            return false;
        arr = js_value_from_cell(&a->gc);
        if (!js_map_set(vm, &o->props, ik, arr))
            return false;
    }
    return js_array_append(vm, js_value_object(arr), value);
}

/* Object.groupBy(items, callback): a prototype-less object of arrays, one per key */
static bool obj_groupBy(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    if (js_is_undefined(ARG(0)) || js_is_null(ARG(0)))
        return native_throw(ctx, r, "TypeError: Object.groupBy called on null or undefined");
    if (!js_is_function(ARG(1)))
        return native_throw_fmt(ctx, r, "TypeError: %v is not a function", ARG(1));
    JsValue groups = js_object_new(ctx);
    if (!js_is_object(groups))
        return oom(ctx, r);
    js_value_object(groups)->proto = js_undefined();
    js_gc_protect(ctx->vm, &groups);
    bool ok = js_group_by(ctx, ARG(0), ARG(1), true, groups, group_add_prop, r);
    js_gc_unprotect(ctx->vm, &groups);
    if (ok)
        *r = groups;
    return ok;
}

/* Object.is(a, b): SameValue (NaN is NaN; 0 is not -0) */
static bool obj_is(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)ctx;
    (void)tv;
    JsValue x = ARG(0), y = ARG(1);
    if (js_is_number(x) && js_is_number(y)) {
        double dx = js_get_number(x), dy = js_get_number(y);
        *r = js_bool(dx != dx ? dy != dy : (dx == dy && __builtin_signbit(dx) == __builtin_signbit(dy)));
    } else {
        *r = js_bool(js_strict_equals(x, y));
    }
    return true;
}

/* The tag V8 keeps as an own Symbol.toStringTag on a few builtin objects, which whatever inherits
 * from them reports too (there are no symbols here, so these are the only ones) */
static const char *inherited_tag(const JsContext *ctx, JsValue p) {
    if (!js_is_object(p))
        return NULL;
    const JsObject *o = js_value_object(p);
    return o == ctx->async_function_proto ? "AsyncFunction"
           : o == ctx->promise_methods    ? "Promise"
           : o == ctx->math_obj           ? "Math"
           : o == ctx->json_obj           ? "JSON"
           : o == ctx->map_proto          ? "Map"
           : o == ctx->set_proto          ? "Set"
           : o == ctx->bigint_methods     ? "BigInt"
           : o == ctx->weakmap_proto      ? "WeakMap"
           : o == ctx->weakset_proto      ? "WeakSet"
           : o == ctx->weakref_proto      ? "WeakRef"
           : o == ctx->finreg_proto       ? "FinalizationRegistry"
           : o == ctx->iter_protos[JS_ITER_PROTO_MAP]    ? "Map Iterator"
           : o == ctx->iter_protos[JS_ITER_PROTO_SET]    ? "Set Iterator"
           : o == ctx->iter_protos[JS_ITER_PROTO_REGEXP] ? "RegExp String Iterator"
           : o == ctx->iter_protos[JS_ITER_PROTO_ARRAY]  ? "Array Iterator"
           : o == ctx->iter_protos[JS_ITER_PROTO_STRING] ? "String Iterator"
           : o == ctx->iter_protos[JS_ITER_PROTO_HELPER] ? "Iterator Helper"
           : o == ctx->iterator_proto                    ? "Iterator" : NULL;
}

/* The prototypes that are an Array, a String, a Number and a Boolean in V8 (Array.prototype is
 * an array there, the others wrapper objects), and the global object */
static const char *exotic_proto_tag(const JsContext *ctx, const JsObject *o) {
    return o == ctx->array_proto        ? "Array"
           : o == ctx->string_methods   ? "String"
           : o == ctx->number_methods   ? "Number"
           : o == ctx->boolean_methods  ? "Boolean"
           : o == ctx->globals          ? "global" : NULL;
}

const char *js_to_string_tag(const JsContext *ctx, JsValue v) {
    if (js_is_object(v) || js_is_function(v) || js_is_promise(v)) {
        /* Get(O, @@toStringTag) first: it overrides the kind's own tag */
        int depth = 0;
        for (JsValue p = v; depth < 1000 && (js_is_object(p) || js_is_function(p) || js_is_promise(p));
             p = js_get_proto((JsContext *)ctx, p), depth++) {
            const char *t = inherited_tag(ctx, p);
            if (t)
                return t;
        }
    }
    /* builtinTag (20.1.3.6): what is no array, function, error, boolean, number, string, date
     * or regular expression is an Object, a Map or a promise cut off its prototype included */
    if (js_is_undefined(v))
        return "Undefined";
    if (js_is_null(v))
        return "Null";
    if (js_is_number(v))
        return "Number";
    if (js_is_string(v))
        return "String";
    if (js_is_bool(v))
        return "Boolean";
    if (js_is_bigint(v))
        return "BigInt";
    if (js_is_function(v))
        return "Function";
    if (!js_is_object(v))
        return "Object";
    const JsObject *o = js_value_object(v);
    switch (o->obj_kind) {
    case JS_OBJ_ARRAY: return "Array";
    case JS_OBJ_ERROR: return "Error";
    case JS_OBJ_DATE: return "Date";
#ifdef LAMASSU_HAS_REGEX
    case JS_OBJ_REGEXP: return "RegExp";
#endif
    default: break;
    }
    if (o->obj_flags & JS_OBJ_NAMESPACE)
        return "Module";
    const char *t = exotic_proto_tag(ctx, o);
    return t ? t : "Object";
}

/* The tag that names V in a message (V8's NoSideEffectsToString): a Symbol.toStringTag found as
 * a data property (Iterator.prototype's is an accessor, so the search ends there), else V8's
 * class name of the object, which a Map, a Set and an Array, Map or Set Iterator keep whatever
 * they inherit from */
const char *js_message_tag(const JsContext *ctx, JsValue v) {
    int depth = 0;
    for (JsValue p = v; depth < 1000 && (js_is_object(p) || js_is_function(p) || js_is_promise(p));
         p = js_get_proto((JsContext *)ctx, p), depth++) {
        if (js_is_object(p) && js_value_object(p) == ctx->iterator_proto)
            break;
        const char *t = inherited_tag(ctx, p);
        if (t)
            return t;
    }
    if (js_is_function(v))
        return "Function";
    if (!js_is_object(v))
        return "Object";
    const JsObject *o = js_value_object(v);
    switch (o->obj_kind) {
    case JS_OBJ_ARRAY: return "Array";
    case JS_OBJ_ERROR: return "Error";
    case JS_OBJ_DATE: return "Date";
    case JS_OBJ_MAP: return "Map";
    case JS_OBJ_SET: return "Set";
    case JS_OBJ_WEAKMAP: return "WeakMap";
    case JS_OBJ_WEAKSET: return "WeakSet";
#ifdef LAMASSU_HAS_REGEX
    case JS_OBJ_REGEXP: return "RegExp";
#endif
    case JS_OBJ_ITER:
        switch (((const JsIterObj *)o)->kind) {
        case JS_ITER_ARRAY_KEYS:
        case JS_ITER_ARRAY_VALUES:
        case JS_ITER_ARRAY_ENTRIES: return "Array Iterator";
        case JS_ITER_MAP_KEYS:
        case JS_ITER_MAP_VALUES:
        case JS_ITER_MAP_ENTRIES: return "Map Iterator";
        case JS_ITER_SET_VALUES:
        case JS_ITER_SET_ENTRIES: return "Set Iterator";
        default: return "Object";
        }
    default: break;
    }
    if (o->obj_flags & JS_OBJ_NAMESPACE)
        return "Module";
    const char *t = exotic_proto_tag(ctx, o);
    return t ? t : "Object";
}

JsObject *js_iterable_to_array(JsContext *ctx, JsValue v, JsValue *err) {
    JsVm *vm = ctx->vm;
    JsIterCursor cur;
    int open = js_iter_open(ctx, v, &cur);
    if (open <= 0) {
        *err = open < 0 ? js_oom_value(vm) : js_iter_not_iterable(ctx, v);
        return NULL;
    }
    js_gc_protect(vm, &cur.src);
    JsObject *a = js_array_new_cell(ctx, 0);
    JsValue av = a ? js_value_from_cell(&a->gc) : js_undefined();
    js_gc_protect(vm, &av);
    bool ok = a != NULL;
    if (!ok)
        *err = js_oom_value(vm);
    JsValue item;
    int rc;
    while (ok && (rc = js_iter_next(ctx, &cur, &item)) != 0) {
        if (rc < 0) {
            *err = item;
            ok = false;
        } else if (!js_array_append(vm, a, item)) {
            *err = js_oom_value(vm);
            ok = false;
        }
    }
    js_gc_unprotect(vm, &av);
    js_gc_unprotect(vm, &cur.src);
    return ok ? a : NULL;
}

/* Object.prototype.toString: "[object Tag]", V8's tag for the receiver */
static bool objp_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    char buf[32];
    snprintf(buf, sizeof buf, "[object %s]", js_to_string_tag(ctx, tv));
    JsString *s = js_ascii_cell(ctx->vm, buf);
    if (!s)
        return oom(ctx, r);
    *r = js_value_from_cell(&s->gc);
    return true;
}

bool js_is_object_to_string(JsValue v) {
    if (!js_is_function(v) || js_value_cell(v)->kind != JS_KIND_NATIVE)
        return false;
    const JsNative *nf = (const JsNative *)js_value_cell(v);
    return !nf->is_bound && nf->fn == objp_toString;
}

/* Object.prototype.valueOf: ToObject(this), which for a primitive is the primitive itself here
 * (there are no wrapper objects) */
static bool objp_valueOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    if (js_is_undefined(tv) || js_is_null(tv))
        return native_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
    *r = tv;
    return true;
}

/* For ToPrimitive, which skips calling it (it gives the object back) */
bool js_native_is_object_valueof(JsValue fn) {
    if (!js_is_function(fn) || js_value_cell(fn)->kind != JS_KIND_NATIVE)
        return false;
    const JsNative *nf = (const JsNative *)js_value_cell(fn);
    return !nf->is_bound && nf->fn == objp_valueOf;
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
        return native_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
    /* an object's, function's or promise's [[Prototype]] (a function's is Function.prototype, or
     * the constructor it inherits from, until a script sets another); a number's, string's and
     * boolean's their constructor's prototype */
    *r = js_get_proto(ctx, v);
    return true;
}

static inline bool is_object_type_arg(JsValue v) {
    return js_is_object(v) || js_is_function(v) || js_is_promise(v);
}

static bool define_properties(JsContext *ctx, JsValue o, JsValue props, JsValue *r);

/* Object.create(proto, props): a new plain object whose prototype is PROTO (an object, function or
 * promise) or none (null), with the properties PROPS describes, as Object.defineProperties */
static bool obj_create(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue proto = ARG(0);
    if (!is_object_type_arg(proto) && !js_is_null(proto))
        return native_throw_fmt(ctx, r, "TypeError: Object prototype may only be an Object or null: %v", proto);
    JsValue o = js_object_new(ctx); /* proto is the caller's argument: rooted */
    if (!js_is_object(o))
        return oom(ctx, r);
    js_object_set_proto(js_value_object(o), is_object_type_arg(proto) ? proto : js_undefined());
    if (!js_is_undefined(ARG(1))) {
        js_gc_protect(ctx->vm, &o);
        bool ok = define_properties(ctx, o, ARG(1), r);
        js_gc_unprotect(ctx->vm, &o);
        if (!ok)
            return false;
    }
    *r = o;
    return true;
}

/* ToPropertyDescriptor(DV) into *D (D->value rooted by the caller): false with the error in *R.
 * Fields are looked up as V8 does, in order (enumerable, configurable, value, writable, get,
 * set), each a [[HasProperty]] then a [[Get]]. A script cannot write an accessor, so a get or set
 * that is not one of the engine's own accessor functions is refused once the descriptor is
 * otherwise valid. */
static bool to_property_descriptor(JsContext *ctx, JsValue dv, JsPropDesc *d, JsValue *r) {
    static const char *const names[] = {"enumerable", "configurable", "value", "writable", "get", "set"};
    if (!is_object_type_arg(dv))
        return native_throw_fmt(ctx, r, "TypeError: Property description must be an object: %v", dv);
    d->has = 0;
    bool accessor = false;
    JsValue getter = js_undefined(), setter = js_undefined();
    for (int i = 0; i < 6; i++) {
        JsString *ks = js_ascii_cell(ctx->vm, names[i]);
        JsValue kv = ks ? js_value_from_cell(&ks->gc) : js_undefined();
        bool has = false;
        JsValue fv = js_undefined();
        js_gc_protect(ctx->vm, &kv);
        bool ok = ks && js_has_property(ctx, dv, kv, &has) && (!has || js_get_property(ctx, dv, kv, &fv));
        js_gc_unprotect(ctx->vm, &kv);
        if (!ok)
            return oom(ctx, r);
        if (!has)
            continue;
        switch (i) {
        case 0: d->has |= JS_DESC_ENUMERABLE; d->enumerable = js_to_boolean(fv); break;
        case 1: d->has |= JS_DESC_CONFIGURABLE; d->configurable = js_to_boolean(fv); break;
        case 2: d->has |= JS_DESC_VALUE; d->value = fv; break;
        case 3: d->has |= JS_DESC_WRITABLE; d->writable = js_to_boolean(fv); break;
        default:
            if (!js_is_undefined(fv) && !js_is_function(fv))
                return native_throw_fmt(ctx, r, i == 4 ? "TypeError: Getter must be a function: %v"
                                                       : "TypeError: Setter must be a function: %v", fv);
            accessor = true;
            d->has |= i == 4 ? JS_DESC_GET : JS_DESC_SET;
            *(i == 4 ? &getter : &setter) = fv; /* the engine's own, if they pass: not collectable */
            break;
        }
    }
    if (accessor && (d->has & (JS_DESC_VALUE | JS_DESC_WRITABLE)))
        return native_throw_fmt(ctx, r, "TypeError: Invalid property descriptor. Cannot both specify accessors and a value or writable attribute, %v", dv);
    if (accessor) {
        /* only the engine's own accessor functions can be a property's getter or setter (a
         * descriptor Object.getOwnPropertyDescriptor gave, restated or taken apart): the marker
         * names the pair, 0 for the one that is undefined or not named */
        unsigned g = js_accessor_id(ctx, getter), st = js_accessor_id(ctx, setter);
        if ((!js_is_undefined(getter) && !g) || (!js_is_undefined(setter) && !st))
            return native_throw(ctx, r, "TypeError: A script function as a getter or setter is not supported");
        d->value = js_accessor(g, st);
    }
    return true;
}

/* Object.defineProperty(o, key, descriptor) */
static bool obj_defineProperty(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue o = ARG(0);
    if (!is_object_type_arg(o))
        return native_throw(ctx, r, "TypeError: Object.defineProperty called on non-object");
    JsString *ks = js_native_string(ctx, ARG(1)); /* ToPropertyKey, kept rooted */
    if (!ks)
        return oom(ctx, r);
    JsPropDesc d = {0};
    d.value = js_undefined();
    js_gc_protect(ctx->vm, &d.value);
    bool ok = to_property_descriptor(ctx, ARG(2), &d, r) &&
              (js_define_own_property(ctx, o, js_value_from_cell(&ks->gc), &d) || oom(ctx, r));
    js_gc_unprotect(ctx->vm, &d.value);
    if (ok)
        *r = o;
    return ok;
}

/* ObjectDefineProperties(o, props): every descriptor is read before any property is defined */
static bool define_properties(JsContext *ctx, JsValue o, JsValue props, JsValue *r) {
    if (js_is_undefined(props) || js_is_null(props))
        return native_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
    JsValue keys = js_undefined(), descs = js_undefined();
    js_gc_protect(ctx->vm, &keys);
    js_gc_protect(ctx->vm, &descs);
    JsObject *dl = js_array_new_cell(ctx, 0);
    bool ok = dl != NULL;
    if (ok) {
        descs = js_value_from_cell(&dl->gc);
        ok = assign_source_keys(ctx, props, &keys);
    }
    if (!ok)
        oom(ctx, r);
    JsObject *ko = ok ? js_value_object(keys) : NULL;
    /* the descriptors: [value, has-and-flags] pairs, kept in DESCS (rooted) */
    for (uint32_t i = 0; ok && i < ko->elem_count; i++) {
        JsValue dv;
        JsPropDesc d = {0};
        d.value = js_undefined();
        if (!js_get_property(ctx, props, ko->elems[i], &dv)) {
            ok = oom(ctx, r);
            break;
        }
        js_gc_protect(ctx->vm, &dv);
        js_gc_protect(ctx->vm, &d.value);
        ok = to_property_descriptor(ctx, dv, &d, r);
        double bits = (double)(d.has | (d.writable ? 64 : 0) | (d.enumerable ? 128 : 0) | (d.configurable ? 256 : 0));
        if (ok && !(js_array_append(ctx->vm, dl, d.value) && js_array_append(ctx->vm, dl, js_number(bits))))
            ok = oom(ctx, r);
        js_gc_unprotect(ctx->vm, &d.value);
        js_gc_unprotect(ctx->vm, &dv);
    }
    for (uint32_t i = 0; ok && i < ko->elem_count; i++) {
        uint32_t bits = (uint32_t)js_get_number(dl->elems[2 * i + 1]);
        JsPropDesc d = {(uint8_t)(bits & 63), (bits & 64) != 0, (bits & 128) != 0, (bits & 256) != 0,
                        dl->elems[2 * i]};
        if (!js_define_own_property(ctx, o, ko->elems[i], &d))
            ok = oom(ctx, r);
    }
    js_gc_unprotect(ctx->vm, &descs);
    js_gc_unprotect(ctx->vm, &keys);
    return ok;
}

static bool obj_defineProperties(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue o = ARG(0);
    if (!is_object_type_arg(o))
        return native_throw(ctx, r, "TypeError: Object.defineProperties called on non-object");
    if (!define_properties(ctx, o, ARG(1), r))
        return false;
    *r = o;
    return true;
}

/* FromPropertyDescriptor: {value, writable, enumerable, configurable}, in V8's order; for an
 * accessor property (ACCESSOR: VALUE is its marker), {get, set, enumerable, configurable} with
 * its two functions, undefined for the one it has not */
static bool descriptor_object(JsContext *ctx, JsValue value, uint8_t attrs, bool accessor, JsValue *out) {
    JsValue o = js_object_new(ctx);
    if (!js_is_object(o))
        return false;
    js_gc_protect(ctx->vm, &o);
    bool ok;
    if (accessor) {
        JsValue f;
        ok = js_accessor_function(ctx, js_accessor_getter(value), &f) &&
             js_object_set_ascii(ctx, js_value_object(o), "get", f) &&
             js_accessor_function(ctx, js_accessor_setter(value), &f) &&
             js_object_set_ascii(ctx, js_value_object(o), "set", f);
    } else {
        ok = js_object_set_ascii(ctx, js_value_object(o), "value", value) &&
             js_object_set_ascii(ctx, js_value_object(o), "writable", js_bool(!(attrs & JS_ATTR_READONLY)));
    }
    ok = ok && js_object_set_ascii(ctx, js_value_object(o), "enumerable", js_bool(!(attrs & JS_ATTR_HIDDEN))) &&
         js_object_set_ascii(ctx, js_value_object(o), "configurable", js_bool(!(attrs & JS_ATTR_FIXED)));
    js_gc_unprotect(ctx->vm, &o);
    *out = o;
    return ok;
}

/* Object.getOwnPropertyDescriptor(o, key): ToObject(o), ToPropertyKey(key) */
static bool obj_getOwnPropertyDescriptor(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue o = ARG(0);
    if (js_is_undefined(o) || js_is_null(o))
        return native_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
    JsString *ks = js_native_string(ctx, ARG(1));
    if (!ks)
        return oom(ctx, r);
    JsValue value;
    uint8_t attrs;
    int st = js_own_property(ctx, o, js_value_from_cell(&ks->gc), &value, &attrs);
    if (st < 0)
        return oom(ctx, r);
    if (st == 0) {
        *r = js_undefined();
        return true;
    }
    js_gc_protect(ctx->vm, &value);
    bool ok = descriptor_object(ctx, value, attrs, st == 2, r);
    js_gc_unprotect(ctx->vm, &value);
    return ok || oom(ctx, r);
}

static bool obj_getOwnPropertyNames(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                                    JsValue *r);

/* Object.getOwnPropertyDescriptors(o): every own property's descriptor, keyed as
 * Object.getOwnPropertyNames lists them */
static bool obj_getOwnPropertyDescriptors(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsValue o = ARG(0);
    JsValue names;
    if (!obj_getOwnPropertyNames(ctx, tv, args, argc, &names)) {
        *r = names;
        return false;
    }
    js_gc_protect(ctx->vm, &names);
    JsValue out = js_object_new(ctx), value = js_undefined(), dv = js_undefined();
    js_gc_protect(ctx->vm, &out);
    js_gc_protect(ctx->vm, &value);
    js_gc_protect(ctx->vm, &dv);
    bool ok = js_is_object(out);
    JsObject *no = js_value_object(names);
    for (uint32_t i = 0; ok && i < no->elem_count; i++) {
        uint8_t attrs;
        int st = js_own_property(ctx, o, no->elems[i], &value, &attrs);
        if (st < 0) {
            ok = false;
            break;
        }
        if (st == 0)
            continue;
        ok = descriptor_object(ctx, value, attrs, st == 2, &dv) &&
             js_map_set(ctx->vm, &js_value_object(out)->props, js_value_string(no->elems[i]), dv);
    }
    js_gc_unprotect(ctx->vm, &dv);
    js_gc_unprotect(ctx->vm, &value);
    js_gc_unprotect(ctx->vm, &out);
    js_gc_unprotect(ctx->vm, &names);
    if (!ok)
        return oom(ctx, r);
    *r = out;
    return true;
}

/* Object.preventExtensions(o): a primitive is returned as it is */
static bool obj_preventExtensions(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue v = ARG(0);
    if (is_object_type_arg(v) && !js_prevent_extensions(ctx, v))
        return oom(ctx, r);
    *r = v;
    return true;
}

static bool obj_isExtensible(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue v = ARG(0);
    *r = js_bool(is_object_type_arg(v) && js_is_extensible(ctx, v));
    return true;
}

/* [[SetPrototypeOf]] of V (an object, function or promise) for a builtin: false with V8's
 * TypeError in *R when it is refused */
static bool set_proto_or_throw(JsContext *ctx, JsValue v, JsValue proto, JsValue *r) {
    switch (js_set_proto(ctx, v, proto)) {
    case 0: return true;
    case 1: return native_throw_fmt(ctx, r, "TypeError: %v is not extensible", v);
    case 2: return native_throw(ctx, r, "TypeError: Cyclic __proto__ value");
    default:
        return native_throw(ctx, r, "TypeError: Immutable prototype object 'Object.prototype' cannot have their prototype set");
    }
}

/* Object.setPrototypeOf(v, proto): for an object, function or promise, as V8 (a sealed or frozen
 * one keeps its prototype; a chain may not loop); a primitive is returned as it is. */
static bool obj_setPrototypeOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                               JsValue *r) {
    (void)tv;
    JsValue v = ARG(0), proto = ARG(1);
    if (js_is_undefined(v) || js_is_null(v))
        return native_throw(ctx, r, "TypeError: Object.setPrototypeOf called on null or undefined");
    if (!is_object_type_arg(proto) && !js_is_null(proto))
        return native_throw_fmt(ctx, r, "TypeError: Object prototype may only be an Object or null: %v", proto);
    if (is_object_type_arg(v) && !set_proto_or_throw(ctx, v, proto, r))
        return false;
    *r = v;
    return true;
}

/* ---- the engine's accessor functions ----
 * Every accessor property a built-in has is an entry holding a marker (js_accessor) that names
 * its getter and setter by number; js_accessor_call is what runs one, for a property read or
 * write that found the marker on the receiver or up its prototype chain, and for the function a
 * descriptor shows (accessor_native), which is made the first time it is asked for. */

#define STRICT_FN_PROPS \
    "TypeError: 'caller', 'callee', and 'arguments' properties may not be accessed on strict mode functions or the arguments objects for calls to them"

bool js_accessor_call(JsContext *ctx, unsigned id, JsValue recv, JsValue arg, JsValue *out) {
    *out = js_undefined();
    switch (id) {
    case JS_ACC_PROTO_GET: /* get __proto__ (B.2.2.1.1): the [[Prototype]] of ToObject(this) */
        if (js_is_undefined(recv) || js_is_null(recv))
            return native_throw(ctx, out, "TypeError: Cannot convert undefined or null to object");
        *out = js_get_proto(ctx, recv);
        return true;
    case JS_ACC_PROTO_SET: /* set __proto__ (B.2.2.1.2) */
        if (js_is_undefined(recv) || js_is_null(recv))
            return native_throw(ctx, out, "TypeError: set Object.prototype.__proto__ called on null or undefined");
        if ((!is_object_type_arg(arg) && !js_is_null(arg)) || !is_object_type_arg(recv))
            return true;
        if (!set_proto_or_throw(ctx, recv, arg, out))
            return false;
        *out = js_undefined();
        return true;
    case JS_ACC_MAP_SIZE:
        if (!js_is_object(recv) || js_value_object(recv)->obj_kind != JS_OBJ_MAP)
            return native_throw_fmt(ctx, out, "TypeError: Method get Map.prototype.size called on incompatible receiver %v", recv);
        *out = js_number((double)((JsMapObj *)js_value_object(recv))->count);
        return true;
    case JS_ACC_SET_SIZE:
        if (!js_is_object(recv) || js_value_object(recv)->obj_kind != JS_OBJ_SET)
            return native_throw_fmt(ctx, out, "TypeError: Method get Set.prototype.size called on incompatible receiver %v", recv);
        *out = js_number((double)((JsSetObj *)js_value_object(recv))->count);
        return true;
    case JS_ACC_ITER_CTOR_GET:
    case JS_ACC_ITER_CTOR_SET:
        return js_iter_ctor_accessor(ctx, id == JS_ACC_ITER_CTOR_SET, recv, arg, out);
    case JS_ACC_STACK_GET:
    case JS_ACC_STACK_SET:
        return js_error_stack_accessor(ctx, id == JS_ACC_STACK_SET, recv, arg, out);
    case JS_ACC_FN_ARGUMENTS_GET:
    case JS_ACC_FN_ARGUMENTS_SET:
    case JS_ACC_FN_CALLER_GET:
    case JS_ACC_FN_CALLER_SET:
        return native_throw_fmt(ctx, out, STRICT_FN_PROPS);
    default:
#ifdef LAMASSU_HAS_REGEX
        if (id >= JS_ACC_RE_DOTALL && id < JS_ACC_ITER_CTOR_GET)
            return js_regexp_accessor(ctx, id, recv, arg, out);
#endif
        return true;
    }
}

/* The function a descriptor shows for an accessor: its number is its userdata */
static bool accessor_native(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    return js_accessor_call(ctx, (unsigned)(uintptr_t)js_native_userdata(ctx), tv, ARG(0), r);
}

static bool accessor_is_setter(unsigned id) {
    return id == JS_ACC_PROTO_SET || (id >= JS_ACC_RE_LEGACY_SET && id < JS_ACC_ITER_CTOR_GET) ||
           id == JS_ACC_ITER_CTOR_SET || id == JS_ACC_STACK_SET || id == JS_ACC_FN_ARGUMENTS_SET ||
           id == JS_ACC_FN_CALLER_SET;
}

/* An accessor function's name, as V8 has it: "get size", "set __proto__", and "" for the two of
 * an error's `stack`. BUF holds the ones that are put together. */
static const char *accessor_name(unsigned id, char buf[40]) {
    static const char *const re_getters[] = {"dotAll", "flags", "global", "hasIndices", "ignoreCase",
                                             "multiline", "source", "sticky", "unicode", "unicodeSets"};
    const char *prop;
    switch (id) {
    case JS_ACC_PROTO_GET: return "get __proto__";
    case JS_ACC_PROTO_SET: return "set __proto__";
    case JS_ACC_MAP_SIZE:
    case JS_ACC_SET_SIZE: return "get size";
    case JS_ACC_ITER_CTOR_GET: return "get constructor";
    case JS_ACC_ITER_CTOR_SET: return "set constructor";
    case JS_ACC_STACK_GET:
    case JS_ACC_STACK_SET: return "";
    case JS_ACC_FN_ARGUMENTS_GET: return "get arguments";
    case JS_ACC_FN_ARGUMENTS_SET: return "set arguments";
    case JS_ACC_FN_CALLER_GET: return "get caller";
    case JS_ACC_FN_CALLER_SET: return "set caller";
    default: break;
    }
    if (id >= JS_ACC_RE_DOTALL && id < JS_ACC_RE_LEGACY_GET)
        prop = re_getters[id - JS_ACC_RE_DOTALL];
#ifdef LAMASSU_HAS_REGEX
    else if (id >= JS_ACC_RE_LEGACY_GET && id < JS_ACC_ITER_CTOR_GET)
        prop = js_regexp_legacy_name((int)(id - JS_ACC_RE_LEGACY_GET) % JS_REGEXP_LEGACY_COUNT);
#endif
    else
        return "";
    snprintf(buf, 40, "%s %s", accessor_is_setter(id) ? "set" : "get", prop);
    return buf;
}

bool js_accessor_function(JsContext *ctx, unsigned id, JsValue *out) {
    *out = js_undefined();
    if (id == JS_ACC_NONE || id >= JS_ACC__COUNT)
        return true;
    if (!ctx->accessor_fns[id]) {
        char buf[40];
        JsValue f = js_native_new(ctx, accessor_name(id, buf), accessor_native, (void *)(uintptr_t)id);
        if (!js_is_function(f))
            return false;
        JsNative *nf = (JsNative *)js_value_cell(f);
        nf->length = accessor_is_setter(id) ? 1 : 0;
        nf->builtin = true;
        nf->ctor = JS_CTOR_NONE;
        ctx->accessor_fns[id] = nf;
    }
    *out = js_value_from_cell(&ctx->accessor_fns[id]->gc);
    return true;
}

unsigned js_accessor_id(JsContext *ctx, JsValue fn) {
    (void)ctx;
    if (!js_is_function(fn) || js_value_cell(fn)->kind != JS_KIND_NATIVE)
        return JS_ACC_NONE;
    JsNative *nf = (JsNative *)js_value_cell(fn);
    return nf->fn == accessor_native ? (unsigned)(uintptr_t)nf->ud : JS_ACC_NONE;
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

/* Array.from's and Array.of's result when `this` is a constructor other than Array:
 * `new this()`, or `new this(len)` with ARG, on which the elements are then defined
 * (create_data_property) and `length` set (from_set_length). 0: a plain array is the result, as
 * for any `this` that is no constructor; 1: *OUT is what it made; -1: thrown, *OUT the error. */
static int from_construct(JsContext *ctx, JsValue tv, const JsValue *arg, JsValue *out) {
    const JsNative *array = ctx->species_ctors[JS_SPECIES_ARRAY];
    if ((js_is_function(tv) && array && js_value_cell(tv) == &array->gc) || !js_is_constructor(tv))
        return 0;
    return js_construct(ctx, tv, arg, arg ? 1 : 0, out) ? 1 : -1;
}

/* Set(a, "length", n, true): A rooted by the caller. False thrown (*r the error). */
static bool from_set_length(JsContext *ctx, JsValue a, double n, JsValue *r) {
    JsString *lk = js_ascii_cell(ctx->vm, "length");
    if (!lk)
        return oom(ctx, r);
    JsValue key = js_value_from_cell(&lk->gc);
    js_gc_protect(ctx->vm, &key);
    bool ok = js_set_property(ctx, a, key, js_number(n));
    js_gc_unprotect(ctx->vm, &key);
    return ok || oom(ctx, r);
}

static bool arr_of(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsValue made = js_undefined(), count = js_number(argc);
    int sp = from_construct(ctx, tv, &count, &made);
    if (sp < 0)
        return (*r = made), false;
    if (sp > 0) {
        js_gc_protect(ctx->vm, &made);
        bool ok = true;
        for (int i = 0; ok && i < argc; i++)
            ok = create_data_property(ctx, made, js_number(i), args[i]) || oom(ctx, r);
        ok = ok && from_set_length(ctx, made, argc, r);
        js_gc_unprotect(ctx->vm, &made);
        if (ok)
            *r = made;
        return ok;
    }
    JsObject *out = js_array_new_cell(ctx, (uint32_t)argc);
    if (!out)
        return oom(ctx, r);
    for (int i = 0; i < argc; i++)
        out->elems[out->elem_count++] = args[i];
    *r = js_value_from_cell(&out->gc);
    return true;
}

/* keys(), values(), entries(): an Array Iterator over the array (or array-like), read live */
static bool am_keys(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    return js_array_iter_method(ctx, tv, JS_ITER_ARRAY_KEYS, r);
}

static bool am_values(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    return js_array_iter_method(ctx, tv, JS_ITER_ARRAY_VALUES, r);
}

static bool am_entries(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    return js_array_iter_method(ctx, tv, JS_ITER_ARRAY_ENTRIES, r);
}

/* Array.from(items, mapFn, thisArg): an iterable's values in order, else an array-like's
 * [0, length) (ToLength of its length), each through mapFn(value, index) when it is given. */
static bool arr_from(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsVm *vm = ctx->vm;
    JsValue src = ARG(0), mapfn = ARG(1), thisArg = ARG(2);
    bool has_map = !js_is_undefined(mapfn);
    if (has_map && !js_is_function(mapfn))
        return native_throw_fmt(ctx, r, "TypeError: %t is not a function", mapfn);
    JsIterCursor cur;
    int open = js_iter_open(ctx, src, &cur);
    if (open < 0)
        return oom(ctx, r);
    if (open == 0 && (js_is_undefined(src) || js_is_null(src)))
        return (*r = js_iter_not_iterable(ctx, src)), false;
    js_gc_protect(vm, &cur.src); /* a Map's or a record's new iterator is held nowhere else */
    JsValue outv = js_undefined(), item = js_undefined();
    js_gc_protect(vm, &outv);
    js_gc_protect(vm, &item);
    bool ok = true, thrown = false;
    /* what a constructor `this` made (from_construct): of an iterable `new this()`, before the
     * first value is asked for; of an array-like `new this(len)`, below */
    int made = open ? from_construct(ctx, tv, NULL, &outv) : 0;
    if (made < 0) {
        *r = outv;
        outv = js_undefined();
        ok = false;
        thrown = true;
        js_gc_protect(vm, r);
        JsValue ignored;
        (void)js_iter_close(ctx, &cur, true, false, &ignored);
        js_gc_unprotect(vm, r);
    }
    double len = 0;
    if (ok && open == 0) {
        /* an array-like: ToLength(items.length) */
        JsValue lv;
        JsString *lk = js_ascii_cell(vm, "length");
        if (!lk || !js_get_property(ctx, src, js_value_from_cell(&lk->gc), &lv) ||
            !js_op_to_number(ctx, lv, &len)) {
            *r = js_take_error(ctx);
            ok = false;
            thrown = true;
        }
        len = len != len || len <= 0 ? 0 : len > 9007199254740991.0 ? 9007199254740991.0 : (double)(uint64_t)len;
        if (ok) {
            JsValue lenv = js_number(len);
            made = from_construct(ctx, tv, &lenv, &outv);
            if (made < 0) {
                *r = outv;
                outv = js_undefined();
                ok = false;
                thrown = true;
            }
        }
        if (ok && !made && len > 4294967295.0) {
            *r = js_error_from_ascii(ctx, "RangeError: Invalid array length");
            ok = false;
            thrown = true;
        }
    }
    if (ok && !made) {
        JsObject *out = js_array_new_cell(ctx, 0);
        if (out)
            outv = js_value_from_cell(&out->gc);
        else
            ok = false;
    }
    double k = 0;
    for (; ok; k++) {
        if (open) {
            int rc = js_iter_next(ctx, &cur, &item);
            if (rc == 0)
                break;
            if (rc < 0) {
                *r = item;
                ok = false;
                thrown = true;
                break;
            }
        } else {
            if (k >= len)
                break;
            if (!js_get_property(ctx, src, js_number(k), &item)) {
                *r = js_take_error(ctx);
                ok = false;
                thrown = true;
                break;
            }
        }
        if (has_map) {
            JsValue argv2[2] = {item, js_number(k)};
            if (!js_call(ctx, mapfn, thisArg, argv2, 2, &item)) {
                *r = item;
                ok = false;
                thrown = true;
                if (open) { /* the mapper's throw closes the iterator */
                    js_gc_protect(vm, r);
                    JsValue ignored;
                    (void)js_iter_close(ctx, &cur, true, false, &ignored);
                    js_gc_unprotect(vm, r);
                }
                break;
            }
        }
        if (!made) {
            ok = js_array_append(vm, js_value_object(outv), item);
            continue;
        }
        if (!js_native_charge(ctx) || !create_data_property(ctx, outv, js_number(k), item)) {
            (void)oom(ctx, r);
            ok = false;
            thrown = true;
            if (open) { /* as the mapper's does */
                js_gc_protect(vm, r);
                JsValue ignored;
                (void)js_iter_close(ctx, &cur, true, false, &ignored);
                js_gc_unprotect(vm, r);
            }
        }
    }
    if (ok && made && !from_set_length(ctx, outv, k, r)) {
        ok = false;
        thrown = true;
    }
    js_gc_unprotect(vm, &item);
    js_gc_unprotect(vm, &outv);
    js_gc_unprotect(vm, &cur.src);
    if (thrown)
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
            return native_throw(ctx, r, "RangeError: Invalid array length");
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
    int radix = 0;
    if (argc > 1 && !js_is_undefined(ARG(1))) {
        double rd;
        TO_NUM(rd, ARG(1));
        radix = (int)js_to_int32(rd);
    }
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

/* ---- encodeURI / decodeURI and their component forms, as the spec's Encode and Decode ---- */

static bool uri_in(uint16_t c, const char *set) {
    return c < 128 && c && strchr(set, (char)c) != NULL;
}
#define URI_UNRESERVED "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.!~*'()"
#define URI_RESERVED ";/?:@&=+$,#"

static int hex_val(uint16_t c) {
    return c >= '0' && c <= '9' ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool uri_finish(JsContext *ctx, StrBuf *sb, JsValue *r) {
    if (!sb->oom && sb->len > JS_MAX_STRING_UNITS) {
        sb_free(sb);
        return native_throw(ctx, r, "RangeError: Invalid string length");
    }
    JsValue v = sb_finish(sb);
    if (js_is_undefined(v))
        return oom(ctx, r);
    *r = v;
    return true;
}

/* Encode: what is not in KEEP becomes %XX of its UTF-8; a lone surrogate is V8's URIError */
static bool uri_encode(JsContext *ctx, const JsValue *args, int argc, JsValue *r, bool component) {
    JsString *s = arg_string(ctx, ARG(0));
    if (!s)
        return oom(ctx, r);
    static const char hex[] = "0123456789ABCDEF";
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    for (uint32_t i = 0; i < s->length; i++) {
        uint16_t c = s->units[i];
        if (uri_in(c, URI_UNRESERVED) || (!component && uri_in(c, URI_RESERVED))) {
            sb_unit(&sb, c);
            continue;
        }
        uint32_t cp = c;
        if (c >= 0xDC00 && c <= 0xDFFF) {
            sb_free(&sb);
            return native_throw(ctx, r, "URIError: URI malformed");
        }
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= s->length || s->units[i + 1] < 0xDC00 || s->units[i + 1] > 0xDFFF) {
                sb_free(&sb);
                return native_throw(ctx, r, "URIError: URI malformed");
            }
            cp = 0x10000 + (((uint32_t)c - 0xD800) << 10) + (s->units[++i] - 0xDC00);
        }
        uint8_t oct[4];
        int n;
        if (cp < 0x80) {
            oct[0] = (uint8_t)cp;
            n = 1;
        } else if (cp < 0x800) {
            oct[0] = (uint8_t)(0xC0 | cp >> 6);
            oct[1] = (uint8_t)(0x80 | (cp & 0x3F));
            n = 2;
        } else if (cp < 0x10000) {
            oct[0] = (uint8_t)(0xE0 | cp >> 12);
            oct[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
            oct[2] = (uint8_t)(0x80 | (cp & 0x3F));
            n = 3;
        } else {
            oct[0] = (uint8_t)(0xF0 | cp >> 18);
            oct[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
            oct[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
            oct[3] = (uint8_t)(0x80 | (cp & 0x3F));
            n = 4;
        }
        for (int k = 0; k < n; k++) {
            uint16_t esc[3] = {'%', (uint16_t)hex[oct[k] >> 4], (uint16_t)hex[oct[k] & 15]};
            sb_units(&sb, esc, 3);
        }
    }
    return uri_finish(ctx, &sb, r);
}

/* The byte %XX at I, or -1 */
static int uri_byte(const JsString *s, uint32_t i) {
    if (i + 2 >= s->length || s->units[i] != '%')
        return -1;
    int hi = hex_val(s->units[i + 1]), lo = hex_val(s->units[i + 2]);
    return hi < 0 || lo < 0 ? -1 : hi << 4 | lo;
}

/* Decode: %XX sequences of UTF-8 become their characters, except (decodeURI) an escaped
 * reserved character, which stays escaped; anything malformed is V8's URIError */
static bool uri_decode(JsContext *ctx, const JsValue *args, int argc, JsValue *r, bool component) {
    JsString *s = arg_string(ctx, ARG(0));
    if (!s)
        return oom(ctx, r);
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    for (uint32_t i = 0; i < s->length; i++) {
        uint16_t c = s->units[i];
        if (c != '%') {
            sb_unit(&sb, c);
            continue;
        }
        int b = uri_byte(s, i);
        if (b < 0)
            goto malformed;
        if (b < 0x80) {
            if (!component && uri_in((uint16_t)b, URI_RESERVED))
                sb_units(&sb, s->units + i, 3);
            else
                sb_unit(&sb, (uint16_t)b);
            i += 2;
            continue;
        }
        int n = (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
        if (!n)
            goto malformed;
        uint32_t cp = (uint32_t)b & (0xFF >> (n + 1));
        for (int k = 1; k < n; k++) {
            uint32_t at = i + 3 * (uint32_t)k;
            int cb = uri_byte(s, at);
            if (cb < 0 || (cb & 0xC0) != 0x80)
                goto malformed;
            cp = cp << 6 | ((uint32_t)cb & 0x3F);
        }
        static const uint32_t min_cp[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < min_cp[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            goto malformed;
        if (cp < 0x10000) {
            sb_unit(&sb, (uint16_t)cp);
        } else {
            cp -= 0x10000;
            sb_unit(&sb, (uint16_t)(0xD800 + (cp >> 10)));
            sb_unit(&sb, (uint16_t)(0xDC00 + (cp & 0x3FF)));
        }
        i += 3 * (uint32_t)n - 1;
    }
    return uri_finish(ctx, &sb, r);
malformed:
    sb_free(&sb);
    return native_throw(ctx, r, "URIError: URI malformed");
}

static bool g_encodeURI(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return uri_encode(ctx, args, argc, r, false);
}
static bool g_encodeURIComponent(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return uri_encode(ctx, args, argc, r, true);
}
static bool g_decodeURI(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return uri_decode(ctx, args, argc, r, false);
}
static bool g_decodeURIComponent(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return uri_decode(ctx, args, argc, r, true);
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
    double d;
    TO_NUM(d, ARG(0));
    *r = js_bool(d != d);
    return true;
}

static bool g_isFinite(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    (void)argc;
    double d;
    TO_NUM(d, ARG(0));
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
    JsString *s = js_op_to_string(ctx, ARG(0));
    if (!s)
        return oom(ctx, r);
    *r = js_value_from_cell(&s->gc);
    return true;
}

static bool g_Number(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    double d = 0;
    if (argc > 0) { /* ToNumeric: a BigInt converts, as Number() is the one place it may */
        JsValue p;
        if (!js_to_primitive(ctx, ARG(0), JS_HINT_NUMBER, &p))
            return oom(ctx, r);
        if (js_is_bigint(p))
            d = js_bigint_to_double(js_value_bigint(p));
        else
            TO_NUM(d, p);
    }
    *r = js_number(d);
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
    for (int i = 0; i < argc; i++) {
        double d;
        if (!js_op_to_number(ctx, args[i], &d)) {
            sb_free(&sb);
            return oom(ctx, r);
        }
        sb_unit(&sb, to_uint16(d));
    }
    *r = sb_finish(&sb);
    return js_is_undefined(*r) && argc ? oom(ctx, r) : true;
}

/* String.raw(template, ...subs): template.raw's pieces (read through its length) with the
 * substitutions between them */
static bool str_raw(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsVm *vm = ctx->vm;
    JsValue cooked = ARG(0), raw = js_undefined(), lenv, piece = js_undefined();
    if (js_is_undefined(cooked) || js_is_null(cooked))
        return native_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
    static const uint16_t kraw[] = {'r', 'a', 'w'}, klen[] = {'l', 'e', 'n', 'g', 't', 'h'};
    JsValue key = js_atom(vm, kraw, 3);
    if (!js_is_string(key) || !js_get_property(ctx, cooked, key, &raw))
        return oom(ctx, r);
    if (js_is_undefined(raw) || js_is_null(raw))
        return native_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
    js_gc_protect(vm, &raw);
    js_gc_protect(vm, &piece);
    double len = 0;
    key = js_atom(vm, klen, 6);
    bool ok = js_is_string(key) && js_get_property(ctx, raw, key, &lenv) && js_op_to_number(ctx, lenv, &len);
    StrBuf sb;
    sb_init(&sb, vm);
    len = to_int(len);
    for (double i = 0; ok && i < len; i++) {
        char ib[24];
        snprintf(ib, sizeof ib, "%.0f", i);
        JsString *ik = js_ascii_cell(vm, ib);
        ok = ik && js_get_property(ctx, raw, js_value_from_cell(&ik->gc), &piece);
        JsString *ps = ok ? js_op_to_string(ctx, piece) : NULL;
        if (!ps) {
            ok = false;
            break;
        }
        sb_units(&sb, ps->units, ps->length);
        if (i + 1 < len && i + 1 < argc) {
            JsString *sub = js_op_to_string(ctx, args[(int)i + 1]);
            if (!sub) {
                ok = false;
                break;
            }
            sb_units(&sb, sub->units, sub->length);
        }
    }
    js_gc_unprotect(vm, &piece);
    js_gc_unprotect(vm, &raw);
    if (!ok) {
        sb_free(&sb);
        return oom(ctx, r);
    }
    *r = sb_finish(&sb);
    return js_is_undefined(*r) ? oom(ctx, r) : true;
}

/* escape / unescape (Annex B): %XX for a unit below 256 outside A-Z a-z 0-9 @*_+-./, %uXXXX above */
static bool g_escape(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsString *s = arg_string(ctx, ARG(0));
    if (!s)
        return oom(ctx, r);
    static const char hex[] = "0123456789ABCDEF";
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    for (uint32_t i = 0; i < s->length; i++) {
        uint16_t c = s->units[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            (c < 128 && strchr("@*_+-./", c))) {
            sb_unit(&sb, c);
        } else if (c < 256) {
            sb_unit(&sb, '%');
            sb_unit(&sb, (uint16_t)hex[c >> 4]);
            sb_unit(&sb, (uint16_t)hex[c & 15]);
        } else {
            sb_unit(&sb, '%');
            sb_unit(&sb, 'u');
            for (int sh = 12; sh >= 0; sh -= 4)
                sb_unit(&sb, (uint16_t)hex[(c >> sh) & 15]);
        }
    }
    *r = sb_finish(&sb);
    return js_is_undefined(*r) ? oom(ctx, r) : true;
}

static int hexval(uint16_t c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool g_unescape(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsString *s = arg_string(ctx, ARG(0));
    if (!s)
        return oom(ctx, r);
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    for (uint32_t i = 0; i < s->length; i++) {
        uint16_t c = s->units[i];
        if (c == '%' && i + 5 < s->length && s->units[i + 1] == 'u' && hexval(s->units[i + 2]) >= 0 &&
            hexval(s->units[i + 3]) >= 0 && hexval(s->units[i + 4]) >= 0 && hexval(s->units[i + 5]) >= 0) {
            c = (uint16_t)(hexval(s->units[i + 2]) << 12 | hexval(s->units[i + 3]) << 8 |
                           hexval(s->units[i + 4]) << 4 | hexval(s->units[i + 5]));
            i += 5;
        } else if (c == '%' && i + 2 < s->length && hexval(s->units[i + 1]) >= 0 && hexval(s->units[i + 2]) >= 0) {
            c = (uint16_t)(hexval(s->units[i + 1]) << 4 | hexval(s->units[i + 2]));
            i += 2;
        }
        sb_unit(&sb, c);
    }
    *r = sb_finish(&sb);
    return js_is_undefined(*r) ? oom(ctx, r) : true;
}

static bool str_fromCodePoint(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    StrBuf sb;
    sb_init(&sb, ctx->vm);
    for (int i = 0; i < argc; i++) {
        double d;
        if (!js_op_to_number(ctx, args[i], &d)) {
            sb_free(&sb);
            return oom(ctx, r);
        }
        /* real RangeError, not a garbage cast, for an invalid code point —
         * String.fromCodePoint(-1) / (1e20) previously cast an out-of-range
         * double straight to uint32_t (UB) */
        if (!(d >= 0 && d <= 0x10FFFF) || d != __builtin_floor(d)) {
            sb_free(&sb);
            return native_throw_fmt(ctx, r, "RangeError: Invalid code point %v", ARG(i));
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
        double x;                                                              \
        TO_NUM(x, ARG(0));                                                     \
        *r = js_number(kernel(x));                                             \
        return true;                                                          \
    }
MATH1(math_abs, m_abs)
MATH1(math_floor, __builtin_floor)
MATH1(math_ceil, __builtin_ceil)
MATH1(math_trunc, __builtin_trunc)
MATH1(math_sign, m_sign)
MATH1(math_sqrt, __builtin_sqrt)
MATH1(math_fround, m_fround)
MATH1(math_cbrt, js_k_cbrt)
MATH1(math_exp, js_k_exp)
MATH1(math_log, js_k_log)
MATH1(math_sin, js_k_sin)
MATH1(math_cos, js_k_cos)
MATH1(math_tan, js_k_tan)
MATH1(math_asin, js_k_asin)
MATH1(math_acos, js_k_acos)
MATH1(math_atan, js_k_atan)
MATH1(math_log2, js_k_log2)
MATH1(math_log10, js_k_log10)
MATH1(math_expm1, js_k_expm1)
MATH1(math_log1p, js_k_log1p)
MATH1(math_sinh, js_k_sinh)
MATH1(math_cosh, js_k_cosh)
MATH1(math_tanh, js_k_tanh)
MATH1(math_asinh, js_k_asinh)
MATH1(math_acosh, js_k_acosh)
MATH1(math_atanh, js_k_atanh)
MATH1(math_f16round, m_f16round)
MATH1(math_clz32, m_clz32)
#undef MATH1

static bool math_round(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    (void)argc;
    double x;
    TO_NUM(x, ARG(0));
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


/* A constant (Math.PI, Number.EPSILON): read-only and not configurable, as V8's */
static bool def_num(JsContext *ctx, JsObject *o, const char *k, double v) {
    JsString *ks = js_ascii_cell(ctx->vm, k);
    JsString *ik = ks ? js_intern_cell(ctx->vm, ks) : NULL;
    if (!ik || !js_map_set(ctx->vm, &o->props, ik, js_number(v)))
        return false;
    js_map_add_attrs(&o->props, ik, JS_ATTR_READONLY | JS_ATTR_FIXED);
    o->obj_flags |= JS_OBJ_ATTRS;
    return true;
}

static bool def_fn(JsContext *ctx, JsObject *o, const char *k, JsNativeFn fn) {
    JsValue nf = js_native_new(ctx, k, fn, NULL);
    return js_is_function(nf) && js_object_set_ascii(ctx, o, k, nf);
}

/* Makes TABLE (a primitive's method table) the prototype of the global constructor NAME, linked
 * both ways as V8's are: Number.prototype.toFixed, (5).constructor === Number */
static bool link_prototype(JsContext *ctx, const char *name, JsObject *table) {
    JsString *k = js_ascii_cell(ctx->vm, name);
    JsString *ik = k ? js_intern_cell(ctx->vm, k) : NULL;
    if (!ik)
        return false;
    bool found;
    JsValue ctor = js_map_get(&ctx->globals->props, ik, &found);
    if (!found || !js_is_function(ctor) || js_value_cell(ctor)->kind != JS_KIND_NATIVE)
        return false;
    js_native_builtin_ctor(ctor, table);
    return js_object_set_ascii(ctx, table, "constructor", ctor);
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
    js_native_builtin_ctor(nf, NULL); /* Number, String, Boolean */
    bool ok = js_object_set_ascii(ctx, ctx->globals, name, nf);
    js_gc_unprotect(ctx->vm, &nf);
    return ok ? native->statics : NULL;
}

/* Every property O has: not enumerable, as V8 makes a built-in object's */
static void hide_all(JsObject *o) {
    if (!o || !o->props.entries)
        return;
    uint8_t *attrs = JS_MAP_ATTRS(&o->props);
    for (uint32_t i = 0; i < o->props.used; i++)
        if (o->props.entries[i].key && o->props.entries[i].key != JS_MAP_TOMBSTONE)
            attrs[i] |= JS_ATTR_HIDDEN;
}

/* The properties the engine made on the global object, the builtin prototypes and Math and JSON
 * are not enumerable (Object.keys(Map.prototype) and Object.keys(globalThis) are [], as in V8);
 * ones a script or the host adds later are. */
static void hide_builtin_keys(JsContext *ctx) {
    JsObject *objs[] = {ctx->globals, ctx->object_proto, ctx->array_proto, ctx->function_methods,
                        ctx->number_methods, ctx->boolean_methods, ctx->string_methods, ctx->promise_methods,
                        ctx->regexp_proto, ctx->date_proto, ctx->map_proto, ctx->set_proto,
                        ctx->weakmap_proto, ctx->weakset_proto, ctx->weakref_proto, ctx->finreg_proto,
                        ctx->bigint_methods, ctx->iterator_proto, ctx->math_obj, ctx->json_obj};
    for (size_t i = 0; i < sizeof objs / sizeof objs[0]; i++)
        hide_all(objs[i]);
    for (int k = 0; k < JS_ERROR_KIND_COUNT; k++)
        hide_all(ctx->error_protos[k]);
    for (int k = 0; k < JS_ITER_PROTO_COUNT; k++)
        hide_all(ctx->iter_protos[k]);
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
        {"isPrototypeOf", objp_isPrototypeOf},
        {"propertyIsEnumerable", objp_propertyIsEnumerable},
        {"toString", objp_toString},
        {"valueOf", objp_valueOf},
        {NULL, NULL}};
    if (!install_methods(ctx, ctx->object_proto, object_proto_methods))
        return false;
    /* __proto__: an accessor, as every accessor a built-in has: its entry holds the marker that
     * names its getter and setter (js_accessor_call). Not enumerable (hide_builtin_keys),
     * configurable. */
    if (!js_object_set_ascii(ctx, ctx->object_proto, "__proto__", js_accessor(JS_ACC_PROTO_GET, JS_ACC_PROTO_SET)))
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
    JsValue boo = js_object_new(ctx);
    if (!js_is_object(boo))
        return false;
    ctx->boolean_methods = js_value_object(boo);
    JsValue fnm = js_object_new(ctx);
    if (!js_is_object(fnm))
        return false;
    ctx->function_methods = js_value_object(fnm);
    /* Function.prototype: a function that takes anything and returns undefined, not a
     * constructor, named "" with length 0; the method table is its own properties */
    JsValue fpv = js_native_new(ctx, "", fp_call, NULL);
    if (!js_is_function(fpv))
        return false;
    JsNative *fp = (JsNative *)js_value_cell(fpv);
    fp->props = ctx->function_methods;
    fp->own_proto = js_value_from_cell(&ctx->object_proto->gc);
    fp->proto_set = true;
    fp->ctor = JS_CTOR_NONE;
    ctx->function_proto = fp;
    /* AsyncFunction.prototype: what async functions inherit from, then Function.prototype */
    JsValue afp = js_object_new(ctx);
    if (!js_is_object(afp))
        return false;
    js_object_set_proto(js_value_object(afp), fpv);
    ctx->async_function_proto = js_value_object(afp);

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
        {"concat", sm_concat}, {"toString", sm_toString}, {"valueOf", sm_valueOf},
        {"isWellFormed", sm_isWellFormed}, {"toWellFormed", sm_toWellFormed},
        {"normalize", sm_normalize},
        {"anchor", sm_anchor}, {"big", sm_big}, {"blink", sm_blink}, {"bold", sm_bold},
        {"fixed", sm_fixed}, {"fontcolor", sm_fontcolor}, {"fontsize", sm_fontsize},
        {"italics", sm_italics}, {"link", sm_link}, {"small", sm_small}, {"strike", sm_strike},
        {"sub", sm_sub}, {"sup", sm_sup},
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
        {"flat", am_flat}, {"toString", am_toString}, {"keys", am_keys},
        {"values", am_values}, {"entries", am_entries},
        {"findLast", am_findLast}, {"findLastIndex", am_findLastIndex},
        {"reduceRight", am_reduceRight}, {"flatMap", am_flatMap}, {"splice", am_splice},
        {"copyWithin", am_copyWithin}, {"toReversed", am_toReversed},
        {"toSorted", am_toSorted}, {"toSpliced", am_toSpliced}, {"with", am_with},
        {NULL, NULL}};
    static const MethodDef function_methods[] = {
        {"apply", fm_apply}, {"bind", fm_bind}, {"call", fm_call}, {"toString", fm_toString},
        {NULL, NULL}};
    static const MethodDef number_methods[] = {
        {"toFixed", nm_toFixed}, {"toString", nm_toString}, {"valueOf", nm_valueOf},
        {"toExponential", nm_toExponential}, {"toPrecision", nm_toPrecision},
        {NULL, NULL}};
    static const MethodDef boolean_methods[] = {
        {"toString", bm_toString}, {"valueOf", bm_valueOf},
        {NULL, NULL}};
    if (!install_methods(ctx, ctx->string_methods, string_methods) ||
        !install_methods(ctx, ctx->array_proto, array_methods) ||
        !install_methods(ctx, ctx->number_methods, number_methods) ||
        !install_methods(ctx, ctx->boolean_methods, boolean_methods) ||
        !install_methods(ctx, ctx->function_methods, function_methods) ||
        /* Function.prototype.arguments and caller: the accessors that throw, as for every strict
         * function in V8 */
        !js_object_set_ascii(ctx, ctx->function_methods, "arguments",
                             js_accessor(JS_ACC_FN_ARGUMENTS_GET, JS_ACC_FN_ARGUMENTS_SET)) ||
        !js_object_set_ascii(ctx, ctx->function_methods, "caller",
                             js_accessor(JS_ACC_FN_CALLER_GET, JS_ACC_FN_CALLER_SET)))
        return false;
    /* trimLeft and trimRight are the very functions trimStart and trimEnd, as in V8 */
    for (int i = 0; i < 2; i++) {
        bool found = false;
        JsString *k = js_ascii_cell(ctx->vm, i ? "trimEnd" : "trimStart");
        k = k ? js_intern_cell(ctx->vm, k) : NULL;
        JsValue fn = k ? js_map_get(&ctx->string_methods->props, k, &found) : js_undefined();
        if (!found || !js_object_set_ascii(ctx, ctx->string_methods, i ? "trimRight" : "trimLeft", fn))
            return false;
    }
    /* call and apply have no frame of their own in a stack trace, as in V8 */
    for (const char *const *m = (const char *const[]){"call", "apply", NULL}; *m; m++) {
        JsString *k = js_ascii_cell(ctx->vm, *m);
        k = k ? js_intern_cell(ctx->vm, k) : NULL;
        bool found = false;
        JsValue v = k ? js_map_get(&ctx->function_methods->props, k, &found) : js_undefined();
        if (found && js_is_function(v) && js_value_cell(v)->kind == JS_KIND_NATIVE)
            ((JsNative *)js_value_cell(v))->builtin = false;
    }

    /* Math */
    JsValue mathv = js_object_new(ctx);
    if (!js_is_object(mathv) || !js_object_set_ascii(ctx, ctx->globals, "Math", mathv))
        return false;
    JsObject *math = js_value_object(mathv);
    ctx->math_obj = math;
    if (!def_fn(ctx, math, "abs", math_abs) || !def_fn(ctx, math, "floor", math_floor) ||
        !def_fn(ctx, math, "ceil", math_ceil) || !def_fn(ctx, math, "round", math_round) ||
        !def_fn(ctx, math, "trunc", math_trunc) || !def_fn(ctx, math, "sign", math_sign) ||
        !def_fn(ctx, math, "sqrt", math_sqrt) || !def_fn(ctx, math, "fround", math_fround) || !def_fn(ctx, math, "cbrt", math_cbrt) ||
        !def_fn(ctx, math, "exp", math_exp) || !def_fn(ctx, math, "log", math_log) ||
        !def_fn(ctx, math, "log2", math_log2) || !def_fn(ctx, math, "log10", math_log10) ||
        !def_fn(ctx, math, "sin", math_sin) || !def_fn(ctx, math, "cos", math_cos) ||
        !def_fn(ctx, math, "tan", math_tan) || !def_fn(ctx, math, "asin", math_asin) ||
        !def_fn(ctx, math, "acos", math_acos) || !def_fn(ctx, math, "atan", math_atan) ||
        !def_fn(ctx, math, "atan2", math_atan2) || !def_fn(ctx, math, "pow", math_pow) ||
        !def_fn(ctx, math, "min", math_min) || !def_fn(ctx, math, "max", math_max) ||
        !def_fn(ctx, math, "hypot", math_hypot) || !def_fn(ctx, math, "random", math_random) ||
        !def_fn(ctx, math, "expm1", math_expm1) || !def_fn(ctx, math, "log1p", math_log1p) ||
        !def_fn(ctx, math, "sinh", math_sinh) || !def_fn(ctx, math, "cosh", math_cosh) ||
        !def_fn(ctx, math, "tanh", math_tanh) || !def_fn(ctx, math, "asinh", math_asinh) ||
        !def_fn(ctx, math, "acosh", math_acosh) || !def_fn(ctx, math, "atanh", math_atanh) ||
        !def_fn(ctx, math, "f16round", math_f16round) || !def_fn(ctx, math, "clz32", math_clz32) ||
        !def_fn(ctx, math, "imul", math_imul))
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
    ctx->json_obj = json;
    if (!def_fn(ctx, json, "parse", json_parse) || !def_fn(ctx, json, "stringify", json_stringify) ||
        !def_fn(ctx, json, "rawJSON", json_rawJSON) || !def_fn(ctx, json, "isRawJSON", json_isRawJSON))
        return false;

    /* Object: a callable native (Object() / new Object() behave identically —
     * see JS_OP_NEW's native-constructor dispatch), same "real prototype
     * object" wiring as Date/Map/Set/RegExp below — Object.prototype is
     * ctx->object_proto itself, not a separately lazily-created object. */
    JsValue objv = js_native_new(ctx, "Object", g_Object, NULL);
    if (!js_is_function(objv))
        return false;
    js_gc_protect(vm, &objv); /* keep rooted through statics + global set */
    js_native_builtin_ctor(objv, ctx->object_proto);
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
        !def_fn(ctx, object, "freeze", obj_freeze) || !def_fn(ctx, object, "isFrozen", obj_isFrozen) ||
        !def_fn(ctx, object, "seal", obj_seal) || !def_fn(ctx, object, "isSealed", obj_isSealed) ||
        !def_fn(ctx, object, "fromEntries", obj_fromEntries) ||
        !def_fn(ctx, object, "hasOwn", obj_hasOwn) || !def_fn(ctx, object, "is", obj_is) ||
        !def_fn(ctx, object, "groupBy", obj_groupBy) ||
        !def_fn(ctx, object, "getOwnPropertyNames", obj_getOwnPropertyNames) ||
        !def_fn(ctx, object, "getPrototypeOf", obj_getPrototypeOf) ||
        !def_fn(ctx, object, "setPrototypeOf", obj_setPrototypeOf) ||
        !def_fn(ctx, object, "create", obj_create) ||
        !def_fn(ctx, object, "defineProperty", obj_defineProperty) ||
        !def_fn(ctx, object, "defineProperties", obj_defineProperties) ||
        !def_fn(ctx, object, "getOwnPropertyDescriptor", obj_getOwnPropertyDescriptor) ||
        !def_fn(ctx, object, "getOwnPropertyDescriptors", obj_getOwnPropertyDescriptors) ||
        !def_fn(ctx, object, "preventExtensions", obj_preventExtensions) ||
        !def_fn(ctx, object, "isExtensible", obj_isExtensible))
        return false;

    /* Array: a callable native (Array(n) / new Array(n) behave identically —
     * see JS_OP_NEW's native-constructor dispatch), same "real prototype
     * object" wiring as Date/Map/Set/RegExp below. */
    JsValue arrv = js_native_new(ctx, "Array", g_Array, NULL);
    if (!js_is_function(arrv))
        return false;
    js_gc_protect(vm, &arrv); /* keep rooted through statics + global set */
    js_native_builtin_ctor(arrv, ctx->array_proto);
    JsValue arrayv = js_object_new(ctx);
    bool arr_ok = js_is_object(arrayv);
    JsObject *array = arr_ok ? js_value_object(arrayv) : NULL;
    if (arr_ok) {
        ((JsNative *)js_value_cell(arrv))->statics = array;
        arr_ok = def_fn(ctx, array, "isArray", arr_isArray) && def_fn(ctx, array, "of", arr_of) &&
                def_fn(ctx, array, "from", arr_from) && def_fn(ctx, array, "fromAsync", js_array_from_async) &&
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
        !def_fn(ctx, stro, "fromCodePoint", str_fromCodePoint) || !def_fn(ctx, stro, "raw", str_raw))
        return false;
    if (!link_prototype(ctx, "Number", ctx->number_methods) ||
        !link_prototype(ctx, "String", ctx->string_methods))
        return false;

    /* Boolean: callable conversion; Boolean.prototype the booleans' method table */
    if (!def_ctor(ctx, "Boolean", g_Boolean) || !link_prototype(ctx, "Boolean", ctx->boolean_methods))
        return false;

    /* Error + TypeError/RangeError/ReferenceError/SyntaxError (js_error.c) */
    if (!js_error_builtins_init(ctx))
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

    /* the iterator prototypes (js_iter.c) */
    if (!js_iter_builtins_init(ctx))
        return false;

    /* Map global + real Map.prototype (js_mapobj.c) */
    if (!js_mapobj_builtins_init(ctx))
        return false;

    /* Set global + real Set.prototype (js_setobj.c) */
    if (!js_weak_builtins_init(ctx) || !js_bigint_builtins_init(ctx))
        return false;
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
        !js_register_native(ctx, n_isFinite, 8, g_isFinite, NULL) ||
        !def_fn(ctx, ctx->globals, "encodeURI", g_encodeURI) ||
        !def_fn(ctx, ctx->globals, "encodeURIComponent", g_encodeURIComponent) ||
        !def_fn(ctx, ctx->globals, "decodeURI", g_decodeURI) ||
        !def_fn(ctx, ctx->globals, "decodeURIComponent", g_decodeURIComponent) ||
        !def_fn(ctx, ctx->globals, "escape", g_escape) || !def_fn(ctx, ctx->globals, "unescape", g_unescape))
        return false;
    /* the constructors with a [Symbol.species] in V8 (js_species) */
    static const char *const species[JS_SPECIES__COUNT] = {"Array", "Map", "Set", "Promise", "RegExp"};
    for (int i = 0; i < JS_SPECIES__COUNT; i++) {
        JsString *k = js_ascii_cell(ctx->vm, species[i]);
        k = k ? js_intern_cell(ctx->vm, k) : NULL;
        if (!k)
            return false;
        bool found;
        JsValue c = js_map_get(&ctx->globals->props, k, &found);
        ctx->species_ctors[i] = found && js_is_function(c) && js_value_cell(c)->kind == JS_KIND_NATIVE
                                    ? (JsNative *)js_value_cell(c) : NULL;
    }
    if (!js_intact_watch(ctx, JS_INTACT_ARRAY_CONSTRUCTOR, ctx->array_proto, "constructor"))
        return false;
    hide_builtin_keys(ctx);
    return true;
}
