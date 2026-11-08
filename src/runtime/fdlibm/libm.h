/*
 * Shared header for the fdlibm-derived math kernel (the files in src/runtime/fdlibm),
 * adapted from musl's src/internal/libm.h. Freestanding: no <math.h>, no
 * libm; sqrt, fabs and floor are compiler builtins that lower to single
 * instructions (or the wasm opcodes).
 *
 * Why these and not the earlier series approximations: the engine promises
 * the same model on every host, and the old kernel was ~1e-10 off
 * (tan(pi/4) = 0.9999999998), enough to move a ceil() on an arc count. These
 * are the functions V8 and musl derive from fdlibm, accurate to <1 ulp, so
 * results agree with Node to the bit in nearly every case.
 */
#ifndef JS_FDLIBM_LIBM_H
#define JS_FDLIBM_LIBM_H

#include <float.h>
#include <stdint.h>

#include "js_bytecode.h" /* js_k_* prototypes */

typedef double double_t;

/* the few <math.h> names the sources use, as builtins */
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
#ifndef INFINITY
#define INFINITY __builtin_inf()
#endif
#ifndef NAN
#define NAN __builtin_nan("")
#endif

#define predict_true(x) __builtin_expect(!!(x), 1)
#define predict_false(x) __builtin_expect(x, 0)

static inline double eval_as_double(double x) { return x; }

#define FORCE_EVAL(x) do {                            \
    if (sizeof(x) == sizeof(float)) {                 \
        volatile float fe__x; fe__x = (x); (void)fe__x;   \
    } else {                                          \
        volatile double fe__x; fe__x = (x); (void)fe__x;  \
    }                                                 \
} while (0)

#define asuint64(f) ((union { double _f; uint64_t _i; }){ f })._i
#define asdouble(i) ((union { uint64_t _i; double _f; }){ i })._f

#define EXTRACT_WORDS(hi, lo, d) do { uint64_t ew__u = asuint64(d); (hi) = (uint32_t)(ew__u >> 32); (lo) = (uint32_t)ew__u; } while (0)
#define GET_HIGH_WORD(hi, d) do { (hi) = (uint32_t)(asuint64(d) >> 32); } while (0)
#define GET_LOW_WORD(lo, d) do { (lo) = (uint32_t)asuint64(d); } while (0)
#define INSERT_WORDS(d, hi, lo) do { (d) = asdouble(((uint64_t)(uint32_t)(hi) << 32) | (uint32_t)(lo)); } while (0)
#define SET_HIGH_WORD(d, hi) INSERT_WORDS(d, hi, (uint32_t)asuint64(d))
#define SET_LOW_WORD(d, lo) INSERT_WORDS(d, asuint64(d) >> 32, lo)

/* internal kernels shared between the files */
double k_scalbn(double x, int n);
int k_rem_pio2_large(double *x, double *y, int e0, int nx, int prec);
int k_rem_pio2(double x, double *y);
double k_sin(double x, double y, int iy);
double k_cos(double x, double y);
double k_tan(double x, double y, int odd);

#endif
