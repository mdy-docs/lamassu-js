/*
 * Stack traces: js_context_error_stack after a throw (frames innermost first,
 * with module specifiers and positions that map to the right lines),
 * js_context_stack from inside a native (its call site), the trace of a
 * resume-with-rejection after await, and the one-frame trace of a compile
 * error in a module graph. Then the place of the error a promise was rejected
 * with (js_promise_error_stack, js_error_stack), which is that error's
 * whatever was thrown after it, and the rejections nothing handles
 * (js_set_rejection_tracker).
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
    n = js_promise_error_stack(ctx, p, fr, JS_TRACE_MAX);
    CHECK(n == 2 && str_is(fr[0].specifier, "lid.js") && str_is(fr[1].specifier, "main.js"),
          "the rejection's frames: the error was made in lid.js, called from main.js");
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
    n = js_promise_error_stack(ctx, p, fr, JS_TRACE_MAX);
    CHECK(n == 1 && str_is(fr[0].specifier, "bad.js") && line_of(g_mods[1].src, fr[0].pos) == 2,
          "the rejection says the same");
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
    CHECK(js_promise_error_stack(ctx, p, fr, JS_TRACE_MAX) == 0, "nor from the rejection");
    js_gc_unprotect(vm, &p);
    js_context_free(ctx);
    js_vm_free(vm);
}

/* Runs SRC as a module and its jobs; the promise is the caller's to unprotect. */
static JsValue run_src(JsVm *vm, JsContext *ctx, const char *src, JsValue *p) {
    size_t len;
    uint16_t *u = to_u16(src, &len);
    const char *em = NULL;
    uint32_t ep = 0;
    JsValue fn = js_compile_module(ctx, u, len, &em, &ep);
    CHECK(js_is_function(fn), "compiles");
    js_gc_protect(vm, &fn);
    *p = js_run_module(ctx, fn);
    js_gc_protect(vm, p);
    js_run_jobs(ctx);
    js_gc_unprotect(vm, &fn);
    free(u);
    return *p;
}

