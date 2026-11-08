/*
 * Stack traces: js_context_error_stack after a throw (frames innermost first,
 * with module specifiers and positions that map to the right lines),
 * js_context_stack from inside a native (its call site), the trace of a
 * resume-with-rejection after await, and the one-frame trace of a compile
 * error in a module graph.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lamassu.h"
#include "lamassu_compile.h"

static int checks_run, checks_failed;
#define CHECK(cond, what)                                                       \
    do {                                                                        \
        checks_run++;                                                           \
        if (!(cond)) {                                                          \
            checks_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, what);      \
        }                                                                       \
    } while (0)

static uint16_t *to_u16(const char *s, size_t *len) {
    size_t n = strlen(s);
    uint16_t *u = malloc((n + 1) * sizeof(uint16_t));
    for (size_t i = 0; i < n; i++)
        u[i] = (uint16_t)(unsigned char)s[i];
    u[n] = 0;
    *len = n;
    return u;
}

static bool str_is(JsValue v, const char *ascii) {
    size_t n;
    const uint16_t *u = js_string_units(v, &n);
    if (!u || n != strlen(ascii))
        return false;
    for (size_t i = 0; i < n; i++)
        if (u[i] != (uint16_t)(unsigned char)ascii[i])
            return false;
    return true;
}

/* ---- an in-memory module table for the loader ---- */

typedef struct { const char *name; const char *src; } Mod;
static Mod g_mods[4];
static int g_nmods;

static JsValue settled(JsContext *ctx, JsVm *vm, JsValue v, bool ok) {
    js_gc_protect(vm, &v);
    JsValue p = js_promise_new(ctx);
    if (ok)
        js_resolve(ctx, p, v);
    else
        js_reject(ctx, p, v);
    js_gc_unprotect(vm, &v);
    return p;
}

static JsValue loader(void *ud, JsContext *ctx, const uint16_t *spec, size_t spec_len,
                      const uint16_t *ref, size_t ref_len) {
    JsVm *vm = ud;
    (void)ref; (void)ref_len;
    for (int i = 0; i < g_nmods; i++) {
        size_t n = strlen(g_mods[i].name);
        if (n != spec_len)
            continue;
        bool same = true;
        for (size_t k = 0; k < n && same; k++)
            same = spec[k] == (uint16_t)g_mods[i].name[k];
        if (!same)
            continue;
        size_t sl;
        uint16_t *su = to_u16(g_mods[i].src, &sl);
        JsValue v = js_atom(vm, su, sl);
        free(su);
        return settled(ctx, vm, v, true);
    }
    size_t ml;
    uint16_t *mu = to_u16("module not found", &ml);
    JsValue reason = js_atom(vm, mu, ml);
    free(mu);
    return settled(ctx, vm, reason, false);
}

/* ---- a native that records its call site ---- */

static JsStackFrame g_seen[JS_TRACE_MAX];
static size_t g_seen_n;
static bool native_where(JsContext *ctx, JsValue this_val, const JsValue *args, int argc, JsValue *result) {
    (void)this_val; (void)args; (void)argc;
    g_seen_n = js_context_stack(ctx, g_seen, JS_TRACE_MAX);
    *result = js_undefined();
    return true;
}

static uint32_t line_of(const char *src, uint32_t pos) {
    size_t len;
    uint16_t *u = to_u16(src, &len);
    uint32_t line, col;
    js_source_line_col(u, len, pos, &line, &col);
    free(u);
    return line;
}

