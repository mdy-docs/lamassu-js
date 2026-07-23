/*
 * Bytecode serialization + a validating loader (phase 8).
 *
 * "Compile once, run many": js_bytecode_serialize turns a compiled top-level
 * function into a portable byte buffer; js_bytecode_load validates that buffer
 * and rebuilds a runnable function. The loader treats its input as untrusted —
 * a corrupt or tampered cache must be rejected, never executed — so it does a
 * full structural + dataflow verification (the same class of checks a JVM/WASM
 * validator does): bounded indices, jump targets on instruction boundaries, a
 * guaranteed terminator, and an abstract stack-depth pass that proves no
 * underflow/overflow and recomputes max_stack from scratch (the stored value
 * is never trusted).
 *
 * Only non-module functions are portable: module bodies carry GET_EXPORT/
 * GET_IMPORT opcodes bound to a live JsModule that has no standalone meaning,
 * so those opcodes are rejected on load and serialize refuses a module fn.
 *
 * Format (all integers little-endian; doubles as their IEEE-754 bit pattern,
 * LE): a 16-byte header (magic "JSBC", u32 version, u32 flags, u32 reserved)
 * then the root function record. A function record is n_params/flags/n_locals/
 * upvalue table/name/code/constants/line-table, with constants tagged by kind
 * and nested functions recursively inlined (tag 6).
 */
#include "js_bytecode.h"
#include "lamassu_internal.h"

#define JSBC_VERSION 2u
#define JSBC_FLAG_HAS_REGEX 1u /* producer had regex enabled (informational) */

/* Sanity caps: reject absurd sizes early (before allocating) rather than
 * trusting the buffer. Generous vs. anything the compiler emits. */
#define JSBC_MAX_CODE (4u * 1024 * 1024)
#define JSBC_MAX_CONSTS 65536u /* const indices are u16 */
#define JSBC_MAX_LOCALS 65536u
#define JSBC_MAX_NAME 4096u
#define JSBC_MAX_LINES (4u * 1024 * 1024)
#define JSBC_MAX_STACK 1000000u
#define JSBC_MAX_FN_DEPTH 200u /* nested-function recursion cap (matches parser) */

/* Constant tags. */
enum {
    CTAG_UNDEFINED = 0,
    CTAG_NULL = 1,
    CTAG_FALSE = 2,
    CTAG_TRUE = 3,
    CTAG_NUMBER = 4,
    CTAG_STRING = 5,
    CTAG_FUNCTION = 6,
};

/* =====================================================================
 * Opcode metadata table (declared in js_bytecode.h). Indexed by opcode.
 * ===================================================================== */

#define OP(name, form, cf, delta, min_in, obytes)                              \
    [name] = {form, cf, delta, min_in, obytes, true}

