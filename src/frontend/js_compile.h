/*
 * The frontend's internal seam: AST -> bytecode.
 *
 * These declarations used to live in js_bytecode.h, which meant the runtime's
 * bytecode header had to include the frontend's AST header — so every runtime
 * translation unit saw the parser's types even though none of them could use
 * one. Keeping them here is what lets js_bytecode.h stand on lamassu_internal.h
 * alone, and it means a runtime source that reaches for the compiler now fails
 * to COMPILE rather than merely failing to link.
 */
#ifndef JS_COMPILE_H
#define JS_COMPILE_H

#include "js_bytecode.h"
#include "js_syntax.h"

typedef struct JsCompileError {
    const char *msg; /* static ASCII */
    uint32_t pos;    /* source offset */
} JsCompileError;

/* AST -> function cell; NULL on error. The result must be rooted by caller.
 * repl=true makes top-level declarations persistent globals (REPL sessions). */
JsFunctionCell *js_compile_ast(JsContext *ctx, const JsAstNode *module, bool repl,
                               JsCompileError *err);

/* Compiles a parsed module AST into a body function bound to `mod`. */
JsFunctionCell *js_compile_module_body(JsContext *ctx, const JsAstNode *module,
                                       JsModule *mod, JsCompileError *err);


/*
 * Serializes one compiled (unlinked) module — its specifier, import/star/dep
 * metadata, and body function tree — to a VM-owned buffer. Runtime state
 * (resolved deps, live exports, status) is not written. false on OOM.
 */
bool js_bc_serialize_module(JsContext *ctx, JsModule *m, uint8_t **out, size_t *out_len);

#endif /* JS_COMPILE_H */
