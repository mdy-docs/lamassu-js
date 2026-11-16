/*
 * Deep module graphs. No walk over a graph recurses on the C stack (js_module.c, "walking the
 * graph"), so how deep a graph may be does not depend on the stack of the thread:
 *
 *  - a chain of imports JS_MAX_MODULE_DEPTH (3000) modules long links and runs, with a top-level
 *    await at its end too, and as one cycle; one module more is V8's
 *    "RangeError: Maximum call stack size exceeded", which leaves the graph as it was: importing
 *    it again fails the same way, and what is short enough of it still works;
 *  - a chain of re-exports is followed at any length (20 000 here, where V8 crashes), by name
 *    and into a namespace;
 *  - all of it on a thread with a stack of 512 KB, what macOS and iOS give a secondary thread.
 *
 * The modules are made by the loader from their names, an in-memory graph of any size.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lamassu.h"
#include "lamassu_compile.h"

#if defined(__APPLE__) || (defined(__linux__) && !defined(__wasi__))
#define HAVE_SMALL_THREAD 1
#include <pthread.h>
#endif

#define LIMIT 3000 /* JS_MAX_MODULE_DEPTH */
#define TOO_DEEP "RangeError: Maximum call stack size exceeded"

static int checks_run, checks_failed;

static void check(bool cond, const char *what) {
    checks_run++;
    if (!cond) {
        checks_failed++;
        fprintf(stderr, "FAIL %s\n", what);
    }
}

static void check_str(const char *actual, const char *expected, const char *what) {
    checks_run++;
    if (strcmp(actual, expected) != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL %s\n  expected: %s\n  actual: %s\n", what, expected, actual);
    }
}

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

/* ---- the graph: what each name is ---- */

static int g_n;             /* how many modules the chain m0 .. or s0 .. has */
static const char *g_leaf;  /* the last of the chain m */
static const char *g_star;  /* what a module of the chain s is, with %d the next */
static const char *g_hub;   /* how the hub ends, after its imports */

/* The source of NAME (malloc), or NULL for a module there is not:
 *   m<i>   imports d from m<i+1> and exports it plus one: the value counts the chain
 *   s<i>   re-exports s<i+1>, whose last exports v
 *   hub    imports every s, last first (so no walk along imports goes deep), then g_hub
 *   slow   comes when the test settles it (see the loader)
 *   x, r1, r2, r3   test_left_over's */
static char *source_of(const char *name) {
    char buf[128];
    int i;
    if (sscanf(name, "m%d", &i) == 1 && i < g_n) {
        if (i == g_n - 1)
            snprintf(buf, sizeof buf, "%s", g_leaf);
        else
            snprintf(buf, sizeof buf, "import { d as below } from 'm%d'; export const d = below + 1;", i + 1);
    } else if (sscanf(name, "s%d", &i) == 1 && i < g_n) {
        if (i == g_n - 1)
            snprintf(buf, sizeof buf, "export const v = 7;");
        else
            snprintf(buf, sizeof buf, g_star, i + 1);
    } else if (strcmp(name, "hub") == 0) {
        size_t cap = (size_t)g_n * 24 + strlen(g_hub) + 1, at = 0;
        char *out = malloc(cap);
        for (i = g_n - 1; i >= 0; i--)
            at += (size_t)snprintf(out + at, cap - at, "import 's%d';", i);
        snprintf(out + at, cap - at, "%s", g_hub);
        return out;
    } else if (strcmp(name, "slow") == 0) {
        snprintf(buf, sizeof buf, "export const one = 1;");
    } else if (strcmp(name, "x") == 0) {
        snprintf(buf, sizeof buf, "throw new Error('x');");
    } else if (strcmp(name, "r1") == 0) {
        snprintf(buf, sizeof buf, "import 'x'; import 'm1';");
    } else if (strcmp(name, "r2") == 0) {
        snprintf(buf, sizeof buf, "import { out as o } from 'r3'; export const out = o;");
    } else if (strcmp(name, "r3") == 0) {
        snprintf(buf, sizeof buf, "import { d } from 'm1'; export const out = d;");
    } else {
        return NULL;
    }
    char *out = malloc(strlen(buf) + 1);
    strcpy(out, buf);
    return out;
}

/* ---- a VM whose loader makes them ---- */