const JsOpInfo js_op_info[JS_OP__COUNT] = {
    OP(JS_OP_NOP, OPF_NONE, CF_NEXT, 0, 0, 0),
    OP(JS_OP_CONST, OPF_CONST, CF_NEXT, +1, 0, 2),
    OP(JS_OP_UNDEFINED, OPF_NONE, CF_NEXT, +1, 0, 0),
    OP(JS_OP_NULL, OPF_NONE, CF_NEXT, +1, 0, 0),
    OP(JS_OP_TRUE, OPF_NONE, CF_NEXT, +1, 0, 0),
    OP(JS_OP_FALSE, OPF_NONE, CF_NEXT, +1, 0, 0),
    OP(JS_OP_POP, OPF_NONE, CF_NEXT, -1, 1, 0),
    OP(JS_OP_DUP, OPF_NONE, CF_NEXT, +1, 1, 0),
    OP(JS_OP_DUP2, OPF_NONE, CF_NEXT, +2, 2, 0),
    OP(JS_OP_SWAP, OPF_NONE, CF_NEXT, 0, 2, 0),
    OP(JS_OP_ROT3, OPF_NONE, CF_NEXT, 0, 3, 0),
    OP(JS_OP_GET_LOCAL, OPF_LOCAL, CF_NEXT, +1, 0, 2),
    OP(JS_OP_SET_LOCAL, OPF_LOCAL, CF_NEXT, 0, 1, 2),
    OP(JS_OP_TDZ, OPF_LOCAL, CF_NEXT, 0, 0, 2),
    OP(JS_OP_GET_GLOBAL, OPF_CONST_STR, CF_NEXT, +1, 0, 2),
    OP(JS_OP_GET_GLOBAL_SOFT, OPF_CONST_STR, CF_NEXT, +1, 0, 2),
    OP(JS_OP_SET_GLOBAL, OPF_CONST_STR, CF_NEXT, 0, 1, 2),
    OP(JS_OP_GET_LEXICAL, OPF_CONST_STR, CF_NEXT, +1, 0, 2),
    OP(JS_OP_GET_LEXICAL_SOFT, OPF_CONST_STR, CF_NEXT, +1, 0, 2),
    OP(JS_OP_SET_LEXICAL, OPF_CONST_STR, CF_NEXT, 0, 1, 2),
    OP(JS_OP_DEFINE_LEXICAL, OPF_DEFLEX, CF_NEXT, 0, 1, 3),
    OP(JS_OP_ADD, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_SUB, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_MUL, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_DIV, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_MOD, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_POW, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_NEG, OPF_NONE, CF_NEXT, 0, 1, 0),
    OP(JS_OP_POS, OPF_NONE, CF_NEXT, 0, 1, 0),
    OP(JS_OP_NOT, OPF_NONE, CF_NEXT, 0, 1, 0),
    OP(JS_OP_BITNOT, OPF_NONE, CF_NEXT, 0, 1, 0),
    OP(JS_OP_BITAND, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_BITOR, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_BITXOR, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_SHL, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_SHR, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_USHR, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_EQ, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_NEQ, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_STRICT_EQ, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_STRICT_NEQ, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_LT, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_GT, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_LE, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_GE, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_IN, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_TYPEOF, OPF_NONE, CF_NEXT, 0, 1, 0),
    OP(JS_OP_TO_STRING, OPF_NONE, CF_NEXT, 0, 1, 0),
    OP(JS_OP_JUMP, OPF_JUMP, CF_JUMP, 0, 0, 4),
    OP(JS_OP_JUMP_IF_FALSE, OPF_JUMP, CF_BRANCH, -1, 1, 4),
    OP(JS_OP_JUMP_IF_TRUE, OPF_JUMP, CF_BRANCH, -1, 1, 4),
    OP(JS_OP_JF_PEEK, OPF_JUMP, CF_BRANCH, 0, 1, 4),
    OP(JS_OP_JT_PEEK, OPF_JUMP, CF_BRANCH, 0, 1, 4),
    OP(JS_OP_JNN_PEEK, OPF_JUMP, CF_BRANCH, 0, 1, 4),
    OP(JS_OP_OPT_CHAIN, OPF_JUMP, CF_BRANCH, 0, 1, 4),
    OP(JS_OP_CASE, OPF_JUMP, CF_BRANCH, -1, 2, 4),
    OP(JS_OP_NEW_OBJECT, OPF_NONE, CF_NEXT, +1, 0, 0),
    OP(JS_OP_NEW_ARRAY, OPF_U16, CF_NEXT, +1, 0, 2), /* delta/min_in: special */
    OP(JS_OP_ARRAY_APPEND, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_ARRAY_SPREAD, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_ARRAY_REST, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_OBJ_SPREAD, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_DEFINE_PROP, OPF_NONE, CF_NEXT, -2, 3, 0),
    OP(JS_OP_GET_PROP, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_SET_PROP, OPF_NONE, CF_NEXT, -2, 3, 0),
    OP(JS_OP_GET_PROP_ATOM, OPF_CONST_STR, CF_NEXT, 0, 1, 2),
    OP(JS_OP_SET_PROP_ATOM, OPF_CONST_STR, CF_NEXT, -1, 2, 2),
    OP(JS_OP_DELETE_PROP, OPF_NONE, CF_NEXT, -1, 2, 0),
    OP(JS_OP_ITER_NEW, OPF_NONE, CF_NEXT, +1, 1, 0),
    OP(JS_OP_ITER_NEXT, OPF_JUMP, CF_BRANCH, +1, 2, 4), /* taken: -2 */
    OP(JS_OP_THROW, OPF_NONE, CF_THROW, -1, 1, 0),
    OP(JS_OP_RETURN, OPF_NONE, CF_RETURN, -1, 1, 0),
    OP(JS_OP_CLOSURE, OPF_CLOSURE, CF_NEXT, +1, 0, 3),
    OP(JS_OP_GET_UPVAL, OPF_UPVAL, CF_NEXT, +1, 0, 2),
    OP(JS_OP_SET_UPVAL, OPF_UPVAL, CF_NEXT, 0, 1, 2),
    OP(JS_OP_CLOSE_UPVALS, OPF_LOCAL, CF_NEXT, 0, 0, 2),
    OP(JS_OP_GET_THIS, OPF_NONE, CF_NEXT, +1, 0, 0),
    OP(JS_OP_GET_CALLEE, OPF_NONE, CF_NEXT, +1, 0, 0),
    OP(JS_OP_CALL, OPF_U8, CF_NEXT, 0, 0, 1),  /* delta/min_in: special */
    OP(JS_OP_CALL_VARARGS, OPF_NONE, CF_NEXT, -2, 3, 0),
    OP(JS_OP_NEW, OPF_U8, CF_NEXT, 0, 0, 1),   /* delta/min_in: special (== CALL) */
    OP(JS_OP_NEW_VARARGS, OPF_NONE, CF_NEXT, -2, 3, 0),
    OP(JS_OP_OPT_CALL_CHECK, OPF_JUMP, CF_BRANCH, 0, 2, 4), /* taken: -2 */
    OP(JS_OP_TRY_PUSH, OPF_JUMP, CF_BRANCH, 0, 0, 4),       /* handler: +1 */
    OP(JS_OP_TRY_POP, OPF_NONE, CF_NEXT, 0, 0, 0),
    OP(JS_OP_GOSUB, OPF_JUMP, CF_GOSUB, 0, 0, 4), /* target: +1 */
    OP(JS_OP_RET_SUB, OPF_NONE, CF_RETSUB, -1, 1, 0),
    OP(JS_OP_AWAIT, OPF_NONE, CF_NEXT, 0, 1, 0),
    OP(JS_OP_GET_EXPORT, OPF_CONST_STR, CF_NEXT, +1, 0, 2),
    OP(JS_OP_SET_EXPORT, OPF_CONST_STR, CF_NEXT, 0, 1, 2),
    OP(JS_OP_GET_IMPORT, OPF_IMPORT_IDX, CF_NEXT, +1, 0, 2),
    OP(JS_OP_IMPORT_NS, OPF_IMPORT_IDX, CF_NEXT, +1, 0, 2),
    OP(JS_OP_NEW_REGEXP, OPF_REGEXP, CF_NEXT, +1, 0, 4),
    OP(JS_OP_DYNAMIC_IMPORT, OPF_NONE, CF_NEXT, 0, 1, 0),
};
#undef OP

/* =====================================================================
 * Serialization
 * ===================================================================== */

typedef struct {
    JsVm *vm;
    uint8_t *data;
    size_t len, cap;
    bool oom;
} OutBuf;

static bool out_reserve(OutBuf *b, size_t extra) {
    if (b->oom)
        return false;
    if (b->len + extra <= b->cap)
        return true;
    size_t ncap = b->cap ? b->cap * 2 : 256;
    while (ncap < b->len + extra)
        ncap *= 2;
    uint8_t *nd = js_realloc_raw(b->vm, b->data, b->cap, ncap);
    if (!nd) {
        b->oom = true;
        return false;
    }
    b->data = nd;
    b->cap = ncap;
    return true;
}

static void out_u8(OutBuf *b, uint8_t v) {
    if (out_reserve(b, 1))
        b->data[b->len++] = v;
}
static void out_u16(OutBuf *b, uint16_t v) {
    out_u8(b, (uint8_t)(v & 0xFF));
    out_u8(b, (uint8_t)(v >> 8));
}
static void out_u32(OutBuf *b, uint32_t v) {
    out_u8(b, (uint8_t)(v & 0xFF));
    out_u8(b, (uint8_t)((v >> 8) & 0xFF));
    out_u8(b, (uint8_t)((v >> 16) & 0xFF));
    out_u8(b, (uint8_t)(v >> 24));
}
static void out_bytes(OutBuf *b, const void *p, size_t n) {
    if (out_reserve(b, n)) {
        memcpy(b->data + b->len, p, n);
        b->len += n;
    }
}
static void out_units(OutBuf *b, const uint16_t *u, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        out_u16(b, u[i]);
}

static bool serialize_fn(OutBuf *b, JsFunctionCell *fn, uint32_t depth);

static bool serialize_const(OutBuf *b, JsValue v, uint32_t depth) {
    if (v.bits == JS_SPECIAL_UNDEFINED) {
        out_u8(b, CTAG_UNDEFINED);
    } else if (v.bits == JS_SPECIAL_NULL) {
        out_u8(b, CTAG_NULL);
    } else if (v.bits == JS_SPECIAL_FALSE) {
        out_u8(b, CTAG_FALSE);
    } else if (v.bits == JS_SPECIAL_TRUE) {
        out_u8(b, CTAG_TRUE);
    } else if (js_is_number(v)) {
        out_u8(b, CTAG_NUMBER);
        out_u32(b, (uint32_t)(v.bits & 0xFFFFFFFF));
        out_u32(b, (uint32_t)(v.bits >> 32));
    } else if (js_is_string(v)) {
        JsString *s = js_value_string(v);
        out_u8(b, CTAG_STRING);
        out_u32(b, s->length);
        out_units(b, s->units, s->length);
    } else if (js_value_is_cell(v) && js_value_cell(v)->kind == JS_KIND_FUNCTION) {
        out_u8(b, CTAG_FUNCTION);
        return serialize_fn(b, js_value_function(v), depth + 1);
    } else {
        /* objects/promises/closures/natives never appear as compiled consts */
        b->oom = true; /* reuse the error channel: aborts with a clean failure */
        return false;
    }
    return true;
}

