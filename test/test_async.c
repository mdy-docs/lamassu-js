/*
 * Async / promise tests. Each script is compiled and run; js_run_module
 * drains the microtask queue, so a top-level-await module's completion value
 * is what we assert. A `defer(v)` host native returns a promise resolved with
 * v on the *next* drain step, exercising real host-driven suspension.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lamassu.h"

static int checks_run, checks_failed;

typedef struct {
    long net_bytes, live_allocs;
} CountAlloc;

static void *count_realloc(void *ud, void *ptr, size_t old_size, size_t new_size) {
    CountAlloc *ca = ud;
    if (new_size == 0) {
        if (ptr) {
            ca->net_bytes -= (long)old_size;
            ca->live_allocs--;
            free(ptr);
        }
        return NULL;
    }
    void *p = realloc(ptr, new_size);
    if (!p)
        return NULL;
    ca->net_bytes += (long)new_size - (long)old_size;
    if (!ptr)
        ca->live_allocs++;
    return p;
}

/* Pending host promises awaiting settlement, so a native can defer. */
#define MAX_DEFERRED 64
typedef struct {
    JsVm *vm;
    JsValue promises[MAX_DEFERRED];
    JsValue values[MAX_DEFERRED];
    int count;
} Deferred;
static Deferred g_deferred;

/* defer(v): returns a promise that resolves with v when flush_deferred runs. */
static bool native_defer(JsContext *ctx, JsValue this_val, const JsValue *args,
                         int argc, JsValue *result) {
    (void)this_val;
    JsValue p = js_promise_new(ctx);
    if (!js_is_promise(p)) {
        *result = js_undefined();
        return false;
    }
    if (g_deferred.count < MAX_DEFERRED) {
        int i = g_deferred.count++;
        g_deferred.promises[i] = p;
        g_deferred.values[i] = argc > 0 ? args[0] : js_undefined();
        /* keep both rooted until we settle */
        js_gc_protect(g_deferred.vm, &g_deferred.promises[i]);
        js_gc_protect(g_deferred.vm, &g_deferred.values[i]);
    }
    *result = p;
    return true;
}

static bool g_reject_flag[MAX_DEFERRED];

/* Drains all deferred host promises (resolving/rejecting), then microtasks,
 * repeatedly, until nothing is pending. */
static void flush_deferred(JsContext *ctx) {
    for (int guard = 0; guard < 1000; guard++) {
        if (g_deferred.count == 0 && !js_has_pending_jobs(ctx))
            break;
        int n = g_deferred.count;
        g_deferred.count = 0;
        for (int i = 0; i < n; i++) {
            if (g_reject_flag[i])
                js_reject(ctx, g_deferred.promises[i], g_deferred.values[i]);
            else
                js_resolve(ctx, g_deferred.promises[i], g_deferred.values[i]);
            js_gc_unprotect(g_deferred.vm, &g_deferred.values[i]);
            js_gc_unprotect(g_deferred.vm, &g_deferred.promises[i]);
            g_reject_flag[i] = false;
        }
        js_run_jobs(ctx);
    }
}

/* rejectLater(e): promise that rejects with e on flush. */
static bool native_reject_later2(JsContext *ctx, JsValue this_val, const JsValue *args,
                                 int argc, JsValue *result) {
    (void)this_val;
    JsValue p = js_promise_new(ctx);
    if (!js_is_promise(p)) {
        *result = js_undefined();
        return false;
    }
    if (g_deferred.count < MAX_DEFERRED) {
        int i = g_deferred.count++;
        g_deferred.promises[i] = p;
        g_deferred.values[i] = argc > 0 ? args[0] : js_undefined();
        g_reject_flag[i] = true;
        js_gc_protect(g_deferred.vm, &g_deferred.promises[i]);
        js_gc_protect(g_deferred.vm, &g_deferred.values[i]);
    }
    *result = p;
    return true;
}