typedef struct {
    CountAlloc ca;
    JsVm *vm;
    JsContext *ctx;
    JsValue slow; /* the promise of the module `slow`, until the test settles it */
    bool slow_asked;
} Vm;

static JsValue ascii(JsVm *vm, const char *s) {
    size_t n = strlen(s);
    uint16_t *u = malloc((n + 1) * sizeof(uint16_t));
    for (size_t i = 0; i < n; i++)
        u[i] = (uint16_t)(unsigned char)s[i];
    JsValue v = js_string_new(vm, u, n);
    free(u);
    return v;
}

static JsValue loader(void *ud, JsContext *ctx, const uint16_t *spec, size_t spec_len,
                      const uint16_t *ref, size_t ref_len) {
    Vm *v = ud;
    (void)ref;
    (void)ref_len;
    char name[32];
    size_t n = spec_len < 31 ? spec_len : 31;
    for (size_t i = 0; i < n; i++)
        name[i] = (char)spec[i];
    name[n] = 0;
    JsValue p = js_promise_new(ctx);
    js_gc_protect(v->vm, &p);
    char *src = source_of(name);
    if (strcmp(name, "slow") == 0) {
        v->slow = p;
        v->slow_asked = true;
    } else {
        JsValue s = ascii(v->vm, src ? src : "module not found");
        js_gc_protect(v->vm, &s);
        if (src)
            js_resolve(ctx, p, s);
        else
            js_reject(ctx, p, s);
        js_gc_unprotect(v->vm, &s);
    }
    free(src);
    js_gc_unprotect(v->vm, &p);
    return p;
}

static void vm_open(Vm *v) {
    memset(v, 0, sizeof *v);
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &v->ca};
    v->vm = js_vm_new(&cfg);
    v->ctx = js_context_new(v->vm);
    v->slow = js_undefined();
    js_gc_protect(v->vm, &v->slow);
    js_set_module_loader(v->ctx, loader, NULL, v);
    js_enable_source_modules(v->ctx);
}

/* Frees the VM; true when everything it allocated came back. */
static bool vm_close(Vm *v) {
    js_gc_unprotect(v->vm, &v->slow);
    js_vm_free(v->vm);
    return v->ca.net_bytes == 0 && v->ca.live_allocs == 0;
}

/* The module `slow` arrives, or fails to. */
static void settle_slow(Vm *v, bool arrives) {
    char *src = source_of("slow");
    JsValue s = ascii(v->vm, arrives ? src : "slow failed");
    free(src);
    js_gc_protect(v->vm, &s);
    if (arrives)
        js_resolve(v->ctx, v->slow, s);
    else
        js_reject(v->ctx, v->slow, s);
    js_gc_unprotect(v->vm, &s);
    v->slow = js_undefined();
    js_run_jobs(v->ctx);
}

static JsValue import_start(Vm *v, const char *name) {
    uint16_t spec[32];
    size_t n = strlen(name);
    for (size_t i = 0; i < n; i++)
        spec[i] = (uint16_t)(unsigned char)name[i];
    JsValue p = js_eval_module(v->ctx, spec, n);
    js_gc_protect(v->vm, &p);
    js_run_jobs(v->ctx);
    js_gc_unprotect(v->vm, &p);
    return p;
}

/* What became of the import P, in BUF: the export EXP as a string, "!" and the reason when it
 * was rejected, "pending" when it is not settled. */
static const char *import_result(Vm *v, JsValue p, const char *exp, char *buf, size_t cap) {
    int st = js_promise_state(p);
    if (st != 1 && st != 2) {
        snprintf(buf, cap, "pending");
        return buf;
    }
    js_gc_protect(v->vm, &p);
    JsValue val = js_promise_result(p);
    if (st == 1) {
        uint16_t eu[32];
        size_t en = strlen(exp);
        for (size_t i = 0; i < en; i++)
            eu[i] = (uint16_t)(unsigned char)exp[i];
        val = js_module_get_export(v->ctx, val, eu, en);
    }
    js_gc_protect(v->vm, &val);
    JsValue s = js_to_string(v->ctx, val);
    js_gc_unprotect(v->vm, &val);
    js_gc_unprotect(v->vm, &p);
    size_t sl, at = 0;
    const uint16_t *su = js_string_units(s, &sl);
    if (st == 2 && at + 1 < cap)
        buf[at++] = '!';
    for (size_t i = 0; su && i < sl && at + 1 < cap; i++)
        buf[at++] = su[i] < 128 ? (char)su[i] : '?';
    buf[at] = 0;
    return buf;
}