static void test_throw_in_nested_calls(void) {
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    /* line 1: inner throws; line 2: outer calls inner; line 4: top-level calls outer */
    const char *src =
        "function inner() { throw new TypeError('boom'); }\n"
        "function outer() { return inner(); }\n"
        "let x = 1;\n"
        "outer();\n";
    size_t len;
    uint16_t *u = to_u16(src, &len);
    const char *em = NULL;
    uint32_t ep = 0;
    JsValue fn = js_compile_module(ctx, u, len, &em, &ep);
    CHECK(js_is_function(fn), "compiles");
    js_gc_protect(vm, &fn);
    JsValue p = js_run_module(ctx, fn);
    CHECK(js_promise_state(p) == 2, "rejected");
    JsStackFrame fr[JS_TRACE_MAX];
    size_t n = js_context_error_stack(ctx, fr, JS_TRACE_MAX);
    CHECK(n == 3, "three frames: inner, outer, module body");
    if (n == 3) {
        CHECK(str_is(fr[0].function, "inner"), "frame 0 is inner");
        CHECK(str_is(fr[1].function, "outer"), "frame 1 is outer");
        CHECK(js_is_undefined(fr[2].function), "frame 2 is the module body");
        CHECK(line_of(src, fr[0].pos) == 1, "inner throws on line 1");
        CHECK(line_of(src, fr[1].pos) == 2, "outer calls on line 2");
        CHECK(line_of(src, fr[2].pos) == 4, "top-level call on line 4");
        CHECK(fr[0].pos == js_context_error_pos(ctx), "frame 0 agrees with error_pos");
        CHECK(js_is_undefined(fr[0].specifier), "no module specifier outside the module pipeline");
    }
    /* the trace survives a collection */
    js_gc_collect(vm);
    n = js_context_error_stack(ctx, fr, JS_TRACE_MAX);
    CHECK(n == 3 && str_is(fr[0].function, "inner"), "trace strings survive GC");
    js_gc_unprotect(vm, &fn);
    free(u);
    js_vm_free(vm);
}

static void test_native_call_site(void) {
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    static const uint16_t name[] = { 'w', 'h', 'e', 'r', 'e' };
    js_register_native(ctx, name, 5, native_where, NULL);
    const char *src =
        "function f() {\n"
        "  where();\n"
        "}\n"
        "f();\n";
    size_t len;
    uint16_t *u = to_u16(src, &len);
    const char *em = NULL;
    uint32_t ep = 0;
    JsValue fn = js_compile_module(ctx, u, len, &em, &ep);
    js_gc_protect(vm, &fn);
    JsValue p = js_run_module(ctx, fn);
    CHECK(js_promise_state(p) == 1, "runs");
    CHECK(g_seen_n == 2, "native sees its caller and the module body");
    if (g_seen_n == 2) {
        CHECK(str_is(g_seen[0].function, "f"), "frame 0 is f");
        CHECK(line_of(src, g_seen[0].pos) == 2, "called from line 2");
        CHECK(line_of(src, g_seen[1].pos) == 4, "f called from line 4");
    }
    js_gc_unprotect(vm, &fn);
    free(u);
    js_vm_free(vm);
}

static void test_async_rejection(void) {
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    const char *src =
        "async function go() {\n"
        "  await Promise.reject(new Error('later'));\n"
        "}\n"
        "await go();\n";
    size_t len;
    uint16_t *u = to_u16(src, &len);
    const char *em = NULL;
    uint32_t ep = 0;
    JsValue fn = js_compile_module(ctx, u, len, &em, &ep);
    js_gc_protect(vm, &fn);
    JsValue p = js_run_module(ctx, fn);
    js_gc_protect(vm, &p);
    js_run_jobs(ctx);
    CHECK(js_promise_state(p) == 2, "rejected after the await");
    JsStackFrame fr[JS_TRACE_MAX];
    size_t n = js_context_error_stack(ctx, fr, JS_TRACE_MAX);
    CHECK(n >= 1, "a frame for the resumed async function");
    if (n >= 1) {
        CHECK(str_is(fr[0].function, "go"), "frame 0 is go");
        CHECK(line_of(src, fr[0].pos) == 2, "the await on line 2");
    }
    js_gc_unprotect(vm, &p);
    js_gc_unprotect(vm, &fn);
    free(u);
    js_vm_free(vm);
}

