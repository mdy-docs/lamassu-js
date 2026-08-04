/*
 * Number <-> string conversions and integer coercions, freestanding (no
 * libm, no snprintf). Double formatting picks the shortest digit string
 * (1..17 significant digits) that round-trips through js_make_double; for
 * magnitudes within ~1e±22 both directions are single-rounded and results
 * are exact. Extreme magnitudes may be ~1 ulp off (TODO exact big-num
 * path); differential tests against Node will quantify this.
 */
#include "js_bytecode.h"

/* 10^0..10^22 are exactly representable doubles. */
static const double js_pow10[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

double js_make_double(uint64_t mant, int exp10) {
    if (mant == 0)
        return 0.0;
    if (mant <= ((uint64_t)1 << 53)) {
        if (exp10 >= 0 && exp10 <= 22)
            return (double)mant * js_pow10[exp10];
        if (exp10 < 0 && exp10 >= -22)
            return (double)mant / js_pow10[-exp10];
    }
    double d = (double)mant;
    int e = exp10;
    while (e > 22) {
        d *= 1e22;
        e -= 22;
        if (d > 1.7e308)
            return __builtin_inf();
    }
    while (e < -22) {
        d /= 1e22;
        e += 22;
        if (d == 0.0)
            return 0.0;
    }
    return e >= 0 ? d * js_pow10[e] : d / js_pow10[-e];
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
    bool any = false;
    while (i < end && units[i] >= '0' && units[i] <= '9') {
        if (digits < 19) {
            mant = mant * 10 + (uint64_t)(units[i] - '0');
            if (mant)
                digits++;
        } else {
            exp_adjust++;
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
    double v = js_make_double(mant, exp_part + exp_adjust);
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
static void extract_digits(double d, uint64_t *digits, int *e10_out) {
    /*
     * Estimate the decimal exponent from the binary one, then scale with a
     * single exact-power multiply/divide when |k| <= 22 (one rounding).
     * The adjust loops fix estimate error (at most one step).
     */
    union { double dd; uint64_t u; } p;
    p.dd = d;
    int e2 = (int)((p.u >> 52) & 0x7FF) - 1023;
    int e10 = (int)((double)e2 * 0.30102999566398119);
    int k = 16 - e10;
    double scaled = d;
    while (k > 22) {
        scaled *= 1e22;
        k -= 22;
    }
    while (k < -22) {
        scaled /= 1e22;
        k += 22;
    }
    if (k >= 0)
        scaled *= js_pow10[k];
    else
        scaled /= js_pow10[-k];
    while (scaled >= 1e17) {
        scaled /= 10;
        e10 += 1;
    }
    while (scaled < 1e16) {
        scaled *= 10;
        e10 -= 1;
    }
    uint64_t D = (uint64_t)(scaled + 0.5);
    if (D >= UINT64_C(100000000000000000)) { /* rounded up to 1e17 */
        D /= 10;
        e10 += 1;
    }
    *digits = D;
    *e10_out = e10;
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

size_t js_dtoa(double d, char *buf) {
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

    uint64_t D17;
    int e10;
    extract_digits(d, &D17, &e10);

    /* Shortest digits (1..17) that round-trip; verify by parsing back. */
    for (int prec = 1; prec <= 17; prec++) {
        uint64_t scale = 1;
        for (int i = 0; i < 17 - prec; i++)
            scale *= 10;
        uint64_t Dr = (D17 + scale / 2) / scale; /* round half up */
        int e = e10;
        /* rounding may carry into an extra digit (e.g. 999.. -> 100..) */
        uint64_t limit = 1;
        for (int i = 0; i < prec; i++)
            limit *= 10;
        if (Dr >= limit) {
            Dr /= 10;
            e += 1;
        }
        union { double dd; uint64_t u; } back;
        back.dd = js_make_double(Dr, e - prec + 1);
        if (back.u == orig.u)
            return format_digits(buf, n, Dr, prec, e);
    }
    return format_digits(buf, n, D17, 17, e10);
}

JsString *js_number_to_string(JsVm *vm, double d) {
    char buf[40];
    size_t len = js_dtoa(d, buf);
    uint16_t units[40];
    for (size_t i = 0; i < len; i++)
        units[i] = (uint16_t)(unsigned char)buf[i];
    return js_string_cell_new(vm, units, len);
}
