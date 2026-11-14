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
 * only; sort is a stable O(n^2) binary insertion sort.
 */
#include "js_bytecode.h"
#include "js_error.h"
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
    JsString *uninit; /* set by JSON.stringify on a namespace's export still in the TDZ */
} StrBuf;

static void sb_init(StrBuf *sb, JsVm *vm) {
    sb->vm = vm;
    sb->u = NULL;
    sb->len = sb->cap = 0;
    sb->oom = false;
    sb->circular = false;
    sb->uninit = NULL;
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

static bool str_this(JsContext *ctx, JsValue this_val, JsString **out, JsValue *result) {
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
#ifdef LAMASSU_HAS_REGEX
    if (js_regexp_is(ARG(0)))
        return js_re_str_replace(ctx, tv, args, argc, r, all);
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
 * Array methods (this_val = array object)
 * ===================================================================== */

static bool array_this(JsContext *ctx, JsValue tv, JsObject **out, JsValue *r) {
    if (!js_is_object(tv) || js_value_object(tv)->obj_kind != JS_OBJ_ARRAY)
        return native_throw(ctx, r, "TypeError: not an array");
    *out = js_value_object(tv);
    return true;
}

/*
 * The mutating array methods write a->elems and a->elem_count directly, so
 * they never reach set_property's freeze check and have to make it themselves.
 * `changes_length` separates the two states: sealing forbids adding and
 * removing elements, freezing forbids touching them at all, so reverse/fill/
 * sort are legal on a sealed array but not a frozen one.
 */
typedef enum {
    MUT_ADD,          /* push, unshift: new elements at the end */
    MUT_REMOVE_END,   /* pop: deletes the last element */
    MUT_REMOVE_FRONT, /* shift: moves the elements down, then deletes the last */
    MUT_WRITE,        /* reverse, fill, sort: writes elements in place */
} ArrayMut;

/* The refusal is worded as V8 words the first write such a method makes. */
static bool array_this_mut(JsContext *ctx, JsValue tv, JsObject **out, JsValue *r, ArrayMut how) {
    if (!array_this(ctx, tv, out, r))
        return false;
    JsObject *a = *out;
    uint8_t f = a->obj_flags;
    if (!f)
        return true;
    bool frozen = (f & JS_OBJ_FROZEN) != 0;
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
        if (!frozen || n == 0)
            return true;
        *r = js_error_fmt(ctx, read_only, "0");
        return false;
    }
    return true;
}

static bool am_push(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsObject *a;
    if (!array_this_mut(ctx, tv, &a, r, MUT_ADD))
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
    if (!array_this_mut(ctx, tv, &a, r, MUT_REMOVE_END))
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
    if (!array_this_mut(ctx, tv, &a, r, MUT_REMOVE_FRONT))
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
    if (!array_this_mut(ctx, tv, &a, r, MUT_ADD))
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
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
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
    JsObject *a;
    if (!array_this(ctx, tv, &a, r))
        return false;
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
    *r = sb_finish(&sb);
    return js_is_undefined(*r) && sb.oom ? oom(ctx, r) : true;
}

static bool am_reverse(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    JsObject *a;
    if (!array_this_mut(ctx, tv, &a, r, MUT_WRITE))
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
    if (!array_this_mut(ctx, tv, &a, r, MUT_WRITE))
        return false;
    uint32_t len = a->elem_count;
    double sd = 0, ed = len;
    if (argc > 1)
        TO_NUM(sd, ARG(1));
    if (argc > 2 && !js_is_undefined(ARG(2)))
        TO_NUM(ed, ARG(2));
    /* a valueOf may have frozen or shortened it: check again, and write an index past the end as
     * a property set would */
    if (!array_this_mut(ctx, tv, &a, r, MUT_WRITE))
        return false;
    uint32_t start = clamp_index(sd, len), end = clamp_index(ed, len);
    for (uint32_t i = start; i < end; i++) {
        if (i < a->elem_count)
            a->elems[i] = ARG(0);
        else if (!js_array_set_index(ctx->vm, a, i, ARG(0)))
            return oom(ctx, r);
    }
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

/* stable binary insertion sort; comparator optional */
static bool am_sort(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsObject *a;
    if (!array_this_mut(ctx, tv, &a, r, MUT_WRITE))
        return false;
    JsValue cmp = ARG(0);
    bool has_cmp = js_is_function(cmp);
    if (argc > 0 && !has_cmp && !js_is_undefined(cmp))
        return native_throw_fmt(ctx, r, "TypeError: The comparison function must be either a function or undefined: %v", cmp);
    JsValue arrv = tv, key = js_undefined(), midstr = js_undefined();
    js_gc_protect(ctx->vm, &arrv);
    js_gc_protect(ctx->vm, &cmp);
    js_gc_protect(ctx->vm, &key);
    js_gc_protect(ctx->vm, &midstr);
    bool failed = false;

    /* Insertion sort is quadratic in element moves and the interpreter charges
     * one fuel unit for the whole call, so a large sort is the longest a guest
     * can keep the engine inside a single native. Poll the stop flag per pass
     * so an interrupt is not left waiting for it. */
    for (uint32_t i = 1; i < a->elem_count && !failed; i++) {
        if (ctx->vm->interrupt) {
            failed = true;
            native_throw(ctx, r, JS_MSG_INTERRUPT);
            break; /* not `return`: the roots below still have to come off */
        }
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
                double d;
                if (!js_op_to_number(ctx, res, &d)) {
                    oom(ctx, r);
                    failed = true;
                    break;
                }
                c = d > 0 ? 1 : (d < 0 ? -1 : 0);
            } else {
                /* Converting `key` allocates, which is a GC safe point — so
                 * `sa` must be rooted across it or the collector can reclaim
                 * the string this comparison is about to read. */
                JsString *sa = js_op_to_string(ctx, midv);
                if (!sa) {
                    failed = true;
                    oom(ctx, r);
                    break;
                }
                midstr = js_value_from_cell(&sa->gc);
                JsString *sk = js_op_to_string(ctx, key);
                if (!sk) {
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
    js_gc_unprotect(ctx->vm, &midstr);
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

/* Calls fn with `this` = self and the arguments `pre` then `post`, gathered in one buffer (the
 * values stay reachable through their owners: the bound array, the caller's stack). */
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

/* Function.prototype.apply(thisArg, argsArray): call with the array's elements as arguments
 * (undefined or null for none). */
static bool fm_apply(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (!js_is_function(tv))
        return native_throw_fmt(ctx, r, "TypeError: Function.prototype.apply was called on %v, which is %s and not a function",
                                tv, type_phrase(tv));
    JsValue self = argc > 0 ? args[0] : js_undefined();
    JsValue list = argc > 1 ? args[1] : js_undefined();
    if (js_is_undefined(list) || js_is_null(list))
        return js_call(ctx, tv, self, NULL, 0, r);
    if (!js_is_array(list))
        return native_throw(ctx, r, "TypeError: CreateListFromArrayLike called on non-object");
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
    /* named "bound " and the target's name, as in V8 */
    js_gc_protect(vm, &fn);
    JsString *prefix = js_ascii_cell(vm, "bound ");
    JsValue pv = prefix ? js_value_from_cell(&prefix->gc) : js_undefined();
    js_gc_protect(vm, &pv);
    const JsString *target = js_function_name(tv); /* tv's, so rooted by the caller */
    JsString *nm = prefix && target && target->length ? js_concat_cells(vm, prefix, target) : prefix;
    js_gc_unprotect(vm, &pv);
    js_gc_unprotect(vm, &fn);
    if (!nm) {
        *r = js_oom_value(vm);
        return false;
    }
    ((JsNative *)js_value_cell(fn))->name = nm;
    /* and the target's length less the arguments bound */
    uint32_t tl = js_function_length(tv), bound = argc > 1 ? (uint32_t)(argc - 1) : 0;
    uint32_t bl = tl > bound ? tl - bound : 0;
    ((JsNative *)js_value_cell(fn))->length = (uint8_t)(bl > 255 ? 255 : bl);
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

static bool nm_toFixed(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    NUMBER_THIS("toFixed");
    double x = js_get_number(tv);
    double dd;
    TO_NUM(dd, ARG(0));
    dd = to_int(dd);
    if (dd < 0 || dd > 100)
        return native_throw(ctx, r, "RangeError: toFixed() digits argument must be between 0 and 100");
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
    /* plain object: own string keys in enumeration order (index keys
     * ascending, then insertion order). Values are written straight into `sb`; a property whose value emits
     * nothing (undefined/function) is undone by rewinding sb->len, so no
     * throwaway per-property buffer is needed. */
    sb_unit(sb, '{');
    bool first = true;
    js_map_sort_keys(&o->props);
    for (uint32_t i = 0; i < o->props.capacity; i++) {
        JsMapEntry *e = &o->props.entries[i];
        if (!e->key || e->key == JS_MAP_TOMBSTONE || js_prop_hidden_at(o, i))
            continue;
        uint32_t mark = sb->len; /* rewind point if the value emits nothing */
        if (!first)
            sb_unit(sb, ',');
        json_newline_indent(sb, indent, indent_len, depth + 1);
        json_quote(sb, e->key->units, e->key->length);
        sb_unit(sb, ':');
        if (indent_len)
            sb_unit(sb, ' ');
        if (e->value.bits == JS_SPECIAL_TDZ) {
            sb->uninit = e->key;
            return false;
        }
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
        int n = (int)to_integer_clamped(js_get_number(space), 0, 10);
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
        JsString *uninit = sb.uninit;
        sb_free(&sb);
        if (circular)
            return native_throw(ctx, r, "TypeError: Converting circular structure to JSON");
        if (uninit)
            return native_throw_fmt(ctx, r, "ReferenceError: Cannot access '%S' before initialization", uninit);
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

static bool json_value(JsonP *p, JsValue *out);
static bool json_value_body(JsonP *p, JsValue *out);

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
static bool json_value(JsonP *p, JsValue *out) {
    if (++p->depth > JSON_PARSE_MAX_DEPTH) {
        json_fail(p, JE_DEPTH, NULL, p->pos);
        p->depth--;
        return false;
    }
    bool ok = json_value_body(p, out);
    p->depth--;
    return ok;
}

static bool json_value_body(JsonP *p, JsValue *out) {
    json_ws(p);
    if (p->pos >= p->len)
        return json_end(p);
    uint16_t c = p->u[p->pos];
    if (c == '{') {
        p->pos++;
        JsValue ov = js_object_new(p->ctx);
        if (!js_is_object(ov))
            return json_oom(p);
        JsObject *o = js_value_object(ov);
        js_gc_protect(p->ctx->vm, &ov);
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
    return json_token(p, p->pos);
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

static bool json_parse(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsString *s = arg_string(ctx, ARG(0));
    if (!s)
        return oom(ctx, r);
    JsValue sv = js_value_from_cell(&s->gc);
    js_gc_protect(ctx->vm, &sv);
    JsonP p = {ctx, s->units, s->length, 0, false, 0, JE_NONE, NULL, 0};
    JsValue out;
    bool ok = json_value(&p, &out);
    if (ok) {
        json_ws(&p);
        if (p.pos != p.len)
            ok = json_at(&p, "Unexpected non-whitespace character after JSON", p.pos);
    }
    if (!ok || p.error) {
        *r = json_error(ctx, &p); /* s is still rooted: the message quotes it */
        js_gc_unprotect(ctx->vm, &sv);
        return false;
    }
    js_gc_unprotect(ctx->vm, &sv);
    *r = out;
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
    if (js_is_function(v)) { /* a function's own properties (none until a script writes one) */
        JsObject *own = js_function_props(ctx, v, false);
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

/* Appends every key in O's property map, enumerable or not, in enumeration order (integer keys
 * ascending, then insertion order), to OUT. O and OUT are rooted. False on OOM. */
static bool append_map_keys(JsContext *ctx, JsObject *out, JsObject *o) {
    js_map_sort_keys(&o->props);
    for (uint32_t i = 0; i < o->props.capacity; i++) {
        JsMapEntry *e = &o->props.entries[i];
        if (!e->key || e->key == JS_MAP_TOMBSTONE)
            continue;
        if (!js_array_append(ctx->vm, out, js_value_from_cell(&e->key->gc)))
            return false;
    }
    return true;
}

/* Object.getOwnPropertyNames(o): every own string key, enumerable or not, in V8's order. Its
 * [[OwnPropertyKeys]] reads no values, so a namespace's uninitialized export is listed, not thrown.
 *   string      its indices, then "length"; a number or boolean has none
 *   array       its indices, then "length", then its named properties
 *   function    "length", "name", "prototype" when it has one, then a builtin's statics and what
 *               a script wrote onto it
 *   error       "stack" while it has one, then "message", "cause" and the rest
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
        ok = append_ascii_key(ctx, out, "length") && append_ascii_key(ctx, out, "name") &&
             (!js_function_has_own_prototype(v) || append_ascii_key(ctx, out, "prototype"));
        if (ok && c->kind == JS_KIND_NATIVE && ((JsNative *)c)->statics)
            ok = append_map_keys(ctx, out, ((JsNative *)c)->statics);
        JsObject *own = js_function_props(ctx, v, false);
        if (ok && own)
            ok = append_map_keys(ctx, out, own);
    } else if (js_is_object(v)) {
        JsObject *o = js_value_object(v);
        if (o->obj_kind == JS_OBJ_ARRAY)
            ok = append_index_keys(ctx, out, o->elem_count) && append_ascii_key(ctx, out, "length");
        else if (o->obj_kind == JS_OBJ_ERROR && js_error_has_stack(o))
            ok = append_ascii_key(ctx, out, "stack");
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

static bool obj_assign(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue target = ARG(0);
    /* a function as target or source: its own-property object */
    JsValue dst = target;
    if (js_is_function(target)) {
        JsObject *own = js_function_props(ctx, target, true);
        if (!own)
            return oom(ctx, r);
        dst = js_value_from_cell(&own->gc);
    }
    if (!js_is_object(dst))
        return native_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
    if (js_value_object(dst)->obj_flags & JS_OBJ_FROZEN)
        return native_throw(ctx, r, "TypeError: cannot assign to a property of a frozen object");
    if (js_value_object(dst)->obj_flags & JS_OBJ_NAMESPACE) {
        /* nothing to copy is fine; any property is a write V8 refuses */
        for (int i = 1; i < argc; i++) {
            if (!js_is_object(args[i]))
                continue;
            JsObject *so = js_value_object(args[i]);
            if (so->obj_kind == JS_OBJ_ARRAY && so->elem_count)
                return native_throw(ctx, r, "TypeError: Cannot assign to read only property '0' of object '[object Module]'");
            if (js_object_size(args[i]) > 0)
                return native_throw_fmt(ctx, r, "TypeError: Cannot assign to read only property '%v' of object '[object Module]'",
                                        js_object_key_at(args[i], 0));
        }
    }
    js_gc_protect(ctx->vm, &target);
    bool ok = true;
    JsString *uninit = NULL;
    for (int i = 1; i < argc && ok; i++) {
        JsValue src = args[i];
        if (js_is_function(src)) {
            JsObject *own = js_function_props(ctx, src, false);
            src = own ? js_value_from_cell(&own->gc) : js_undefined();
        }
        if (js_is_object(src))
            ok = js_spread_into_object(ctx, js_value_object(dst), src, &uninit);
    }
    js_gc_unprotect(ctx->vm, &target);
    if (!ok && uninit)
        return native_throw_fmt(ctx, r, "ReferenceError: %S is not defined", uninit);
    if (!ok)
        return oom(ctx, r);
    if (js_is_function(target)) {
        /* a function's name and length are read-only: a copy that landed among its own
         * properties goes */
        static const uint16_t nm[] = {'n', 'a', 'm', 'e'}, ln[] = {'l', 'e', 'n', 'g', 't', 'h'};
        JsValue kn = js_atom(ctx->vm, nm, 4);
        js_gc_protect(ctx->vm, &kn);
        JsValue kl = js_atom(ctx->vm, ln, 6);
        js_gc_unprotect(ctx->vm, &kn);
        if (!js_is_string(kn) || !js_is_string(kl))
            return oom(ctx, r);
        bool had_name = js_map_delete(&js_value_object(dst)->props, js_value_string(kn));
        bool had_length = js_map_delete(&js_value_object(dst)->props, js_value_string(kl));
        if (had_name || had_length)
            return native_throw_fmt(ctx, r, "TypeError: Cannot assign to read only property '%s' of function '%v'",
                                    had_name ? "name" : "length", target);
    }
    *r = target;
    return true;
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
    }
    if (js_is_function(v)) { /* freezes the function's own properties (made now, so it sticks) */
        JsObject *own = js_function_props(ctx, v, true);
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

/* A non-object has no properties to write, so it is vacuously frozen/sealed —
 * again matching the spec, and the reason these take the object test first. */
static bool obj_isFrozen(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)ctx;
    (void)tv;
    (void)argc;
    JsValue v = ARG(0);
    if (js_is_function(v)) {
        JsObject *own = js_function_props(ctx, v, false);
        *r = js_bool(own && (own->obj_flags & JS_OBJ_FROZEN) != 0);
        return true;
    }
    *r = js_bool(!js_is_object(v) ||
                 (js_value_object(v)->obj_flags & JS_OBJ_FROZEN) != 0);
    return true;
}

static bool obj_isSealed(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)ctx;
    (void)tv;
    (void)argc;
    JsValue v = ARG(0);
    if (js_is_function(v)) {
        JsObject *own = js_function_props(ctx, v, false);
        *r = js_bool(own && (own->obj_flags & JS_OBJ_SEALED) != 0);
        return true;
    }
    *r = js_bool(!js_is_object(v) ||
                 (js_value_object(v)->obj_flags & JS_OBJ_SEALED) != 0);
    return true;
}

static bool obj_fromEntries(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue src = ARG(0);
    if (!js_is_object(src) || js_value_object(src)->obj_kind != JS_OBJ_ARRAY)
        return native_throw_fmt(ctx, r, js_is_undefined(src) || js_is_null(src)
                                    ? "TypeError: %v is not iterable"
                                    : "TypeError: %t is not iterable (cannot read property Symbol(Symbol.iterator))",
                                src);
    JsValue outv = js_object_new(ctx);
    if (!js_is_object(outv))
        return oom(ctx, r);
    JsObject *out = js_value_object(outv);
    js_gc_protect(ctx->vm, &outv);
    JsObject *arr = js_value_object(src);
    bool ok = true;
    JsValue bad = js_undefined();
    bool have_bad = false;
    for (uint32_t i = 0; i < arr->elem_count && ok; i++) {
        JsValue pair = arr->elems[i];
        if (!js_is_object(pair) || js_value_object(pair)->obj_kind != JS_OBJ_ARRAY) {
            bad = pair;
            have_bad = true;
            ok = false;
            break;
        }
        JsObject *pa = js_value_object(pair);
        JsValue k = pa->elem_count > 0 ? pa->elems[0] : js_undefined();
        JsValue val = pa->elem_count > 1 ? pa->elems[1] : js_undefined();
        /* ToPropertyKey may run k's toString, which may drop val from the pair */
        js_gc_protect(ctx->vm, &val);
        JsString *ks = js_op_to_string(ctx, k);
        JsString *iks = ks ? js_intern_cell(ctx->vm, ks) : NULL;
        ok = iks && js_map_set(ctx->vm, &out->props, iks, val);
        js_gc_unprotect(ctx->vm, &val);
    }
    js_gc_unprotect(ctx->vm, &outv);
    if (have_bad) /* bad is an element of src, which the caller holds */
        return native_throw_fmt(ctx, r, "TypeError: Iterator value %v is not an entry object", bad);
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
        if (str_is_ascii(kv, "name") || str_is_ascii(kv, "length") ||
            (str_is_ascii(kv, "prototype") && js_function_has_own_prototype(v))) {
            *out = true;
            return true;
        }
        JsGcCell *c = js_value_cell(v);
        maps[0] = js_function_props(ctx, v, false);
        if (c->kind == JS_KIND_NATIVE)
            maps[1] = ((JsNative *)c)->statics;
    } else if (js_is_object(v)) {
        JsObject *o = js_value_object(v);
        bool special = (o->obj_kind == JS_OBJ_ARRAY &&
                        (is_index_below(ks, o->elem_count) || str_is_ascii(kv, "length"))) ||
                       (o->obj_kind == JS_OBJ_ERROR && js_error_stack_has(o, kv));
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

const char *js_to_string_tag(const JsContext *ctx, JsValue v) {
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
    if (js_is_function(v))
        return "Function";
    if (js_is_promise(v))
        return "Promise";
    if (!js_is_object(v))
        return "Object";
    const JsObject *o = js_value_object(v);
    switch (o->obj_kind) {
    case JS_OBJ_ARRAY: return "Array";
    case JS_OBJ_ERROR: return "Error";
    case JS_OBJ_DATE: return "Date";
    case JS_OBJ_MAP: return "Map";
    case JS_OBJ_SET: return "Set";
#ifdef LAMASSU_HAS_REGEX
    case JS_OBJ_REGEXP: return "RegExp";
#endif
    default: break;
    }
    if (o->obj_flags & JS_OBJ_NAMESPACE)
        return "Module";
    if (o == ctx->math_obj)
        return "Math";
    if (o == ctx->json_obj)
        return "JSON";
    return "Object";
}

JsObject *js_iterable_to_array(JsContext *ctx, JsValue v, JsValue *err) {
    JsVm *vm = ctx->vm;
    if (js_mapobj_is(v) || js_setobj_is(v)) {
        JsObject *a = js_mapobj_is(v) ? js_mapobj_entries_array(ctx, js_value_object(v))
                                      : js_setobj_values_array(ctx, js_value_object(v));
        if (!a)
            *err = js_oom_value(vm);
        return a;
    }
    bool str = js_is_string(v);
    if (!str && !(js_is_object(v) && js_value_object(v)->obj_kind == JS_OBJ_ARRAY)) {
        *err = js_error_fmt(ctx, "TypeError: %t is not iterable (cannot read property Symbol(Symbol.iterator))", v);
        return NULL;
    }
    js_gc_protect(vm, &v);
    JsObject *a = js_array_new_cell(ctx, str ? 0 : js_value_object(v)->elem_count);
    JsValue av = a ? js_value_from_cell(&a->gc) : js_undefined();
    js_gc_protect(vm, &av);
    bool ok = a != NULL;
    if (ok && str) {
        for (uint32_t i = 0; ok && i < js_value_string(v)->length;) {
            JsString *s = js_value_string(v);
            uint32_t n = 1;
            if (s->units[i] >= 0xD800 && s->units[i] <= 0xDBFF && i + 1 < s->length &&
                s->units[i + 1] >= 0xDC00 && s->units[i + 1] <= 0xDFFF)
                n = 2;
            JsString *c = js_string_cell_new(vm, s->units + i, n);
            ok = c && js_array_append(vm, a, js_value_from_cell(&c->gc));
            i += n;
        }
    } else if (ok) {
        for (uint32_t i = 0; ok && i < js_value_object(v)->elem_count; i++)
            ok = js_array_append(vm, a, js_value_object(v)->elems[i]);
    }
    js_gc_unprotect(vm, &av);
    js_gc_unprotect(vm, &v);
    if (!ok) {
        *err = js_oom_value(vm);
        return NULL;
    }
    return a;
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
    if (js_is_object(v)) {
        JsValue proto = js_value_object(v)->proto;
        *r = js_is_object(proto) ? proto : js_null();
        return true;
    }
    /* a function's is Function.prototype, or the constructor it inherits from (TypeError's is
     * Error); a promise's, number's and string's their constructor's prototype. A boolean's is
     * null: there is no Boolean.prototype object. */
    JsObject *p = js_is_function(v)  ? ctx->function_methods
                  : js_is_promise(v) ? ctx->promise_methods
                  : js_is_number(v)  ? ctx->number_methods
                  : js_is_string(v)  ? ctx->string_methods
                  : js_is_bool(v)    ? ctx->boolean_methods : NULL;
    if (js_is_function(v) && js_value_cell(v)->kind == JS_KIND_NATIVE && ((JsNative *)js_value_cell(v))->parent) {
        *r = js_value_from_cell(&((JsNative *)js_value_cell(v))->parent->gc);
        return true;
    }
    *r = p ? js_value_from_cell(&p->gc) : js_null();
    return true;
}

/* Object.create(proto): a new plain object whose prototype is PROTO (an object) or none (null).
 * A second argument (property descriptors) is refused: there are no property attributes here
 * to give the properties it describes, so there is nothing faithful to do with it. */
static bool obj_create(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue proto = ARG(0);
    if (!js_is_object(proto) && !js_is_null(proto))
        return native_throw_fmt(ctx, r, "TypeError: Object prototype may only be an Object or null: %v", proto);
    if (!js_is_undefined(ARG(1)))
        return native_throw(ctx, r, "TypeError: Object.create with property descriptors is not supported");
    JsValue o = js_object_new(ctx); /* proto is the caller's argument: rooted */
    if (!js_is_object(o))
        return oom(ctx, r);
    js_value_object(o)->proto = js_is_object(proto) ? proto : js_undefined();
    *r = o;
    return true;
}

static bool obj_setPrototypeOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                               JsValue *r) {
    (void)tv;
    JsValue v = ARG(0), proto = ARG(1);
    if (js_is_undefined(v) || js_is_null(v))
        return native_throw(ctx, r, "TypeError: Object.setPrototypeOf called on null or undefined");
    if (!js_is_object(proto) && !js_is_null(proto))
        return native_throw_fmt(ctx, r, "TypeError: Object prototype may only be an Object or null: %v", proto);
    if (js_is_object(v)) {
        JsObject *o = js_value_object(v);
        JsValue np = js_is_object(proto) ? proto : js_undefined();
        /* a sealed or frozen object (a namespace too) is not extensible: its prototype stays */
        if ((o->obj_flags & (JS_OBJ_SEALED | JS_OBJ_FROZEN)) && np.bits != o->proto.bits)
            return native_throw_fmt(ctx, r, "TypeError: %v is not extensible", v);
        o->proto = np;
    }
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
    if (argc > 0)
        TO_NUM(d, ARG(0));
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


static bool def_num(JsContext *ctx, JsObject *o, const char *k, double v) {
    return js_object_set_ascii(ctx, o, k, js_number(v));
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
                        ctx->math_obj, ctx->json_obj};
    for (size_t i = 0; i < sizeof objs / sizeof objs[0]; i++)
        hide_all(objs[i]);
    for (int k = 0; k < JS_ERROR_KIND_COUNT; k++)
        hide_all(ctx->error_protos[k]);
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
    JsValue boo = js_object_new(ctx);
    if (!js_is_object(boo))
        return false;
    ctx->boolean_methods = js_value_object(boo);
    JsValue fnm = js_object_new(ctx);
    if (!js_is_object(fnm))
        return false;
    ctx->function_methods = js_value_object(fnm);

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
    static const MethodDef function_methods[] = {
        {"apply", fm_apply}, {"bind", fm_bind}, {"call", fm_call}, {"toString", fm_toString},
        {NULL, NULL}};
    static const MethodDef number_methods[] = {
        {"toFixed", nm_toFixed}, {"toString", nm_toString}, {"valueOf", nm_valueOf},
        {NULL, NULL}};
    static const MethodDef boolean_methods[] = {
        {"toString", bm_toString}, {"valueOf", bm_valueOf},
        {NULL, NULL}};
    if (!install_methods(ctx, ctx->string_methods, string_methods) ||
        !install_methods(ctx, ctx->array_proto, array_methods) ||
        !install_methods(ctx, ctx->number_methods, number_methods) ||
        !install_methods(ctx, ctx->boolean_methods, boolean_methods) ||
        !install_methods(ctx, ctx->function_methods, function_methods))
        return false;
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
    ctx->json_obj = json;
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
        !def_fn(ctx, object, "hasOwn", obj_hasOwn) ||
        !def_fn(ctx, object, "getOwnPropertyNames", obj_getOwnPropertyNames) ||
        !def_fn(ctx, object, "getPrototypeOf", obj_getPrototypeOf) ||
        !def_fn(ctx, object, "setPrototypeOf", obj_setPrototypeOf) ||
        !def_fn(ctx, object, "create", obj_create))
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
        !js_register_native(ctx, n_isFinite, 8, g_isFinite, NULL) ||
        !def_fn(ctx, ctx->globals, "encodeURI", g_encodeURI) ||
        !def_fn(ctx, ctx->globals, "encodeURIComponent", g_encodeURIComponent) ||
        !def_fn(ctx, ctx->globals, "decodeURI", g_decodeURI) ||
        !def_fn(ctx, ctx->globals, "decodeURIComponent", g_decodeURIComponent))
        return false;
    hide_builtin_keys(ctx);
    return true;
}