static bool serialize_fn(OutBuf *b, JsFunctionCell *fn, uint32_t depth) {
    if (depth > JSBC_MAX_FN_DEPTH)
        return false;
    out_u16(b, fn->n_params);
    out_u8(b, fn->fn_flags);
    out_u32(b, fn->n_locals);
    out_u16(b, fn->n_upvals);
    for (uint16_t i = 0; i < fn->n_upvals; i++) {
        out_u16(b, fn->upvals[i].idx);
        out_u8(b, fn->upvals[i].from_local);
    }
    if (fn->name) {
        out_u8(b, 1);
        out_u32(b, fn->name->length);
        out_units(b, fn->name->units, fn->name->length);
    } else {
        out_u8(b, 0);
    }
    out_u32(b, fn->code_len);
    out_bytes(b, fn->code, fn->code_len);
    out_u32(b, fn->const_count);
    for (uint32_t i = 0; i < fn->const_count; i++) {
        if (!serialize_const(b, fn->consts[i], depth))
            return false;
    }
    out_u32(b, fn->line_count);
    for (uint32_t i = 0; i < fn->line_count; i++) {
        out_u32(b, fn->lines[i].off);
        out_u32(b, fn->lines[i].pos);
    }
    return !b->oom;
}

/* 16-byte header: magic, version, feature flags, buffer kind (script/module). */
static void write_header(OutBuf *b, uint32_t kind) {
    out_u8(b, 'J');
    out_u8(b, 'S');
    out_u8(b, 'B');
    out_u8(b, 'C');
    out_u32(b, JSBC_VERSION);
    uint32_t flags = 0;
#ifdef LAMASSU_HAS_REGEX
    flags |= JSBC_FLAG_HAS_REGEX;
#endif
    out_u32(b, flags);
    out_u32(b, kind);
}

bool js_bytecode_serialize(JsContext *ctx, JsValue fn, uint8_t **out, size_t *out_len) {
    if (!js_is_function(fn) || !js_value_is_cell(fn) ||
        js_value_cell(fn)->kind != JS_KIND_FUNCTION)
        return false;
    if (js_value_function(fn)->module) /* a module body — use the module API */
        return false;
    OutBuf b = {ctx->vm, NULL, 0, 0, false};
    write_header(&b, JS_BC_SCRIPT);
    if (!serialize_fn(&b, js_value_function(fn), 0) || b.oom) {
        js_realloc_raw(ctx->vm, b.data, b.cap, 0);
        return false;
    }
    /* trim to exact length so js_bytecode_free's size is unambiguous */
    if (b.cap != b.len) {
        uint8_t *trimmed = js_realloc_raw(ctx->vm, b.data, b.cap, b.len);
        if (trimmed)
            b.data = trimmed;
    }
    *out = b.data;
    *out_len = b.len;
    return true;
}

void js_bytecode_free(JsContext *ctx, uint8_t *buf, size_t len) {
    js_realloc_raw(ctx->vm, buf, len, 0);
}

/* =====================================================================
 * Loading + validation
 * ===================================================================== */

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    bool bad;
} InBuf;

static uint8_t in_u8(InBuf *b) {
    if (b->p + 1 > b->end) {
        b->bad = true;
        return 0;
    }
    return *b->p++;
}
static uint16_t in_u16(InBuf *b) {
    if (b->p + 2 > b->end) {
        b->bad = true;
        return 0;
    }
    uint16_t v = (uint16_t)(b->p[0] | ((uint16_t)b->p[1] << 8));
    b->p += 2;
    return v;
}
static uint32_t in_u32(InBuf *b) {
    if (b->p + 4 > b->end) {
        b->bad = true;
        return 0;
    }
    uint32_t v = (uint32_t)b->p[0] | ((uint32_t)b->p[1] << 8) |
                 ((uint32_t)b->p[2] << 16) | ((uint32_t)b->p[3] << 24);
    b->p += 4;
    return v;
}

/*
 * Loads one function record into a fresh cell. Everything the cell owns is
 * allocated through the VM so a partial failure is swept by the GC (the cell
 * is created first and kept reachable via `hold`, a caller-provided rooted
 * slot). Returns NULL with *err set; the cell, if created, is left for GC.
 */
static JsFunctionCell *load_fn(JsContext *ctx, InBuf *b, uint32_t depth,
                               const char **err);

/* Verifies one already-loaded function's code (structure + dataflow). The
 * child-closure recursion validates each nested function's upvalue
 * descriptors against THIS function's local/upvalue counts. */
static bool verify_fn(JsContext *ctx, JsFunctionCell *fn, bool is_module,
                      uint32_t import_count, const char **err);

static JsValue load_string_const(JsContext *ctx, InBuf *b, const char **err) {
    uint32_t n = in_u32(b);
    if (b->bad || n > JSBC_MAX_NAME * 256) {
        *err = "SyntaxError: bytecode string too long or truncated";
        return js_undefined();
    }
    if (b->p + (size_t)n * 2 > b->end) {
        b->bad = true;
        *err = "SyntaxError: truncated bytecode string";
        return js_undefined();
    }
    /* read code units LE without assuming input alignment */
    uint16_t stackbuf[128];
    uint16_t *u = stackbuf;
    if (n > 128) {
        u = js_realloc_raw(ctx->vm, NULL, 0, (size_t)n * sizeof(uint16_t));
        if (!u) {
            *err = "out of memory";
            return js_undefined();
        }
    }
    for (uint32_t i = 0; i < n; i++)
        u[i] = (uint16_t)(b->p[i * 2] | ((uint16_t)b->p[i * 2 + 1] << 8));
    b->p += (size_t)n * 2;
    JsValue a = js_atom(ctx->vm, u, n);
    if (u != stackbuf)
        js_realloc_raw(ctx->vm, u, (size_t)n * sizeof(uint16_t), 0);
    if (!js_is_string(a))
        *err = "out of memory";
    return a;
}