static void test_modules(void) {
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    js_enable_source_modules(ctx);
    js_set_module_loader(ctx, loader, NULL, vm);

    /* a throw in a dependency: frames name both modules */
    g_nmods = 0;
    g_mods[g_nmods++] = (Mod){ "main.js", "import { lid } from 'lid.js';\nlid();\n" };
    g_mods[g_nmods++] = (Mod){ "lid.js", "export function lid() {\n  throw new RangeError('no lid');\n}\n" };
    static const uint16_t root[] = { 'm', 'a', 'i', 'n', '.', 'j', 's' };
    JsValue p = js_eval_module(ctx, root, 7);
    js_gc_protect(vm, &p);
    js_run_jobs(ctx);
    CHECK(js_promise_state(p) == 2, "module graph rejects");
    JsStackFrame fr[JS_TRACE_MAX];
    size_t n = js_context_error_stack(ctx, fr, JS_TRACE_MAX);
    CHECK(n == 2, "two frames across modules");
    if (n == 2) {
        CHECK(str_is(fr[0].specifier, "lid.js"), "frame 0 in lid.js");
        CHECK(line_of(g_mods[1].src, fr[0].pos) == 2, "throw on lid.js line 2");
        CHECK(str_is(fr[1].specifier, "main.js"), "frame 1 in main.js");
        CHECK(line_of(g_mods[0].src, fr[1].pos) == 2, "call on main.js line 2");
    }
    js_gc_unprotect(vm, &p);
    js_context_free(ctx);

    /* a syntax error in a dependency: one frame naming it (a fresh context: the
     * registry keeps the errored main.js, see js_eval_module) */
    ctx = js_context_new(vm);
    js_enable_source_modules(ctx);
    js_set_module_loader(ctx, loader, NULL, vm);
    g_nmods = 0;
    g_mods[g_nmods++] = (Mod){ "main.js", "import { lid } from 'bad.js';\nlid();\n" };
    g_mods[g_nmods++] = (Mod){ "bad.js", "export function lid() {\n  return (1 +;\n}\n" };
    p = js_eval_module(ctx, root, 7);
    js_gc_protect(vm, &p);
    js_run_jobs(ctx);
    CHECK(js_promise_state(p) == 2, "compile error rejects");
    n = js_context_error_stack(ctx, fr, JS_TRACE_MAX);
    CHECK(n == 1, "one frame for a compile error");
    if (n == 1) {
        CHECK(str_is(fr[0].specifier, "bad.js"), "it names the module");
        CHECK(line_of(g_mods[1].src, fr[0].pos) == 2, "at line 2");
        CHECK(fr[0].pos == js_context_error_pos(ctx), "same offset as error_pos");
    }
    js_gc_unprotect(vm, &p);
    js_context_free(ctx);

    /* a loader failure carries no frames */
    ctx = js_context_new(vm);
    js_enable_source_modules(ctx);
    js_set_module_loader(ctx, loader, NULL, vm);
    g_nmods = 0;
    g_mods[g_nmods++] = (Mod){ "main.js", "import { x } from 'missing.js';\n" };
    p = js_eval_module(ctx, root, 7);
    js_gc_protect(vm, &p);
    js_run_jobs(ctx);
    CHECK(js_promise_state(p) == 2, "missing module rejects");
    CHECK(js_context_error_stack(ctx, fr, JS_TRACE_MAX) == 0, "no frames for a load failure");
    js_gc_unprotect(vm, &p);
    js_context_free(ctx);
    js_vm_free(vm);
}

static void test_line_col(void) {
    size_t len;
    uint16_t *u = to_u16("ab\ncd\r\nef", &len);
    uint32_t line, col;
    js_source_line_col(u, len, 0, &line, &col);
    CHECK(line == 1 && col == 1, "start");
    js_source_line_col(u, len, 4, &line, &col);
    CHECK(line == 2 && col == 2, "second line");
    js_source_line_col(u, len, 8, &line, &col);
    CHECK(line == 3 && col == 2, "after CRLF");
    free(u);
}

int main(void) {
    test_throw_in_nested_calls();
    test_native_call_site();
    test_async_rejection();
    test_modules();
    test_line_col();
    printf("test_stack: %d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
