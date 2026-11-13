/*
 * Bytecode wire format — the constants the writer and the reader must agree on.
 *
 * The two halves live on opposite sides of the frontend/runtime split
 * (src/frontend/js_bytecode_write.c emits, src/runtime/js_bytecode_read.c
 * validates and loads) and a fleet runtime links only the reader. This header
 * is the entire contract between them, so a format change is one edit here plus
 * one on each side — and a version bump nobody can forget, because the reader
 * checks JSBC_VERSION on every load.
 *
 * Format (all integers little-endian; doubles as their IEEE-754 bit pattern,
 * LE): a 16-byte header (magic "JSBC", u32 version, u32 flags, u32 reserved)
 * then the root function record. A function record is n_params/flags/n_locals/
 * upvalue table/name/code/constants/line-table, with constants tagged by kind
 * and nested functions recursively inlined (CTAG_FUNCTION).
 */
#ifndef JS_BCFORMAT_H
#define JS_BCFORMAT_H

#define JSBC_VERSION 12u /* 3: JS_FN_USES_ARGUMENTS function flag; 4: callee-name table after the line table;
                           5: JS_OP_THROW_CONST_ASSIGN, and the name table also marks iteration,
                           destructuring and TDZ-able reads;
                           6: JS_OP_SET_LOCAL_CHECKED, JS_OP_SET_UPVAL_CHECKED, JS_OP_INIT_EXPORT,
                           and a module's let/const export names after its dependency list;
                           7: JS_OP_MODULE_READY after a module's hoisted functions;
                           8: JS_OP_SET_FUNCTION_NAME, inferred names (arrows too), and a named function expression's
                           own name (JS_OP_GET_CALLEE);
                           9: a function's length after its parameter count;
                           10: a function's source range after its name, and the root's source text
                           (u32 length, then UTF-16 units; length 0 for none) after its range;
                           11: a function's inferred debug name (u8 present, then the string) after
                           the source;
                           12: JS_OP_INSTANCEOF */
#define JSBC_FLAG_HAS_REGEX 1u /* producer had regex enabled (informational) */

/* Sanity caps: reject absurd sizes early (before allocating) rather than
 * trusting the buffer. Generous vs. anything the compiler emits. */
#define JSBC_MAX_CODE (4u * 1024 * 1024)
#define JSBC_MAX_SOURCE (64u * 1024 * 1024) /* UTF-16 units of the source text */
#define JSBC_MAX_CONSTS 65536u /* const indices are u16 */
#define JSBC_MAX_LOCALS 65536u
#define JSBC_MAX_NAME 4096u
#define JSBC_MAX_LINES (4u * 1024 * 1024)
#define JSBC_MAX_CALLEE_TEXT (64u * 1024 * 1024) /* UTF-16 units of callee text per function */
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

#endif /* JS_BCFORMAT_H */
