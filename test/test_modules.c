/*
 * ES module tests. A tiny in-memory resolver maps specifiers to source
 * strings; each test registers a set of modules and evaluates a root,
 * asserting on a named export.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsvm.h"

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

/* module table: name -> UTF-16 source */
#define MAX_MODS 16
typedef struct {
    char names[MAX_MODS][32];
    uint16_t *sources[MAX_MODS];
    size_t source_lens[MAX_MODS];
    int count;
} ModTable;
static ModTable g_mods;

static uint16_t *to_u16(const char *s, size_t *len) {
    size_t n = strlen(s);
    uint16_t *u = malloc((n + 1) * sizeof(uint16_t));
    for (size_t i = 0; i < n; i++)
        u[i] = (uint16_t)(unsigned char)s[i];
    u[n] = 0;
    *len = n;
    return u;
}

static void mod_add(const char *name, const char *src) {
    int i = g_mods.count++;
    snprintf(g_mods.names[i], 32, "%s", name);
    g_mods.sources[i] = to_u16(src, &g_mods.source_lens[i]);
}

static void mods_reset(void) {
    for (int i = 0; i < g_mods.count; i++)
        free(g_mods.sources[i]);
    g_mods.count = 0;
}

/* buffers for handing UTF-16 back to the resolver */
static uint16_t g_spec_buf[128];

static bool resolver(void *ud, const uint16_t *spec, size_t spec_len,
                     const uint16_t *ref, size_t ref_len, const uint16_t **out_spec,
                     size_t *out_spec_len, const uint16_t **out_source, size_t *out_len) {
    (void)ud;
    (void)ref;
    (void)ref_len;
    char name[64];
    size_t n = spec_len < 63 ? spec_len : 63;
    for (size_t i = 0; i < n; i++)
        name[i] = (char)spec[i];
    name[n] = 0;
    for (int i = 0; i < g_mods.count; i++) {
        if (strcmp(g_mods.names[i], name) == 0) {
            /* canonical specifier = the input, echoed via a static buffer */
            for (size_t k = 0; k < spec_len && k < 128; k++)
                g_spec_buf[k] = spec[k];
            *out_spec = g_spec_buf;
            *out_spec_len = spec_len;
            *out_source = g_mods.sources[i];
            *out_len = g_mods.source_lens[i];
            return true;
        }
    }
    return false;
}

/* Evaluates root module 'main' (added by caller); reads export `exp`, returns
 * its ToString. Sets *ok to eval success. */
static char *eval_export(const char *root_src, const char *exp, bool *ok) {
    CountAlloc ca = {0, 0};
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    js_set_module_resolver(ctx, resolver, NULL);

    size_t root_len;
    uint16_t *root_u = to_u16(root_src, &root_len);
    static const uint16_t root_spec[] = {'m', 'a', 'i', 'n'};
    bool ev;
    const char *em;
    uint32_t ep;
    JsValue ns = js_eval_module(ctx, root_spec, 4, root_u, root_len, &ev, &em, &ep);
    js_gc_protect(vm, &ns);
    char *out;
    *ok = ev;
    if (!ev) {
        JsValue s = js_to_string(ctx, ns);
        size_t sl;
        const uint16_t *su = js_string_units(s, &sl);
        out = malloc(sl + 32);
        int p = snprintf(out, 32, "%s", em ? em : "");
        for (size_t i = 0; i < sl; i++)
            out[p + i] = su && su[i] < 128 ? (char)su[i] : '?';
        out[p + sl] = 0;
    } else {
        size_t elen;
        uint16_t *eu = to_u16(exp, &elen);
        JsValue v = js_module_get_export(ctx, ns, eu, elen);
        free(eu);
        js_gc_protect(vm, &v);
        JsValue s = js_to_string(ctx, v);
        size_t sl;
        const uint16_t *su = js_string_units(s, &sl);
        out = malloc(sl + 1);
        for (size_t i = 0; i < sl; i++)
            out[i] = su && su[i] < 128 ? (char)su[i] : '?';
        out[sl] = 0;
        js_gc_unprotect(vm, &v);
    }
    js_gc_unprotect(vm, &ns);
    free(root_u);
    js_vm_free(vm);
    checks_run++;
    if (ca.net_bytes != 0 || ca.live_allocs != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL leak (net=%ld allocs=%ld)\n", ca.net_bytes, ca.live_allocs);
    }
    return out;
}