/* Imports NAME in V and gives what import_result does. */
static const char *import_of(Vm *v, const char *name, const char *exp, char *buf, size_t cap) {
    JsValue p = import_start(v, name);
    return import_result(v, p, exp, buf, cap);
}

/* ---- chains of imports ---- */

static void test_import_chain(void) {
    char buf[128];
    Vm v;

    /* as long as the limit: linked and run, the value carried from the last to the first */
    g_n = LIMIT;
    g_leaf = "export const d = 1;";
    vm_open(&v);
    check_str(import_of(&v, "m0", "d", buf, sizeof buf), "3000", "a chain of 3000 runs");
    check(vm_close(&v), "a chain of 3000: no leak");

    /* one more: V8's error, at any time it is asked for; and the graph stays what it was, so
     * the 3000 modules under the first still work, and with them there the first does too */
    g_n = LIMIT + 1;
    vm_open(&v);
    check_str(import_of(&v, "m0", "d", buf, sizeof buf), "!" TOO_DEEP, "a chain of 3001 is too deep");
    check_str(import_of(&v, "m0", "d", buf, sizeof buf), "!" TOO_DEEP, "a chain of 3001 is too deep again");
    check_str(import_of(&v, "m1", "d", buf, sizeof buf), "3000", "the 3000 under it run");
    check_str(import_of(&v, "m0", "d", buf, sizeof buf), "3001", "and then the one above them");
    check(vm_close(&v), "a chain of 3001: no leak");

    /* far more: an error still, not a crash */
    g_n = 30000;
    vm_open(&v);
    check_str(import_of(&v, "m0", "d", buf, sizeof buf), "!" TOO_DEEP, "a chain of 30000 is too deep");
    /* all of it fetched now: the graph load goes through it in one call, and not on the stack */
    check_str(import_of(&v, "m0", "d", buf, sizeof buf), "!" TOO_DEEP, "a chain of 30000, loaded, again");
    check(vm_close(&v), "a chain of 30000: no leak");

    /* the last throws: every module above it fails with its error */
    g_n = LIMIT;
    g_leaf = "export const d = 1; throw new Error('leaf');";
    vm_open(&v);
    check_str(import_of(&v, "m0", "d", buf, sizeof buf), "!Error: leaf", "a throw at the end of 3000");
    check_str(import_of(&v, "m1500", "d", buf, sizeof buf), "!Error: leaf", "which the middle keeps");
    check(vm_close(&v), "a throw at the end: no leak");

    /* the last imports the first: one cycle of 3000, one strongly connected component */
    g_leaf = "import 'm0'; export const d = 1;";
    vm_open(&v);
    check_str(import_of(&v, "m0", "d", buf, sizeof buf), "3000", "a cycle of 3000 runs");
    check(vm_close(&v), "a cycle of 3000: no leak");

    /* and a cycle one longer is too deep, from wherever it is entered */
    g_n = LIMIT + 1;
    vm_open(&v);
    check_str(import_of(&v, "m0", "d", buf, sizeof buf), "!" TOO_DEEP, "a cycle of 3001 is too deep");
    check_str(import_of(&v, "m7", "d", buf, sizeof buf), "!" TOO_DEEP, "from its middle too");
    check(vm_close(&v), "a cycle of 3001: no leak");
}

/* a top-level await at the end of the chain: every module above waits, and none on the C stack */
static void test_await_chain(void) {
    char buf[128];
    Vm v;
    g_n = LIMIT;
    g_leaf = "const ns = await import('slow'); export const d = ns.one;";

    vm_open(&v);
    JsValue p = import_start(&v, "m0");
    js_gc_protect(v.vm, &p);
    check(v.slow_asked, "the await at the end of 3000 is reached");
    check_str(import_result(&v, p, "d", buf, sizeof buf), "pending", "and the chain waits for it");
    settle_slow(&v, true);
    check_str(import_result(&v, p, "d", buf, sizeof buf), "3000", "then runs, from the last to the first");
    js_gc_unprotect(v.vm, &p);
    check(vm_close(&v), "an await at the end: no leak");

    vm_open(&v);
    p = import_start(&v, "m0");
    js_gc_protect(v.vm, &p);
    check_str(import_result(&v, p, "d", buf, sizeof buf), "pending", "the chain waits again");
    settle_slow(&v, false);
    check_str(import_result(&v, p, "d", buf, sizeof buf), "!slow failed", "and fails with what it awaited");
    js_gc_unprotect(v.vm, &p);
    check(vm_close(&v), "a failed await at the end: no leak");
}