/* An error's place is its own: the frames it was made in, whatever was thrown after it. */
static void test_rejection_keeps_its_place(void) {
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    JsStackFrame fr[JS_TRACE_MAX];
    JsValue p;

    /* the module throws on line 5 an error made on line 3; a function it started earlier
     * throws afterwards, in a job, and that is the last error to leave a function */
    const char *src =
        "async function later() { await 0; throw new Error('second'); }\n"
        "later().catch(() => {});\n"
        "function make() { return new Error('first'); }\n"
        "const e = make();\n"
        "throw e;\n";
    run_src(vm, ctx, src, &p);
    CHECK(js_promise_state(p) == 2, "rejected");
    size_t n = js_context_error_stack(ctx, fr, JS_TRACE_MAX);
    CHECK(n >= 1 && str_is(fr[0].function, "later"), "the context's last error is the later one");
    n = js_promise_error_stack(ctx, p, fr, JS_TRACE_MAX);
    CHECK(n == 2, "the rejection has the frames its error was made in");
    if (n == 2) {
        CHECK(str_is(fr[0].function, "make"), "frame 0 is make");
        CHECK(line_of(src, fr[0].pos) == 3, "made on line 3");
        CHECK(js_is_undefined(fr[1].function), "frame 1 is the module body");
        CHECK(line_of(src, fr[1].pos) == 4, "make called on line 4");
    }
    JsValue reason = js_promise_result(p);
    n = js_error_stack(ctx, reason, fr, JS_TRACE_MAX);
    CHECK(n == 2 && line_of(src, fr[0].pos) == 3, "js_error_stack reads them from the error");
    js_gc_collect(vm);
    n = js_promise_error_stack(ctx, p, fr, JS_TRACE_MAX);
    CHECK(n == 2 && str_is(fr[0].function, "make"), "and they survive a collection");
    CHECK(js_error_stack(ctx, js_number(1), fr, JS_TRACE_MAX) == 0, "no frames for a number");
    CHECK(js_promise_error_stack(ctx, js_number(1), fr, JS_TRACE_MAX) == 0, "nor for no promise");
    js_gc_unprotect(vm, &p);

    /* a thrown value that is no error has the trace of its throw */
    src = "function f() { throw 'plain'; }\n"
          "f();\n";
    run_src(vm, ctx, src, &p);
    CHECK(js_promise_state(p) == 2, "rejected with a string");
    CHECK(js_error_stack(ctx, js_promise_result(p), fr, JS_TRACE_MAX) == 0, "a string has no frames");
    n = js_promise_error_stack(ctx, p, fr, JS_TRACE_MAX);
    CHECK(n == 2, "the rejection has the trace of the throw");
    if (n == 2) {
        CHECK(str_is(fr[0].function, "f") && line_of(src, fr[0].pos) == 1, "thrown in f, line 1");
        CHECK(line_of(src, fr[1].pos) == 2, "f called on line 2");
    }
    js_gc_unprotect(vm, &p);

    /* ...through an await: the functions that waited for it are in the trace */
    src = "function f() { throw 'deep'; }\n"
          "async function go() { await 0; f(); }\n"
          "await go();\n";
    run_src(vm, ctx, src, &p);
    CHECK(js_promise_state(p) == 2, "rejected after the await");
    n = js_promise_error_stack(ctx, p, fr, JS_TRACE_MAX);
    CHECK(n == 3, "the trace of the throw, through the await");
    if (n == 3) {
        CHECK(str_is(fr[0].function, "f") && line_of(src, fr[0].pos) == 1, "thrown in f, line 1");
        CHECK(str_is(fr[1].function, "go") && line_of(src, fr[1].pos) == 2, "f called in go, line 2");
        CHECK(js_is_undefined(fr[2].function) && line_of(src, fr[2].pos) == 3, "go awaited on line 3");
    }
    js_gc_unprotect(vm, &p);

    /* ...but not once another error has left a function: the same value thrown later, in a
     * job, is another throw, and its trace is not this rejection's */
    src = "async function later() { await 0; throw 5; }\n"
          "later().catch(() => {});\n"
          "throw 5;\n";
    run_src(vm, ctx, src, &p);
    CHECK(js_promise_state(p) == 2, "rejected with 5");
    n = js_context_error_stack(ctx, fr, JS_TRACE_MAX);
    CHECK(n >= 1 && str_is(fr[0].function, "later"), "the last throw of 5 was the later one");
    CHECK(js_promise_error_stack(ctx, p, fr, JS_TRACE_MAX) == 0, "which is not the rejection's: no frames");
    js_gc_unprotect(vm, &p);

    /* an error whose stack the script took has no frames of its own (as in V8): the trace of
     * its throw stands in */
    src = "const e = new Error('read');\n"
          "const s = e.stack;\n"
          "throw e;\n";
    run_src(vm, ctx, src, &p);
    CHECK(js_error_stack(ctx, js_promise_result(p), fr, JS_TRACE_MAX) == 0, "frames gone with the stack");
    n = js_promise_error_stack(ctx, p, fr, JS_TRACE_MAX);
    CHECK(n == 1 && line_of(src, fr[0].pos) == 3, "the throw on line 3");
    js_gc_unprotect(vm, &p);

    /* a value nothing threw until an await did: the await is its place */
    run_src(vm, ctx, "await Promise.reject(7);\n", &p);
    CHECK(js_promise_state(p) == 2, "rejected with 7");
    n = js_promise_error_stack(ctx, p, fr, JS_TRACE_MAX);
    CHECK(n == 1, "the await that threw it");
    js_gc_unprotect(vm, &p);
    run_src(vm, ctx, "Promise.reject(7);\n", &p);
    CHECK(js_promise_state(p) == 1, "the module is fulfilled");
    CHECK(js_promise_error_stack(ctx, p, fr, JS_TRACE_MAX) == 0, "no frames for a fulfilled promise");
    js_gc_unprotect(vm, &p);

    js_vm_free(vm);
}

/* ---- the rejections nothing handles ---- */

#define UNHANDLED_CAP 8
static JsValue g_unhandled[UNHANDLED_CAP];
static int g_unhandled_n, g_tracker_calls;
static size_t g_tracker_frames; /* of the last rejection reported, asked for as it was */