static JsFunctionCell *load_fn(JsContext *ctx, InBuf *b, uint32_t depth,
                               const char **err) {
    JsVm *vm = ctx->vm;
    if (depth > JSBC_MAX_FN_DEPTH) {
        *err = "SyntaxError: bytecode nesting too deep";
        return NULL;
    }
    uint16_t n_params = in_u16(b);
    uint8_t fn_flags = in_u8(b);
    uint32_t n_locals = in_u32(b);
    uint16_t n_upvals = in_u16(b);
    if (b->bad) {
        *err = "SyntaxError: truncated bytecode header";
        return NULL;
    }
    if ((fn_flags & ~(uint8_t)(JS_FN_ARROW | JS_FN_HAS_REST | JS_FN_ASYNC)) ||
        n_locals > JSBC_MAX_LOCALS || n_params > n_locals) {
        *err = "SyntaxError: invalid bytecode function header";
        return NULL;
    }
    /* The root (depth-0) function is invoked directly, never instantiated by a
     * CLOSURE op, so its upvalue slots are never bound. A forged n_upvals > 0
     * would leave GET_UPVAL reading an unbound (NULL) upvalue — reject it. */
    if (depth == 0 && n_upvals != 0) {
        *err = "SyntaxError: root function cannot have upvalues";
        return NULL;
    }

    JsGcCell *c = js_gc_new_cell(vm, JS_KIND_FUNCTION, sizeof(JsFunctionCell));
    if (!c) {
        *err = "out of memory";
        return NULL;
    }
    JsFunctionCell *fn = (JsFunctionCell *)c;
    /* zero the body only — offset 0 is the GC header, and clobbering
     * gc.next would orphan every older cell in the allocation chain */
    memset((char *)fn + sizeof(JsGcCell), 0, sizeof *fn - sizeof(JsGcCell));
    fn->n_params = n_params;
    fn->fn_flags = fn_flags;
    fn->n_locals = n_locals;

    /* Root the half-built cell across every subsequent allocation. */
    JsValue hold = js_value_from_cell(c);
    if (!js_gc_protect(vm, &hold)) {
        *err = "out of memory";
        return NULL;
    }

    JsFunctionCell *result = NULL;

    /* upvalue descriptors */
    if (n_upvals) {
        fn->upvals = js_realloc_raw(vm, NULL, 0, (size_t)n_upvals * sizeof(JsUpvalDesc));
        if (!fn->upvals) {
            *err = "out of memory";
            goto done;
        }
        fn->upval_cap = n_upvals;
        fn->n_upvals = n_upvals;
        for (uint16_t i = 0; i < n_upvals; i++) {
            fn->upvals[i].idx = in_u16(b);
            fn->upvals[i].from_local = in_u8(b) ? 1 : 0;
        }
    }

    /* name */
    uint8_t has_name = in_u8(b);
    if (b->bad) {
        *err = "SyntaxError: truncated bytecode";
        goto done;
    }
    if (has_name) {
        const char *serr = NULL;
        JsValue nm = load_string_const(ctx, b, &serr);
        if (!js_is_string(nm)) {
            *err = serr ? serr : "SyntaxError: bad function name";
            goto done;
        }
        fn->name = js_value_string(nm);
    }

    /* code */
    uint32_t code_len = in_u32(b);
    if (b->bad || code_len == 0 || code_len > JSBC_MAX_CODE ||
        b->p + code_len > b->end) {
        *err = "SyntaxError: bad or truncated bytecode body";
        goto done;
    }
    fn->code = js_realloc_raw(vm, NULL, 0, code_len);
    if (!fn->code) {
        *err = "out of memory";
        goto done;
    }
    memcpy(fn->code, b->p, code_len);
    b->p += code_len;
    fn->code_len = code_len;
    fn->code_cap = code_len;

    /* constants */
    uint32_t const_count = in_u32(b);
    if (b->bad || const_count > JSBC_MAX_CONSTS) {
        *err = "SyntaxError: bad constant count";
        goto done;
    }
    if (const_count) {
        fn->consts = js_realloc_raw(vm, NULL, 0, (size_t)const_count * sizeof(JsValue));
        if (!fn->consts) {
            *err = "out of memory";
            goto done;
        }
        fn->const_cap = const_count;
        /* fill with undefined first so a mid-load GC traces valid values */
        for (uint32_t i = 0; i < const_count; i++)
            fn->consts[i] = js_undefined();
        fn->const_count = const_count;
        for (uint32_t i = 0; i < const_count; i++) {
            uint8_t tag = in_u8(b);
            if (b->bad) {
                *err = "SyntaxError: truncated constant";
                goto done;
            }
            switch (tag) {
            case CTAG_UNDEFINED: fn->consts[i] = js_undefined(); break;
            case CTAG_NULL: fn->consts[i] = js_null(); break;
            case CTAG_FALSE: fn->consts[i] = js_bool(false); break;
            case CTAG_TRUE: fn->consts[i] = js_bool(true); break;
            case CTAG_NUMBER: {
                uint32_t lo = in_u32(b), hi = in_u32(b);
                if (b->bad) {
                    *err = "SyntaxError: truncated number constant";
                    goto done;
                }
                JsValue v;
                v.bits = (uint64_t)lo | ((uint64_t)hi << 32);
                /* A tampered cache can carry any 64-bit pattern here. Anything
                 * with a boxed tag (>= JS_TAG_SPECIAL) would be type-confused
                 * as a pointer/special downstream, so reject it outright — a
                 * CTAG_NUMBER constant must decode to an actual number. */
                if (!js_is_number(v)) {
                    *err = "SyntaxError: non-number number constant";
                    goto done;
                }
                /* canonicalize any non-canonical NaN bit pattern */
                if (js_get_number(v) != js_get_number(v))
                    v = js_number(js_get_number(v));
                fn->consts[i] = v;
                break;
            }
            case CTAG_STRING: {
                const char *serr = NULL;
                JsValue s = load_string_const(ctx, b, &serr);
                if (!js_is_string(s)) {
                    *err = serr ? serr : "SyntaxError: bad string constant";
                    goto done;
                }
                fn->consts[i] = s;
                break;
            }
            case CTAG_FUNCTION: {
                JsFunctionCell *child = load_fn(ctx, b, depth + 1, err);
                if (!child)
                    goto done;
                fn->consts[i] = js_value_from_cell(&child->gc);
                break;
            }
            default:
                *err = "SyntaxError: unknown constant tag";
                goto done;
            }
        }
    }

    /* line table */
    uint32_t line_count = in_u32(b);
    if (b->bad || line_count > JSBC_MAX_LINES) {
        *err = "SyntaxError: bad line-table size";
        goto done;
    }
    if (line_count) {
        fn->lines = js_realloc_raw(vm, NULL, 0, (size_t)line_count * sizeof(JsLineEntry));
        if (!fn->lines) {
            *err = "out of memory";
            goto done;
        }
        fn->line_cap = line_count;
        for (uint32_t i = 0; i < line_count; i++) {
            fn->lines[i].off = in_u32(b);
            fn->lines[i].pos = in_u32(b);
        }
        fn->line_count = line_count;
        if (b->bad) {
            *err = "SyntaxError: truncated line table";
            goto done;
        }
    }

    result = fn;
done:
    js_gc_unprotect(vm, &hold);
    return result;
}

