/*
 * Compiling an ES module from SOURCE — the frontend side of the module seam.
 *
 * The module pipeline (registry, resolution, linking, evaluation, and loading
 * precompiled module bytecode) is all runtime code in src/runtime/js_module.c.
 * Exactly one branch of it needs a parser: a loader that fulfills with a source
 * string. That branch calls through ctx->compile_source, which this file
 * installs — so linking the frontend is what grants a context the ability to
 * turn source into code, and a runtime-only process simply cannot.
 *
 * js_bytecode_compile_module lives here for the same reason: it is a build-time
 * producer (source -> cacheable module bytecode), the exact operation a fleet
 * does offline and never at request time.
 */
#include "js_bcformat.h"
#include "js_compile.h"
#include "lamassu_compile.h"
#include "lamassu_internal.h"

static JsValue module_value(JsModule *m) { return js_value_from_cell(&m->gc); }
static JsModule *value_module(JsValue v) { return (JsModule *)js_value_cell(v); }

/* Parses + compiles `source` into m (exports object, body, import/star/dep
 * metadata). m must be rooted by the caller (registry or protected slot). */
static bool module_compile_source(JsContext *ctx, JsModule *m, const uint16_t *source,
                                  size_t source_len, JsModError *err) {
    JsVm *vm = ctx->vm;
    JsValue mv = module_value(m);
    js_gc_protect(vm, &mv);

    /* exports (namespace) object. Real ESM's Module Namespace Exotic Object
     * has [[Prototype]] === null (not Object.prototype like an ordinary
     * object) — js_object_new(ctx) gives every new object Object.prototype
     * by default, so undo that here to match spec exactly. */
    JsValue exports = js_object_new(ctx);
    if (!js_is_object(exports)) {
        js_gc_unprotect(vm, &mv);
        err->oom = true;
        return false;
    }
    js_value_object(exports)->proto = js_undefined();
    value_module(mv)->exports = js_value_object(exports);

    JsArena arena;
    js_arena_init(&arena, vm);
    JsParseResult pr;
    JsFunctionCell *body = NULL;
    if (!js_parse_module(&arena, source, source_len, &pr)) {
        err->msg = pr.err_msg;
        err->pos = pr.err_pos;
    } else {
        JsCompileError ce;
        body = js_compile_module_body(ctx, pr.module, value_module(mv), &ce);
        if (!body) {
            err->msg = ce.msg;
            err->pos = ce.pos;
        }
    }
    js_arena_free(&arena);
    js_gc_unprotect(vm, &mv);
    if (!body)
        return false;
    m->body = body;
    return true;
}

void js_enable_source_modules(JsContext *ctx) {
    if (ctx)
        ctx->compile_source = module_compile_source;
}

/* ---- bytecode producer (host-side cache tooling) ---- */

bool js_bytecode_compile_module(JsContext *ctx, const uint16_t *specifier,
                                size_t spec_len, const uint16_t *source,
                                size_t source_len, uint8_t **out, size_t *out_len,
                                const char **err_msg, uint32_t *err_pos) {
    JsVm *vm = ctx->vm;
    *err_msg = NULL;
    *err_pos = 0;

    JsValue specv = js_atom(vm, specifier, spec_len);
    if (!js_is_string(specv)) {
        *err_msg = "out of memory";
        return false;
    }
    js_gc_protect(vm, &specv);

    /* Compile this module alone — unregistered, no resolution, linking, or
     * evaluation — so it can be cached independently of its deps. */
    JsModule *m = js_module_alloc_placeholder(ctx, js_value_string(specv));
    if (!m) {
        js_gc_unprotect(vm, &specv);
        *err_msg = "out of memory";
        return false;
    }
    JsValue mv = module_value(m);
    js_gc_protect(vm, &mv);
    JsModError err = {NULL, 0, false};
    bool ok = module_compile_source(ctx, m, source, source_len, &err);
    if (!ok) {
        *err_msg = err.oom ? "out of memory" : (err.msg ? err.msg : "module compile error");
        *err_pos = err.pos;
    } else {
        ok = js_bc_serialize_module(ctx, value_module(mv), out, out_len);
        if (!ok)
            *err_msg = "bytecode serialization failed";
    }
    js_gc_unprotect(vm, &mv);
    js_gc_unprotect(vm, &specv);
    return ok;
}