static void tracker(JsContext *ctx, JsValue promise, bool handled, void *ud) {
    CHECK(ud == (void *)g_unhandled, "the tracker's userdata");
    g_tracker_calls++;
    if (!handled) {
        JsStackFrame fr[JS_TRACE_MAX];
        g_tracker_frames = js_promise_error_stack(ctx, promise, fr, JS_TRACE_MAX);
        if (g_unhandled_n < UNHANDLED_CAP)
            g_unhandled[g_unhandled_n++] = promise;
        return;
    }
    for (int i = 0; i < g_unhandled_n; i++) {
        if (js_same_value(g_unhandled[i], promise)) {
            for (int k = i + 1; k < g_unhandled_n; k++)
                g_unhandled[k - 1] = g_unhandled[k];
            g_unhandled[--g_unhandled_n] = js_undefined();
            return;
        }
    }
    CHECK(false, "handled is reported only of a promise reported unhandled");
}

static void test_rejection_tracker(void) {
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    for (int i = 0; i < UNHANDLED_CAP; i++) {
        g_unhandled[i] = js_undefined();
        js_gc_protect(vm, &g_unhandled[i]);
    }
    g_unhandled_n = g_tracker_calls = 0;
    js_set_rejection_tracker(ctx, tracker, g_unhandled);
    JsStackFrame fr[JS_TRACE_MAX];
    JsValue p;

    /* a: never handled. b: handled a job later. f(): rejected, and awaited at once. */
    const char *src =
        "const a = Promise.reject(new Error('a'));\n"
        "const b = Promise.reject(new Error('b'));\n"
        "Promise.resolve().then(() => b.catch(() => {}));\n"
        "async function f() { throw new Error('c'); }\n"
        "try { await f(); } catch (e) {}\n"
        "Promise.reject(1).then(() => {}, () => {});\n"
        "Promise.all([Promise.reject(2)]).catch(() => {});\n";
    run_src(vm, ctx, src, &p);
    CHECK(js_promise_state(p) == 1, "the module is fulfilled");
    CHECK(g_tracker_calls >= 4, "a and b unhandled, b and f() handled");
    CHECK(g_unhandled_n == 1, "one rejection is left unhandled");
    if (g_unhandled_n == 1) {
        CHECK(js_promise_state(g_unhandled[0]) == 2, "a rejected promise");
        CHECK(str_is(js_to_string(ctx, js_promise_result(g_unhandled[0])), "Error: a"), "it is a");
        size_t n = js_promise_error_stack(ctx, g_unhandled[0], fr, JS_TRACE_MAX);
        CHECK(n == 1 && line_of(src, fr[0].pos) == 1, "made on line 1");
    }
    js_gc_unprotect(vm, &p);
    g_unhandled[0] = js_undefined();
    g_unhandled_n = 0;

    /* a promise derived from a handled one is another promise, and nothing handles it */
    run_src(vm, ctx, "Promise.reject(new Error('d')).then(() => {});\n", &p);
    CHECK(g_unhandled_n == 1, "the promise then() returned is unhandled");
    CHECK(g_unhandled_n == 1 && str_is(js_to_string(ctx, js_promise_result(g_unhandled[0])), "Error: d"),
          "rejected with d");
    js_gc_unprotect(vm, &p);
    g_unhandled[0] = js_undefined();
    g_unhandled_n = 0;

    /* a rejection nothing threw has no place, though the same value was thrown before it */
    run_src(vm, ctx, "try { throw 7; } catch (e) {}\nPromise.reject(7);\n", &p);
    CHECK(g_unhandled_n == 1 && js_promise_state(g_unhandled[0]) == 2, "Promise.reject(7) is unhandled");
    CHECK(g_unhandled_n == 1 && js_promise_error_stack(ctx, g_unhandled[0], fr, JS_TRACE_MAX) == 0,
          "and has no frames: the throw of 7 that was caught is not its");
    js_gc_unprotect(vm, &p);
    g_unhandled[0] = js_undefined();
    g_unhandled_n = 0;

    /* the promise of the run is the host's: reported like any other */
    run_src(vm, ctx, "throw new Error('own');\n", &p);
    CHECK(js_promise_state(p) == 2, "the module is rejected");
    CHECK(g_unhandled_n == 1 && js_same_value(g_unhandled[0], p), "and its promise is reported");
    js_gc_unprotect(vm, &p);
    g_unhandled[0] = js_undefined();
    g_unhandled_n = 0;
    run_src(vm, ctx, "function f() { throw 'value'; }\nf();\n", &p);
    CHECK(g_unhandled_n == 1 && js_same_value(g_unhandled[0], p), "so is one rejected with a string");
    CHECK(g_tracker_frames == 2, "whose place the tracker can ask for as it is told");
    js_gc_unprotect(vm, &p);
    g_unhandled[0] = js_undefined();
    g_unhandled_n = 0;

    /* no tracker, no reports */
    js_set_rejection_tracker(ctx, NULL, NULL);
    int calls = g_tracker_calls;
    run_src(vm, ctx, "Promise.reject(new Error('quiet'));\n", &p);
    CHECK(g_tracker_calls == calls && g_unhandled_n == 0, "nothing is reported without a tracker");
    js_gc_unprotect(vm, &p);

    for (int i = 0; i < UNHANDLED_CAP; i++)
        js_gc_unprotect(vm, &g_unhandled[i]);
    js_vm_free(vm);
}