/* Adds `main` = root_src, evaluates, asserts export `exp` == expected. */
static void eq_export(const char *root_src, const char *exp, const char *expected) {
    mod_add("main", root_src);
    bool ok;
    char *out = eval_export(root_src, exp, &ok);
    checks_run++;
    if (!ok || strcmp(out, expected) != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL export %s\n  root: %s\n  expected: %s\n  actual: %s%s\n",
                exp, root_src, expected, out, ok ? "" : " (error)");
    }
    free(out);
    mods_reset();
}

static void test_basic_exports(void) {
    eq_export("export const x = 42;", "x", "42");
    eq_export("export let y = 1; y = y + 9;", "y", "10");
    eq_export("export function f() { return 7; } export const r = f();", "r", "7");
    eq_export("const a = 1, b = 2; export { a, b };", "a", "1");
    eq_export("const a = 1; export { a as renamed };", "renamed", "1");
    eq_export("export default 99;", "default", "99");
    eq_export("export default function() { return 5; } export const v = 1;", "v", "1");
    eq_export("export const list = [1,2,3].map(n => n * n);", "list", "1,4,9");
}

static void test_imports(void) {
    mod_add("helpers", "export const PI = 3; export function double(x) { return x * 2; }");
    eq_export("import { PI, double } from 'helpers'; export const r = double(PI);", "r", "6");
    mods_reset();

    mod_add("m", "export const value = 'hello';");
    eq_export("import { value as v } from 'm'; export const out = v + ' world';", "out",
              "hello world");
    mods_reset();

    mod_add("lib", "export default function greet(n) { return 'Hi ' + n; }");
    eq_export("import greet from 'lib'; export const msg = greet('Al');", "msg", "Hi Al");
    mods_reset();

    mod_add("ns", "export const a = 1; export const b = 2; export const c = 3;");
    eq_export("import * as all from 'ns'; export const sum = all.a + all.b + all.c;", "sum",
              "6");
    mods_reset();
}

static void test_live_bindings(void) {
    /* importer sees a mutation the exporter makes later (via a function) */
    mod_add("counter",
            "export let n = 0; export function inc() { n = n + 1; }");
    eq_export("import { n, inc } from 'counter';"
              "inc(); inc(); inc();"
              "export const result = n;",
              "result", "3");
    mods_reset();
}

static void test_transitive(void) {
    mod_add("a", "export const A = 10;");
    mod_add("b", "import { A } from 'a'; export const B = A + 5;");
    eq_export("import { B } from 'b'; export const C = B * 2;", "C", "30");
    mods_reset();
}

static void test_diamond(void) {
    /* main -> {left, right} -> base; base must evaluate once */
    mod_add("base", "export let calls = 0; export function tick() { calls = calls + 1; return calls; }");
    mod_add("left", "import { tick } from 'base'; export const l = tick();");
    mod_add("right", "import { tick } from 'base'; export const r = tick();");
    eq_export("import { l } from 'left'; import { r } from 'right';"
              "export const total = l + r;",
              "total", "3"); /* tick() called once each: 1 + 2 */
    mods_reset();
}

static void test_cycle(void) {
    /* A imports B, B imports A; function bindings are hoisted so calls work */
    mod_add("evenmod",
            "import { isOdd } from 'oddmod';"
            "export function isEven(n) { return n === 0 ? true : isOdd(n - 1); }");
    mod_add("oddmod",
            "import { isEven } from 'evenmod';"
            "export function isOdd(n) { return n === 0 ? false : isEven(n - 1); }");
    eq_export("import { isEven } from 'evenmod';"
              "export const r = isEven(10);",
              "r", "true");
    mods_reset();
}

