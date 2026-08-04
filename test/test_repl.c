/*
 * REPL / persistent lexical environment tests. Multiple evaluations run in
 * ONE context (js_compile_module_repl), so top-level let/const/function
 * bindings persist across evals with const enforcement — while block- and
 * loop-scoped bindings stay local.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lamassu.h"
#include "lamassu_compile.h"

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

/* A REPL session: one VM+context, multiple evals. */
typedef struct {
    JsVm *vm;
    JsContext *ctx;
    CountAlloc ca;
} Repl;

static Repl *repl_open(bool stress) {
    Repl *r = calloc(1, sizeof *r);
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &r->ca, .gc_stress = stress};
    r->vm = js_vm_new(&cfg);
    r->ctx = js_context_new(r->vm);
    return r;
}

static void repl_close(Repl *r) {
    js_vm_free(r->vm);
    checks_run++;
    if (r->ca.net_bytes != 0 || r->ca.live_allocs != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL repl leak (net=%ld allocs=%ld)\n", r->ca.net_bytes,
                r->ca.live_allocs);
    }
    free(r);
}

/* Eval one input; return malloc'd ASCII of the result, prefixed "!" on error. */
static char *repl_eval(Repl *r, const char *src) {
    size_t len = strlen(src);
    uint16_t *u = malloc(len * sizeof(uint16_t));
    for (size_t i = 0; i < len; i++)
        u[i] = (uint16_t)(unsigned char)src[i];
    const char *em;
    uint32_t ep;
    JsValue fn = js_compile_module_repl(r->ctx, u, len, &em, &ep);
    char *out;
    if (!js_is_function(fn)) {
        out = malloc(strlen(em) + 2);
        out[0] = '!';
        strcpy(out + 1, em);
    } else {
        js_gc_protect(r->vm, &fn);
        JsValue p = js_run_module(r->ctx, fn);
        int st = js_promise_state(p);
        bool ok = st == 0 || st == 1;
        JsValue res = js_promise_result(p);
        js_gc_protect(r->vm, &res);
        JsValue s = js_to_string(r->ctx, res);
        size_t sl;
        const uint16_t *su = js_string_units(s, &sl);
        out = malloc(sl + 2);
        size_t off = 0;
        if (!ok)
            out[off++] = '!';
        for (size_t i = 0; i < sl; i++)
            out[off + i] = su && su[i] < 128 ? (char)su[i] : '?';
        out[off + sl] = 0;
        js_gc_unprotect(r->vm, &res);
        js_gc_unprotect(r->vm, &fn);
    }
    free(u);
    return out;
}

static void expect(Repl *r, const char *src, const char *want) {
    char *out = repl_eval(r, src);
    checks_run++;
    if (strcmp(out, want) != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL  %s\n  expected: %s\n  actual:   %s\n", src, want, out);
    }
    free(out);
}

static void run_session(bool stress) {
    Repl *r = repl_open(stress);

    /* persistence across evals */
    expect(r, "let a = 10;", "undefined");
    expect(r, "a + 5;", "15");
    expect(r, "a = a * 2;", "20");
    expect(r, "a;", "20");

    /* const enforcement across evals */
    expect(r, "const K = 3;", "undefined");
    expect(r, "K;", "3");
    expect(r, "K = 4;", "!TypeError: assignment to constant variable");
    expect(r, "K;", "3");

    /* const enforcement within a single eval */
    expect(r, "const Z = 1; Z = 2;", "!TypeError: assignment to constant variable");

    /* functions persist and are hoisted within an eval */
    expect(r, "function sq(n) { return n * n; }", "undefined");
    expect(r, "sq(7);", "49");
    expect(r, "early(); function early() { return 'ok'; }", "ok");

    /* closures over persistent bindings */
    expect(r, "function makeAdder(x) { return y => x + y; }", "undefined");
    expect(r, "const add10 = makeAdder(10);", "undefined");
    expect(r, "add10(32);", "42");
    /* closure reads the CURRENT value of a persistent let (live) */
    expect(r, "let base = 100; function getBase() { return base; }", "undefined");
    expect(r, "base = 200; getBase();", "200");

    /* block-scoped let does NOT leak to the session */
    expect(r, "{ let scoped = 5; }", "undefined");
    expect(r, "typeof scoped;", "undefined");

    /* loop variable stays local; the accumulator (top-level) persists.
     * The completion value is the last evaluated expression (total += i). */
    expect(r, "let total = 0; for (let i = 0; i < 5; i++) total += i;", "10");
    expect(r, "total;", "10");
    expect(r, "typeof i;", "undefined"); /* loop var did not leak */

    /* redeclaration across evals is allowed (REPL) and can change const-ness */
    expect(r, "const RED = 1;", "undefined");
    expect(r, "let RED = 2;", "undefined"); /* re-declare as mutable */
    expect(r, "RED = 3; RED;", "3");        /* now assignable */

    /* builtins still resolve through the global fallback */
    expect(r, "Math.max(4, 9, 2);", "9");
    expect(r, "[1, 2, 3].map(sq).join(',');", "1,4,9");
    /* object key order is hash order (documented deviation) */
    expect(r, "JSON.stringify({ a: base, k: K });", "{\"k\":3,\"a\":200}");

    /* destructuring declarations persist */
    expect(r, "const [p, q] = [1, 2]; const { m } = { m: 3 };", "undefined");
    expect(r, "p + q + m;", "6");

    /* assignment to an undeclared name is still a ReferenceError */
    expect(r, "nope = 1;", "!ReferenceError: nope is not defined");

    /* async carries over too */
    expect(r, "async function load() { return await Promise.resolve(a); }", "undefined");
    expect(r, "await load();", "20");

    repl_close(r);
}

int main(void) {
    run_session(false);
    run_session(true); /* under gc_stress */
    if (checks_failed) {
        fprintf(stderr, "%d/%d repl checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d repl checks passed\n", checks_run);
    return 0;
}