/* In a module graph: an import that fails and is caught leaves nothing unhandled, and a syntax
 * error keeps its place however long the script holds it. */
static void test_module_rejections(void) {
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    js_enable_source_modules(ctx);
    js_set_module_loader(ctx, loader, NULL, vm);
    for (int i = 0; i < UNHANDLED_CAP; i++) {
        g_unhandled[i] = js_undefined();
        js_gc_protect(vm, &g_unhandled[i]);
    }
    g_unhandled_n = g_tracker_calls = 0;
    js_set_rejection_tracker(ctx, tracker, g_unhandled);
    JsStackFrame fr[JS_TRACE_MAX];

    g_nmods = 0;
    g_mods[g_nmods++] = (Mod){ "main.js",
        "let kept;\n"
        "try { await import('bad.js'); } catch (e) { kept = e; }\n"
        "try { await import('thrower.js'); } catch (e) {}\n"
        "try { await import('thrower.js'); } catch (e) {}\n"
        "try { await import('missing.js'); } catch (e) {}\n"
        "try { null.x; } catch (e) {}\n"
        "throw kept;\n" };
    g_mods[g_nmods++] = (Mod){ "bad.js", "export function lid() {\n  return (1 +;\n}\n" };
    g_mods[g_nmods++] = (Mod){ "thrower.js", "throw new TypeError('thrower');\n" };
    static const uint16_t root[] = { 'm', 'a', 'i', 'n', '.', 'j', 's' };
    JsValue p = js_eval_module(ctx, root, 7);
    js_gc_protect(vm, &p);
    js_run_jobs(ctx);
    CHECK(js_promise_state(p) == 2, "the root rejects with the syntax error it kept");
    CHECK(g_unhandled_n == 1 && js_same_value(g_unhandled[0], p),
          "only the root's promise is unhandled: the imports were caught");
    size_t n = js_promise_error_stack(ctx, p, fr, JS_TRACE_MAX);
    CHECK(n == 1, "one frame for the syntax error");
    if (n == 1) {
        CHECK(str_is(fr[0].specifier, "bad.js"), "it names the module");
        CHECK(line_of(g_mods[1].src, fr[0].pos) == 2, "at line 2");
    }
    n = js_error_stack(ctx, js_promise_result(p), fr, JS_TRACE_MAX);
    CHECK(n == 1 && str_is(fr[0].specifier, "bad.js"), "the error itself says so");
    js_gc_unprotect(vm, &p);
    for (int i = 0; i < UNHANDLED_CAP; i++)
        js_gc_unprotect(vm, &g_unhandled[i]);
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
    test_rejection_keeps_its_place();
    test_rejection_tracker();
    test_module_rejections();
    test_line_col();
    printf("test_stack: %d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
