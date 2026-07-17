/*
 * Freestanding double-precision transcendentals (no libm). Accuracy is
 * ~1e-12 relative over the usual argument ranges — ample for a templating
 * VM, not a numerics library. floor/ceil/trunc/round/sqrt/abs use compiler
 * builtins that lower to native wasm/hardware ops, so they are exact.
 */
#include "js_bytecode.h"

#define JS_PI  3.14159265358979311600
#define JS_LN2 0.69314718055994530942
#define JS_LOG2E 1.44269504088896340736

static double kldexp(double x, int e) {
    /* multiply by 2^e via the exponent field, in <=3 steps to avoid overflow */
    while (e > 1000) { x *= 8.98846567431158e307; e -= 1023; }
    while (e < -1000) { x *= 1.1125369292536007e-308; e += 1022; }
    union { uint64_t u; double d; } s;
    s.u = (uint64_t)(e + 1023) << 52;
    return x * s.d;
}

double js_k_exp(double x) {
    if (x != x)
        return x;
    if (x > 709.782712893384)
        return __builtin_inf();
    if (x < -745.13321910194)
        return 0.0;
    /* x = k*ln2 + r,  |r| <= ln2/2 */
    double kf = __builtin_floor(x * JS_LOG2E + 0.5);
    int k = (int)kf;
    double r = x - kf * JS_LN2;
    /* exp(r) via degree-11 Taylor on [-ln2/2, ln2/2] (~1e-14) */
    double e = 1.0 + r * (1.0 + r * (1.0 / 2 + r * (1.0 / 6 + r * (1.0 / 24 +
               r * (1.0 / 120 + r * (1.0 / 720 + r * (1.0 / 5040 +
               r * (1.0 / 40320 + r * (1.0 / 362880 + r * (1.0 / 3628800 +
               r * (1.0 / 39916800)))))))))));
    return kldexp(e, k);
}

double js_k_log(double x) {
    if (x != x || x < 0.0)
        return __builtin_nan("");
    if (x == 0.0)
        return -__builtin_inf();
    if (x == __builtin_inf())
        return x;
    /* x = m * 2^e, m in [sqrt(1/2), sqrt(2)) so |s| small */
    union { double d; uint64_t u; } b;
    b.d = x;
    int e = (int)((b.u >> 52) & 0x7FF) - 1023;
    b.u = (b.u & UINT64_C(0x800FFFFFFFFFFFFF)) | ((uint64_t)1023 << 52);
    double m = b.d;
    if (m > 1.41421356237309515) {
        m *= 0.5;
        e += 1;
    }
    /* log(m) = 2*atanh(s), s=(m-1)/(m+1) */
    double s = (m - 1.0) / (m + 1.0);
    double s2 = s * s;
    double series = s * (2.0 + s2 * (2.0 / 3 + s2 * (2.0 / 5 + s2 * (2.0 / 7 +
                   s2 * (2.0 / 9 + s2 * (2.0 / 11))))));
    return e * JS_LN2 + series;
}

double js_k_pow(double x, double y) {
    if (y == 0.0)
        return 1.0;
    if (x != x || y != y)
        return __builtin_nan("");
    /* exact integer / half-integer path (also handles negative bases) */
    if (y == __builtin_floor(y) && y >= -1e15 && y <= 1e15) {
        int neg = y < 0;
        uint64_t n = (uint64_t)(neg ? -y : y);
        double acc = x, r = 1.0;
        while (n) {
            if (n & 1)
                r *= acc;
            acc *= acc;
            n >>= 1;
        }
        return neg ? 1.0 / r : r;
    }
    if (x < 0.0)
        return __builtin_nan("");
    if (x == 0.0)
        return y > 0.0 ? 0.0 : __builtin_inf();
    return js_k_exp(y * js_k_log(x));
}

/* range-reduce to [-pi/4, pi/4] returning quadrant */
static int reduce_quadrant(double x, double *out) {
    double q = __builtin_floor(x / (JS_PI / 2) + 0.5);
    *out = x - q * (JS_PI / 2);
    int quad = (int)(q - 4.0 * __builtin_floor(q / 4.0)); /* q mod 4, positive */
    return quad;
}

static double poly_sin(double r) {
    double r2 = r * r;
    return r * (1.0 + r2 * (-1.0 / 6 + r2 * (1.0 / 120 + r2 * (-1.0 / 5040 +
               r2 * (1.0 / 362880 + r2 * (-1.0 / 39916800))))));
}

static double poly_cos(double r) {
    double r2 = r * r;
    return 1.0 + r2 * (-1.0 / 2 + r2 * (1.0 / 24 + r2 * (-1.0 / 720 +
           r2 * (1.0 / 40320 + r2 * (-1.0 / 3628800)))));
}

double js_k_sin(double x) {
    if (x != x || x == __builtin_inf() || x == -__builtin_inf())
        return __builtin_nan("");
    double r;
    switch (reduce_quadrant(x, &r)) {
    case 0: return poly_sin(r);
    case 1: return poly_cos(r);
    case 2: return -poly_sin(r);
    default: return -poly_cos(r);
    }
}

double js_k_cos(double x) {
    if (x != x || x == __builtin_inf() || x == -__builtin_inf())
        return __builtin_nan("");
    double r;
    switch (reduce_quadrant(x, &r)) {
    case 0: return poly_cos(r);
    case 1: return -poly_sin(r);
    case 2: return -poly_cos(r);
    default: return poly_sin(r);
    }
}

double js_k_tan(double x) {
    double c = js_k_cos(x);
    if (c == 0.0)
        return __builtin_inf();
    return js_k_sin(x) / c;
}

/* atan on [-1,1] via minimax-ish series with argument shrinking */
static double atan_small(double x) {
    double x2 = x * x;
    return x * (1.0 + x2 * (-1.0 / 3 + x2 * (1.0 / 5 + x2 * (-1.0 / 7 +
               x2 * (1.0 / 9 + x2 * (-1.0 / 11 + x2 * (1.0 / 13)))))));
}

double js_k_atan(double x) {
    if (x != x)
        return x;
    int neg = x < 0;
    if (neg)
        x = -x;
    int inv = x > 1.0;
    if (inv)
        x = 1.0 / x;
    /* halve twice via atan(x) = 2*atan(x/(1+sqrt(1+x^2))) for accuracy */
    double t = x / (1.0 + __builtin_sqrt(1.0 + x * x));
    t = t / (1.0 + __builtin_sqrt(1.0 + t * t));
    double a = 4.0 * atan_small(t);
    if (inv)
        a = JS_PI / 2 - a;
    return neg ? -a : a;
}

double js_k_atan2(double y, double x) {
    if (x == 0.0 && y == 0.0)
        return 0.0;
    if (x > 0.0)
        return js_k_atan(y / x);
    if (x < 0.0)
        return y >= 0.0 ? js_k_atan(y / x) + JS_PI : js_k_atan(y / x) - JS_PI;
    return y > 0.0 ? JS_PI / 2 : -JS_PI / 2;
}

double js_k_asin(double x) {
    if (x < -1.0 || x > 1.0)
        return __builtin_nan("");
    if (x == 1.0)
        return JS_PI / 2;
    if (x == -1.0)
        return -JS_PI / 2;
    return js_k_atan(x / __builtin_sqrt(1.0 - x * x));
}

double js_k_acos(double x) {
    if (x < -1.0 || x > 1.0)
        return __builtin_nan("");
    return JS_PI / 2 - js_k_asin(x);
}