static char *run(const char *src, bool stress, bool *ok) {
    CountAlloc ca = {0, 0};
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca, .gc_stress = stress};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    g_deferred.vm = vm;
    g_deferred.count = 0;
    memset(g_reject_flag, 0, sizeof g_reject_flag);

    static const uint16_t n_defer[] = {'d', 'e', 'f', 'e', 'r'};
    static const uint16_t n_reject[] = {'r', 'e', 'j', 'e', 'c', 't', 'L', 'a', 't', 'e', 'r'};
    js_register_native(ctx, n_defer, 5, native_defer, NULL);
    js_register_native(ctx, n_reject, 11, native_reject_later2, NULL);
    /* predefine global OUT so strict-mode `OUT = ...` assignment is allowed */
    static const uint16_t k_out_def[] = {'O', 'U', 'T'};
    JsValue outkey = js_atom(vm, k_out_def, 3);
    js_gc_protect(vm, &outkey);
    js_object_set(vm, js_context_globals(ctx), outkey, js_undefined());
    js_gc_unprotect(vm, &outkey);

    size_t len = strlen(src);
    uint16_t *u = malloc(len * sizeof(uint16_t));
    for (size_t i = 0; i < len; i++)
        u[i] = (uint16_t)(unsigned char)src[i];
    const char *em;
    uint32_t ep;
    JsValue fn = js_compile_module(ctx, u, len, &em, &ep);
    char *out;
    if (!js_is_function(fn)) {
        out = strdup(em ? em : "compile error");
        *ok = false;
    } else {
        js_gc_protect(vm, &fn);
        JsValue p = js_run_module(ctx, fn);
        int st = js_promise_state(p);
        *ok = st == 0 || st == 1;
        JsValue res = js_promise_result(p);
        js_gc_protect(vm, &res);
        /* settle any deferred host promises, then re-drain */
        flush_deferred(ctx);
        /* For a module that awaited a host promise, the completion value
         * arrives after js_run_module returns; scripts assign it to global
         * OUT so we can read the settled value. Fall back to res otherwise. */
        static const uint16_t k_out[] = {'O', 'U', 'T'};
        JsValue key = js_atom(vm, k_out, 3);
        JsValue outv = js_object_get(vm, js_context_globals(ctx), key);
        JsValue chosen = js_is_undefined(outv) ? res : outv;
        js_gc_protect(vm, &chosen);
        JsValue s = js_to_string(ctx, chosen);
        js_gc_unprotect(vm, &chosen);
        size_t sl;
        const uint16_t *su = js_string_units(s, &sl);
        out = malloc(sl + 1);
        for (size_t i = 0; i < sl; i++)
            out[i] = su && su[i] < 128 ? (char)su[i] : '?';
        out[sl] = 0;
    }
    free(u);
    js_vm_free(vm);
    checks_run++;
    if (ca.net_bytes != 0 || ca.live_allocs != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL leak: %s (net=%ld allocs=%ld)\n", src, ca.net_bytes,
                ca.live_allocs);
    }
    return out;
}

static void eq_mode(const char *src, const char *expected, bool stress) {
    bool ok;
    char *out = run(src, stress, &ok);
    checks_run++;
    if (!ok || strcmp(out, expected) != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL%s %s\n  expected: %s\n  actual:   %s%s\n",
                stress ? " (stress)" : "", src, expected, out, ok ? "" : " (error)");
    }
    free(out);
}
static void eq(const char *src, const char *expected) {
    eq_mode(src, expected, false);
    eq_mode(src, expected, true);
}
/* expect the module to end in a rejected/uncaught state */
static void err(const char *src, const char *substr) {
    bool ok;
    char *out = run(src, false, &ok);
    checks_run++;
    if (ok || !strstr(out, substr)) {
        checks_failed++;
        fprintf(stderr, "FAIL(err) %s\n  wanted: %s\n  got: %s%s\n", src, substr, out,
                ok ? " (ok)" : "");
    }
    free(out);
}

static void test_then(void) {
    eq("let r = 0; Promise.resolve(41).then(v => { r = v + 1; }); r;", "0"); /* sync: not yet run */
    /* completion via TLA to observe post-drain state */
    eq("let out; await Promise.resolve('hi').then(v => out = v); out;", "hi");
    eq("await Promise.resolve(10).then(v => v * 2);", "20");
    eq("await Promise.resolve(1).then(v => v + 1).then(v => v + 1).then(v => v + 1);", "4");
    eq("let log = ''; await Promise.resolve().then(() => log += 'a').then(() => log += 'b'); log;",
       "ab");
    eq("await Promise.reject('boom').catch(e => 'caught: ' + e);", "caught: boom");
    eq("let log = ''; await Promise.resolve(1).finally(() => log += 'F').then(v => log += v); log;",
       "F1");
    eq("await Promise.reject('x').catch(() => 'ok').finally(() => {});", "ok");
    /* finally whose onFinally returns a thenable: the settlement waits for it,
     * and its rejection overrides the original fulfillment (spec adoption). */
    eq("await Promise.resolve(2).finally(() => Promise.reject('fin')).then(v => 'f' + v, e => 'r' + e);",
       "rfin");
    eq("await Promise.resolve(3).finally(() => Promise.resolve('ignored'));", "3");
    eq("await Promise.reject('orig').finally(() => Promise.resolve('ignored')).catch(e => 'c' + e);",
       "corig");
}

