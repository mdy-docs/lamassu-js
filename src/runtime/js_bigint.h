/*
 * BigInt: arbitrary-precision integers, a primitive of their own (JS_TAG_BIGINT, a JsBigInt cell).
 * Sign and magnitude, the magnitude in 32-bit limbs, least significant first, with no leading
 * zero limb (zero has none and is never negative). A value is immutable once made.
 */
#ifndef JS_BIGINT_H
#define JS_BIGINT_H

#include "lamassu_internal.h"

typedef struct JsBigInt {
    JsGcCell gc;
    uint32_t len, cap; /* limbs in use, limbs allocated (the cell's size) */
    bool neg;
    uint32_t d[];
} JsBigInt;

static inline JsBigInt *js_value_bigint(JsValue v) {
    return (JsBigInt *)js_value_cell(v);
}

size_t js_bigint_cell_size(const JsBigInt *b);

/* Construction. NULL on OOM (or past the size limit, with the RangeError pending). */
JsBigInt *js_bigint_from_int64(JsContext *ctx, int64_t v);
JsBigInt *js_bigint_from_limbs(JsContext *ctx, const uint32_t *m, uint32_t n, bool neg);
JsBigInt *js_bigint_from_double(JsContext *ctx, double d); /* D an integer */
/* Text in RADIX (2-36), digits only, optional leading '-' when SIGNED; NULL with *bad set when
 * it is not one (no error pending), or on OOM */
JsBigInt *js_bigint_parse(JsContext *ctx, const uint16_t *u, uint32_t n, int radix, bool *bad);
/* StringToBigInt: the trimmed text as BigInt() takes it (0x/0o/0b prefixes, a sign for decimal,
 * empty is 0n). NULL with *bad set when it is not one, or on OOM. */
JsBigInt *js_bigint_from_string(JsContext *ctx, const JsString *s, bool *bad);

/* Conversion */
JsString *js_bigint_to_string(JsVm *vm, const JsBigInt *b, int radix);
double    js_bigint_to_double(const JsBigInt *b);
bool      js_bigint_is_zero(const JsBigInt *b);
int       js_bigint_cmp(const JsBigInt *a, const JsBigInt *b);      /* -1, 0, 1 */
int       js_bigint_cmp_double(const JsBigInt *a, double d);         /* -1, 0, 1; 2 for NaN */
bool      js_bigint_equal(const JsBigInt *a, const JsBigInt *b);
uint32_t  js_bigint_hash(const JsBigInt *b);

/* Arithmetic for the operators: OP is the JsOp (ADD, SUB, MUL, DIV, MOD, POW, BITAND, BITOR,
 * BITXOR, SHL, SHR, USHR), NEG or BITNOT for the unary ones (B unused). NULL with the error
 * pending (RangeError for division by zero, a negative exponent, a result too big). */
JsBigInt *js_bigint_binary(JsContext *ctx, int op, const JsBigInt *a, const JsBigInt *b);
JsBigInt *js_bigint_unary(JsContext *ctx, int op, const JsBigInt *a);

/* BigInt.asIntN / asUintN */
JsBigInt *js_bigint_as_n(JsContext *ctx, uint64_t bits, const JsBigInt *a, bool is_signed);

bool js_bigint_builtins_init(JsContext *ctx);

#endif