static void test_star_reexport(void) {
    mod_add("inner", "export const a = 1; export const b = 2;");
    mod_add("mid", "export * from 'inner'; export const c = 3;");
    eq_export("import * as m from 'mid'; export const sum = m.a + m.b + m.c;", "sum", "6");
    mods_reset();
}

static void test_reexport_named(void) {
    /* export { x } from 'm' — re-export by name */
    mod_add("shapes", "export function circle() { return 'O'; }"
                      "export function square() { return '[]'; }");
    mod_add("index", "export { circle } from 'shapes';"
                     "export { square as box } from 'shapes';");
    eq_export("import { circle, box } from 'index';"
              "export const out = circle() + box();",
              "out", "O[]");
    mods_reset();

    /* the re-exported name appears on the namespace too */
    mod_add("lib", "export const VERSION = '1.0'; export const NAME = 'jsvm';");
    mod_add("barrel", "export { VERSION } from 'lib'; export { NAME as title } from 'lib';");
    eq_export("import * as b from 'barrel';"
              "export const info = b.title + ' ' + b.VERSION;",
              "info", "jsvm 1.0");
    mods_reset();
}

static void test_reexport_ns(void) {
    /* export * as ns from 'm' — namespace re-export (live by reference) */
    mod_add("math", "export const pi = 3.14; export function sq(x) { return x * x; }");
    mod_add("facade", "export * as m from 'math'; export const own = true;");
    eq_export("import { m, own } from 'facade';"
              "export const out = m.pi + '/' + m.sq(4) + '/' + own;",
              "out", "3.14/16/true");
    mods_reset();
}

static void test_barrel(void) {
    /* a realistic index/barrel file aggregating several modules */
    mod_add("format", "export function money(n) { return '$' + n.toFixed(2); }");
    mod_add("dates", "export function year() { return 2026; }");
    mod_add("text", "export function upper(s) { return s.toUpperCase(); }");
    mod_add("utils",
            "export * from 'format';"
            "export * from 'dates';"
            "export { upper as shout } from 'text';");
    eq_export("import { money, year, shout } from 'utils';"
              "export const line = shout('total') + ': ' + money(9.5) + ' (' + year() + ')';",
              "line", "TOTAL: $9.50 (2026)");
    mods_reset();
}

static void test_async_module(void) {
    mod_add("data", "export async function load() { return await Promise.resolve(42); }");
    eq_export("import { load } from 'data';"
              "export let result = 0;"
              "load().then(v => result = v);",
              "result", "42"); /* microtasks drained during evaluation */
    mods_reset();
    /* top-level await in a module */
    eq_export("export const value = await Promise.resolve('ready');", "value", "ready");
}

static void test_errors(void) {
    mods_reset();
    bool ok;
    char *out = eval_export("import { x } from 'missing';", "x", &ok);
    checks_run++;
    if (ok || !strstr(out, "not found")) {
        checks_failed++;
        fprintf(stderr, "FAIL expected 'not found', got: %s\n", out);
    }
    free(out);

    mod_add("bad", "this is not valid js @#$");
    out = eval_export("import { y } from 'bad';", "y", &ok);
    checks_run++;
    if (ok) {
        checks_failed++;
        fprintf(stderr, "FAIL expected compile error in dependency\n");
    }
    free(out);
    mods_reset();

    /* runtime error during module evaluation propagates */
    out = eval_export("throw 'module boom';", "x", &ok);
    checks_run++;
    if (ok || !strstr(out, "boom")) {
        checks_failed++;
        fprintf(stderr, "FAIL expected 'boom', got: %s\n", out);
    }
    free(out);
    mods_reset();
}

int main(void) {
    test_basic_exports();
    test_imports();
    test_live_bindings();
    test_transitive();
    test_diamond();
    test_cycle();
    test_star_reexport();
    test_reexport_named();
    test_reexport_ns();
    test_barrel();
    test_async_module();
    test_errors();
    mods_reset();
    if (checks_failed) {
        fprintf(stderr, "%d/%d module checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d module checks passed\n", checks_run);
    return 0;
}
