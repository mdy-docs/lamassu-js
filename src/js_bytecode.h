/*
 * Bytecode: stack machine, u8 opcodes, little-endian u16/u32 immediates,
 * absolute u32 jump targets. Compiled functions and fibers are GC cells
 * (layouts in jsvm_internal.h) so everything the interpreter touches is
 * always reachable — the fiber's stack roots all intermediate values, which
 * is what makes GC at any allocation site safe.
 */
#ifndef JS_BYTECODE_H
#define JS_BYTECODE_H

#include "js_syntax.h"

typedef enum JsOp {
    JS_OP_NOP = 0,
    JS_OP_CONST,           /* u16 const idx        -> push */
    JS_OP_UNDEFINED, JS_OP_NULL, JS_OP_TRUE, JS_OP_FALSE,
    JS_OP_POP,
    JS_OP_DUP,             /* a -> a a */
    JS_OP_DUP2,            /* a b -> a b a b */
    JS_OP_SWAP,            /* a b -> b a */
    JS_OP_ROT3,            /* a b c -> b c a */
    JS_OP_GET_LOCAL,       /* u16; TDZ-checked */
    JS_OP_SET_LOCAL,       /* u16; peeks (value stays) */
    JS_OP_TDZ,             /* u16; slot <- TDZ sentinel */
    JS_OP_GET_GLOBAL,      /* u16 const idx (atom); ReferenceError if absent */
    JS_OP_GET_GLOBAL_SOFT, /* u16; undefined if absent (typeof) */
    JS_OP_SET_GLOBAL,      /* u16; peeks; ReferenceError if undeclared */
    JS_OP_ADD, JS_OP_SUB, JS_OP_MUL, JS_OP_DIV, JS_OP_MOD, JS_OP_POW,
    JS_OP_NEG, JS_OP_POS, JS_OP_NOT, JS_OP_BITNOT,
    JS_OP_BITAND, JS_OP_BITOR, JS_OP_BITXOR,
    JS_OP_SHL, JS_OP_SHR, JS_OP_USHR,
    JS_OP_EQ, JS_OP_NEQ, JS_OP_STRICT_EQ, JS_OP_STRICT_NEQ,
    JS_OP_LT, JS_OP_GT, JS_OP_LE, JS_OP_GE, JS_OP_IN,
    JS_OP_TYPEOF, JS_OP_TO_STRING,
    JS_OP_JUMP,            /* u32 */
    JS_OP_JUMP_IF_FALSE,   /* u32; pops */
    JS_OP_JUMP_IF_TRUE,    /* u32; pops */
    JS_OP_JF_PEEK,         /* u32; jump if falsy, keep (&&) */
    JS_OP_JT_PEEK,         /* u32; jump if truthy, keep (||) */
    JS_OP_JNN_PEEK,        /* u32; jump if not nullish, keep (??) */
    JS_OP_OPT_CHAIN,       /* u32; if nullish: pop, push undefined, jump */
    JS_OP_CASE,            /* u32; pop t; if t === peek: pop, jump */
    JS_OP_NEW_OBJECT,
    JS_OP_NEW_ARRAY,       /* u16 count; pops count elements */
    JS_OP_ARRAY_APPEND,    /* arr v -> arr */
    JS_OP_ARRAY_SPREAD,    /* arr iterable -> arr */
    JS_OP_ARRAY_REST,      /* src i -> restArray (elems src[i..]) */
    JS_OP_OBJ_SPREAD,      /* obj src -> obj */
    JS_OP_DEFINE_PROP,     /* obj k v -> obj */
    JS_OP_GET_PROP,        /* obj k -> v */
    JS_OP_SET_PROP,        /* obj k v -> v */
    JS_OP_GET_PROP_ATOM,   /* u16; obj -> v */
    JS_OP_SET_PROP_ATOM,   /* u16; obj v -> v */
    JS_OP_DELETE_PROP,     /* obj k -> bool */
    JS_OP_ITER_NEW,        /* iterable -> iterable 0 */
    JS_OP_ITER_NEXT,       /* u32 done target; it i -> it i' v | (pops both, jumps) */
    JS_OP_THROW,
    JS_OP_RETURN,          /* pops; closes frame upvalues; pops frame */
    /* phase 4 */
    JS_OP_CLOSURE,         /* u16 proto const idx, u8 flags(1=bind this) -> closure */
    JS_OP_GET_UPVAL,       /* u16; TDZ-checked */
    JS_OP_SET_UPVAL,       /* u16; peeks */
    JS_OP_CLOSE_UPVALS,    /* u16 slot floor (frame-relative) */
    JS_OP_GET_THIS,
    JS_OP_GET_CALLEE,
    JS_OP_CALL,            /* u8 argc; callee this args... -> result */
    JS_OP_CALL_VARARGS,    /* callee this argsArray -> result */
    JS_OP_OPT_CALL_CHECK,  /* u32; if callee (under this) nullish: pop2, push undefined, jump */
    JS_OP_TRY_PUSH,        /* u32 handler ip */
    JS_OP_TRY_POP,
    JS_OP_GOSUB,           /* u32; pushes return ip, jumps */
    JS_OP_RET_SUB,         /* pops return ip, jumps */
} JsOp;

/* TDZ sentinel: internal special value, never exposed to scripts. */
#define JS_SPECIAL_TDZ (JS_TAG_SPECIAL | 4)

typedef struct JsCompileError {
    const char *msg; /* static ASCII */
    uint32_t pos;    /* source offset */
} JsCompileError;

/* AST -> function cell; NULL on error. The result must be rooted by caller. */
JsFunctionCell *js_compile_ast(JsContext *ctx, const JsAstNode *module,
                               JsCompileError *err);

/* Runs fn on a fresh fiber. False -> *result is the error value. */
bool js_interp_run(JsContext *ctx, JsFunctionCell *fn, JsValue *result);

/* Runtime conversions shared beyond the interpreter. */
bool      js_to_boolean(JsValue v);
double    js_to_number_value(JsContext *ctx, JsValue v);
JsString *js_to_string_cell(JsContext *ctx, JsValue v, int depth); /* NULL on OOM */
bool      js_strict_equals(JsValue a, JsValue b);
JsString *js_ascii_cell(JsVm *vm, const char *s); /* <=128 chars */
JsString *js_concat_cells(JsVm *vm, const JsString *a, const JsString *b);
bool      js_spread_into_object(JsContext *ctx, JsObject *dst, JsValue src);

/* js_builtins.c: installs the standard library into a fresh context. */
bool js_builtins_init(JsContext *ctx);

/* Creates a native function cell (not registered anywhere); undefined on OOM. */
JsValue js_native_new(JsContext *ctx, const char *name, JsNativeFn fn, void *ud);
/* Defines an own property by ASCII key; false on OOM. */
bool js_object_set_ascii(JsContext *ctx, JsObject *obj, const char *key, JsValue v);

/* js_number.c */
double  js_make_double(uint64_t mant, int exp10);
size_t  js_dtoa(double d, char *buf /* >= 32 bytes */);
double  js_units_to_number(const uint16_t *units, size_t len);
int32_t js_to_int32(double d);
uint32_t js_to_uint32(double d);
JsString *js_number_to_string(JsVm *vm, double d);

#endif /* JS_BYTECODE_H */