static void test_async_await(void) {
    eq("async function f() { return 42; } await f();", "42");
    eq("async function f() { return 1 + 1; } await f();", "2");
    eq("async function f() { let x = await Promise.resolve(10); return x + 5; } await f();", "15");
    eq("async function add(a, b) { return await a + await b; }"
       " await add(Promise.resolve(3), Promise.resolve(4));", "7");
    /* await a plain value */
    eq("async function f() { return await 7; } await f();", "7");
    /* sequential awaits */
    eq("async function f() { let s = 0; for (let i = 0; i < 4; i++) s += await Promise.resolve(i); return s; }"
       " await f();", "6");
    /* try/catch across await */
    eq("async function f() { try { await Promise.reject('e'); return 'no'; } catch (x) { return 'got ' + x; } }"
       " await f();", "got e");
    /* async arrow */
    eq("let g = async x => (await x) * 10; await g(Promise.resolve(5));", "50");
    /* nested async calls */
    eq("async function inner() { return await Promise.resolve(21); }"
       " async function outer() { return (await inner()) * 2; } await outer();", "42");
    /* error propagation through async chain */
    eq("async function bad() { throw 'fail'; }"
       " async function f() { try { await bad(); } catch (e) { return 'caught ' + e; } } await f();",
       "caught fail");
}

static void test_host_defer(void) {
    /* awaiting a host promise resolved on a later tick; OUT captures the
     * completion value that arrives after js_run_module returns */
    eq("OUT = await defer('resolved-by-host');", "resolved-by-host");
    eq("async function f() { let v = await defer(100); return v + 1; } OUT = await f();", "101");
    eq("let a = await defer('a'); let b = await defer('b'); OUT = a + b;", "ab");
    eq("OUT = await Promise.all([defer(1), defer(2), defer(3)]).then(xs => xs.join(','));", "1,2,3");
    eq("async function f() { try { return await rejectLater('nope'); } catch (e) { return 'E:' + e; } }"
       " OUT = await f();", "E:nope");
}

static void test_promise_combinators(void) {
    eq("await Promise.all([1, 2, 3]).then(a => a.join('+'));", "1+2+3");
    eq("await Promise.all([Promise.resolve('x'), 'y', Promise.resolve('z')]).then(a => a.join(''));",
       "xyz");
    eq("await Promise.all([]).then(a => 'empty:' + a.length);", "empty:0");
    eq("await Promise.race([Promise.resolve('fast'), defer('slow')]);", "fast");
    eq("await Promise.reject('r').then(() => 'no', e => 'handled ' + e);", "handled r");
    eq("await Promise.allSettled([Promise.resolve(1), Promise.reject('e')])"
       ".then(rs => rs.map(x => x.status).join(','));", "fulfilled,rejected");
    eq("await Promise.allSettled([Promise.resolve(5)]).then(rs => rs[0].value);", "5");
}

static void test_ctor_executor(void) {
    eq("await Promise((res, rej) => res(99));", "99");
    eq("await Promise((res, rej) => res('hello')).then(v => v + '!');", "hello!");
    eq("await Promise((res, rej) => rej('bad')).catch(e => 'caught ' + e);", "caught bad");
    eq("await Promise((res) => res(1)).then(v => v + 1).then(v => v + 1);", "3");
    /* executor that throws rejects the promise */
    eq("await Promise(() => { throw 'oops'; }).catch(e => 'E:' + e);", "E:oops");
    /* deferred resolve inside executor via host */
    eq("let p = Promise((res) => { defer('later').then(res); }); OUT = await p;", "later");
}