/*
 * The depth that counts is the path of the walk, and evaluation has its own: modules that were
 * linked but never run (an earlier sibling threw) are walked again by whoever imports them next.
 */
static void test_left_over(void) {
    char buf[128];
    Vm v;
    g_n = LIMIT;
    g_leaf = "export const d = 1;";
    vm_open(&v);
    /* r1 -> x (throws), m1 .. m2999: 3000 deep, linked, and nothing of the chain run */
    check_str(import_of(&v, "r1", "d", buf, sizeof buf), "!Error: x", "the first import throws");
    /* r2 -> r3 -> m1 .. m2999: nothing to link below r3, and 3001 to evaluate */
    check_str(import_of(&v, "r2", "out", buf, sizeof buf), "!" TOO_DEEP, "evaluation is too deep by itself");
    check(vm_close(&v), "left over: no leak");

    vm_open(&v);
    check_str(import_of(&v, "r1", "d", buf, sizeof buf), "!Error: x", "the first import throws again");
    check_str(import_of(&v, "r3", "out", buf, sizeof buf), "2999", "and one module less above the chain runs it");
    check(vm_close(&v), "left over, run: no leak");
}

/* ---- chains of re-exports ---- */

static void test_reexport_chain(int n) {
    char buf[128];
    Vm v;
    g_n = n;

    /* `export *`, the name asked for at its top (V8's ResolveExport) */
    g_star = "export * from 's%d';";
    g_hub = "import { v } from 's0'; export const out = v;";
    vm_open(&v);
    check_str(import_of(&v, "hub", "out", buf, sizeof buf), "7", "a name through a chain of export *");
    check(vm_close(&v), "export * by name: no leak");

    /* the same into a namespace (V8's FetchStarExports) */
    g_hub = "import * as ns from 's0'; export const out = ns.v + Object.keys(ns).length;";
    vm_open(&v);
    check_str(import_of(&v, "hub", "out", buf, sizeof buf), "8", "a namespace over a chain of export *");
    check(vm_close(&v), "export * as a namespace: no leak");

    /* a name there is not: asked of every module of the chain, and V8's error */
    g_hub = "import { w } from 's0';";
    vm_open(&v);
    check_str(import_of(&v, "hub", "out", buf, sizeof buf),
              "!SyntaxError: The requested module 's0' does not provide an export named 'w'",
              "a name a chain of export * lacks");
    check(vm_close(&v), "export * without the name: no leak");

    /* `export { v } from`, each of which must find it in the next */
    g_star = "export { v } from 's%d';";
    g_hub = "import { v } from 's0'; export const out = v;";
    vm_open(&v);
    check_str(import_of(&v, "hub", "out", buf, sizeof buf), "7", "a name through a chain of export { v } from");
    check(vm_close(&v), "export { v } from: no leak");
}

/* ---- on a small stack ---- */

static void run_all(void) {
    test_import_chain();
    test_await_chain();
    test_left_over();
    test_reexport_chain(20000);
}

#ifdef HAVE_SMALL_THREAD
static void *small_thread(void *arg) {
    (void)arg;
    run_all();
    return NULL;
}
#endif

int main(void) {
    run_all();
#ifdef HAVE_SMALL_THREAD
    /* what a secondary thread of macOS or iOS has: the same graphs, the same answers */
    pthread_attr_t attr;
    pthread_t th;
    pthread_attr_init(&attr);
    bool made = pthread_attr_setstacksize(&attr, 512 * 1024) == 0 &&
                pthread_create(&th, &attr, small_thread, NULL) == 0;
    check(made, "a thread with a stack of 512 KB");
    if (made)
        pthread_join(th, NULL);
    pthread_attr_destroy(&attr);
#endif
    if (checks_failed) {
        fprintf(stderr, "%d/%d module-depth checks failed\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d module-depth checks passed\n", checks_run);
    return 0;
}
