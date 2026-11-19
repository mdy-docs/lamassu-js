/*
 * BigInt: see js_bigint.h. Magnitudes are arrays of 32-bit limbs, least significant first; the
 * schoolbook algorithms (Knuth's algorithm D for division) over them, with scratch buffers from
 * the VM's allocator and one cell for each result.
 */
#include "js_bigint.h"
#include "js_bytecode.h"
#include "js_error.h"
#include <string.h>

/* V8's limit: 2^30 bits */
#define BI_MAX_LIMBS (1u << 25)

size_t js_bigint_cell_size(const JsBigInt *b) {
    return sizeof(JsBigInt) + (size_t)b->cap * sizeof(uint32_t);
}

static bool too_big(JsContext *ctx) {
    js_set_error(ctx, js_error_fmt(ctx, "RangeError: Maximum BigInt size exceeded"));
    return false;
}

static JsBigInt *bi_alloc(JsContext *ctx, uint64_t cap) {
    if (cap > BI_MAX_LIMBS) {
        too_big(ctx);
        return NULL;
    }
    JsGcCell *c = js_gc_new_cell(ctx->vm, JS_KIND_BIGINT, sizeof(JsBigInt) + (size_t)cap * sizeof(uint32_t));
    if (!c)
        return NULL;
    JsBigInt *b = (JsBigInt *)c;
    b->len = 0;
    b->cap = (uint32_t)cap;
    b->neg = false;
    return b;
}

static JsBigInt *bi_finish(JsBigInt *b) {
    while (b->len && !b->d[b->len - 1])
        b->len--;
    if (!b->len)
        b->neg = false;
    return b;
}

/* A new BigInt of the magnitude M (N limbs) with sign NEG */
static JsBigInt *bi_make(JsContext *ctx, const uint32_t *m, uint32_t n, bool neg) {
    while (n && !m[n - 1])
        n--;
    JsBigInt *b = bi_alloc(ctx, n);
    if (!b)
        return NULL;
    if (n)
        memcpy(b->d, m, n * sizeof *m);
    b->len = n;
    b->neg = neg && n;
    return b;
}

JsBigInt *js_bigint_from_limbs(JsContext *ctx, const uint32_t *m, uint32_t n, bool neg) {
    return bi_make(ctx, m, n, neg);
}

static uint32_t *scratch(JsContext *ctx, uint64_t n) {
    if (n > BI_MAX_LIMBS + 2) {
        too_big(ctx);
        return NULL;
    }
    uint32_t *p = js_realloc_raw(ctx->vm, NULL, 0, (size_t)(n ? n : 1) * sizeof *p);
    if (p)
        memset(p, 0, (size_t)(n ? n : 1) * sizeof *p);
    return p;
}

static void scratch_free(JsContext *ctx, uint32_t *p, uint64_t n) {
    js_realloc_raw(ctx->vm, p, (size_t)(n ? n : 1) * sizeof *p, 0);
}

/* ---- magnitudes ---- */

