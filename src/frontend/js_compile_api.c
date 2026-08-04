/*
 * The frontend's public entry points: source text -> a runnable function.
 *
 * These two functions are the whole reason a build links the frontend at all.
 * Everything else the embedder calls (js_run_module, js_bytecode_load,
 * js_eval_module over precompiled modules, the entire value/object/GC surface)
 * is runtime-only, which is what lets a fleet process ship without a parser.
 *
 * Declared in include/lamassu_compile.h, not include/lamassu.h.
 */
#include "js_compile.h"
#include "lamassu_compile.h"
#include "lamassu_internal.h"

static JsValue compile_module_impl(JsContext *ctx, const uint16_t *src, size_t len,
                                   bool repl, const char **err_msg, uint32_t *err_pos) {
    *err_msg = NULL;
    *err_pos = 0;
    JsArena arena;
    js_arena_init(&arena, ctx->vm);
    JsParseResult pr;
    if (!js_parse_module(&arena, src, len, &pr)) {
        *err_msg = pr.err_msg;
        *err_pos = pr.err_pos;
        js_arena_free(&arena);
        return js_undefined();
    }
    JsCompileError ce;
    JsFunctionCell *fn = js_compile_ast(ctx, pr.module, repl, &ce);
    js_arena_free(&arena);
    if (!fn) {
        *err_msg = ce.msg;
        *err_pos = ce.pos;
        return js_undefined();
    }
    return js_value_from_cell(&fn->gc);
}

JsValue js_compile_module(JsContext *ctx, const uint16_t *src, size_t len,
                          const char **err_msg, uint32_t *err_pos) {
    return compile_module_impl(ctx, src, len, false, err_msg, err_pos);
}

JsValue js_compile_module_repl(JsContext *ctx, const uint16_t *src, size_t len,
                               const char **err_msg, uint32_t *err_pos) {
    return compile_module_impl(ctx, src, len, true, err_msg, err_pos);
}