/* ---- code verification ---- */

/*
 * Reads a u16/u32 operand at a known-in-range offset (the caller has already
 * bounds-checked operand_bytes against code_len).
 */
static uint16_t code_u16(const uint8_t *code, uint32_t off) {
    return (uint16_t)(code[off] | ((uint16_t)code[off + 1] << 8));
}
static uint32_t code_u32(const uint8_t *code, uint32_t off) {
    return (uint32_t)code[off] | ((uint32_t)code[off + 1] << 8) |
           ((uint32_t)code[off + 2] << 16) | ((uint32_t)code[off + 3] << 24);
}

static bool const_is_string(JsFunctionCell *fn, uint16_t idx) {
    return idx < fn->const_count && js_is_string(fn->consts[idx]);
}
static bool const_is_function(JsFunctionCell *fn, uint16_t idx) {
    return idx < fn->const_count && js_value_is_cell(fn->consts[idx]) &&
           js_value_cell(fn->consts[idx])->kind == JS_KIND_FUNCTION;
}

/*
 * Pass 1: linear decode. Confirms every byte belongs to a known instruction,
 * operands fit, indices are in range and of the right kind, module opcodes
 * are absent, and records instruction-start offsets in `boundary`. Nested
 * closures are verified recursively (their upvalue descriptors are checked
 * against this function here, where the counts are known).
 */
/* True for the four opcodes that dereference fn->module. */
static bool is_module_opcode(uint8_t opb) {
    return opb == JS_OP_GET_EXPORT || opb == JS_OP_SET_EXPORT ||
           opb == JS_OP_GET_IMPORT || opb == JS_OP_IMPORT_NS;
}

static bool verify_pass1(JsFunctionCell *fn, uint8_t *boundary, bool is_module,
                         uint32_t import_count, const char **err) {
    const uint8_t *code = fn->code;
    uint32_t n = fn->code_len;
    uint32_t off = 0;
    while (off < n) {
        boundary[off] = 1;
        uint8_t opb = code[off];
        if (opb >= JS_OP__COUNT || !js_op_info[opb].valid) {
            *err = "SyntaxError: unknown opcode in bytecode";
            return false;
        }
        /* module opcodes deref fn->module — only legal inside a module body */
        if (!is_module && is_module_opcode(opb)) {
            *err = "SyntaxError: module opcode in non-module bytecode";
            return false;
        }
        const JsOpInfo *oi = &js_op_info[opb];
        uint32_t ob = oi->operand_bytes;
        if (off + 1 + ob > n) {
            *err = "SyntaxError: opcode operands run past end of code";
            return false;
        }
        uint32_t opnd = off + 1;
        switch (oi->form) {
        case OPF_CONST:
            if (code_u16(code, opnd) >= fn->const_count) {
                *err = "SyntaxError: constant index out of range";
                return false;
            }
            break;
        case OPF_CONST_STR:
            if (!const_is_string(fn, code_u16(code, opnd))) {
                *err = "SyntaxError: expected a string constant";
                return false;
            }
            break;
        case OPF_LOCAL: {
            uint16_t slot = code_u16(code, opnd);
            /* CLOSE_UPVALS is a slot floor and may equal n_locals; the
             * accessors (GET/SET_LOCAL, TDZ) must index a real slot */
            bool ok_slot = opb == JS_OP_CLOSE_UPVALS ? slot <= fn->n_locals
                                                     : slot < fn->n_locals;
            if (!ok_slot) {
                *err = "SyntaxError: local slot out of range";
                return false;
            }
            break;
        }
        case OPF_UPVAL:
            if (code_u16(code, opnd) >= fn->n_upvals) {
                *err = "SyntaxError: upvalue index out of range";
                return false;
            }
            break;
        case OPF_CLOSURE: {
            uint16_t cidx = code_u16(code, opnd);
            if (!const_is_function(fn, cidx)) {
                *err = "SyntaxError: CLOSURE needs a function constant";
                return false;
            }
            /* the child's upvalue descriptors must reference this function */
            JsFunctionCell *child = js_value_function(fn->consts[cidx]);
            for (uint16_t i = 0; i < child->n_upvals; i++) {
                JsUpvalDesc *d = &child->upvals[i];
                uint32_t lim = d->from_local ? fn->n_locals : fn->n_upvals;
                if (d->idx >= lim) {
                    *err = "SyntaxError: closure upvalue descriptor out of range";
                    return false;
                }
            }
            break;
        }
        case OPF_DEFLEX:
            if (!const_is_string(fn, code_u16(code, opnd))) {
                *err = "SyntaxError: DEFINE_LEXICAL needs a string constant";
                return false;
            }
            break;
        case OPF_REGEXP:
#ifndef LAMASSU_HAS_REGEX
            *err = "SyntaxError: regex bytecode in a build without regex support";
            return false;
#else
            if (!const_is_string(fn, code_u16(code, opnd)) ||
                !const_is_string(fn, code_u16(code, opnd + 2))) {
                *err = "SyntaxError: NEW_REGEXP needs string constants";
                return false;
            }
            break;
#endif
        case OPF_IMPORT_IDX:
            /* GET_IMPORT/IMPORT_NS: the interpreter indexes fn->module->imports
             * with NO bounds check, so this is the load-bearing guard against a
             * tampered import index becoming an out-of-bounds read */
            if (code_u16(code, opnd) >= import_count) {
                *err = "SyntaxError: import index out of range";
                return false;
            }
            break;
        case OPF_U8:
        case OPF_U16:
        case OPF_JUMP:
        case OPF_NONE:
            break;
        }
        off += 1 + ob;
    }
    if (off != n) {
        *err = "SyntaxError: bytecode does not end on an instruction boundary";
        return false;
    }
    return true;
}

/* branch target operand offset -> the u32 target it names */
static uint32_t branch_target(const uint8_t *code, uint32_t op_off) {
    return code_u32(code, op_off + 1);
}

/*
 * Distinct operand-stack depths one instruction may be reached at. Straight-
 * line code reaches each offset at exactly one depth; the exception is a
 * finally block, which the compiler lowers to a GOSUB/RET_SUB subroutine
 * entered at different depths depending on the exit path (normal completion
 * vs. unwinding an exception or a pending return value) — the classic jsr/ret
 * polymorphism. The verifier therefore tracks a small SET of depths per
 * offset; K bounds it (nested finally blocks add depths). Real compiler output
 * uses very few; input that drives an offset past K distinct depths is
 * rejected rather than executed.
 */
#define JSBC_DEPTHS_PER_OFF 8