static int mag_cmp(const uint32_t *a, uint32_t an, const uint32_t *b, uint32_t bn) {
    while (an && !a[an - 1])
        an--;
    while (bn && !b[bn - 1])
        bn--;
    if (an != bn)
        return an < bn ? -1 : 1;
    for (uint32_t i = an; i-- > 0;)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* OUT (max(an, bn) + 1 limbs) = A + B */
static void mag_add(uint32_t *out, const uint32_t *a, uint32_t an, const uint32_t *b, uint32_t bn) {
    if (an < bn) {
        const uint32_t *t = a; a = b; b = t;
        uint32_t tn = an; an = bn; bn = tn;
    }
    uint64_t carry = 0;
    for (uint32_t i = 0; i < an; i++) {
        carry += (uint64_t)a[i] + (i < bn ? b[i] : 0);
        out[i] = (uint32_t)carry;
        carry >>= 32;
    }
    out[an] = (uint32_t)carry;
}

/* OUT (an limbs) = A - B, A >= B */
static void mag_sub(uint32_t *out, const uint32_t *a, uint32_t an, const uint32_t *b, uint32_t bn) {
    int64_t borrow = 0;
    for (uint32_t i = 0; i < an; i++) {
        int64_t t = (int64_t)a[i] - (i < bn ? b[i] : 0) - borrow;
        borrow = t < 0;
        out[i] = (uint32_t)(t + (borrow ? ((int64_t)1 << 32) : 0));
    }
}

/* ---- fuel ----
 *
 * Multiplication, division, parsing and printing are quadratic in the length, and a BigInt may
 * have 2^25 limbs, so each pays for its work as it goes, at about what the time would buy in
 * instructions (one is about 2.5 ns), and reads the interrupt between batches. The meter counts
 * quarter-instructions, so each inner step can be charged its measured cost:
 *   a multiplication's limb product  0.7 ns: 1 quarter
 *   a parse's limb step              0.5 ns: 1 quarter
 *   a division's limb step           0.9 ns: 2 quarters
 *   a printing division's limb       8 ns:   12 quarters (a 64-by-32-bit hardware division)
 * (measured: within 1.3 times of the interpreter's own rate of fuel per millisecond) */
#define Q_MUL 1
#define Q_DIV 2
#define Q_PARSE 1
#define Q_PRINT 12
#define METER_BATCH (4u * 16384) /* about 40 us of work between charges */

typedef struct {
    JsContext *ctx;
    uint64_t q; /* quarters not yet charged */
} Meter;

/* Q quarters' more work; false with the budget's or the interrupt's error pending */
static bool meter(Meter *m, uint64_t q) {
    m->q += q;
    if (m->q < METER_BATCH)
        return true;
    uint64_t n = m->q / 4;
    m->q %= 4;
    return js_native_charge_n(m->ctx, n);
}

/* what is left, rounded up; less than 16 instructions' worth (the arithmetic of numbers of a few
 * limbs, the common case) goes with the instruction that asked for it */
static bool meter_end(Meter *m) {
    uint64_t n = (m->q + 3) / 4;
    m->q = 0;
    return n < 16 || js_native_charge_n(m->ctx, n);
}

/* OUT (an + bn limbs, zeroed) = A * B. False with the budget's or the interrupt's error pending. */
static bool mag_mul(JsContext *ctx, uint32_t *out, const uint32_t *a, uint32_t an, const uint32_t *b,
                    uint32_t bn) {
    Meter m = {ctx, 0};
    for (uint32_t i = 0; i < an; i++) {
        uint64_t carry = 0, ai = a[i];
        if (!meter(&m, (uint64_t)bn * Q_MUL))
            return false;
        if (!ai)
            continue;
        for (uint32_t j = 0; j < bn; j++) {
            carry += ai * b[j] + out[i + j];
            out[i + j] = (uint32_t)carry;
            carry >>= 32;
        }
        out[i + bn] = (uint32_t)carry;
    }
    return meter_end(&m);
}

/* A (n limbs) = A / D in place; the remainder */
static uint32_t mag_divmod_small(uint32_t *a, uint32_t n, uint32_t d) {
    uint64_t r = 0;
    for (uint32_t i = n; i-- > 0;) {
        uint64_t cur = (r << 32) | a[i];
        a[i] = (uint32_t)(cur / d);
        r = cur % d;
    }
    return (uint32_t)r;
}

static int clz32(uint32_t x) {
    int n = 0;
    if (!x)
        return 32;
    while (!(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return n;
}

/* Q (an - bn + 1 limbs, or NULL) and R (bn limbs, or NULL) of A / B, B nonzero with bn > 1
 * significant limbs: Knuth's algorithm D. False with the budget's or the interrupt's error
 * pending, or none: out of memory. */
static bool mag_divmod(JsContext *ctx, uint32_t *q, uint32_t *r, const uint32_t *a, uint32_t an,
                       const uint32_t *b, uint32_t bn) {
    int s = clz32(b[bn - 1]);
    uint32_t *u = scratch(ctx, (uint64_t)an + 1), *v = scratch(ctx, bn);
    if (!u || !v) {
        if (u)
            scratch_free(ctx, u, (uint64_t)an + 1);
        if (v)
            scratch_free(ctx, v, bn);
        return false;
    }
    for (uint32_t i = bn; i-- > 0;)
        v[i] = (b[i] << s) | (s && i ? b[i - 1] >> (32 - s) : 0);
    u[an] = s ? a[an - 1] >> (32 - s) : 0;
    for (uint32_t i = an; i-- > 0;)
        u[i] = (a[i] << s) | (s && i ? a[i - 1] >> (32 - s) : 0);
    Meter m = {ctx, 0};
    bool ok = true;
    for (uint32_t j = an - bn + 1; j-- > 0;) {
        if (!(ok = meter(&m, (uint64_t)bn * Q_DIV)))
            break;
        uint64_t num = ((uint64_t)u[j + bn] << 32) | u[j + bn - 1];
        uint64_t qhat = num / v[bn - 1], rhat = num % v[bn - 1];
        while (qhat >= ((uint64_t)1 << 32) ||
               qhat * v[bn - 2] > ((rhat << 32) | u[j + bn - 2])) {
            qhat--;
            rhat += v[bn - 1];
            if (rhat >= ((uint64_t)1 << 32))
                break;
        }
        int64_t borrow = 0;
        uint64_t carry = 0;
        for (uint32_t i = 0; i < bn; i++) {
            uint64_t p = qhat * v[i] + carry;
            carry = p >> 32;
            int64_t t = (int64_t)u[i + j] - (int64_t)(uint32_t)p - borrow;
            borrow = t < 0;
            u[i + j] = (uint32_t)(t + (borrow ? ((int64_t)1 << 32) : 0));
        }
        int64_t t = (int64_t)u[j + bn] - (int64_t)carry - borrow;
        borrow = t < 0;
        u[j + bn] = (uint32_t)(t + (borrow ? ((int64_t)1 << 32) : 0));
        if (borrow) { /* qhat was one too many: add back */
            qhat--;
            uint64_t c = 0;
            for (uint32_t i = 0; i < bn; i++) {
                c += (uint64_t)u[i + j] + v[i];
                u[i + j] = (uint32_t)c;
                c >>= 32;
            }
            u[j + bn] += (uint32_t)c;
        }
        if (q)
            q[j] = (uint32_t)qhat;
    }
    ok = ok && meter_end(&m);
    if (ok && r) /* the remainder is in u[0 .. bn], still shifted left by s */
        for (uint32_t i = 0; i < bn; i++)
            r[i] = s ? (u[i] >> s) | (uint32_t)((uint64_t)u[i + 1] << (32 - s)) : u[i];
    scratch_free(ctx, u, (uint64_t)an + 1);
    scratch_free(ctx, v, bn);
    return ok;
}

/* M[e/32 ..] = MANT << E (MANT < 2^53): three limbs */
static void put_shifted(uint32_t *m, uint64_t mant, uint32_t e) {
    uint32_t w = e / 32, sh = e % 32;
    uint64_t lo = mant << sh, hi = sh ? mant >> (64 - sh) : 0;
    m[w] = (uint32_t)lo;
    m[w + 1] = (uint32_t)(lo >> 32);
    m[w + 2] = (uint32_t)hi;
}

static uint64_t bit_length(const JsBigInt *a) {
    return a->len ? (uint64_t)a->len * 32 - (uint64_t)clz32(a->d[a->len - 1]) : 0;
}

/* ---- construction and conversion ---- */

JsBigInt *js_bigint_from_int64(JsContext *ctx, int64_t v) {
    uint64_t m = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    uint32_t limbs[2] = {(uint32_t)m, (uint32_t)(m >> 32)};
    return bi_make(ctx, limbs, 2, v < 0);
}

JsBigInt *js_bigint_from_double(JsContext *ctx, double d) {
    bool neg = d < 0;
    if (neg)
        d = -d;
    if (d < 18446744073709551616.0) {
        uint64_t m = (uint64_t)d;
        uint32_t limbs[2] = {(uint32_t)m, (uint32_t)(m >> 32)};
        return bi_make(ctx, limbs, 2, neg);
    }
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    int e = (int)((bits >> 52) & 0x7FF) - 1075; /* d = mant * 2^e, e >= 12 here */
    uint64_t mant = (bits & ((UINT64_C(1) << 52) - 1)) | (UINT64_C(1) << 52);
    uint32_t n = (uint32_t)((e + 53) / 32 + 2);
    uint32_t *m = scratch(ctx, n);
    if (!m)
        return NULL;
    put_shifted(m, mant, (uint32_t)e);
    JsBigInt *b = bi_make(ctx, m, n, neg);
    scratch_free(ctx, m, n);
    return b;
}

double js_bigint_to_double(const JsBigInt *b) {
    uint64_t n = bit_length(b);
    double r;
    if (n <= 64) {
        uint64_t m = (uint64_t)(b->len > 0 ? b->d[0] : 0) | ((uint64_t)(b->len > 1 ? b->d[1] : 0) << 32);
        r = (double)m;
    } else {
        /* the top 64 bits, the rest as a sticky bit: one correct rounding to 53 bits */
        uint64_t top = 0;
        for (uint64_t i = 0; i < 64; i++) {
            uint64_t bit = n - 1 - i;
            top = (top << 1) | ((b->d[bit / 32] >> (bit % 32)) & 1);
        }
        bool sticky = false;
        for (uint64_t bit = 0; bit < n - 64 && !sticky; bit++)
            sticky = (b->d[bit / 32] >> (bit % 32)) & 1;
        if (sticky)
            top |= 1;
        r = (double)top;
        int shift = (int)(n - 64);
        r = shift > 2000 ? __builtin_inf() : __builtin_ldexp(r, shift);
    }
    return b->neg ? -r : r;
}

bool js_bigint_is_zero(const JsBigInt *b) {
    return b->len == 0;
}

int js_bigint_cmp(const JsBigInt *a, const JsBigInt *b) {
    if (a->neg != b->neg)
        return a->neg ? -1 : 1;
    int c = mag_cmp(a->d, a->len, b->d, b->len);
    return a->neg ? -c : c;
}

bool js_bigint_equal(const JsBigInt *a, const JsBigInt *b) {
    return a->neg == b->neg && mag_cmp(a->d, a->len, b->d, b->len) == 0;
}

uint32_t js_bigint_hash(const JsBigInt *b) {
    uint32_t h = b->neg ? 0x9E3779B9u : 0x85EBCA6Bu;
    for (uint32_t i = 0; i < b->len; i++)
        h = (h ^ b->d[i]) * 16777619u;
    return h;
}

int js_bigint_cmp_double(const JsBigInt *a, double d) {
    if (d != d)
        return 2;
    if (d == __builtin_inf())
        return -1;
    if (d == -__builtin_inf())
        return 1;
    bool dneg = d < 0;
    if (a->neg != dneg && !(a->len == 0 && d == 0))
        return (a->len == 0) ? (dneg ? 1 : -1) : (a->neg ? -1 : 1);
    double ad = dneg ? -d : d, t = __builtin_trunc(ad);
    /* |a| against trunc(|d|), exactly: the double's integer part as limbs */
    uint32_t m[40] = {0};
    uint32_t mn = 0;
    if (t > 0) {
        uint64_t bits;
        memcpy(&bits, &t, sizeof bits);
        int e = (int)((bits >> 52) & 0x7FF) - 1075;
        uint64_t mant = (bits & ((UINT64_C(1) << 52) - 1)) | (UINT64_C(1) << 52);
        if (e < 0) {
            mant >>= -e;
            e = 0;
        }
        put_shifted(m, mant, (uint32_t)e);
        mn = (uint32_t)e / 32 + 3;
    }
    int c = mag_cmp(a->d, a->len, m, mn);
    if (c == 0 && ad != t)
        c = -1; /* |a| == trunc(|d|) < |d| */
    return dneg ? -c : c;
}

/* ---- text ---- */

static int digit_val(uint16_t u) {
    if (u >= '0' && u <= '9')
        return u - '0';
    if (u >= 'a' && u <= 'z')
        return u - 'a' + 10;
    if (u >= 'A' && u <= 'Z')
        return u - 'A' + 10;
    return 99;
}

JsBigInt *js_bigint_parse(JsContext *ctx, const uint16_t *u, uint32_t n, int radix, bool *bad) {
    *bad = false;
    bool neg = false;
    if (n && u[0] == '-') {
        neg = true;
        u++;
        n--;
    }
    if (!n) {
        *bad = true;
        return NULL;
    }
    for (uint32_t i = 0; i < n; i++)
        if (digit_val(u[i]) >= radix) {
            *bad = true;
            return NULL;
        }
    uint32_t chunk_digits = 1;
    uint64_t chunk_mul = (uint64_t)radix;
    while (chunk_mul * (uint64_t)radix <= 0xFFFFFFFFu) {
        chunk_mul *= (uint64_t)radix;
        chunk_digits++;
    }
    uint64_t cap = (uint64_t)n * 6 / 32 + 2; /* log2(36) < 6 */
    uint32_t *m = scratch(ctx, cap);
    if (!m)
        return NULL;
    uint32_t mn = 0;
    Meter mt = {ctx, 0};
    for (uint32_t i = 0; i < n;) {
        if (!meter(&mt, ((uint64_t)mn + 1) * Q_PARSE)) { /* each chunk multiplies every limb so far */
            scratch_free(ctx, m, cap);
            return NULL;
        }
        uint32_t k = n - i < chunk_digits ? n - i : chunk_digits;
        uint64_t mul = 1, val = 0;
        for (uint32_t j = 0; j < k; j++, i++) {
            val = val * (uint64_t)radix + (uint64_t)digit_val(u[i]);
            mul *= (uint64_t)radix;
        }
        uint64_t carry = val;
        for (uint32_t j = 0; j < mn; j++) {
            carry += (uint64_t)m[j] * mul;
            m[j] = (uint32_t)carry;
            carry >>= 32;
        }
        if (carry)
            m[mn++] = (uint32_t)carry;
    }
    JsBigInt *b = meter_end(&mt) ? bi_make(ctx, m, mn, neg) : NULL;
    scratch_free(ctx, m, cap);
    return b;
}

static bool str_ws(uint16_t c) {
    return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000 || c == 0xFEFF;
}

JsBigInt *js_bigint_from_string(JsContext *ctx, const JsString *s, bool *bad) {
    const uint16_t *u = s->units;
    uint32_t n = s->length;
    while (n && str_ws(u[0])) {
        u++;
        n--;
    }
    while (n && str_ws(u[n - 1]))
        n--;
    *bad = false;
    if (!n)
        return js_bigint_from_int64(ctx, 0);
    if (n > 2 && u[0] == '0') {
        int radix = u[1] == 'x' || u[1] == 'X' ? 16 : u[1] == 'o' || u[1] == 'O' ? 8
                    : u[1] == 'b' || u[1] == 'B' ? 2 : 0;
        if (radix) {
            if (u[2] == '-') { /* no sign after a prefix */
                *bad = true;
                return NULL;
            }
            return js_bigint_parse(ctx, u + 2, n - 2, radix, bad);
        }
    }
    if (u[0] == '+') {
        if (n == 1 || u[1] == '-' || u[1] == '+') {
            *bad = true;
            return NULL;
        }
        return js_bigint_parse(ctx, u + 1, n - 1, 10, bad);
    }
    if (u[0] == '-' && (n == 1 || u[1] == '-' || u[1] == '+')) {
        *bad = true;
        return NULL;
    }
    return js_bigint_parse(ctx, u, n, 10, bad);
}

/* ceil(log2(r)): a number of BITS bits has at least (BITS - 1) / log2(r) + 1 digits in radix r,
 * and dividing by ceil(log2(r)) instead only lowers that bound, so a number it puts past the
 * string limit surely is past it. */
static unsigned ceil_log2(int r) {
    unsigned k = 0;
    while ((1 << k) < r)
        k++;
    return k;
}

JsString *js_bigint_to_string(JsContext *ctx, const JsBigInt *b, int radix) {
    static const char digs[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    JsVm *vm = ctx->vm;
    if (!b->len)
        return js_ascii_cell(vm, "0");
    /* the digits at least, sign aside: refused before the conversion */
    if ((bit_length(b) - 1) / ceil_log2(radix) + 1 > JS_MAX_STRING_UNITS) {
        js_set_error(ctx, js_error_from_ascii(ctx, JS_STRING_LENGTH_ERROR));
        return NULL;
    }
    uint32_t chunk_digits = 1;
    uint64_t chunk_div = (uint64_t)radix;
    while (chunk_div * (uint64_t)radix <= 0xFFFFFFFFu) {
        chunk_div *= (uint64_t)radix;
        chunk_digits++;
    }
    uint64_t max_digits = bit_length(b) + 2; /* radix 2 is the longest */
    uint32_t *m = js_realloc_raw(vm, NULL, 0, (size_t)b->len * sizeof *m);
    uint16_t *out = m ? js_realloc_raw(vm, NULL, 0, (size_t)(max_digits + chunk_digits + 1) * sizeof *out) : NULL;
    if (!out) {
        if (m)
            js_realloc_raw(vm, m, (size_t)b->len * sizeof *m, 0);
        return NULL;
    }
    memcpy(m, b->d, b->len * sizeof *m);
    uint32_t mn = b->len;
    size_t pos = max_digits + chunk_digits + 1, total = pos;
    Meter mt = {ctx, 0};
    bool ok = true;
    while (mn) {
        /* quadratic in the length: each pass divides every limb left */
        if (!(ok = meter(&mt, (uint64_t)mn * Q_PRINT)))
            break;
        uint32_t rem = mag_divmod_small(m, mn, (uint32_t)chunk_div);
        while (mn && !m[mn - 1])
            mn--;
        for (uint32_t j = 0; j < chunk_digits; j++) {
            out[--pos] = (uint16_t)digs[rem % (uint32_t)radix];
            rem /= (uint32_t)radix;
            if (!mn && !rem)
                break;
        }
    }
    ok = ok && meter_end(&mt);
    if (ok && b->neg)
        out[--pos] = '-';
    if (ok && total - pos > JS_MAX_STRING_UNITS) {
        js_set_error(ctx, js_error_from_ascii(ctx, JS_STRING_LENGTH_ERROR));
        ok = false;
    }
    JsString *s = ok ? js_string_cell_new(vm, out + pos, total - pos) : NULL;
    js_realloc_raw(vm, out, total * sizeof *out, 0);
    js_realloc_raw(vm, m, (size_t)b->len * sizeof *m, 0);
    return s;
}

/* ---- arithmetic ---- */

/* A + B with signs: SUBTRACT flips B's */
static JsBigInt *add_signed(JsContext *ctx, const JsBigInt *a, const JsBigInt *b, bool subtract) {
    bool bneg = subtract ? (b->len && !b->neg) : b->neg;
    uint32_t n = (a->len > b->len ? a->len : b->len) + 1;
    JsBigInt *r = bi_alloc(ctx, n);
    if (!r)
        return NULL;
    if (a->neg == bneg) {
        mag_add(r->d, a->d, a->len, b->d, b->len);
        r->len = n;
        r->neg = a->neg;
    } else if (mag_cmp(a->d, a->len, b->d, b->len) >= 0) {
        mag_sub(r->d, a->d, a->len, b->d, b->len);
        r->len = a->len;
        r->neg = a->neg;
    } else {
        mag_sub(r->d, b->d, b->len, a->d, a->len);
        r->len = b->len;
        r->neg = bneg;
    }
    return bi_finish(r);
}

static JsBigInt *mul_signed(JsContext *ctx, const JsBigInt *a, const JsBigInt *b) {
    if (!a->len || !b->len)
        return js_bigint_from_int64(ctx, 0);
    JsBigInt *r = bi_alloc(ctx, (uint64_t)a->len + b->len);
    if (!r)
        return NULL;
    memset(r->d, 0, ((size_t)a->len + b->len) * sizeof(uint32_t));
    if (!mag_mul(ctx, r->d, a->d, a->len, b->d, b->len))
        return NULL;
    r->len = a->len + b->len;
    r->neg = a->neg != b->neg;
    return bi_finish(r);
}

/* A / B (truncated) or A % B (the dividend's sign) */
static JsBigInt *divmod_signed(JsContext *ctx, const JsBigInt *a, const JsBigInt *b, bool want_rem) {
    if (!b->len) {
        js_set_error(ctx, js_error_fmt(ctx, "RangeError: Division by zero"));
        return NULL;
    }
    if (mag_cmp(a->d, a->len, b->d, b->len) < 0)
        return want_rem ? bi_make(ctx, a->d, a->len, a->neg) : js_bigint_from_int64(ctx, 0);
    if (b->len == 1) {
        JsBigInt *r = bi_alloc(ctx, a->len);
        if (!r)
            return NULL;
        memcpy(r->d, a->d, a->len * sizeof(uint32_t));
        uint32_t rem = mag_divmod_small(r->d, a->len, b->d[0]);
        if (want_rem) {
            r->d[0] = rem;
            r->len = 1;
            r->neg = a->neg;
        } else {
            r->len = a->len;
            r->neg = a->neg != b->neg;
        }
        return bi_finish(r);
    }
    JsBigInt *r = bi_alloc(ctx, want_rem ? b->len : a->len - b->len + 1);
    if (!r)
        return NULL;
    if (!mag_divmod(ctx, want_rem ? NULL : r->d, want_rem ? r->d : NULL, a->d, a->len, b->d, b->len))
        return NULL;
    r->len = r->cap;
    r->neg = want_rem ? a->neg : a->neg != b->neg;
    return bi_finish(r);
}

static JsBigInt *pow_signed(JsContext *ctx, const JsBigInt *a, const JsBigInt *b) {
    if (b->neg) {
        js_set_error(ctx, js_error_fmt(ctx, "RangeError: Exponent must be positive"));
        return NULL;
    }
    if (!b->len)
        return js_bigint_from_int64(ctx, 1);
    if (!a->len || (a->len == 1 && a->d[0] == 1)) /* 0, 1, -1 */
        return bi_make(ctx, a->d, a->len, a->neg && (b->d[0] & 1));
    if (b->len > 1 || (uint64_t)b->d[0] * bit_length(a) > (uint64_t)BI_MAX_LIMBS * 32) {
        too_big(ctx);
        return NULL;
    }
    uint32_t e = b->d[0];
    uint64_t cap = (bit_length(a) * e) / 32 + 2;
    uint32_t *res = scratch(ctx, cap), *base = scratch(ctx, cap), *tmp = scratch(ctx, cap * 2);
    JsBigInt *r = NULL;
    bool ok = res && base && tmp;
    if (ok) {
        uint32_t rn = 1, bn = a->len;
        res[0] = 1;
        memcpy(base, a->d, a->len * sizeof *base);
        for (;;) {
            if (e & 1) {
                memset(tmp, 0, (size_t)(rn + bn) * sizeof *tmp);
                if (!(ok = mag_mul(ctx, tmp, res, rn, base, bn)))
                    break;
                rn = rn + bn;
                while (rn > 1 && !tmp[rn - 1])
                    rn--;
                memcpy(res, tmp, rn * sizeof *res);
            }
            e >>= 1;
            if (!e)
                break;
            memset(tmp, 0, (size_t)(2 * bn) * sizeof *tmp);
            if (!(ok = mag_mul(ctx, tmp, base, bn, base, bn)))
                break;
            bn = 2 * bn;
            while (bn > 1 && !tmp[bn - 1])
                bn--;
            memcpy(base, tmp, bn * sizeof *base);
        }
        if (ok)
            r = bi_make(ctx, res, rn, a->neg && (b->d[0] & 1));
    }
    if (res)
        scratch_free(ctx, res, cap);
    if (base)
        scratch_free(ctx, base, cap);
    if (tmp)
        scratch_free(ctx, tmp, cap * 2);
    return r;
}

/* A << S (S may be negative: a right shift, rounding down, as BigInt's >> does) */
static JsBigInt *shift_signed(JsContext *ctx, const JsBigInt *a, const JsBigInt *s, bool right) {
    bool left = s->neg ? right : !right;
    if (!a->len || !s->len)
        return bi_make(ctx, a->d, a->len, a->neg);
    if (left) {
        if (s->len > 1 || (uint64_t)s->d[0] + bit_length(a) > (uint64_t)BI_MAX_LIMBS * 32) {
            too_big(ctx);
            return NULL;
        }
        uint32_t sh = s->d[0], w = sh / 32, bsh = sh % 32;
        JsBigInt *r = bi_alloc(ctx, (uint64_t)a->len + w + 1);
        if (!r)
            return NULL;
        memset(r->d, 0, (size_t)r->cap * sizeof(uint32_t));
        for (uint32_t i = 0; i < a->len; i++) {
            uint64_t x = (uint64_t)a->d[i] << bsh;
            r->d[i + w] |= (uint32_t)x;
            r->d[i + w + 1] |= (uint32_t)(x >> 32);
        }
        r->len = r->cap;
        r->neg = a->neg;
        return bi_finish(r);
    }
    /* right: floor(a / 2^s) */
    uint64_t sh = s->len > 1 ? UINT64_MAX : s->d[0];
    if (sh >= bit_length(a))
        return js_bigint_from_int64(ctx, a->neg ? -1 : 0);
    uint32_t w = (uint32_t)(sh / 32), bsh = (uint32_t)(sh % 32);
    /* a negative a: -((|a| - 1) >> s) - 1 */
    uint32_t *m = scratch(ctx, a->len);
    if (!m)
        return NULL;
    memcpy(m, a->d, a->len * sizeof *m);
    if (a->neg)
        for (uint32_t i = 0; i < a->len && m[i]-- == 0; i++) /* |a| - 1 */
            ;
    JsBigInt *r = bi_alloc(ctx, a->len - w + 1);
    if (r) {
        memset(r->d, 0, (size_t)r->cap * sizeof(uint32_t));
        for (uint32_t i = w; i < a->len; i++) {
            uint64_t x = ((uint64_t)(i + 1 < a->len ? m[i + 1] : 0) << 32) | m[i];
            r->d[i - w] = (uint32_t)(x >> bsh);
        }
        r->len = a->len - w;
        if (a->neg) { /* + 1, negated */
            uint32_t i = 0;
            while (i < r->len && ++r->d[i] == 0)
                i++;
            if (i == r->len)
                r->d[r->len++] = 1;
            r->neg = true;
        }
        bi_finish(r);
    }
    scratch_free(ctx, m, a->len);
    return r;
}

/* two's complement of A in N limbs (A's magnitude fits in N - 1) */
static void twos(uint32_t *out, const JsBigInt *a, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        out[i] = i < a->len ? a->d[i] : 0;
    if (a->neg) {
        for (uint32_t i = 0; i < n; i++)
            out[i] = ~out[i];
        for (uint32_t i = 0; i < n && ++out[i] == 0; i++)
            ;
    }
}

static JsBigInt *bitwise(JsContext *ctx, int op, const JsBigInt *a, const JsBigInt *b) {
    uint32_t n = (a->len > b->len ? a->len : b->len) + 1;
    uint32_t *x = scratch(ctx, n), *y = scratch(ctx, n);
    JsBigInt *r = NULL;
    if (x && y) {
        twos(x, a, n);
        twos(y, b, n);
        for (uint32_t i = 0; i < n; i++)
            x[i] = op == JS_OP_BITAND ? x[i] & y[i] : op == JS_OP_BITOR ? x[i] | y[i] : x[i] ^ y[i];
        bool neg = (x[n - 1] >> 31) & 1;
        if (neg) { /* back to sign and magnitude */
            for (uint32_t i = 0; i < n; i++)
                x[i] = ~x[i];
            for (uint32_t i = 0; i < n && ++x[i] == 0; i++)
                ;
        }
        r = bi_make(ctx, x, n, neg);
    }
    if (x)
        scratch_free(ctx, x, n);
    if (y)
        scratch_free(ctx, y, n);
    return r;
}

JsBigInt *js_bigint_binary(JsContext *ctx, int op, const JsBigInt *a, const JsBigInt *b) {
    switch (op) {
    case JS_OP_ADD: return add_signed(ctx, a, b, false);
    case JS_OP_SUB: return add_signed(ctx, a, b, true);
    case JS_OP_MUL: return mul_signed(ctx, a, b);
    case JS_OP_DIV: return divmod_signed(ctx, a, b, false);
    case JS_OP_MOD: return divmod_signed(ctx, a, b, true);
    case JS_OP_POW: return pow_signed(ctx, a, b);
    case JS_OP_SHL: return shift_signed(ctx, a, b, false);
    case JS_OP_SHR: return shift_signed(ctx, a, b, true);
    case JS_OP_USHR:
        js_set_error(ctx, js_error_fmt(ctx, "TypeError: BigInts have no unsigned right shift, use >> instead"));
        return NULL;
    default: return bitwise(ctx, op, a, b);
    }
}

JsBigInt *js_bigint_unary(JsContext *ctx, int op, const JsBigInt *a) {
    if (op == JS_OP_NEG)
        return bi_make(ctx, a->d, a->len, !a->neg);
    /* ~a = -a - 1 */
    uint32_t n = a->len + 1;
    JsBigInt *r = bi_alloc(ctx, n);
    if (!r)
        return NULL;
    memset(r->d, 0, n * sizeof(uint32_t));
    if (a->neg) { /* -a - 1 = |a| - 1 */
        memcpy(r->d, a->d, a->len * sizeof(uint32_t));
        for (uint32_t i = 0; i < a->len && r->d[i]-- == 0; i++)
            ;
        r->len = a->len;
        r->neg = false;
    } else { /* -(a + 1) */
        memcpy(r->d, a->d, a->len * sizeof(uint32_t));
        uint32_t i = 0;
        while (i < a->len && ++r->d[i] == 0)
            i++;
        if (i == a->len)
            r->d[i] = 1;
        r->len = n;
        r->neg = true;
    }
    return bi_finish(r);
}

JsBigInt *js_bigint_as_n(JsContext *ctx, uint64_t bits, const JsBigInt *a, bool is_signed) {
    if (!bits || !a->len)
        return js_bigint_from_int64(ctx, 0);
    if (!a->neg && bit_length(a) < (is_signed ? bits : bits + 1)) /* already in range */
        return bi_make(ctx, a->d, a->len, false);
    if (a->neg && is_signed && bit_length(a) < bits) /* -2^(bits-1) < a: in range */
        return bi_make(ctx, a->d, a->len, true);
    if (bits > (uint64_t)BI_MAX_LIMBS * 32) {
        too_big(ctx);
        return NULL;
    }
    uint32_t n = (uint32_t)((bits + 31) / 32);
    uint32_t m = n > a->len ? n : a->len;
    uint32_t *x = scratch(ctx, (uint64_t)m + 1);
    if (!x)
        return NULL;
    twos(x, a, m + 1); /* a mod 2^bits: the low bits of its two's complement */
    for (uint32_t i = n; i <= m; i++)
        x[i] = 0;
    if (bits % 32)
        x[n - 1] &= (UINT32_C(1) << (bits % 32)) - 1;
    bool neg = false;
    if (is_signed && ((x[(bits - 1) / 32] >> ((bits - 1) % 32)) & 1)) {
        /* the top bit set: x - 2^bits, a negative of magnitude 2^bits - x */
        for (uint32_t i = 0; i < n; i++)
            x[i] = ~x[i];
        if (bits % 32)
            x[n - 1] &= (UINT32_C(1) << (bits % 32)) - 1;
        for (uint32_t i = 0; i < n && ++x[i] == 0; i++)
            ;
        neg = true;
    }
    JsBigInt *r = bi_make(ctx, x, n + 1, neg);
    scratch_free(ctx, x, (uint64_t)m + 1);
    return r;
}

/* ---- BigInt, BigInt.prototype ---- */

#define ARG(i) ((i) < argc ? args[i] : js_undefined())

static bool bthrow(JsContext *ctx, JsValue *r) {
    *r = js_is_undefined(ctx->pending_error) ? js_error_from_ascii(ctx, "out of memory") : js_take_error(ctx);
    return false;
}

/* ToBigInt (7.1.13) of V (rooted by the caller): NULL with the error pending */
static JsBigInt *to_bigint(JsContext *ctx, JsValue v) {
    JsValue p;
    if (!js_to_primitive(ctx, v, JS_HINT_NUMBER, &p))
        return NULL;
    if (js_is_bigint(p))
        return js_value_bigint(p);
    if (js_is_bool(p))
        return js_bigint_from_int64(ctx, js_get_bool(p) ? 1 : 0);
    if (js_is_string(p)) {
        bool bad;
        js_gc_protect(ctx->vm, &p);
        JsBigInt *b = js_bigint_from_string(ctx, js_value_string(p), &bad);
        js_gc_unprotect(ctx->vm, &p);
        if (!b && bad)
            js_set_error(ctx, js_error_fmt(ctx, "SyntaxError: Cannot convert %v to a BigInt", p));
        return b;
    }
    js_set_error(ctx, js_error_fmt(ctx, "TypeError: Cannot convert %v to a BigInt", p));
    return NULL;
}

/* BigInt(value): a number must be an integer; anything else as ToBigInt takes it */
static bool g_BigInt(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    if (js_native_is_new(ctx)) {
        *r = js_error_fmt(ctx, "TypeError: BigInt is not a constructor");
        return false;
    }
    JsValue p;
    if (!js_to_primitive(ctx, ARG(0), JS_HINT_NUMBER, &p))
        return bthrow(ctx, r);
    JsBigInt *b;
    if (js_is_number(p)) {
        double d = js_get_number(p);
        if (d != d || d - d != 0 || __builtin_trunc(d) != d) {
            *r = js_error_fmt(ctx, "RangeError: The number %v cannot be converted to a BigInt because it is not an integer", p);
            return false;
        }
        b = js_bigint_from_double(ctx, d);
    } else {
        js_gc_protect(ctx->vm, &p);
        b = to_bigint(ctx, p);
        js_gc_unprotect(ctx->vm, &p);
    }
    if (!b)
        return bthrow(ctx, r);
    *r = js_value_from_cell(&b->gc);
    return true;
}

/* ToIndex for asIntN / asUintN */
static bool to_bits(JsContext *ctx, JsValue v, uint64_t *out, JsValue *r) {
    double d = 0;
    if (!js_is_undefined(v) && !js_op_to_number(ctx, v, &d))
        return bthrow(ctx, r);
    d = d != d ? 0 : __builtin_trunc(d);
    if (d < 0 || d > 9007199254740991.0) {
        *r = js_error_fmt(ctx, "RangeError: Invalid value: not (convertible to) a safe integer");
        return false;
    }
    *out = (uint64_t)d;
    return true;
}

static bool as_n(JsContext *ctx, const JsValue *args, int argc, JsValue *r, bool is_signed) {
    uint64_t bits;
    if (!to_bits(ctx, ARG(0), &bits, r))
        return false;
    JsBigInt *b = to_bigint(ctx, ARG(1));
    if (!b)
        return bthrow(ctx, r);
    JsValue bv = js_value_from_cell(&b->gc);
    js_gc_protect(ctx->vm, &bv);
    JsBigInt *res = js_bigint_as_n(ctx, bits, js_value_bigint(bv), is_signed);
    js_gc_unprotect(ctx->vm, &bv);
    if (!res)
        return bthrow(ctx, r);
    *r = js_value_from_cell(&res->gc);
    return true;
}

static bool bi_asIntN(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return as_n(ctx, args, argc, r, true);
}

static bool bi_asUintN(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return as_n(ctx, args, argc, r, false);
}

static bool this_bigint(JsContext *ctx, JsValue tv, const char *method, JsValue *r) {
    if (js_is_bigint(tv))
        return true;
    *r = js_error_fmt(ctx, "TypeError: BigInt.prototype.%s requires that 'this' be a BigInt", method);
    return false;
}

static bool bp_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (!this_bigint(ctx, tv, "toString", r))
        return false;
    double radix = 10;
    if (!js_is_undefined(ARG(0))) {
        if (!js_op_to_number(ctx, ARG(0), &radix))
            return bthrow(ctx, r);
        radix = radix != radix ? 0 : __builtin_trunc(radix);
    }
    if (radix < 2 || radix > 36) {
        *r = js_error_fmt(ctx, "RangeError: toString() radix argument must be between 2 and 36");
        return false;
    }
    JsString *s = js_bigint_to_string(ctx, js_value_bigint(tv), (int)radix);
    if (!s)
        return bthrow(ctx, r); /* the length limit, the budget, else out of memory */
    *r = js_value_from_cell(&s->gc);
    return true;
}

static bool bp_valueOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    if (!this_bigint(ctx, tv, "valueOf", r))
        return false;
    *r = tv;
    return true;
}

static bool def_fn(JsContext *ctx, JsObject *o, const char *name, JsNativeFn fn) {
    JsValue nf = js_native_new(ctx, name, fn, NULL);
    return js_is_function(nf) && js_object_set_ascii(ctx, o, name, nf);
}

bool js_bigint_builtins_init(JsContext *ctx) {
    JsValue t = js_object_new(ctx);
    if (!js_is_object(t))
        return false;
    ctx->bigint_methods = js_value_object(t); /* rooted via the context now */
    JsValue ctor = js_native_new(ctx, "BigInt", g_BigInt, NULL);
    if (!js_is_function(ctor))
        return false;
    js_native_builtin_ctor(ctor, ctx->bigint_methods);
    if (!js_object_set_ascii(ctx, ctx->bigint_methods, "constructor", ctor) ||
        !def_fn(ctx, ctx->bigint_methods, "toString", bp_toString) ||
        !def_fn(ctx, ctx->bigint_methods, "valueOf", bp_valueOf) ||
        !js_object_set_ascii(ctx, ctx->globals, "BigInt", ctor))
        return false;
    JsValue st = js_object_new(ctx);
    if (!js_is_object(st))
        return false;
    ((JsNative *)js_value_cell(ctor))->statics = js_value_object(st);
    return def_fn(ctx, js_value_object(st), "asUintN", bi_asUintN) &&
           def_fn(ctx, js_value_object(st), "asIntN", bi_asIntN);
}
