/*
 * lamassu-js — the FRONTEND API: turning JavaScript source into code.
 *
 * Everything here needs a lexer, parser and compiler. Everything in
 * <lamassu.h> does not. A build that includes only lamassu.h and links only
 * liblamassu_runtime.a can load and run precompiled bytecode, evaluate module
 * graphs of precompiled modules, and drive the whole value/object/promise/GC
 * surface — but it has no way to turn text into executable code, because the
 * code that would do it is not in the binary.
 *
 * That is the point of the split, not a side effect of it. A fleet process
 * running precompiled bytecode links the runtime alone, so "this process
 * cannot compile attacker-supplied source" is guaranteed by the link, not by a
 * policy someone has to remember to enforce. Build-time tooling — whatever
 * produces your .jsbc files — links both halves and includes this header.
 */
#ifndef LAMASSU_COMPILE_H
#define LAMASSU_COMPILE_H

#include "lamassu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- source -> function ---- */

/*
 * The exact *err_msg js_compile_module reports when the source uses top-level
 * import/export (so it must go through the module pipeline, not run as a plain
 * script). A host distinguishes "this source is a module" by comparing *err_msg
 * to this string — an exact match on a shared constant, not a brittle substring
 * search.
 */
#define JS_ERR_NEEDS_MODULE_LOADER \
    "import/export requires the module loader (js_eval_module)"

/*
 * Compiles UTF-16 source as a strict-mode module body. Returns a function
 * value (root it per the GC contract), or undefined on error with *err_msg
 * (static ASCII) and *err_pos (source offset) set. If the source uses top-level
 * import/export, compilation fails with *err_msg == JS_ERR_NEEDS_MODULE_LOADER.
 */
JsValue js_compile_module(JsContext *ctx, const uint16_t *src, size_t len,
                          const char **err_msg, uint32_t *err_pos);

/*
 * Like js_compile_module, but top-level let/const/function declarations
 * become persistent globals on the context — so successive evaluations in
 * the same context share state (a REPL session).
 */
JsValue js_compile_module_repl(JsContext *ctx, const uint16_t *src, size_t len,
                               const char **err_msg, uint32_t *err_pos);

/* ---- source ES modules ---- */

/*
 * Grants `ctx` the ability to compile module source: after this call, a module
 * loader that fulfills with a JS string has that string parsed and compiled.
 * Without it — and there is no way to call it in a runtime-only build, because
 * this symbol is not in liblamassu_runtime.a — a source specifier fails with
 * "source modules unavailable in this build (precompile to bytecode)", while
 * loaders fulfilling with js_bytecode_value keep working normally.
 *
 * Per context, so a host embedding both trusted tooling and untrusted execution
 * can hand out contexts that differ in this one capability.
 */
void js_enable_source_modules(JsContext *ctx);

/* ---- bytecode production ---- */

/*
 * Serializes a compiled top-level function (from js_compile_module — NOT a
 * module body; import/export bytecode is not portable) into a self-describing
 * byte buffer. On success returns true with *out / *out_len set to a buffer
 * owned by the VM allocator; free it with js_bytecode_free. Returns false on
 * OOM or if fn is not a plain compiled function.
 *
 * The format is versioned and carries no source; runtime error positions
 * survive (line table is included) but mapping them to line:col needs the
 * original source. The byte order is canonical (little-endian), so a cache
 * is portable across machines of the same pointer width.
 */
bool js_bytecode_serialize(JsContext *ctx, JsValue fn, uint8_t **out, size_t *out_len);
void js_bytecode_free(JsContext *ctx, uint8_t *buf, size_t len);

/*
 * Compiles ONE module (import/export source) to a bytecode buffer WITHOUT
 * resolving, linking, or evaluating it — so each page/partial can be compiled
 * independently and cached. The buffer records the module's body plus its link
 * metadata (import descriptors, star re-exports, dependency specifiers); the
 * resolved dependency modules and live exports are runtime state rebuilt at
 * load. `specifier` becomes the module's identity/cache key. On success
 * returns true with *out / *out_len (free via js_bytecode_free); on a
 * parse/compile error returns false with *err_msg / *err_pos set.
 *
 * A module buffer is distinct from a script buffer (js_bytecode_serialize):
 * js_bytecode_load rejects a module buffer and the module loader rejects a
 * script buffer, so the two can't be confused.
 */
bool js_bytecode_compile_module(JsContext *ctx, const uint16_t *specifier,
                                size_t spec_len, const uint16_t *source,
                                size_t source_len, uint8_t **out, size_t *out_len,
                                const char **err_msg, uint32_t *err_pos);

#ifdef __cplusplus
}
#endif

#endif /* LAMASSU_COMPILE_H */