/*
 * Pass 2: control flow + stack dataflow. Verifies every jump/branch/gosub
 * target lands on an instruction boundary, that the final instruction cannot
 * fall off the end, and — the load-bearing safety pass — propagates the
 * operand-stack depth along every edge to prove no underflow/overflow and to
 * recompute max_stack (the stored value is never trusted). See js_bytecode.h's
 * op-info notes for the per-edge deltas; CALL/NEW_ARRAY read their count.
 */
static bool verify_pass2(JsFunctionCell *fn, const uint8_t *boundary,
                         int32_t *seen, uint32_t *worklist, const char **err) {
    const uint8_t *code = fn->code;
    uint32_t n = fn->code_len;
    const uint32_t K = JSBC_DEPTHS_PER_OFF;

    for (size_t i = 0; i < (size_t)n * K; i++)
        seen[i] = -1;

    uint32_t max_depth = 0;
    uint32_t wl_len = 0; /* pending states, packed as (off, depth) pairs */

    /*
     * Records (off, depth) in `seen`, the per-offset depth set (dedup +
     * subroutine-polymorphism). A newly seen state is also pushed onto the
     * worklist for later processing; an already-present one is dropped. The
     * macro returns false from the enclosing function on any rejection (target
     * off a boundary, depth out of range, or more than K distinct depths at
     * one instruction). Because a state is enqueued only on its first sighting,
     * the worklist holds at most n*K entries over the whole run — the fixpoint
     * touches each state once instead of re-sweeping all of them per round.
     */
#define SEED(TARGET, DEPTH)                                                    \
    do {                                                                       \
        int32_t _dp = (DEPTH);                                                 \
        uint32_t _t = (TARGET);                                                \
        if (_dp < 0 || _dp > (int32_t)JSBC_MAX_STACK) {                        \
            *err = "SyntaxError: bytecode stack out of range";                 \
            return false;                                                      \
        }                                                                      \
        if (_t >= n || !boundary[_t]) {                                        \
            *err = "SyntaxError: control-flow target off instruction boundary"; \
            return false;                                                      \
        }                                                                      \
        int32_t *_slot = &seen[(size_t)_t * K];                                \
        bool _found = false;                                                   \
        uint32_t _free = K;                                                    \
        for (uint32_t _i = 0; _i < K; _i++) {                                  \
            if (_slot[_i] == _dp) { _found = true; break; }                    \
            if (_slot[_i] < 0 && _free == K) _free = _i;                       \
        }                                                                      \
        if (!_found) {                                                         \
            if (_free == K) {                                                  \
                *err = "SyntaxError: too many stack shapes at one instruction"; \
                return false;                                                  \
            }                                                                  \
            _slot[_free] = _dp;                                                \
            worklist[2 * wl_len] = _t;                                         \
            worklist[2 * wl_len + 1] = (uint32_t)_dp;                          \
            wl_len++;                                                          \
        }                                                                      \
    } while (0)

    SEED(0, 0); /* entry state: empty operand stack at offset 0 */

    /*
     * Worklist fixpoint: pop a discovered (off, depth) state, propagate it to
     * its successors, and enqueue any that are new. Each state is processed
     * exactly once, so the whole pass is O(n*K) rather than the O((n*K)^2) of
     * repeated full sweeps.
     */
    while (wl_len) {
        wl_len--;
        uint32_t off = worklist[2 * wl_len];
        int32_t d = (int32_t)worklist[2 * wl_len + 1];
        uint8_t opb = code[off];
        const JsOpInfo *oi = &js_op_info[opb];
        uint32_t next = off + 1 + oi->operand_bytes;

        uint32_t min_in = oi->min_in;
        int32_t delta = oi->delta;
        if (opb == JS_OP_CALL || opb == JS_OP_NEW) {
            uint32_t argc = code[off + 1];
            min_in = argc + 2;
            delta = -(int32_t)(argc + 1);
        } else if (opb == JS_OP_NEW_ARRAY) {
            uint32_t count = code_u16(code, off + 1);
            min_in = count;
            delta = 1 - (int32_t)count;
        }
        if ((uint32_t)d < min_in) {
            *err = "SyntaxError: bytecode stack underflow";
            return false;
        }
        if ((uint32_t)d > max_depth)
            max_depth = (uint32_t)d;
        int32_t ft = d + delta;

        switch (oi->cf) {
        case CF_NEXT:
            SEED(next, ft);
            break;
        case CF_JUMP:
            SEED(branch_target(code, off), ft);
            break;
        case CF_BRANCH: {
            SEED(next, ft); /* fall-through edge */
            /* Taken edge. Most branches carry the fall-through delta; the
             * asymmetric ones override it: CASE/OPT_CALL_CHECK/ITER_NEXT pop
             * two on the taken path, TRY_PUSH's handler is entered with the
             * thrown value pushed. */
            int32_t taken;
            if (opb == JS_OP_CASE || opb == JS_OP_OPT_CALL_CHECK ||
                opb == JS_OP_ITER_NEXT)
                taken = d - 2;
            else if (opb == JS_OP_TRY_PUSH)
                taken = d + 1;
            else
                taken = ft;
            SEED(branch_target(code, off), taken);
            break;
        }
        case CF_GOSUB:
            /* fall-through resumes at d (RET_SUB pops the return address); the
             * subroutine body starts at d+1 */
            SEED(next, d);
            SEED(branch_target(code, off), d + 1);
            break;
        case CF_RETSUB:
        case CF_RETURN:
        case CF_THROW:
            break; /* no static successor */
        }
    }
#undef SEED

    /* The last instruction must not fall through past the end of code. */
    uint32_t last = 0, off = 0;
    while (off < n) {
        last = off;
        off += 1 + js_op_info[code[off]].operand_bytes;
    }
    uint8_t lastcf = js_op_info[code[last]].cf;
    if (lastcf != CF_RETURN && lastcf != CF_THROW && lastcf != CF_JUMP &&
        lastcf != CF_RETSUB) {
        *err = "SyntaxError: bytecode does not end in a terminator";
        return false;
    }

    fn->max_stack = max_depth + 8; /* mirror the compiler's headroom */
    return true;
}

/*
 * Verifies fn and, recursively, every nested function. `is_module` enables the
 * module opcodes (GET/SET_EXPORT, GET_IMPORT/IMPORT_NS) and `import_count`
 * bounds the import-index operands; nested functions in a module share its
 * module, so they inherit both.
 */
