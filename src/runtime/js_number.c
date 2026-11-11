/*
 * Number <-> string conversions and integer coercions, freestanding (no
 * libm). Double formatting picks the shortest digit string (1..17 significant
 * digits) that round-trips, and of those the one nearest the value (ties to
 * even), as Number::toString specifies and V8 prints; the digits come from the
 * C library's correctly rounded conversion. Parsing is correctly rounded too
 * (js_make_double).
 */
#include <stdio.h>
#include <stdlib.h>

#include "js_bytecode.h"

/* 10^0..10^22 are exactly representable doubles. */
static const double js_pow10[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

double js_make_double(uint64_t mant, int exp10) {
    if (mant == 0)
        return 0.0;

    /*
     * One rounding, so exactly right: a mantissa the format can hold and a
     * power of ten that is itself exact.
     */
    if (mant <= ((uint64_t)1 << 53)) {
        if (exp10 >= 0 && exp10 <= 22)
            return (double)mant * js_pow10[exp10];
        if (exp10 < 0 && exp10 >= -22)
            return (double)mant / js_pow10[-exp10];
    }

    /*
     * Otherwise musl's decimal conversion (fdlibm/floatscan.c), correctly rounded and the same on
     * every host. The C library's strtod was used here until it turned out not to be correctly
     * rounded everywhere: next to a power of two, Windows' (UCRT) returns the neighbouring double,
     * which printed 7.678447687145631e-239 as 7.67844768714563e-239.
     *
     * Scaling by powers of ten rounds at EVERY step, and for a 17-digit
     * mantissa — the exact shape of a coordinate written out in full, or of
     * anything that has been through `%.17g` — the accumulated error reaches a
     * whole ulp. The value is then a different double from the one every other
     * engine produces for the same literal, and it prints differently: a page
     * of computed positions disagreed with Node in the last digit, which is
     * how this was found.
     */
    uint16_t buf[32];
    size_t n = 0;
    char digs[24];
    int nd = 0;
    do { digs[nd++] = (char)('0' + mant % 10); mant /= 10; } while (mant);
    while (nd) buf[n++] = (uint16_t)digs[--nd];
    buf[n++] = 'e';
    unsigned e = exp10 < 0 ? (unsigned)-(long)exp10 : (unsigned)exp10;
    if (exp10 < 0) buf[n++] = '-';
    nd = 0;
    do { digs[nd++] = (char)('0' + e % 10); e /= 10; } while (e);
    while (nd) buf[n++] = (uint16_t)digs[--nd];
    return js_k_decimal(buf, n);
}

int32_t js_to_int32(double d) {
    return (int32_t)js_to_uint32(d);
}

uint32_t js_to_uint32(double d) {
    if (d != d)
        return 0;
    /* Fast path: representable in int64 (covers |d| < 2^63). The upper bound is
     * strict: the literal rounds to exactly 2^63, which is INT64_MAX+1, so
     * casting it to int64 is UB — leave d == 2^63 to the slow path below. The
     * lower bound may be inclusive since -2^63 == INT64_MIN is representable. */
    if (d >= -9.2233720368547758e18 && d < 9.2233720368547758e18)
        return (uint32_t)(uint64_t)(int64_t)d;
    union { double d; uint64_t u; } pun;
    pun.d = d;
    int exp = (int)((pun.u >> 52) & 0x7FF);
    if (exp == 0x7FF)
        return 0; /* inf */
    uint64_t mant = (pun.u & UINT64_C(0xFFFFFFFFFFFFF)) | (UINT64_C(1) << 52);
    int shift = exp - 1075; /* value = mant * 2^shift; here shift > 0 */
    uint32_t bits = shift >= 64 ? 0 : (uint32_t)(mant << shift);
    return (pun.u >> 63) ? (uint32_t)(0u - bits) : bits;
}

double js_units_to_number(const uint16_t *units, size_t len) {
    size_t i = 0;
    while (i < len && (units[i] == ' ' || units[i] == '\t' || units[i] == '\n' ||
                       units[i] == '\r' || units[i] == 0x0B || units[i] == 0x0C ||
                       units[i] == 0xA0 || units[i] == 0xFEFF))
        i++;
    size_t end = len;
    while (end > i && (units[end - 1] == ' ' || units[end - 1] == '\t' ||
                       units[end - 1] == '\n' || units[end - 1] == '\r' ||
                       units[end - 1] == 0x0B || units[end - 1] == 0x0C ||
                       units[end - 1] == 0xA0 || units[end - 1] == 0xFEFF))
        end--;
    if (i == end)
        return 0.0; /* empty / whitespace-only */

    bool neg = false;
    if (units[i] == '+' || units[i] == '-') {
        neg = units[i] == '-';
        i++;
    }
#define JS_NAN (__builtin_nan(""))
    if (i == end)
        return JS_NAN;

    /* Infinity */
    static const char inf_word[] = "Infinity";
    if (end - i == 8) {
        bool is_inf = true;
        for (int k = 0; k < 8; k++)
            if (units[i + (size_t)k] != (uint16_t)inf_word[k])
                is_inf = false;
        if (is_inf)
            return neg ? -__builtin_inf() : __builtin_inf();
    }

    /* Radix prefixes (no sign allowed before them in JS, but we already
     * consumed one; JS rejects "-0x10" -> NaN). */
    if (units[i] == '0' && end - i >= 2 &&
        (units[i + 1] == 'x' || units[i + 1] == 'X' || units[i + 1] == 'o' ||
         units[i + 1] == 'O' || units[i + 1] == 'b' || units[i + 1] == 'B')) {
        if (neg || units[i] != '0')
            return JS_NAN;
        int radix = units[i + 1] == 'x' || units[i + 1] == 'X' ? 16
                    : units[i + 1] == 'o' || units[i + 1] == 'O' ? 8 : 2;
        i += 2;
        if (i == end)
            return JS_NAN;
        double v = 0;
        for (; i < end; i++) {
            uint16_t u = units[i];
            int dv;
            if (u >= '0' && u <= '9') dv = u - '0';
            else if (u >= 'a' && u <= 'f') dv = u - 'a' + 10;
            else if (u >= 'A' && u <= 'F') dv = u - 'A' + 10;
            else return JS_NAN;
            if (dv >= radix)
                return JS_NAN;
            v = v * radix + dv;
        }
        return v;
    }

    uint64_t mant = 0;
    int digits = 0, exp_adjust = 0, exp_part = 0;
    bool any = false, dropped = false; /* digits past the 19th: the whole text decides the rounding */
    size_t num_start = i;
    while (i < end && units[i] >= '0' && units[i] <= '9') {
        if (digits < 19) {
            mant = mant * 10 + (uint64_t)(units[i] - '0');
            if (mant)
                digits++;
        } else {
            exp_adjust++;
            dropped = true;
        }
        i++;
        any = true;
    }
    if (i < end && units[i] == '.') {
        i++;
        while (i < end && units[i] >= '0' && units[i] <= '9') {
            if (digits < 19) {
                mant = mant * 10 + (uint64_t)(units[i] - '0');
                if (mant)
                    digits++;
                exp_adjust--;
            } else {
                dropped = true;
            }
            i++;
            any = true;
        }
    }
    if (!any)
        return JS_NAN;
    if (i < end && (units[i] == 'e' || units[i] == 'E')) {
        i++;
        bool eneg = false;
        if (i < end && (units[i] == '+' || units[i] == '-')) {
            eneg = units[i] == '-';
            i++;
        }
        if (i == end)
            return JS_NAN;
        while (i < end && units[i] >= '0' && units[i] <= '9') {
            if (exp_part < 100000)
                exp_part = exp_part * 10 + (units[i] - '0');
            i++;
        }
        if (eneg)
            exp_part = -exp_part;
    }
    if (i != end)
        return JS_NAN;
    double v = dropped ? js_k_decimal(units + num_start, end - num_start) : js_make_double(mant, exp_part + exp_adjust);
    return neg ? -v : v;
}

/* ---- double -> shortest decimal string ---- */

static size_t put_ascii(char *buf, size_t n, const char *s) {
    while (*s)
        buf[n++] = *s++;
    return n;
}

static size_t put_u64(char *buf, size_t n, uint64_t v) {
    char tmp[20];
    int t = 0;
    do {
        tmp[t++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (t)
        buf[n++] = tmp[--t];
    return n;
}

/*
 * Extracts a 17-significant-digit decimal approximation of |d|:
 * digits (as u64 in [1e16, 1e17)) and the exponent e10 such that
 * d ~= digits * 10^(e10-16).
 */
/*
 * The significant digits of d (> 0), correctly rounded by the C library: up to 26 digits as ASCII
 * into digs (NUL-terminated), and the decimal exponent of the first one. `%.*e` is one digit before
 * the point and the rest after; the separator is whatever the locale uses, so digits are picked out
 * by being digits rather than by position. Returns the number of digits (0 on failure).
 */
static int sci_digits(double d, int ndigits, char *digs, int *e10_out) {
    char tmp[64];
    int n = snprintf(tmp, sizeof tmp, "%.*e", ndigits - 1, d);
    if (n <= 0 || (size_t)n >= sizeof tmp)
        return 0;
    int seen = 0;
    const char *p = tmp;
    while (*p && *p != 'e' && *p != 'E') {
        if (*p >= '0' && *p <= '9' && seen < ndigits)
            digs[seen++] = *p;
        p++;
    }
    digs[seen] = 0;
    *e10_out = (*p) ? (int)strtol(p + 1, NULL, 10) : 0;
    return seen;
}

/*
 * The p-digit decimal nearest to d, as an integer and the exponent of its first digit, from the
 * 26-digit expansion in digs. Rounding that expansion again is only wrong when it lands exactly on
 * a p-digit midpoint (digits p.. are "5000…"): the 26-digit rounding may have moved the value onto
 * the midpoint from either side, or the value may truly be a tie (which goes to even). Only then
 * is the library asked for the correctly rounded p digits directly; otherwise digit p decides.
 *
 * Rounding the 17-digit string instead (what this did before) was the bug: about 1% of doubles
 * printed with the last digit one too high, or with 17 digits where 16 round-trip
 * (0.09983341664682816 for what every other engine prints as 0.09983341664682815).
 */
static uint64_t nearest_digits(double d, const char *digs, int nd, int e10, int p, int *e_out) {
    bool midpoint = p < nd && digs[p] == '5';
    for (int i = p + 1; midpoint && i < nd; i++)
        if (digs[i] != '0')
            midpoint = false;
    uint64_t D = 0;
    int e = e10;
    if (midpoint) {
        char exact[32];
        int got = sci_digits(d, p, exact, &e);
        for (int i = 0; i < got; i++)
            D = D * 10 + (uint64_t)(exact[i] - '0');
        *e_out = e;
        return D;
    }
    for (int i = 0; i < p; i++)
        D = D * 10 + (uint64_t)(i < nd ? digs[i] - '0' : 0);
    if (p < nd && digs[p] >= '5') {
        D++;
        uint64_t limit = 1;
        for (int i = 0; i < p; i++)
            limit *= 10;
        if (D >= limit) { /* 999.. rounded up to 1000.. */
            D /= 10;
            e++;
        }
    }
    *e_out = e;
    return D;
}

static bool round_trips(uint64_t D, int p, int e, uint64_t bits) {
    union { double dd; uint64_t u; } back;
    back.dd = js_make_double(D, e - p + 1);
    return back.u == bits;
}

/* The p-digit candidate that round-trips to bits, if any: the nearest to d, else (at a binade edge,
 * where the interval is wider above) the one above it. */
static bool shortest_at(double d, const char *digs, int nd, int e10, int p, bool binade_edge,
                        uint64_t bits, uint64_t *D_out, int *e_out) {
    int e;
    uint64_t Dr = nearest_digits(d, digs, nd, e10, p, &e);
    if (round_trips(Dr, p, e, bits)) {
        *D_out = Dr;
        *e_out = e;
        return true;
    }
    if (binade_edge) {
        uint64_t up = Dr + 1, limit = 1;
        int eu = e;
        for (int i = 0; i < p; i++)
            limit *= 10;
        if (up >= limit) { up /= 10; eu++; }
        if (round_trips(up, p, eu, bits)) {
            *D_out = up;
            *e_out = eu;
            return true;
        }
    }
    return false;
}

/* Formats digit string (drop trailing zeros) per Number::toString rules. */
static size_t format_digits(char *buf, size_t n, uint64_t D, int ndigits, int e10) {
    char digs[20];
    for (int i = ndigits - 1; i >= 0; i--) {
        digs[i] = (char)('0' + D % 10);
        D /= 10;
    }
    while (ndigits > 1 && digs[ndigits - 1] == '0')
        ndigits--;

    if (e10 >= ndigits - 1 && e10 < 21) {
        /* integer-like: digits then zeros */
        for (int i = 0; i < ndigits; i++)
            buf[n++] = digs[i];
        for (int i = ndigits - 1; i < e10; i++)
            buf[n++] = '0';
    } else if (e10 >= 0 && e10 < ndigits - 1) {
        for (int i = 0; i <= e10; i++)
            buf[n++] = digs[i];
        buf[n++] = '.';
        for (int i = e10 + 1; i < ndigits; i++)
            buf[n++] = digs[i];
    } else if (e10 < 0 && e10 > -7) {
        n = put_ascii(buf, n, "0.");
        for (int i = 0; i < -e10 - 1; i++)
            buf[n++] = '0';
        for (int i = 0; i < ndigits; i++)
            buf[n++] = digs[i];
    } else {
        /* exponential */
        buf[n++] = digs[0];
        if (ndigits > 1) {
            buf[n++] = '.';
            for (int i = 1; i < ndigits; i++)
                buf[n++] = digs[i];
        }
        buf[n++] = 'e';
        buf[n++] = e10 < 0 ? '-' : '+';
        n = put_u64(buf, n, (uint64_t)(e10 < 0 ? -e10 : e10));
    }
    return n;
}

/*
 * STATIC, and it matters beyond hygiene. An embedder that links lamassu beside
 * another JavaScript engine hits a duplicate-symbol error otherwise: QuickJS
 * exports a js_dtoa of its own, and ours was external for no reason — it is
 * used here and nowhere else. Comparing the two archives' symbol tables, this
 * was the ONLY name they had in common, so hiding it makes the two link
 * cleanly with no pre-link step at all.
 *
 * The workaround it replaces was `ld -r -unexported_symbol`, which is ld64
 * only. That made macOS the one platform an embedder could do this on.
 */
static size_t js_dtoa(double d, char *buf) {
    size_t n = 0;
    if (d != d)
        return put_ascii(buf, 0, "NaN");
    if (d == 0.0) {
        /* JS: (-0).toString() === "0" */
        return put_ascii(buf, 0, "0");
    }
    if (d < 0) {
        buf[n++] = '-';
        d = -d;
    }
    if (d > 1.7976931348623157e308)
        return put_ascii(buf, n, "Infinity");

    /* Exact fast path for safe integers (|d| < 2^53): the float-scaling dtoa
     * below loses precision once the scaled mantissa exceeds 2^53, so format
     * these by exact 64-bit integer division instead. */
    if (d < 9007199254740992.0 && d == (double)(uint64_t)d) {
        return put_u64(buf, n, (uint64_t)d);
    }

    union { double dd; uint64_t u; } orig;
    orig.dd = d;

    char digs[32];
    int e10;
    int nd = sci_digits(d, 26, digs, &e10);
    if (!nd)
        return put_ascii(buf, n, "NaN"); /* unreachable: snprintf of a finite double */
    /* An exact power of two sits at the edge of a binade: the gap to the next double below is half
     * the gap above, so the nearest p-digit decimal can fall outside the round-trip interval on the
     * narrow side while its neighbour fits on the wide side. That neighbour is the shortest form. */
    bool binade_edge = (orig.u & (((uint64_t)1 << 52) - 1)) == 0 && (orig.u >> 52) > 1;

    /* Shortest digits (1..17) that round-trip; of those, the nearest to d. Whether some p-digit
     * decimal round-trips only goes from false to true as p grows (p-digit decimals are (p+1)-digit
     * ones too), and 17 always does, so the shortest p is found by bisection: about four parses
     * instead of up to seventeen, which matters now that parsing is musl's exact scan rather than
     * the C library's strtod. At each p the candidates are the nearest, and at a binade edge the
     * one above it (see binade_edge). */
    int lo = 1, hi = 17;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int e;
        uint64_t D;
        if (shortest_at(d, digs, nd, e10, mid, binade_edge, orig.u, &D, &e))
            hi = mid;
        else
            lo = mid + 1;
    }
    {
        int e;
        uint64_t D;
        if (shortest_at(d, digs, nd, e10, lo, binade_edge, orig.u, &D, &e))
            return format_digits(buf, n, D, lo, e);
    }
    int e;
    uint64_t D17 = nearest_digits(d, digs, nd, e10, 17, &e);
    return format_digits(buf, n, D17, 17, e);
}

JsString *js_number_to_string(JsVm *vm, double d) {
    char buf[40];
    size_t len = js_dtoa(d, buf);
    uint16_t units[40];
    for (size_t i = 0; i < len; i++)
        units[i] = (uint16_t)(unsigned char)buf[i];
    return js_string_cell_new(vm, units, len);
}