static void test_ordering(void) {
    /* microtasks run in FIFO order, after synchronous code */
    eq("let log = 'start;';"
       "Promise.resolve().then(() => log += 'p1;');"
       "Promise.resolve().then(() => log += 'p2;');"
       "log += 'end;';"
       "await Promise.resolve().then(() => log += 'p3;');"
       "log;",
       "start;end;p1;p2;p3;");
    /* await yields to the queue */
    eq("let log = '';"
       "async function f() { log += '1'; await Promise.resolve(); log += '3'; }"
       "f(); log += '2'; await Promise.resolve().then(()=>{}); log;",
       "123");
    /* multiple reactions attached to one *pending* promise fire in FIFO
       attachment order (regression: they used to fire LIFO). */
    eq("let log = ''; let r; let p = Promise((res) => { r = res; });"
       "p.then(() => log += 'a'); p.then(() => log += 'b'); p.then(() => log += 'c');"
       "r(1); await p; log;",
       "abc");
}

/*
 * A genuinely still-pending completion must be exposed to the host as a live
 * promise — settled on a later, initially-quiescent turn — rather than
 * silently reported as done. This is the cross-turn scenario a single-drain
 * test can't catch: between js_run_module and the settlement there are no
 * queued jobs at all, and a full GC runs, so the suspended fiber survives
 * only through the host-protected result promise's reaction chain.
 */
static void test_pending_promise_exposed(bool stress) {
    CountAlloc ca = {0, 0};
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca, .gc_stress = stress};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    g_deferred.vm = vm;
    g_deferred.count = 0;
    memset(g_reject_flag, 0, sizeof g_reject_flag);
    static const uint16_t n_defer[] = {'d', 'e', 'f', 'e', 'r'};
    js_register_native(ctx, n_defer, 5, native_defer, NULL);

    const char *src = "let v = await defer('cross'); v + '-turn';";
    size_t len = strlen(src);
    uint16_t *u = malloc(len * sizeof(uint16_t));
    for (size_t i = 0; i < len; i++)
        u[i] = (uint16_t)(unsigned char)src[i];
    const char *em;
    uint32_t ep;
    JsValue fn = js_compile_module(ctx, u, len, &em, &ep);
    free(u);
    js_gc_protect(vm, &fn);

    JsValue p = js_run_module(ctx, fn);
    js_gc_protect(vm, &p);

    /* truly quiescent gap: the fiber is suspended, nothing is queued */
    checks_run++;
    if (js_promise_state(p) != 0 || g_deferred.count != 1 || js_has_pending_jobs(ctx)) {
        checks_failed++;
        fprintf(stderr, "FAIL pending-exposure: state=%d deferred=%d jobs=%d\n",
                js_promise_state(p), g_deferred.count, js_has_pending_jobs(ctx));
    }

    /* a full collection in the gap must not reclaim the suspended fiber */
    js_gc_collect(vm);

    /* the host's later turn: settle the awaited promise, drain */
    js_resolve(ctx, g_deferred.promises[0], g_deferred.values[0]);
    js_gc_unprotect(vm, &g_deferred.values[0]);
    js_gc_unprotect(vm, &g_deferred.promises[0]);
    g_deferred.count = 0;
    js_run_jobs(ctx);

    checks_run++;
    JsValue s = js_to_string(ctx, js_promise_result(p));
    size_t sl;
    const uint16_t *su = js_string_units(s, &sl);
    char out[32] = {0};
    for (size_t i = 0; i < sl && i < sizeof out - 1; i++)
        out[i] = su && su[i] < 128 ? (char)su[i] : '?';
    if (js_promise_state(p) != 1 || strcmp(out, "cross-turn") != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL pending-exposure settle: state=%d result=%s\n",
                js_promise_state(p), out);
    }

    js_gc_unprotect(vm, &p);
    js_gc_unprotect(vm, &fn);
    js_vm_free(vm);
    checks_run++;
    if (ca.net_bytes != 0 || ca.live_allocs != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL pending-exposure leak (net=%ld allocs=%ld)\n", ca.net_bytes,
                ca.live_allocs);
    }
}

int main(void) {
    test_then();
    test_async_await();
    test_host_defer();
    test_promise_combinators();
    test_ctor_executor();
    test_ordering();
    test_pending_promise_exposed(false);
    test_pending_promise_exposed(true);
    (void)err;
    if (checks_failed) {
        fprintf(stderr, "%d/%d async checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d async checks passed\n", checks_run);
    return 0;
}