static bool verify_fn(JsContext *ctx, JsFunctionCell *fn, bool is_module,
                      uint32_t import_count, const char **err) {
    JsVm *vm = ctx->vm;
    uint8_t *boundary = js_realloc_raw(vm, NULL, 0, fn->code_len);
    if (!boundary) {
        *err = "out of memory";
        return false;
    }
    memset(boundary, 0, fn->code_len);
    /* the depth set: JSBC_DEPTHS_PER_OFF int32 slots per code offset */
    size_t seen_bytes = (size_t)fn->code_len * JSBC_DEPTHS_PER_OFF * sizeof(int32_t);
    int32_t *seen = js_realloc_raw(vm, NULL, 0, seen_bytes);
    if (!seen) {
        js_realloc_raw(vm, boundary, fn->code_len, 0);
        *err = "out of memory";
        return false;
    }
    /* the pass-2 worklist: at most one (off, depth) pair per distinct state,
     * i.e. n*K states, each two uint32 wide */
    size_t wl_bytes = (size_t)fn->code_len * JSBC_DEPTHS_PER_OFF * 2 * sizeof(uint32_t);
    uint32_t *worklist = js_realloc_raw(vm, NULL, 0, wl_bytes);
    if (!worklist) {
        js_realloc_raw(vm, seen, seen_bytes, 0);
        js_realloc_raw(vm, boundary, fn->code_len, 0);
        *err = "out of memory";
        return false;
    }

    bool ok = verify_pass1(fn, boundary, is_module, import_count, err) &&
              verify_pass2(fn, boundary, seen, worklist, err);

    js_realloc_raw(vm, worklist, wl_bytes, 0);
    js_realloc_raw(vm, seen, seen_bytes, 0);
    if (!ok) {
        js_realloc_raw(vm, boundary, fn->code_len, 0);
        return false;
    }
    /* Retain the instruction-start bitmap so RET_SUB can validate its (runtime,
     * attacker-influenced) return address lands on an instruction boundary. */
    fn->insn_boundary = boundary;

    /* recurse into nested functions */
    for (uint32_t i = 0; i < fn->const_count; i++) {
        JsValue c = fn->consts[i];
        if (js_value_is_cell(c) && js_value_cell(c)->kind == JS_KIND_FUNCTION) {
            if (!verify_fn(ctx, js_value_function(c), is_module, import_count, err))
                return false;
        }
    }
    return true;
}

/* Reads + checks the 16-byte header; returns the buffer kind, or -1 (with
 * *err set) on bad magic/version. Leaves b positioned at the first record. */
static int read_header(InBuf *b, const char **err) {
    if (b->end - b->p < 16) {
        *err = "SyntaxError: bytecode too short";
        return -1;
    }
    if (in_u8(b) != 'J' || in_u8(b) != 'S' || in_u8(b) != 'B' || in_u8(b) != 'C') {
        *err = "SyntaxError: bad bytecode magic";
        return -1;
    }
    if (in_u32(b) != JSBC_VERSION) {
        *err = "SyntaxError: unsupported bytecode version";
        return -1;
    }
    (void)in_u32(b); /* flags (informational) */
    uint32_t kind = in_u32(b);
    if (kind != JS_BC_SCRIPT && kind != JS_BC_MODULE) {
        *err = "SyntaxError: unknown bytecode kind";
        return -1;
    }
    return (int)kind;
}

int js_bytecode_kind(const uint8_t *buf, size_t len) {
    InBuf b = {buf, buf + len, false};
    const char *err = NULL;
    return read_header(&b, &err);
}
int js_bytecode_peek_kind(const uint8_t *buf, size_t len) {
    return js_bytecode_kind(buf, len);
}

JsValue js_bytecode_load(JsContext *ctx, const uint8_t *buf, size_t len,
                         const char **err_msg) {
    const char *dummy;
    if (!err_msg)
        err_msg = &dummy;
    *err_msg = NULL;

    InBuf b = {buf, buf + len, false};
    int kind = read_header(&b, err_msg);
    if (kind < 0)
        return js_undefined();
    if (kind != JS_BC_SCRIPT) {
        *err_msg = "SyntaxError: expected a script bytecode buffer, got a module";
        return js_undefined();
    }

    JsFunctionCell *fn = load_fn(ctx, &b, 0, err_msg);
    if (!fn) {
        if (!*err_msg)
            *err_msg = "SyntaxError: malformed bytecode";
        return js_undefined();
    }
    /* trailing bytes are suspicious — reject rather than silently ignore */
    if (b.p != b.end) {
        *err_msg = "SyntaxError: trailing bytes after bytecode";
        return js_undefined();
    }

    /* Root the loaded fn across verification (which allocates scratch). */
    JsValue fv = js_value_from_cell(&fn->gc);
    if (!js_gc_protect(ctx->vm, &fv)) {
        *err_msg = "out of memory";
        return js_undefined();
    }
    bool ok = verify_fn(ctx, fn, /*is_module=*/false, /*import_count=*/0, err_msg);
    js_gc_unprotect(ctx->vm, &fv);
    if (!ok) {
        if (!*err_msg)
            *err_msg = "SyntaxError: bytecode failed validation";
        return js_undefined();
    }
    return fv;
}

/* =====================================================================
 * Module records (specifier + import/star/dep metadata + body function).
 * Runtime state (resolved deps, live exports, status) is never written; it is
 * rebuilt by the loader and the existing instantiate/link/evaluate pipeline.
 * ===================================================================== */

static void out_str(OutBuf *b, const JsString *s) {
    out_u32(b, s->length);
    out_units(b, s->units, s->length);
}
static void out_opt_str(OutBuf *b, const JsString *s) {
    if (s) {
        out_u8(b, 1);
        out_str(b, s);
    } else {
        out_u8(b, 0);
    }
}

bool js_bc_serialize_module(JsContext *ctx, JsModule *m, uint8_t **out, size_t *out_len) {
    if (!m || !m->body)
        return false;
    OutBuf b = {ctx->vm, NULL, 0, 0, false};
    write_header(&b, JS_BC_MODULE);
    out_str(&b, m->specifier);

    out_u32(&b, m->import_count);
    for (uint32_t i = 0; i < m->import_count; i++) {
        out_u8(&b, m->imports[i].kind);
        out_str(&b, m->imports[i].specifier);
        out_opt_str(&b, m->imports[i].imported_name);
    }
    out_u32(&b, m->star_count);
    for (uint32_t i = 0; i < m->star_count; i++) {
        out_u8(&b, m->stars[i].kind);
        out_str(&b, m->stars[i].specifier);
        out_opt_str(&b, m->stars[i].imported);
        out_opt_str(&b, m->stars[i].exported);
    }
    out_u32(&b, m->dep_spec_count);
    for (uint32_t i = 0; i < m->dep_spec_count; i++)
        out_str(&b, m->dep_specs[i]);

    if (!serialize_fn(&b, m->body, 0) || b.oom) {
        js_realloc_raw(ctx->vm, b.data, b.cap, 0);
        return false;
    }
    if (b.cap != b.len) {
        uint8_t *trimmed = js_realloc_raw(ctx->vm, b.data, b.cap, b.len);
        if (trimmed)
            b.data = trimmed;
    }
    *out = b.data;
    *out_len = b.len;
    return true;
}

/* Reads a required string field into an interned JsString*; NULL on error. */
static JsString *load_req_str(JsContext *ctx, InBuf *b, const char **err) {
    JsValue v = load_string_const(ctx, b, err);
    return js_is_string(v) ? js_value_string(v) : NULL;
}
/* Reads an optional string (u8 present-flag + string). *out may be NULL. */
static bool load_opt_str(JsContext *ctx, InBuf *b, JsString **out, const char **err) {
    uint8_t has = in_u8(b);
    if (b->bad) {
        *err = "SyntaxError: truncated module record";
        return false;
    }
    if (!has) {
        *out = NULL;
        return true;
    }
    JsString *s = load_req_str(ctx, b, err);
    if (!s)
        return false;
    *out = s;
    return true;
}

/* Recursively points every function in the body tree at the loaded module, so
 * GET_IMPORT/GET_EXPORT resolve against it at runtime (the compiler wires this
 * at compile time; the loader must reconstruct it). */
static void wire_module(JsFunctionCell *fn, JsModule *m) {
    fn->module = m;
    for (uint32_t i = 0; i < fn->const_count; i++) {
        JsValue c = fn->consts[i];
        if (js_value_is_cell(c) && js_value_cell(c)->kind == JS_KIND_FUNCTION)
            wire_module(js_value_function(c), m);
    }
}

bool js_bc_load_module(JsContext *ctx, JsModule *m, const uint8_t *buf, size_t len,
                       const char **err) {
    JsVm *vm = ctx->vm;
    *err = NULL;
    InBuf b = {buf, buf + len, false};
    int kind = read_header(&b, err);
    if (kind < 0)
        return false;
    if (kind != JS_BC_MODULE) {
        *err = "SyntaxError: expected a module bytecode buffer, got a script";
        return false;
    }

    /* The caller provides the (registered, rooted-via-registry) module cell;
     * its owned arrays and the body are attached as they load, so a mid-load
     * GC stays consistent. On failure the partial fill is left on the cell —
     * the caller marks it ERRORED and the sweep reclaims the arrays. */
    JsValue mv = js_value_from_cell(&m->gc);
    if (!js_gc_protect(vm, &mv)) {
        *err = "out of memory";
        return false;
    }
    bool result = false;

    /* the record's own embedded specifier (kept only if needed; identity uses
     * canon_spec). Reading it also advances past the field. */
    JsString *embedded = load_req_str(ctx, &b, err);
    if (!embedded)
        goto done;

    /* imports */
    uint32_t import_count = in_u32(&b);
    if (b.bad || import_count > JSBC_MAX_CONSTS) {
        *err = "SyntaxError: bad module import count";
        goto done;
    }
    if (import_count) {
        m->imports = js_realloc_raw(vm, NULL, 0, (size_t)import_count * sizeof(JsModuleImport));
        if (!m->imports) {
            *err = "out of memory";
            goto done;
        }
        memset(m->imports, 0, (size_t)import_count * sizeof(JsModuleImport));
        m->import_count = import_count;
        for (uint32_t i = 0; i < import_count; i++) {
            uint8_t ik = in_u8(&b);
            if (b.bad || ik > JS_IMPORT_SIDE) {
                *err = "SyntaxError: bad import kind";
                goto done;
            }
            m->imports[i].kind = ik;
            m->imports[i].specifier = load_req_str(ctx, &b, err);
            if (!m->imports[i].specifier)
                goto done;
            if (!load_opt_str(ctx, &b, &m->imports[i].imported_name, err))
                goto done;
        }
    }

    /* star re-exports */
    uint32_t star_count = in_u32(&b);
    if (b.bad || star_count > JSBC_MAX_CONSTS) {
        *err = "SyntaxError: bad module re-export count";
        goto done;
    }
    if (star_count) {
        m->stars = js_realloc_raw(vm, NULL, 0, (size_t)star_count * sizeof(JsStarExport));
        if (!m->stars) {
            *err = "out of memory";
            goto done;
        }
        memset(m->stars, 0, (size_t)star_count * sizeof(JsStarExport));
        m->star_count = star_count;
        for (uint32_t i = 0; i < star_count; i++) {
            uint8_t sk = in_u8(&b);
            if (b.bad || sk > JS_REEXP_NAMED) {
                *err = "SyntaxError: bad re-export kind";
                goto done;
            }
            m->stars[i].kind = sk;
            m->stars[i].specifier = load_req_str(ctx, &b, err);
            if (!m->stars[i].specifier)
                goto done;
            if (!load_opt_str(ctx, &b, &m->stars[i].imported, err) ||
                !load_opt_str(ctx, &b, &m->stars[i].exported, err))
                goto done;
        }
    }

    /* dependency specifiers */
    uint32_t dep_count = in_u32(&b);
    if (b.bad || dep_count > JSBC_MAX_CONSTS) {
        *err = "SyntaxError: bad module dependency count";
        goto done;
    }
    if (dep_count) {
        m->dep_specs = js_realloc_raw(vm, NULL, 0, (size_t)dep_count * sizeof(JsString *));
        if (!m->dep_specs) {
            *err = "out of memory";
            goto done;
        }
        memset(m->dep_specs, 0, (size_t)dep_count * sizeof(JsString *));
        m->dep_spec_count = dep_count;
        for (uint32_t i = 0; i < dep_count; i++) {
            m->dep_specs[i] = load_req_str(ctx, &b, err);
            if (!m->dep_specs[i])
                goto done;
        }
    }

    /* exports (namespace) object — populated when the body runs. Real ESM's
     * Module Namespace Exotic Object has [[Prototype]] === null, not
     * Object.prototype (see js_module.c's module_get_or_compile). */
    JsValue exports = js_object_new(ctx);
    if (!js_is_object(exports)) {
        *err = "out of memory";
        goto done;
    }
    js_value_object(exports)->proto = js_undefined();
    ((JsModule *)js_value_cell(mv))->exports = js_value_object(exports);
    m = (JsModule *)js_value_cell(mv);

    /* body function */
    JsFunctionCell *body = load_fn(ctx, &b, 0, err);
    if (!body)
        goto done;
    m->body = body;
    if (b.p != b.end) {
        *err = "SyntaxError: trailing bytes after module bytecode";
        goto done;
    }
    wire_module(body, m);

    /* verify the body tree as a module body (enables + bounds module opcodes) */
    if (!verify_fn(ctx, body, /*is_module=*/true, m->import_count, err))
        goto done;

    result = true;
done:
    js_gc_unprotect(vm, &mv);
    return result;
}

JsValue js_bytecode_value(JsContext *ctx, const uint8_t *buf, size_t len) {
    JsGcCell *cell = js_gc_new_cell(ctx->vm, JS_KIND_BYTECODE, sizeof(JsBytecode) + len);
    if (!cell)
        return js_undefined();
    JsBytecode *bc = (JsBytecode *)cell;
    bc->length = len;
    if (len)
        memcpy(bc->bytes, buf, len);
    return js_value_from_cell(cell);
}
