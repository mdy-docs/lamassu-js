/*
 * ES module tests. A tiny in-memory table backs the async module loader:
 * each entry resolves as source text (immediately or deferred to a later
 * host turn), precompiled bytecode, or a synthetic exports object. Each
 * test registers modules and evaluates a root via js_eval_module's promise,
 * asserting on a named export — including genuinely cross-turn loads where
 * the job queue is empty between rounds.
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

/* ---- module table ---- */

enum { MODE_SOURCE, MODE_DEFERRED, MODE_SYNTHETIC, MODE_BYTECODE };

#define MAX_MODS 16
typedef struct {
    char names[MAX_MODS][64];
    uint16_t *sources[MAX_MODS];
    size_t source_lens[MAX_MODS];
    uint8_t *bytecode[MAX_MODS];
    size_t bytecode_lens[MAX_MODS];
    int mode[MAX_MODS];
    int loads[MAX_MODS]; /* loader invocations per entry */
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

static int mod_add_mode(const char *name, const char *src, int mode) {
    int i = g_mods.count++;
    snprintf(g_mods.names[i], 64, "%s", name);
    g_mods.sources[i] = src ? to_u16(src, &g_mods.source_lens[i]) : NULL;
    g_mods.bytecode[i] = NULL;
    g_mods.bytecode_lens[i] = 0;
    g_mods.mode[i] = mode;
    g_mods.loads[i] = 0;
    return i;
}

static void mod_add(const char *name, const char *src) {
    mod_add_mode(name, src, MODE_SOURCE);
}

static void mods_reset(void) {
    for (int i = 0; i < g_mods.count; i++) {
        free(g_mods.sources[i]);
        free(g_mods.bytecode[i]);
    }
    g_mods.count = 0;
}

static int mod_find(const char *name) {
    for (int i = 0; i < g_mods.count; i++)
        if (strcmp(g_mods.names[i], name) == 0)
            return i;
    return -1;
}

/* ---- deferred loads (settled by the test on a later "turn") ---- */

typedef struct {
    JsVm *vm;
    JsValue promises[MAX_MODS];
    int table_idx[MAX_MODS];
    bool reject[MAX_MODS];
    const char *reject_msg[MAX_MODS];
    int count;
} PendingLoads;
static PendingLoads g_pending;

/* Settles the pending load for `name` (fulfilling with its table source,
 * or rejecting) and drains jobs; false if no such load is pending. */
static bool settle_load(JsContext *ctx, const char *name) {
    for (int i = 0; i < g_pending.count; i++) {
        if (strcmp(g_mods.names[g_pending.table_idx[i]], name) != 0)
            continue;
        int t = g_pending.table_idx[i];
        if (g_pending.reject[i]) {
            size_t ml;
            uint16_t *mu = to_u16(g_pending.reject_msg[i], &ml);
            js_reject(ctx, g_pending.promises[i], js_atom(g_pending.vm, mu, ml));
            free(mu);
        } else {
            JsValue src = js_atom(g_pending.vm, g_mods.sources[t], g_mods.source_lens[t]);
            js_resolve(ctx, g_pending.promises[i], src);
        }
        js_gc_unprotect(g_pending.vm, &g_pending.promises[i]);
        g_pending.count--;
        for (int k = i; k < g_pending.count; k++) {
            g_pending.promises[k] = g_pending.promises[k + 1];
            g_pending.table_idx[k] = g_pending.table_idx[k + 1];
            g_pending.reject[k] = g_pending.reject[k + 1];
            g_pending.reject_msg[k] = g_pending.reject_msg[k + 1];
            /* re-point the root slot at the moved value */
            js_gc_unprotect(g_pending.vm, &g_pending.promises[k + 1]);
            js_gc_protect(g_pending.vm, &g_pending.promises[k]);
        }
        js_run_jobs(ctx);
        return true;
    }
    return false;
}

static void settle_all_loads(JsContext *ctx) {
    while (g_pending.count > 0)
        settle_load(ctx, g_mods.names[g_pending.table_idx[0]]);
}

/* ---- the loader + canonicalizer ---- */

static JsValue fulfilled_with(JsContext *ctx, JsVm *vm, JsValue v) {
    js_gc_protect(vm, &v);
    JsValue p = js_promise_new(ctx);
    js_resolve(ctx, p, v);
    js_gc_unprotect(vm, &v);
    return p;
}

static JsValue rejected_with_ascii(JsContext *ctx, JsVm *vm, const char *msg) {
    size_t ml;
    uint16_t *mu = to_u16(msg, &ml);
    JsValue reason = js_atom(vm, mu, ml);
    free(mu);
    js_gc_protect(vm, &reason);
    JsValue p = js_promise_new(ctx);
    js_reject(ctx, p, reason);
    js_gc_unprotect(vm, &reason);
    return p;
}

static JsValue loader(void *ud, JsContext *ctx, const uint16_t *spec, size_t spec_len,
                      const uint16_t *ref, size_t ref_len) {
    JsVm *vm = ud;
    (void)ref;
    (void)ref_len;
    char name[64];
    size_t n = spec_len < 63 ? spec_len : 63;
    for (size_t i = 0; i < n; i++)
        name[i] = (char)spec[i];
    name[n] = 0;
    int idx = mod_find(name);
    if (idx < 0)
        return rejected_with_ascii(ctx, vm, "module not found");
    g_mods.loads[idx]++;
    switch (g_mods.mode[idx]) {
    case MODE_SOURCE:
        return fulfilled_with(ctx, vm,
                              js_atom(vm, g_mods.sources[idx], g_mods.source_lens[idx]));
    case MODE_BYTECODE:
        return fulfilled_with(ctx, vm,
                              js_bytecode_value(ctx, g_mods.bytecode[idx],
                                                g_mods.bytecode_lens[idx]));
    case MODE_SYNTHETIC: {
        /* exports object: { default: 'cls-<name>', color: 'red' } */
        JsValue obj = js_object_new(ctx);
        js_gc_protect(vm, &obj);
        char dflt[80];
        snprintf(dflt, sizeof dflt, "cls-%s", name);
        size_t dl, kl;
        uint16_t *du = to_u16(dflt, &dl);
        uint16_t *ku = to_u16("default", &kl);
        js_object_set(vm, obj, js_atom(vm, ku, kl), js_atom(vm, du, dl));
        free(du);
        free(ku);
        uint16_t *ck = to_u16("color", &kl);
        uint16_t *cv = to_u16("red", &dl);
        js_object_set(vm, obj, js_atom(vm, ck, kl), js_atom(vm, cv, dl));
        free(ck);
        free(cv);
        JsValue p = fulfilled_with(ctx, vm, obj);
        js_gc_unprotect(vm, &obj);
        return p;
    }
    default: { /* MODE_DEFERRED: pending until the test settles it */
        JsValue p = js_promise_new(ctx);
        int i = g_pending.count++;
        g_pending.vm = vm;
        g_pending.promises[i] = p;
        g_pending.table_idx[i] = idx;
        g_pending.reject[i] = false;
        g_pending.reject_msg[i] = NULL;
        js_gc_protect(vm, &g_pending.promises[i]);
        return p;
    }
    }
}

/* './x' resolves against the referrer's directory; '@'-prefixed specifiers
 * fail; everything else is already canonical. */
static uint16_t g_canon_buf[256];
static bool canonicalize(void *ud, const uint16_t *spec, size_t spec_len,
                         const uint16_t *ref, size_t ref_len,
                         const uint16_t **out, size_t *out_len) {
    (void)ud;
    if (spec_len >= 1 && spec[0] == '@')
        return false;
    if (spec_len >= 2 && spec[0] == '.' && spec[1] == '/') {
        size_t dir = 0;
        for (size_t i = 0; i < ref_len; i++)
            if (ref[i] == '/')
                dir = i + 1;
        size_t o = 0;
        for (size_t i = 0; i < dir && o < 255; i++)
            g_canon_buf[o++] = ref[i];
        for (size_t i = 2; i < spec_len && o < 255; i++)
            g_canon_buf[o++] = spec[i];
        *out = g_canon_buf;
        *out_len = o;
        return true;
    }
    *out = spec;
    *out_len = spec_len;
    return true;
}

/* ---- harness ---- */

typedef struct {
    JsVm *vm;
    JsContext *ctx;
    CountAlloc ca;
} TestVm;

/* set for a second pass of the module-loading tests with a collection at every safe point */
static bool g_gc_stress;

static void tv_open(TestVm *tv, JsModuleCanonicalizer canon) {
    tv->ca.net_bytes = 0;
    tv->ca.live_allocs = 0;
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &tv->ca, .gc_stress = g_gc_stress};
    tv->vm = js_vm_new(&cfg);
    tv->ctx = js_context_new(tv->vm);
    g_pending.count = 0;
    js_set_module_loader(tv->ctx, loader, canon, tv->vm);
    js_enable_source_modules(tv->ctx);
}

static void tv_close(TestVm *tv, const char *label) {
    js_vm_free(tv->vm);
    checks_run++;
    if (tv->ca.net_bytes != 0 || tv->ca.live_allocs != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL leak: %s (net=%ld allocs=%ld)\n", label, tv->ca.net_bytes,
                tv->ca.live_allocs);
    }
}

static char *value_to_cstr(JsContext *ctx, JsValue v) {
    JsValue s = js_to_string(ctx, v);
    size_t sl;
    const uint16_t *su = js_string_units(s, &sl);
    char *out = malloc(sl + 1);
    for (size_t i = 0; i < sl; i++)
        out[i] = su && su[i] < 128 ? (char)su[i] : '?';
    out[sl] = 0;
    return out;
}

/* Reads export `exp` off namespace `ns` as a C string. */
static char *export_to_cstr(JsContext *ctx, JsVm *vm, JsValue ns, const char *exp) {
    size_t elen;
    uint16_t *eu = to_u16(exp, &elen);
    JsValue v = js_module_get_export(ctx, ns, eu, elen);
    free(eu);
    js_gc_protect(vm, &v);
    char *out = value_to_cstr(ctx, v);
    js_gc_unprotect(vm, &v);
    return out;
}

/* Evaluates root `spec`, settling every deferred load; returns the export's
 * (or rejection reason's) ToString, with *ok = fulfilled. */
static char *eval_export_spec(const char *spec, const char *exp, bool *ok) {
    TestVm tv;
    tv_open(&tv, NULL);
    size_t slen;
    uint16_t *su = to_u16(spec, &slen);
    JsValue p = js_eval_module(tv.ctx, su, slen);
    free(su);
    js_gc_protect(tv.vm, &p);
    settle_all_loads(tv.ctx);
    int st = js_promise_state(p);
    *ok = st == 1;
    char *out;
    if (st == 1)
        out = export_to_cstr(tv.ctx, tv.vm, js_promise_result(p), exp);
    else
        out = value_to_cstr(tv.ctx, js_promise_result(p));
    js_gc_unprotect(tv.vm, &p);
    tv_close(&tv, spec);
    return out;
}

static char *eval_export(const char *root_src, const char *exp, bool *ok) {
    (void)root_src; /* already in the table as 'main' */
    return eval_export_spec("main", exp, ok);
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

static void check(bool cond, const char *what) {
    checks_run++;
    if (!cond) {
        checks_failed++;
        fprintf(stderr, "FAIL %s\n", what);
    }
}

/* ---- original behavioral suite (sync-shaped loads) ---- */

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

    mod_add("m", "export const value = 'hello';");
    eq_export("import { value as v } from 'm'; export const out = v + ' world';", "out",
              "hello world");

    mod_add("lib", "export default function greet(n) { return 'Hi ' + n; }");
    eq_export("import greet from 'lib'; export const msg = greet('Al');", "msg", "Hi Al");

    mod_add("ns", "export const a = 1; export const b = 2; export const c = 3;");
    eq_export("import * as all from 'ns'; export const sum = all.a + all.b + all.c;", "sum",
              "6");
}

static void test_live_bindings(void) {
    /* importer sees a mutation the exporter makes later (via a function) */
    mod_add("counter",
            "export let n = 0; export function inc() { n = n + 1; }");
    eq_export("import { n, inc } from 'counter';"
              "inc(); inc(); inc();"
              "export const result = n;",
              "result", "3");
}

static void test_transitive(void) {
    mod_add("a", "export const A = 10;");
    mod_add("b", "import { A } from 'a'; export const B = A + 5;");
    eq_export("import { B } from 'b'; export const C = B * 2;", "C", "30");
}

static void test_diamond(void) {
    /* main -> {left, right} -> base; base must evaluate once */
    mod_add("base", "export let calls = 0; export function tick() { calls = calls + 1; return calls; }");
    mod_add("left", "import { tick } from 'base'; export const l = tick();");
    mod_add("right", "import { tick } from 'base'; export const r = tick();");
    eq_export("import { l } from 'left'; import { r } from 'right';"
              "export const total = l + r;",
              "total", "3"); /* tick() called once each: 1 + 2 */
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
}

static void test_star_reexport(void) {
    mod_add("inner", "export const a = 1; export const b = 2;");
    mod_add("mid", "export * from 'inner'; export const c = 3;");
    eq_export("import * as m from 'mid'; export const sum = m.a + m.b + m.c;", "sum", "6");
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

    /* the re-exported name appears on the namespace too */
    mod_add("lib", "export const VERSION = '1.0'; export const NAME = 'lamassu';");
    mod_add("barrel", "export { VERSION } from 'lib'; export { NAME as title } from 'lib';");
    eq_export("import * as b from 'barrel';"
              "export const info = b.title + ' ' + b.VERSION;",
              "info", "lamassu 1.0");
}

static void test_reexport_live(void) {
    /* A NAMED re-export is a live binding: importing through the re-exporter
     * observes a mutation the origin module makes later (not a snapshot taken
     * at evaluation time). */
    mod_add("counter2", "export let n = 0; export function inc() { n = n + 1; }");
    mod_add("passthru", "export { n, inc } from 'counter2';");
    eq_export("import { n, inc } from 'passthru';"
              "inc(); inc();"
              "export const result = n;",
              "result", "2");

    /* Liveness survives a chained + renamed re-export. */
    mod_add("counter3", "export let c = 0; export function bump() { c = c + 1; }");
    mod_add("mid1", "export { c as v, bump } from 'counter3';");
    mod_add("mid2", "export { v, bump } from 'mid1';");
    eq_export("import { v, bump } from 'mid2';"
              "bump(); bump(); bump();"
              "export const result = v;",
              "result", "3");
}

static void test_reexport_ns(void) {
    /* export * as ns from 'm' — namespace re-export (live by reference) */
    mod_add("math", "export const pi = 3.14; export function sq(x) { return x * x; }");
    mod_add("facade", "export * as m from 'math'; export const own = true;");
    eq_export("import { m, own } from 'facade';"
              "export const out = m.pi + '/' + m.sq(4) + '/' + own;",
              "out", "3.14/16/true");
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
}

static void test_async_module(void) {
    mod_add("data", "export async function load() { return await Promise.resolve(42); }");
    eq_export("import { load } from 'data';"
              "export let result = 0;"
              "load().then(v => result = v);",
              "result", "42"); /* microtasks drained during evaluation */
    /* top-level await in a module */
    eq_export("export const value = await Promise.resolve('ready');", "value", "ready");
}

static void test_errors(void) {
    mods_reset();
    mod_add("main", "import { x } from 'missing';");
    bool ok;
    char *out = eval_export_spec("main", "x", &ok);
    checks_run++;
    if (ok || !strstr(out, "not found")) {
        checks_failed++;
        fprintf(stderr, "FAIL expected 'not found', got: %s\n", out);
    }
    free(out);
    mods_reset();

    mod_add("main", "import { y } from 'bad';");
    mod_add("bad", "this is not valid js @#$");
    out = eval_export_spec("main", "y", &ok);
    checks_run++;
    if (ok) {
        checks_failed++;
        fprintf(stderr, "FAIL expected compile error in dependency\n");
    }
    free(out);
    mods_reset();

    /* runtime error during module evaluation propagates */
    mod_add("main", "throw 'module boom';");
    out = eval_export_spec("main", "x", &ok);
    checks_run++;
    if (ok || !strstr(out, "boom")) {
        checks_failed++;
        fprintf(stderr, "FAIL expected 'boom', got: %s\n", out);
    }
    free(out);
    mods_reset();
}

/* ---- async-specific suite ---- */

/*
 * Diamond whose branches load on different host turns. Between rounds the
 * job queue must be empty — a same-drain false positive can't pass this.
 */
static void test_cross_turn_rounds(void) {
    mod_add("shared", "export let hits = 0; export function hit() { hits = hits + 1; return hits; }");
    mod_add_mode("left", "import { hit } from 'shared'; export const l = hit();", MODE_DEFERRED);
    mod_add_mode("right", "import { hit } from 'shared'; export const r = hit();", MODE_DEFERRED);
    mod_add("main", "import { l } from 'left'; import { r } from 'right';"
                    "export const total = l * 10 + r;");
    TestVm tv;
    tv_open(&tv, NULL);
    static const uint16_t spec[] = {'m', 'a', 'i', 'n'};
    JsValue p = js_eval_module(tv.ctx, spec, 4);
    js_gc_protect(tv.vm, &p);

    check(js_promise_state(p) == 0 && g_pending.count == 2 && !js_has_pending_jobs(tv.ctx),
          "cross-turn: pending with a quiescent job queue, two loads outstanding");

    settle_load(tv.ctx, "left"); /* round 1 */
    check(js_promise_state(p) == 0 && !js_has_pending_jobs(tv.ctx),
          "cross-turn: still pending after round 1, queue quiescent again");

    settle_load(tv.ctx, "right"); /* round 2 */
    check(js_promise_state(p) == 1, "cross-turn: fulfilled after final round");
    if (js_promise_state(p) == 1) {
        char *out = export_to_cstr(tv.ctx, tv.vm, js_promise_result(p), "total");
        check(strcmp(out, "12") == 0, "cross-turn: exports correct (12)");
        free(out);
    }
    js_gc_unprotect(tv.vm, &p);
    tv_close(&tv, "cross-turn");
    mods_reset();
}

/* Synthetic module: the loader resolves with a plain object adopted as the
 * exports. Its table "source" is invalid JS — proving it is never parsed. */
static void test_synthetic(void) {
    mod_add_mode("styles.css", ".card { color: red } /* not js! @# */", MODE_SYNTHETIC);
    mod_add("main", "import css from 'styles.css';"
                    "import { color } from 'styles.css';"
                    "export const out = css + '/' + color;");
    bool ok;
    char *out = eval_export_spec("main", "out", &ok);
    check(ok && strcmp(out, "cls-styles.css/red") == 0, "synthetic exports adopted");
    free(out);
    mods_reset();
}

/* One graph mixing all three fulfillment types. */
static void test_mixed_graph(void) {
    TestVm tv;
    tv_open(&tv, NULL);
    /* precompile 'bcmod' to bytecode in this context */
    size_t blen;
    uint16_t *bu = to_u16("export const b = 20;", &blen);
    static const uint16_t bspec[] = {'b', 'c', 'm', 'o', 'd'};
    uint8_t *buf;
    size_t buf_len;
    const char *em;
    uint32_t ep;
    bool cok = js_bytecode_compile_module(tv.ctx, bspec, 5, bu, blen, &buf, &buf_len, &em, &ep);
    free(bu);
    check(cok, "mixed: bytecode compile");
    int bi = mod_add_mode("bcmod", NULL, MODE_BYTECODE);
    g_mods.bytecode[bi] = malloc(buf_len);
    memcpy(g_mods.bytecode[bi], buf, buf_len);
    g_mods.bytecode_lens[bi] = buf_len;
    js_bytecode_free(tv.ctx, buf, buf_len);

    mod_add("srcmod", "export const s = 1;");
    mod_add_mode("theme.css", "not js either {", MODE_SYNTHETIC);
    mod_add("main", "import { s } from 'srcmod';"
                    "import { b } from 'bcmod';"
                    "import { color } from 'theme.css';"
                    "export const out = s + b + '-' + color;");

    static const uint16_t spec[] = {'m', 'a', 'i', 'n'};
    JsValue p = js_eval_module(tv.ctx, spec, 4);
    js_gc_protect(tv.vm, &p);
    char *out = js_promise_state(p) == 1
                    ? export_to_cstr(tv.ctx, tv.vm, js_promise_result(p), "out")
                    : value_to_cstr(tv.ctx, js_promise_result(p));
    check(js_promise_state(p) == 1 && strcmp(out, "21-red") == 0,
          "mixed source/bytecode/synthetic graph");
    free(out);
    js_gc_unprotect(tv.vm, &p);
    tv_close(&tv, "mixed graph");
    mods_reset();
}

/* Diamond dedup: one loader invocation per specifier, even when both
 * importers' loads are deferred and settle on different turns. */
static void test_load_dedup(void) {
    mod_add("shared", "export const x = 1;");
    mod_add_mode("l", "import { x } from 'shared'; export const a = x;", MODE_DEFERRED);
    mod_add_mode("r", "import { x } from 'shared'; export const b = x;", MODE_DEFERRED);
    mod_add("main", "import { a } from 'l'; import { b } from 'r'; export const s = a + b;");
    TestVm tv;
    tv_open(&tv, NULL);
    static const uint16_t spec[] = {'m', 'a', 'i', 'n'};
    JsValue p = js_eval_module(tv.ctx, spec, 4);
    js_gc_protect(tv.vm, &p);
    settle_load(tv.ctx, "l");
    settle_load(tv.ctx, "r");
    check(js_promise_state(p) == 1, "dedup: graph completed");
    check(g_mods.loads[mod_find("shared")] == 1, "dedup: shared loaded exactly once");
    js_gc_unprotect(tv.vm, &p);
    tv_close(&tv, "dedup");
    mods_reset();
}

/* A deep dependency's load rejects: the root completion must reject with
 * that exact reason. */
static void test_reject_propagation(void) {
    mod_add("mid", "import { d } from 'deep'; export const m = d;");
    mod_add_mode("deep", "export const d = 1;", MODE_DEFERRED);
    mod_add("main", "import { m } from 'mid'; export const out = m;");
    TestVm tv;
    tv_open(&tv, NULL);
    static const uint16_t spec[] = {'m', 'a', 'i', 'n'};
    JsValue p = js_eval_module(tv.ctx, spec, 4);
    js_gc_protect(tv.vm, &p);
    check(js_promise_state(p) == 0, "reject-prop: pending before settle");
    /* reject deep's load */
    g_pending.reject[0] = true;
    g_pending.reject_msg[0] = "fetch failed: 404";
    settle_load(tv.ctx, "deep");
    char *out = value_to_cstr(tv.ctx, js_promise_result(p));
    check(js_promise_state(p) == 2 && strcmp(out, "fetch failed: 404") == 0,
          "reject-prop: root rejects with the loader's exact reason");
    free(out);
    js_gc_unprotect(tv.vm, &p);
    tv_close(&tv, "reject propagation");
    mods_reset();
}

static void test_canonicalization(void) {
    /* convergent: sibling files' './a.js' resolve to the same module */
    mod_add("dir/main.js", "import { tag } from './a.js'; export const m1 = tag;");
    mod_add("dir/other.js", "import { tag } from './a.js'; export const m2 = tag;");
    mod_add("dir/a.js", "export let n = 0; n = n + 1; export const tag = 'a' + n;");
    mod_add("main", "import { m1 } from 'dir/main.js'; import { m2 } from 'dir/other.js';"
                    "export const out = m1 + '/' + m2;");
    TestVm tv;
    tv_open(&tv, canonicalize);
    static const uint16_t spec[] = {'m', 'a', 'i', 'n'};
    JsValue p = js_eval_module(tv.ctx, spec, 4);
    js_gc_protect(tv.vm, &p);
    char *out = js_promise_state(p) == 1
                    ? export_to_cstr(tv.ctx, tv.vm, js_promise_result(p), "out")
                    : value_to_cstr(tv.ctx, js_promise_result(p));
    check(js_promise_state(p) == 1 && strcmp(out, "a1/a1") == 0,
          "canon convergent: one shared dir/a.js");
    check(g_mods.loads[mod_find("dir/a.js")] == 1,
          "canon convergent: loader fired once for dir/a.js");
    free(out);
    js_gc_unprotect(tv.vm, &p);
    tv_close(&tv, "canon convergent");
    mods_reset();

    /* divergent: the same raw './util.js' from different directories is two
     * distinct modules */
    mod_add("x/main.js", "import { u } from './util.js'; export const xu = u;");
    mod_add("y/main.js", "import { u } from './util.js'; export const yu = u;");
    mod_add("x/util.js", "export const u = 'X';");
    mod_add("y/util.js", "export const u = 'Y';");
    mod_add("main", "import { xu } from 'x/main.js'; import { yu } from 'y/main.js';"
                    "export const out = xu + yu;");
    tv_open(&tv, canonicalize);
    JsValue p2 = js_eval_module(tv.ctx, spec, 4);
    js_gc_protect(tv.vm, &p2);
    out = js_promise_state(p2) == 1
              ? export_to_cstr(tv.ctx, tv.vm, js_promise_result(p2), "out")
              : value_to_cstr(tv.ctx, js_promise_result(p2));
    check(js_promise_state(p2) == 1 && strcmp(out, "XY") == 0,
          "canon divergent: distinct modules, no false sharing");
    check(g_mods.loads[mod_find("x/util.js")] == 1 && g_mods.loads[mod_find("y/util.js")] == 1,
          "canon divergent: each util loaded once");
    free(out);
    js_gc_unprotect(tv.vm, &p2);
    tv_close(&tv, "canon divergent");
    mods_reset();

    /* failure: canonicalizer refuses the specifier */
    mod_add("main", "import { z } from '@bad'; export const out = z;");
    tv_open(&tv, canonicalize);
    JsValue p3 = js_eval_module(tv.ctx, spec, 4);
    js_gc_protect(tv.vm, &p3);
    out = value_to_cstr(tv.ctx, js_promise_result(p3));
    check(js_promise_state(p3) == 2 && strstr(out, "cannot resolve module specifier") != NULL,
          "canon failure: rejection with resolve error");
    free(out);
    js_gc_unprotect(tv.vm, &p3);
    tv_close(&tv, "canon failure");
    mods_reset();
}

/* ---- TLA body awaiting a host promise inside a dependency ---- */

static JsValue g_defer_promise;
static JsValue g_defer_value;
static int g_defer_count;

static bool native_defer(JsContext *ctx, JsValue this_val, const JsValue *args,
                         int argc, JsValue *result) {
    (void)this_val;
    JsValue p = js_promise_new(ctx);
    g_defer_promise = p;
    g_defer_value = argc > 0 ? args[0] : js_undefined();
    g_defer_count++;
    *result = p;
    return true;
}

static void test_tla_dep_cross_turn(void) {
    mod_add("slow", "export const v = await defer('late');");
    mod_add("main", "import { v } from 'slow'; export const out = v + '!';");
    TestVm tv;
    tv_open(&tv, NULL);
    static const uint16_t n_defer[] = {'d', 'e', 'f', 'e', 'r'};
    js_register_native(tv.ctx, n_defer, 5, native_defer, NULL);
    g_defer_count = 0;

    /* the host keeps the promise it handed out alive from the start: the suspended evaluation
     * hangs from it */
    g_defer_promise = js_undefined();
    g_defer_value = js_undefined();
    js_gc_protect(tv.vm, &g_defer_promise);
    js_gc_protect(tv.vm, &g_defer_value);
    static const uint16_t spec[] = {'m', 'a', 'i', 'n'};
    JsValue p = js_eval_module(tv.ctx, spec, 4);
    js_gc_protect(tv.vm, &p);

    check(js_promise_state(p) == 0 && g_defer_count == 1 && !js_has_pending_jobs(tv.ctx),
          "tla-dep: root pending while a dependency body awaits the host");

    /* a full GC in the gap must not lose the suspended evaluation */
    js_gc_collect(tv.vm);
    js_resolve(tv.ctx, g_defer_promise, g_defer_value);
    js_run_jobs(tv.ctx);

    check(js_promise_state(p) == 1, "tla-dep: fulfilled after the host settles");
    if (js_promise_state(p) == 1) {
        char *out = export_to_cstr(tv.ctx, tv.vm, js_promise_result(p), "out");
        check(strcmp(out, "late!") == 0, "tla-dep: export observed the awaited value");
        free(out);
    }
    js_gc_unprotect(tv.vm, &g_defer_value);
    js_gc_unprotect(tv.vm, &g_defer_promise);
    js_gc_unprotect(tv.vm, &p);
    tv_close(&tv, "tla dependency");
    mods_reset();
}


/*
 * The frontend/runtime split, observed from the embedder's side.
 *
 * A context that has NOT been granted source compilation must refuse a loader
 * that fulfills with source, while a loader fulfilling with bytecode keeps
 * working. This is the one behaviour that distinguishes a fleet runtime from a
 * build-time process, and the only way to test it in a binary that links both
 * halves is to simply not call js_enable_source_modules — which is exactly the
 * state a runtime-only link is permanently in, since the symbol isn't there.
 */
static void test_source_modules_gated(void) {
    mods_reset();
    mod_add("main", "export const x = 1;");

    TestVm tv;
    tv.ca.net_bytes = 0;
    tv.ca.live_allocs = 0;
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &tv.ca};
    tv.vm = js_vm_new(&cfg);
    tv.ctx = js_context_new(tv.vm);
    g_pending.count = 0;
    js_set_module_loader(tv.ctx, loader, NULL, tv.vm);
    /* deliberately NO js_enable_source_modules(tv.ctx) */

    size_t slen;
    uint16_t *su = to_u16("main", &slen);
    JsValue p = js_eval_module(tv.ctx, su, slen);
    free(su);
    js_gc_protect(tv.vm, &p);
    settle_all_loads(tv.ctx);
    int st = js_promise_state(p);
    char *out = value_to_cstr(tv.ctx, js_promise_result(p));
    checks_run++;
    if (st != 2 || !strstr(out, "source modules unavailable")) {
        checks_failed++;
        fprintf(stderr, "FAIL expected source-module rejection, got state=%d: %s\n", st, out);
    }
    free(out);
    js_gc_unprotect(tv.vm, &p);
    tv_close(&tv, "source modules gated");
    mods_reset();
}

/* ---- js_eval_entry: top-level return in the root ---- */

/* Evaluates `main` with js_eval_entry; the fulfilled value's (or the rejection's) ToString. */
static char *eval_entry_str(bool *ok) {
    TestVm tv;
    tv_open(&tv, NULL);
    static const uint16_t spec[] = {'m', 'a', 'i', 'n'};
    JsValue p = js_eval_entry(tv.ctx, spec, 4);
    js_gc_protect(tv.vm, &p);
    settle_all_loads(tv.ctx);
    int st = js_promise_state(p);
    *ok = st == 1;
    char *out = value_to_cstr(tv.ctx, js_promise_result(p));
    js_gc_unprotect(tv.vm, &p);
    tv_close(&tv, "main");
    return out;
}

static void eq_entry(const char *root_src, bool want_ok, const char *expected) {
    mod_add("main", root_src);
    bool ok;
    char *out = eval_entry_str(&ok);
    checks_run++;
    if (ok != want_ok || !strstr(out, expected) || (want_ok && strcmp(out, expected) != 0)) {
        checks_failed++;
        fprintf(stderr, "FAIL entry\n  root: %s\n  expected: %s%s\n  actual: %s%s\n", root_src,
                expected, want_ok ? "" : " (error)", out, ok ? "" : " (error)");
    }
    free(out);
    mods_reset();
}

static void test_entry_return(void) {
    eq_entry("return 42;", true, "42");
    eq_entry("const a = 6; return a * 7;", true, "42");
    eq_entry("1 + 1;", true, "undefined");                          /* off the end: undefined, not the last expression */
    eq_entry("let n = 0; for (;;) { if (++n > 3) return n; } ", true, "4");
    eq_entry("if (true) { return 'early'; } throw 'not reached';", true, "early");
    eq_entry("let log = ''; try { return 'r'; } finally { log = 'f'; }", true, "r");
    eq_entry("function f() { return 1; } return f() + (() => { return 2; })();", true, "3");
    eq_entry("return await Promise.resolve('tla');", true, "tla");  /* top-level await too */
    eq_entry("return;", true, "undefined");
    eq_entry("throw new Error('boom');", false, "boom");
    /* imports still load first, and their own top-level return is a SyntaxError */
    mod_add("dep", "export const v = 5;");
    eq_entry("import { v } from 'dep'; return v * 2;", true, "10");
    /* a module that does not compile rejects with a SyntaxError object, worded as V8 words it */
    mod_add("bad", "let x = ;");
    eq_entry("try { await import('bad'); } catch (e) { return e.name + '|' + e.message; }", true,
             "SyntaxError|Unexpected token ';'");
    mod_add("bad", "let a = 1; let a = 2;");
    eq_entry("try { await import('bad'); } catch (e) { return e.name + '|' + e.message; }", true,
             "SyntaxError|Identifier 'a' has already been declared");
    mod_add("dep", "return 1; export const v = 5;");
    eq_entry("import { v } from 'dep'; return v;", false, "Illegal return statement");
    /* a deferred (async) loader: the root's record is still marked while it waits */
    mod_add_mode("main", "return 'later';", MODE_DEFERRED);
    {
        bool ok;
        char *out = eval_entry_str(&ok);
        check(ok && strcmp(out, "later") == 0, "entry: return from a root loaded on a later turn");
        free(out);
        mods_reset();
    }
    /* js_eval_module keeps refusing it */
    mod_add("main", "return 1;");
    {
        bool ok;
        char *out = eval_export_spec("main", "x", &ok);
        check(!ok && strstr(out, "Illegal return statement"), "js_eval_module: top-level return is a SyntaxError");
        free(out);
        mods_reset();
    }
    /* exports declared after the return exist; the cached graph hands back the namespace and the value */
    mod_add("main", "export const before = 1; return 'v'; export function after() { return 2; }");
    {
        TestVm tv;
        tv_open(&tv, NULL);
        static const uint16_t spec[] = {'m', 'a', 'i', 'n'};
        JsValue p = js_eval_entry(tv.ctx, spec, 4);
        js_gc_protect(tv.vm, &p);
        settle_all_loads(tv.ctx);
        char *v = value_to_cstr(tv.ctx, js_promise_result(p));
        JsValue ns = js_eval_module(tv.ctx, spec, 4);
        js_gc_protect(tv.vm, &ns);
        settle_all_loads(tv.ctx);
        char *b = export_to_cstr(tv.ctx, tv.vm, js_promise_result(ns), "before");
        char *t = export_to_cstr(tv.ctx, tv.vm, js_promise_result(ns), "after");
        JsValue again = js_eval_entry(tv.ctx, spec, 4);
        js_gc_protect(tv.vm, &again);
        settle_all_loads(tv.ctx);
        char *v2 = value_to_cstr(tv.ctx, js_promise_result(again));
        check(strcmp(v, "v") == 0 && strcmp(b, "1") == 0 && strstr(t, "function") && strcmp(v2, "v") == 0,
              "entry: exports past the return, namespace and value from the cached graph");
        free(v); free(b); free(t); free(v2);
        js_gc_unprotect(tv.vm, &again);
        js_gc_unprotect(tv.vm, &ns);
        js_gc_unprotect(tv.vm, &p);
        tv_close(&tv, "main");
        mods_reset();
    }
}

/* js_check_module: the loader's compile without registering, linking or running. */
static bool check_src(JsContext *ctx, const char *src, bool entry, const char **msg, uint32_t *pos) {
    size_t n;
    uint16_t *u = to_u16(src, &n);
    bool ok = js_check_module(ctx, u, n, entry, msg, pos);
    free(u);
    return ok;
}

static void test_check_module(void) {
    TestVm tv;
    tv_open(&tv, NULL);
    const char *msg;
    uint32_t pos;
    check(check_src(tv.ctx, "import { v } from 'nowhere'; export const w = v + 1;", false, &msg, &pos) && !msg,
          "check: imports and exports compile; the import is not loaded");
    check(check_src(tv.ctx, "const a = 1;\nreturn a;", true, &msg, &pos), "check: an entry may return at top level");
    check(!check_src(tv.ctx, "const a = 1;\nreturn a;", false, &msg, &pos) && strstr(msg, "Illegal return statement"),
          "check: a module may not");
    check(!check_src(tv.ctx, "let x = 1;\nlet y = (x + ;", true, &msg, &pos) && msg && pos >= 11 && pos <= 24,
          "check: a parse error has a message and an offset on its line");
    check(!check_src(tv.ctx, "var x = 1;", true, &msg, &pos) && msg, "check: words outside the subset are errors");
    check(check_src(tv.ctx, "throw new Error('not run');", true, &msg, &pos), "check: nothing runs");
    /* the context is unchanged: a checked declaration is not a global */
    check(check_src(tv.ctx, "const leaked = 1; export const k = 2;", false, &msg, &pos), "check: declarations compile");
    {
        size_t n;
        uint16_t *k = to_u16("leaked", &n);
        JsValue g = js_object_get(tv.vm, js_context_globals(tv.ctx), js_atom(tv.vm, k, n));
        free(k);
        check(js_is_undefined(g), "check: leaves no globals behind");
    }
    tv_close(&tv, "check");
}


/* ---- declaration rules: the TDZ of exports, export lists, namespaces ---- */

/* A module that records what each thunk returns or throws, as "value|" or "Name: message|". */
#define TRY_ALL "const t = (fs) => fs.map(f => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }).join('|');"

static void test_declaration_rules(void) {
    /* a cycle: b reads a's exports before a has run its declarations */
    mod_add("a",
            "import { early } from 'b';"
            "export const r = early;"
            "export let y = 1; export const z = 2; let w = 3; export { w as ww };");
    mod_add("b",
            "import * as ns from 'a'; import { y as yy, z, ww } from 'a';" TRY_ALL
            "export const early = t([() => yy, () => z, () => ww, () => ns.y, () => 'y' in ns,"
            "  () => Object.keys(ns), () => JSON.stringify(ns), () => Object.values(ns),"
            "  () => Object.entries(ns), () => ({ ...ns }), () => Object.assign({}, ns),"
            "  () => Object.getOwnPropertyNames(ns), () => Object.hasOwn(ns, 'y'),"
            "  () => Object.prototype.hasOwnProperty.call(ns, 'z'), () => Object.hasOwn(ns, 'q')]);");
    eq_export("import { r } from 'a'; export const out = r;", "out",
              "ReferenceError: Cannot access 'yy' before initialization|"
              "ReferenceError: Cannot access 'z' before initialization|"
              "ReferenceError: Cannot access 'ww' before initialization|"
              "ReferenceError: Cannot access 'y' before initialization|true|"
              "ReferenceError: Cannot access 'r' before initialization|"
              "ReferenceError: Cannot access 'r' before initialization|"
              "ReferenceError: r is not defined|ReferenceError: r is not defined|"
              "ReferenceError: r is not defined|ReferenceError: r is not defined|"
              "r,ww,y,z|ReferenceError: y is not defined|ReferenceError: z is not defined|false");
    /* and once a has run, the same reads see the values */
    mod_add("a", "export let y = 1; export const z = 2;");
    eq_export("import * as ns from 'a'; import { y } from 'a';"
              "export const out = [y, ns.z, Object.keys(ns), JSON.stringify(ns)].join(' ');",
              "out", "1 2 y,z {\"y\":1,\"z\":2}");

    /* the exporting module itself: a hoisted function, an assignment, a const */
    eq_export(TRY_ALL "function h() { return e; }"
              "export const out = t([h, () => { e = 2; }, () => { c = 2; }]);"
              "export let e = 1; export const c = 1;",
              "out", "ReferenceError: Cannot access 'e' before initialization|"
                     "ReferenceError: Cannot access 'e' before initialization|"
                     "ReferenceError: Cannot access 'c' before initialization");
    eq_export("let w = 1; export { w as v }; function f() { return w; }"
              TRY_ALL "export const out = t([() => { k = 1; }]) + ' ' + f(); const k = 0;",
              "out", "ReferenceError: Cannot access 'k' before initialization 1");
    /* an export default expression is in the TDZ too; a default function is not */
    mod_add("da", "import { early } from 'db'; export const r = early; export default 5;");
    mod_add("db", "import d from 'da';" TRY_ALL "export const early = t([() => d]);");
    eq_export("import { r } from 'da'; export const out = r;", "out",
              "ReferenceError: Cannot access 'd' before initialization");

    /* export lists: every name of a binding, const rules, imports re-exported, unknown names */
    eq_export("export let a = 1; export { a as b }; a = 2;"
              "export function f() {} export { f as g }; let c = 3; export { c, c as c2 };"
              "import * as me from 'main'; export let out;" /* out itself is in the TDZ until declared */
              "out = JSON.stringify(me) + ' ' + typeof me.g;",
              "out", "{\"a\":2,\"b\":2,\"c\":3,\"c2\":3} function");
    mod_add("k", "export const k = 1;");
    eq_export("import { k } from 'k'; import * as kn from 'k'; export { k, k as k2, kn };"
              "import * as me from 'main'; export const out = '' + me.k + me.k2 + me.kn.k;",
              "out", "111");
    eq_export("const w = 1; export { w };" TRY_ALL "export const out = t([() => { w = 2; }]);",
              "out", "TypeError: Assignment to constant variable.");
    mod_add("main", "export { nope };");
    {
        bool ok;
        char *msg = eval_export_spec("main", "x", &ok);
        check(!ok && strstr(msg, "SyntaxError: Export 'nope' is not defined in module"),
              "export of an undeclared name is a SyntaxError");
        free(msg);
    }
    mods_reset();

    /* import attributes: type json makes a JSON module (its default export the parsed text); the
     * same specifier imported as JS is another module; other attributes are refused */
    mod_add("data", "{\"a\": 1, \"b\": [true, null]}");
    mod_add("both", "42");
    eq_export("import d from 'data' with { type: 'json' }; import * as ns from 'data' with { 'type': \"json\" };"
              "import j from 'both' with { type: 'json' }; import * as js from 'both';"
              "export const out = [JSON.stringify(d), Object.keys(ns).join(), ns.default === d, j,"
              "  Object.keys(js).length, Object.isSealed(ns)].join(' ');",
              "out", "{\"a\":1,\"b\":[true,null]} default true 42 0 true");
    mods_reset();
    {
        const char *cases[][2] = {
            {"import d from 'data' with { type: 'css' }; export const x = 1;",
             "TypeError: Import attribute \"type\" with value \"css\" is not supported"},
            {"import d from 'data' with { foo: 'x' }; export const x = 1;",
             "TypeError: Import attribute \"foo\" with value \"x\" is not supported"},
            {"import d from 'data' with { type: 'json', type: 'json' }; export const x = 1;",
             "SyntaxError: Import attribute has duplicate key 'type'"},
            {"import d from 'bad' with { type: 'json' }; export const x = 1;", "SyntaxError: "},
            {"import { a } from 'data' with { type: 'json' }; export const x = 1;",
             "SyntaxError: The requested module 'data' does not provide an export named 'a'"},
            {"import { nope } from 'lib'; export const x = 1;",
             "SyntaxError: The requested module 'lib' does not provide an export named 'nope'"},
            {"export { nope } from 'lib'; export const x = 1;",
             "SyntaxError: The requested module 'lib' does not provide an export named 'nope'"},
            {"import { default as d } from 'lib'; export const x = 1;",
             "SyntaxError: The requested module 'lib' does not provide an export named 'default'"},
            {"import d from 'data' assert { type: 'json' }; export const x = 1;",
             "SyntaxError: Unexpected identifier 'assert'"},
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            mod_add("data", "{\"a\": 1}");
            mod_add("bad", "{a: 1}");
            mod_add("lib", "export function f() {} export const c = 1; export * from 'lib2';");
            mod_add("lib2", "export default 1; export let z = 2;");
            mod_add("main", cases[i][0]);
            bool ok;
            char *msg = eval_export_spec("main", "x", &ok);
            check(!ok && strstr(msg, cases[i][1]) != NULL, cases[i][0]);
            if (ok || !strstr(msg, cases[i][1]))
                fprintf(stderr, "  got: %s\n", msg);
            free(msg);
            mods_reset();
        }
    }

    /* import() with options: { with: { type: 'json' } } makes a JSON module; V8's TypeErrors */
    mod_add("data", "{\"a\": [1, 2]}");
    mod_add("lib", "export const v = 7;");
    eq_export("const show = (p) => p.then((m) => JSON.stringify(m.default ?? m.v), (e) => e.constructor.name + ': ' + e.message);"
              "export const out = (await Promise.all([show(import('data', { with: { type: 'json' } })),"
              "  show(import('data', 1)), show(import('data', { with: 1 })), show(import('data', { with: { type: 1 } })),"
              "  show(import('lib', {})), show(import('lib', undefined)), show(import('lib', null)),"
              "  show(import('data', { with: { type: 'json', foo: 'x' } })), show(import('data', { with: { type: 'css' } })),"
              "  show(import('a\\0b'))])).join(' | ');",
              "out", "{\"a\":[1,2]} | TypeError: The second argument to import() must be an object | "
                     "TypeError: The 'with' option must be an object | TypeError: Import attribute value must be a string | "
                     "7 | 7 | TypeError: The second argument to import() must be an object | {\"a\":[1,2]} | "
                     "TypeError: Import attribute \"type\" with value \"css\" is not supported | "
                     "TypeError: A module specifier may not contain a NUL character");
    mods_reset();

    /* names provided through functions, let/const, `export *` (not its default), and a cycle */
    mod_add("lib", "export function f() { return 'f'; } export const c = 1; export * from 'lib2';");
    mod_add("lib2", "export default 1; export let z = 2; export * from 'lib';");
    eq_export("import { f, c, z } from 'lib'; export const out = [f(), c, z].join();", "out", "f,1,2");
    mods_reset();

    /* import.meta: one object per module, no prototype, its url the module's specifier */
    eq_export("function f() { return import.meta; } const g = () => import.meta;"
              "export const out = [import.meta.url, f() === import.meta, g() === import.meta,"
              "  Object.getPrototypeOf(import.meta), typeof import.meta].join();",
              "out", "main,true,true,,object");
    mods_reset();

    /* a namespace is sealed, its exports read-only, its keys in name order */
    eq_export("export let q = 4; export const fz = 1; import * as ns from 'main';" TRY_ALL
              "export let out; out = t([() => { ns.zz = 1; }, () => { ns.q = 1; }, () => { delete ns.q; },"
              "  () => Object.isSealed(ns) + ' ' + Object.isFrozen(ns), () => Object.freeze(ns),"
              "  () => Object.setPrototypeOf(ns, {}), () => Object.assign(ns, { q: 1 }),"
              "  () => Object.keys(ns)]);",
              "out", "TypeError: Cannot assign to property 'zz' of [object Module]|"
                     "TypeError: Cannot assign to property 'q' of [object Module]|"
                     "TypeError: Cannot delete property 'q' of [object Module]|true false|"
                     "TypeError: Cannot redefine property: fz|"
                     "TypeError: [object Module] is not extensible|"
                     "TypeError: Cannot assign to property 'q' of [object Module]|"
                     "fz,out,q");
}

/* V8's InitializeEnvironment: every module's functions exist before any module body runs, so
 * in a cycle the module that runs first can call what the others export */
static void test_functions_at_instantiation(void) {
    mods_reset();
    mod_add("fa",
            "import { fromB } from 'fb';"
            "let n = 1;"
            "export function f() { return 'f'; }"
            "export function g() { return n; }"
            "export default function named() { return 'named'; }"
            "function h() { return 'h' + typeof named; }"
            "export { h as alias };"
            "export const out = fromB + ' ' + named() + ' ' + g();");
    mod_add("fb",
            "import d, { f, g, alias } from 'fa';" TRY_ALL
            "export const fromB = t([f, d, alias, g]);");
    eq_export("import { out } from 'fa'; export const r = out;", "r",
              "f|named|hfunction|ReferenceError: Cannot access 'n' before initialization named 1");

    /* an anonymous default function, and `export { name }` of a named one */
    mods_reset();
    mod_add("da", "import { early } from 'db'; export const r = early;"
                  "export default function () { return 'anon'; }");
    mod_add("db", "import d from 'da'; export const early = d();");
    mod_add("dc", "export default function nd() { return 'nd'; } export { nd, nd as nd2 };");
    eq_export("import { r } from 'da'; import d, { nd, nd2 } from 'dc';"
              "export const out = r + ' ' + (d === nd && nd === nd2) + ' ' + nd();",
              "out", "anon true nd");

    /* three modules and top-level await: each body runs after its dependencies, in V8's order */
    mods_reset();
    mod_add("log", "export const log = [];");
    mod_add("ca", "import { log } from 'log'; import { b } from 'cb';"
                  "export function a() { return 'a'; } log.push('a:' + b());");
    mod_add("cb", "import { log } from 'log'; import { c } from 'cc'; await 0;"
                  "export function b() { return 'b'; } log.push('b:' + c());");
    mod_add("cc", "import { log } from 'log'; import { a } from 'ca';"
                  "export function c() { return 'c'; } log.push('c:' + a());");
    eq_export("import { log } from 'log'; import 'ca'; export const out = log.join(' ');",
              "out", "c:a b:c a:b");

    /* a body that throws after calling into the module that runs later */
    mods_reset();
    mod_add("ea", "import { g } from 'eb'; export function f() { return 1; }");
    mod_add("eb", "import { f } from 'ea'; export function g() {}"
                  "if (f() === 1) throw new Error('boom');");
    {
        mod_add("main", "import 'ea';");
        bool ok;
        char *msg = eval_export_spec("main", "x", &ok);
        check(!ok && strstr(msg, "boom"), "a cycle body that throws rejects the graph");
        free(msg);
    }
    mods_reset();
}

/* e.stack in a module graph (Node's output for the same files, the importer's columns shifted for
 * its './lib.mjs'): an engine error raised in a callback a builtin made, frames kept by errors
 * while garbage is made, an async frame; under the collector at every safe point too */
static void test_error_stack(void) {
    mods_reset();
    mod_add("lib", "export function thrower(o) { return o.missing.x; }\n"
                   "export const each = (a, f) => a.map(f);\n");
    eq_export("import { thrower, each } from 'lib'; let s1; try { each([{}], thrower); } catch (e) { s1 = e.stack; }"
              " const kept = []; for (let i = 0; i < 50; i++) kept.push(new Error('e' + i));"
              " const junk = kept.map((e) => ({ big: 'x'.repeat(100) }));"
              " async function a1() { await null; return new Error('late'); } const late = await a1();"
              " export const r = [s1, kept[49].stack, late.stack].map((s) => s.split('\\n')"
              ".filter((l) => !l.includes('node:')).join('\\n')).join('|');",
              "r",
              "TypeError: Cannot read properties of undefined (reading 'x')\n    at thrower (lib:1:47)\n"
              "    at Array.map (<anonymous>)\n    at each (lib:2:33)\n    at main:1:52|Error: e49\n"
              "    at main:1:159|Error: late\n    at a1 (main:1:279)\n    at async main:1:313");
}

/* ---- the order of evaluation: V8's, each case printed by Node 26 for the same files ---- */

static char g_log[1024];

/* p(x): x on the log */
static bool native_p(JsContext *ctx, JsValue this_val, const JsValue *args, int argc,
                     JsValue *result) {
    (void)this_val;
    char *s = value_to_cstr(ctx, argc > 0 ? args[0] : js_undefined());
    size_t n = strlen(g_log);
    snprintf(g_log + n, sizeof g_log - n, "%s%s", n ? "|" : "", s);
    free(s);
    *result = js_undefined();
    return true;
}

static void log_open(TestVm *tv) {
    static const uint16_t n_p[] = {'p'};
    js_register_native(tv->ctx, n_p, 1, native_p, NULL);
    g_log[0] = 0;
}

static void check_log(const char *label, const char *expected) {
    checks_run++;
    if (strcmp(g_log, expected) != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL %s\n  expected: %s\n  actual:   %s\n", label, expected, g_log);
    }
}

static JsValue eval_named(TestVm *tv, const char *spec) {
    size_t n;
    uint16_t *u = to_u16(spec, &n);
    JsValue p = js_eval_module(tv->ctx, u, n);
    free(u);
    return p;
}

/* a promise the modules wait for and main opens, and the turns everything else needs to come
 * to rest */
#define GATES \
    "const settle = async () => { for (let i = 0; i < 60; i++) await 0; };" \
    "globalThis.gate = new Promise((r) => { globalThis.open = r; });"

typedef struct {
    const char *name;
    struct {
        const char *name, *src;
    } files[8];
    const char *expected;
} EvalCase;

static const EvalCase k_eval_cases[] = {
    /* no job runs between two module bodies */
    {"micro",
     {
         {"a", "Promise.resolve().then(() => p('a micro')); p('a');"},
         {"b", "p('b');"},
         {"main", "import './a'; import './b'; p('main');"},
     },
     "a|b|main|a micro"},
    /* a module with a top-level await beside one without */
    {"sib",
     {
         {"t", "p('t1'); await 0; p('t2'); await 0; p('t3');"},
         {"s", "Promise.resolve().then(() => p('s micro')); p('s');"},
         {"main", "import './t'; import './s'; p('main');"},
     },
     "t1|s|t2|s micro|t3|main"},
    /* modules waiting for one top-level await, and one that does not wait */
    {"sib2",
     {
         {"t", "p('t1'); await 0; p('t2');"},
         {"x", "import './t'; p('x');"},
         {"y", "import './t'; p('y1'); await 0; p('y2');"},
         {"z", "p('z');"},
         {"w", "import './t'; p('w');"},
         {"main", "import './x'; import './y'; import './z'; import './w'; p('main');"},
     },
     "t1|z|t2|x|y1|w|y2|main"},
    /* an async module that reaches no await still takes a turn */
    {"noawait",
     {
         {"t", "p('t1'); if (!p) await 0; p('t2');"},
         {"s", "Promise.resolve().then(() => p('s micro')); p('s');"},
         {"m2", "import './t'; import './s'; p('m2');"},
         {"main", "import './m2'; p('main');"},
     },
     "t1|t2|s|m2|main|s micro"},
    /* waiting modules run in the order their evaluation was started */
    {"order",
     {
         {"t", "p('t1'); await 0; p('t2');"},
         {"p1", "import './t'; p('p1a'); await 0; p('p1b');"},
         {"p2", "import './t'; p('p2');"},
         {"p3", "import './p1'; import './p2'; p('p3');"},
         {"q", "p('q');"},
         {"main", "import './p3'; import './p2'; import './q'; import './p1'; p('main');"},
     },
     "t1|q|t2|p1a|p2|p1b|p3|main"},
    /* top-level awaits at two depths of one graph */
    {"deep",
     {
         {"a", "p('a1'); await 0; p('a2');"},
         {"b", "import './a'; p('b');"},
         {"c", "import './b'; p('c1'); await 0; p('c2');"},
         {"d", "import './c'; import './a'; p('d');"},
         {"e", "p('e');"},
         {"main", "import './d'; import './e'; import './b'; p('main');"},
     },
     "a1|e|a2|b|c1|c2|d|main"},
    /* five modules waiting for one, some with awaits of their own */
    {"fan2",
     {
         {"t", "p('t1'); await globalThis.gate; p('t2');"},
         {"a", "import './t'; p('a1'); await 0; p('a2');"},
         {"b", "import './t'; p('b');"},
         {"c", "import './a'; import './b'; p('c1'); await 0; p('c2');"},
         {"d", "import './t'; Promise.resolve().then(() => p('d micro')); p('d');"},
         {"main",
          GATES
          "const all = []; for (const n of ['d', 'c', 'b', 'a']) { all.push(import('./' + n + "
          "'')); await settle(); } p('open'); open(); await Promise.all(all); p('main');"},
     },
     "t1|open|t2|d|a1|b|d micro|a2|c1|c2|main"},
    /* a second importer of a module parked on a top-level await waits for it */
    {"tla2",
     {
         {"x", "export const one = 1;"},
         {"t", "const ns = await import('./x'); await globalThis.gate; export const v = ns.one;"},
         {"u", "import { v } from './t'; export const got = v;"},
         {"main",
          GATES
          "const a = import('./t'); await settle(); const b = import('./u').then(ns => p('u: ' + "
          "ns.got), e => p('u failed: ' + e.message)); await settle(); p('open'); open(); await "
          "a; await b; p('main');"},
     },
     "open|u: 1|main"},
    /* two imports of one waiting module: one evaluation, one namespace */
    {"twice",
     {
         {"t", "p('t1'); await globalThis.gate; export const v = 3; p('t2');"},
         {"main",
          GATES
          "const a = import('./t').then(ns => (p('a ' + ns.v), ns)); await settle(); const b = "
          "import('./t').then(ns => (p('b ' + ns.v), ns)); await settle(); p('open'); open(); "
          "p((await a) === (await b));"},
     },
     "t1|open|t2|a 3|b 3|true"},
    /* a later graph over a waiting module: its other modules run at once, the rest when it is
     * done */
    {"wait2",
     {
         {"t", "p('t1'); await globalThis.gate; export const v = 9; p('t2');"},
         {"free", "p('free');"},
         {"u",
          "import './free'; import { v } from './t'; Promise.resolve().then(() => p('u micro')); "
          "p('u ' + v);"},
         {"w", "import { v } from './t'; p('w1 ' + v); await 0; p('w2');"},
         {"top", "import './u'; import './w'; p('top');"},
         {"main",
          GATES
          "const a = import('./t'); await settle(); const b = import('./top'); await settle(); "
          "p('open'); open(); await b; p((await a).v); p('main');"},
     },
     "t1|free|open|t2|u 9|w1 9|u micro|w2|top|9|main"},
    /* a second importer of a cycle whose root is waiting waits for the root */
    {"cyc",
     {
         {"t", "import './s'; p('t1'); await globalThis.gate; export const v = 1; p('t2');"},
         {"s", "import { v } from './t'; export const get = () => v; p('s');"},
         {"u", "import { get } from './s'; p('u ' + get());"},
         {"main",
          GATES
          "const a = import('./t'); await settle(); const b = import('./u').then(() => p('u "
          "ok'), e => p('u failed: ' + e.message)); await settle(); p('open'); open(); await a; "
          "await b; p('main');"},
     },
     "s|t1|open|t2|u 1|u ok|main"},
    /* a cycle with a top-level await, imported at three of its members */
    {"cyctla",
     {
         {"a", "import './b'; p('a');"},
         {"b", "import './a'; p('b1'); await globalThis.gate; p('b2');"},
         {"c", "import './a'; p('c');"},
         {"main",
          GATES
          "const x = import('./a').then(() => p('a done')); await settle(); const y = "
          "import('./c').then(() => p('c done')); await settle(); const z = "
          "import('./b').then(() => p('b done')); await settle(); p('open'); open(); await x; "
          "await y; await z; p('main');"},
     },
     "b1|open|b2|a|c|a done|b done|c done|main"},
    /* a cycle imported from outside, then at a member, while its root waits */
    {"cycwait",
     {
         {"t", "import './s'; p('t1'); await globalThis.gate; export const v = 7; p('t2');"},
         {"s", "import { v } from './t'; export const get = () => v; p('s');"},
         {"m2", "import './t'; p('m2');"},
         {"main",
          GATES
          "const a = import('./m2').then(() => p('m2 done')); await settle(); const b = "
          "import('./s').then(ns => p('s done ' + ns.get()), e => p('s failed ' + e.message)); "
          "await settle(); p('open'); open(); await a; await b;"},
     },
     "s|t1|open|t2|m2|s done 7|m2 done"},
    /* the same, the member first */
    {"cycwait2",
     {
         {"t", "import './s'; p('t1'); await globalThis.gate; export const v = 7; p('t2');"},
         {"s", "import { v } from './t'; export const get = () => v; p('s');"},
         {"m2", "import './t'; p('m2');"},
         {"main",
          GATES
          "const b = import('./s').then(ns => p('s done ' + ns.get()), e => p('s failed ' + "
          "e.message)); await settle(); const a = import('./m2').then(() => p('m2 done')); await "
          "settle(); const c = import('./t').then(ns => p('t done ' + ns.v)); await settle(); "
          "p('open'); open(); await a; await b; await c;"},
     },
     "t1|open|t2|s|m2|s done 7|t done 7|m2 done"},
    /* a graph over a module that failed: what is before it runs, and the error is the same
     * object */
    {"err1",
     {
         {"bad", "p('bad runs'); throw new Error('bad');"},
         {"first", "p('first runs');"},
         {"both", "import './first'; import './bad'; p('both runs');"},
         {"main",
          "try { await import('./bad'); } catch (e) { p('1: ' + e.message); }\ntry { await "
          "import('./both'); } catch (e) { p('2: ' + e.message); }\ntry { await import('./both'); "
          "} catch (e) { p('3: ' + e.message); }\nconst e1 = await import('./both').catch(e => "
          "e), e2 = await import('./bad').catch(e => e); p(e1 === e2);"},
     },
     "bad runs|1: bad|first runs|2: bad|3: bad|true"},
    /* the failed module deep in the graph: the modules after it do not run, and can run later */
    {"err2",
     {
         {"bad", "p('bad runs'); throw new Error('bad');"},
         {"first", "p('first runs');"},
         {"last", "p('last runs');"},
         {"mid", "import './bad'; p('mid runs');"},
         {"both", "import './first'; import './mid'; import './last'; p('both runs');"},
         {"main",
          "try { await import('./both'); } catch (e) { p('1: ' + e.message); }\ntry { await "
          "import('./mid'); } catch (e) { p('2: ' + e.message); }\ntry { await import('./last'); "
          "p('last ok'); } catch (e) { p('3: ' + e.message); }\ntry { await import('./both'); } "
          "catch (e) { p('4: ' + e.message); }"},
     },
     "first runs|bad runs|1: bad|2: bad|last runs|last ok|4: bad"},
    /* the failed module after a top-level await in the same graph */
    {"err3",
     {
         {"bad", "p('bad runs'); throw new Error('bad');"},
         {"t", "p('t1'); await 0; p('t2');"},
         {"first", "import './t'; p('first');"},
         {"g", "import './first'; import './bad'; p('g runs');"},
         {"main",
          "await import('./bad').catch(e => p('1: ' + e.message)); await import('./g').catch(e "
          "=> p('2: ' + e.message)); await import('./first').then(() => p('first ok')); await "
          "import('./g').catch(e => p('3: ' + e.message));"},
     },
     "bad runs|1: bad|t1|t2|first|2: bad|first ok|3: bad"},
    /* a module with a top-level await that throws before it */
    {"throwfirst",
     {
         {"t", "p('t1'); if (p) throw new Error('t early'); await 0;"},
         {"s", "p('s');"},
         {"m2", "import './t'; import './s'; p('m2');"},
         {"main",
          "await import('./m2').catch(e => p('1: ' + e.message)); await import('./s').then(() => "
          "p('s ok'));"},
     },
     "t1|s|1: t early|s ok"},
    /* a rejection after a top-level await reaches every module waiting above it */
    {"asyncerr",
     {
         {"t", "p('t1'); await globalThis.gate; p('t2'); throw new Error('t failed');"},
         {"par", "import './t'; p('par');"},
         {"gp", "import './par'; p('gp');"},
         {"main",
          GATES
          "const a = import('./gp').catch(e => p('a: ' + e.message)); await settle(); const b = "
          "import('./t').catch(e => p('b: ' + e.message)); await settle(); const c = "
          "import('./par').catch(e => p('c: ' + e.message)); await settle(); p('open'); open(); "
          "await a; await b; await c;\nawait import('./par').catch(e => p('again: ' + "
          "e.message));\nconst x = await import('./gp').catch(e => e), y = await "
          "import('./t').catch(e => e); p(x === y);"},
     },
     "t1|open|t2|b: t failed|c: t failed|a: t failed|again: t failed|true"},
    /* a body that throws beside a waiting module: the waiting module still completes */
    {"syncerr",
     {
         {"t", "p('t1'); await globalThis.gate; p('t2'); export const v = 5;"},
         {"bad", "p('bad runs'); throw new Error('bad');"},
         {"m2", "import './t'; import './bad'; p('m2 runs');"},
         {"m3", "import { v } from './t'; p('m3 ' + v);"},
         {"main",
          GATES
          "const a = import('./m2').catch(e => p('m2: ' + e.message)); await settle(); const b = "
          "import('./m3').then(() => p('m3 ok'), e => p('m3: ' + e.message)); await settle(); "
          "p('open'); open(); await a; await b; await import('./t'); p('t ok'); await "
          "import('./m2').catch(e => p('m2 again: ' + e.message));"},
     },
     "t1|bad runs|m2: bad|open|t2|m3 5|m3 ok|t ok|m2 again: bad"},
    /* the order a rejection reaches four importers in */
    {"rejorder",
     {
         {"t", "p('t1'); await globalThis.gate; throw new Error('boom');"},
         {"a", "import './t'; p('a');"},
         {"b", "import './t'; p('b');"},
         {"c", "import './a'; p('c');"},
         {"main",
          GATES
          "const w = n => import('./' + n + '').then(() => p(n + ' ok'), e => p(n + ': ' + "
          "e.message)); const all = []; for (const n of ['c', 'b', 'a', 't']) { all.push(w(n)); "
          "await settle(); } p('open'); open(); await Promise.all(all);"},
     },
     "t1|open|t: boom|a: boom|c: boom|b: boom"},
    /* a cycle over a module that throws: every member has the error */
    {"cycerr",
     {
         {"a", "import './b'; p('a');"},
         {"b", "import './a'; import './c'; p('b');"},
         {"c", "p('c'); throw new Error('c failed');"},
         {"main",
          "await import('./a').catch(e => p('1: ' + e.message)); await import('./b').catch(e => "
          "p('2: ' + e.message)); await import('./c').catch(e => p('3: ' + e.message));"},
     },
     "c|1: c failed|2: c failed|3: c failed"},
    /* a cycle whose root failed after its await, imported at another member (V8 aborts here:
     * what ECMA-262 says, the root's error) */
    {"cycerr2",
     {
         {"t", "import './s'; p('t1'); await 0; throw new Error('t failed');"},
         {"s", "import './t'; p('s');"},
         {"m2", "import './t'; p('m2');"},
         {"main",
          "await import('./m2').catch(e => p('1: ' + e.message)); await import('./s').then(() => "
          "p('s ok'), e => p('2: ' + e.message)); await import('./t').then(() => p('t ok'), e => "
          "p('3: ' + e.message));"},
     },
     "s|t1|1: t failed|2: t failed|3: t failed"},
};

static void test_evaluation_order(void) {
    for (size_t c = 0; c < sizeof k_eval_cases / sizeof k_eval_cases[0]; c++) {
        const EvalCase *ec = &k_eval_cases[c];
        mods_reset();
        for (int i = 0; i < 8 && ec->files[i].name; i++)
            mod_add(ec->files[i].name, ec->files[i].src);
        TestVm tv;
        tv_open(&tv, canonicalize);
        log_open(&tv);
        JsValue p = eval_named(&tv, "main");
        js_gc_protect(tv.vm, &p);
        while (js_has_pending_jobs(tv.ctx))
            js_run_jobs(tv.ctx);
        char label[96];
        snprintf(label, sizeof label, "evaluation order: %s", ec->name);
        check_log(label, ec->expected);
        check(js_promise_state(p) == 1, label);
        js_gc_unprotect(tv.vm, &p);
        tv_close(&tv, label);
    }
    mods_reset();
}

/* The same through the host's calls, a turn of the host between them: a module that failed is in
 * a later graph, and a module waiting for the host is imported again. */
static void test_evaluation_across_turns(void) {
    /* the graph over a failed module runs what comes before it, and fails with its error */
    mods_reset();
    mod_add("bad", "p('bad'); throw new Error('boom');");
    mod_add("first", "p('first');");
    mod_add("last", "p('last');");
    mod_add("both", "import 'first'; import 'bad'; import 'last'; p('both');");
    {
        TestVm tv;
        tv_open(&tv, NULL);
        log_open(&tv);
        JsValue p1 = eval_named(&tv, "bad");
        js_gc_protect(tv.vm, &p1);
        JsValue p2 = eval_named(&tv, "both");
        js_gc_protect(tv.vm, &p2);
        JsValue p3 = eval_named(&tv, "both");
        js_gc_protect(tv.vm, &p3);
        check_log("failed module: the modules before it ran once, it and the rest did not",
                  "bad|first");
        check(js_promise_state(p1) == 2 && js_promise_state(p2) == 2 && js_promise_state(p3) == 2,
              "failed module: each graph over it is rejected");
        check(js_promise_result(p1).bits == js_promise_result(p2).bits &&
                  js_promise_result(p2).bits == js_promise_result(p3).bits,
              "failed module: with the one error object");
        JsValue p4 = eval_named(&tv, "last");
        js_gc_protect(tv.vm, &p4);
        check(js_promise_state(p4) == 1, "failed module: a module after it in the graph is sound");
        check_log("failed module: and runs when it is asked for", "bad|first|last");
        js_gc_unprotect(tv.vm, &p4);
        js_gc_unprotect(tv.vm, &p3);
        js_gc_unprotect(tv.vm, &p2);
        js_gc_unprotect(tv.vm, &p1);
        tv_close(&tv, "failed module in a later graph");
    }

    /* a module waiting for the host: its importers wait with it, whenever they come */
    for (int reject = 0; reject < 2; reject++) {
        mods_reset();
        mod_add("slow", "p('slow1'); export const v = await defer('late'); p('slow2');");
        mod_add("u", "import { v } from 'slow'; p('u ' + v); export const got = v;");
        TestVm tv;
        tv_open(&tv, NULL);
        log_open(&tv);
        static const uint16_t n_defer[] = {'d', 'e', 'f', 'e', 'r'};
        static const uint16_t n_slow[] = {'s', 'l', 'o', 'w'};
        js_register_native(tv.ctx, n_defer, 5, native_defer, NULL);
        g_defer_count = 0;
        g_defer_promise = js_undefined();
        g_defer_value = js_undefined();
        js_gc_protect(tv.vm, &g_defer_promise);
        js_gc_protect(tv.vm, &g_defer_value);
        JsValue p1 = eval_named(&tv, "slow");
        js_gc_protect(tv.vm, &p1);
        check(js_promise_state(p1) == 0 && !js_has_pending_jobs(tv.ctx),
              "waiting module: pending, and nothing left to run");
        JsValue p2 = eval_named(&tv, "u");
        js_gc_protect(tv.vm, &p2);
        JsValue p3 = eval_named(&tv, "slow");
        js_gc_protect(tv.vm, &p3);
        check(js_promise_state(p2) == 0 && js_promise_state(p3) == 0 &&
                  !js_has_pending_jobs(tv.ctx),
              "waiting module: a second importer and a second import of it wait");
        check_log("waiting module: the importer's body has not run", "slow1");
        check(g_defer_count == 1, "waiting module: its body ran once");
        check(!js_context_evict_module(tv.ctx, n_slow, 4),
              "waiting module: cannot be evicted while it waits");
        js_gc_collect(tv.vm);
        if (reject)
            js_reject(tv.ctx, g_defer_promise, g_defer_value);
        else
            js_resolve(tv.ctx, g_defer_promise, g_defer_value);
        js_run_jobs(tv.ctx);
        int want = reject ? 2 : 1;
        check(js_promise_state(p1) == want && js_promise_state(p2) == want &&
                  js_promise_state(p3) == want,
              "waiting module: all three settle with it");
        if (reject) {
            check_log("waiting module rejected: neither body went on", "slow1");
            check(js_promise_result(p1).bits == g_defer_value.bits &&
                      js_promise_result(p2).bits == g_defer_value.bits &&
                      js_promise_result(p3).bits == g_defer_value.bits,
                  "waiting module rejected: the reason reaches every importer");
            JsValue p4 = eval_named(&tv, "u");
            js_gc_protect(tv.vm, &p4);
            check(js_promise_state(p4) == 2 && js_promise_result(p4).bits == g_defer_value.bits,
                  "waiting module rejected: and a later import of its importer");
            js_gc_unprotect(tv.vm, &p4);
        } else {
            check_log("waiting module: then its importer ran", "slow1|slow2|u late");
            check(js_promise_result(p1).bits == js_promise_result(p3).bits,
                  "waiting module: one namespace for both imports");
            char *got = export_to_cstr(tv.ctx, tv.vm, js_promise_result(p2), "got");
            check(strcmp(got, "late") == 0, "waiting module: the importer saw the awaited value");
            free(got);
            check(js_context_evict_module(tv.ctx, n_slow, 4),
                  "waiting module: can be evicted once it is done");
        }
        js_gc_unprotect(tv.vm, &p3);
        js_gc_unprotect(tv.vm, &p2);
        js_gc_unprotect(tv.vm, &p1);
        js_gc_unprotect(tv.vm, &g_defer_value);
        js_gc_unprotect(tv.vm, &g_defer_promise);
        tv_close(&tv, reject ? "waiting module rejected" : "waiting module");
    }
    mods_reset();
}

/* drain(): the host runs the job queue from inside a native, in the middle of a module body. V8
 * has no such state (its embedders do not run microtasks under an evaluation), so what is checked
 * is that every body still runs once, in an order that respects the imports. */
static bool native_drain(JsContext *ctx, JsValue this_val, const JsValue *args, int argc,
                         JsValue *result) {
    (void)this_val;
    (void)args;
    (void)argc;
    js_run_jobs(ctx);
    *result = js_undefined();
    return true;
}

static int log_count(const char *entry) {
    int n = 0;
    size_t el = strlen(entry);
    for (const char *s = g_log; *s;) {
        const char *e = strchr(s, '|');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l == el && memcmp(s, entry, el) == 0)
            n++;
        s += l + (e ? 1 : 0);
    }
    return n;
}

static void drain_case(const char *label, const char *expected) {
    TestVm tv;
    tv_open(&tv, NULL);
    log_open(&tv);
    static const uint16_t n_drain[] = {'d', 'r', 'a', 'i', 'n'};
    js_register_native(tv.ctx, n_drain, 5, native_drain, NULL);
    JsValue p = eval_named(&tv, "main");
    js_gc_protect(tv.vm, &p);
    while (js_has_pending_jobs(tv.ctx))
        js_run_jobs(tv.ctx);
    check(js_promise_state(p) == 1, label);
    check_log(label, expected);
    js_gc_unprotect(tv.vm, &p);
    tv_close(&tv, label);
    mods_reset();
}

static void test_jobs_inside_a_body(void) {
    /* a module the walk is waiting on finishes inside the drain: its importer runs when the
     * walk gets back to it */
    mods_reset();
    mod_add("t", "p('t1'); await 0; p('t2');");
    mod_add("a", "drain(); p('a');");
    mod_add("main", "import 't'; import 'a'; p('main');");
    drain_case("jobs inside a body: a waiting dependency completes", "t1|t2|a|main");

    /* and fails inside the drain */
    mods_reset();
    mod_add("t", "p('t1'); await 0; throw new Error('late');");
    mod_add("a", "drain(); p('a');");
    mod_add("mid", "import 't'; import 'a'; p('mid');");
    mod_add("main", "p(await import('mid').then(() => 'mid ok', (e) => 'mid: ' + e.message));");
    drain_case("jobs inside a body: a waiting dependency fails", "t1|a|mid: late");

    /* import() of a module on the path, and of one the walk has not reached, from a job: the
     * first is given its namespace as it is, main's bindings not yet initialized */
    mods_reset();
    mod_add("a", "import('main').then((ns) => p('main ns ' + typeof ns.f), (e) => p('failed'));"
                 "import('b').then((ns) => p('b ns ' + ns.k), (e) => p('failed'));"
                 "drain(); p('a');");
    mod_add("b", "export const k = 'kb'; p('b');");
    mod_add("main", "import 'a'; import 'b'; export function f() {} p('main');");
    {
        TestVm tv;
        tv_open(&tv, NULL);
        log_open(&tv);
        static const uint16_t n_drain[] = {'d', 'r', 'a', 'i', 'n'};
        js_register_native(tv.ctx, n_drain, 5, native_drain, NULL);
        JsValue p = eval_named(&tv, "main");
        js_gc_protect(tv.vm, &p);
        while (js_has_pending_jobs(tv.ctx))
            js_run_jobs(tv.ctx);
        check(js_promise_state(p) == 1, "jobs inside a body: import() from a job, root fulfilled");
        check(log_count("a") == 1 && log_count("b") == 1 && log_count("main") == 1 &&
                  log_count("b ns kb") == 1 && log_count("main ns function") == 1 &&
                  log_count("failed") == 0,
              "jobs inside a body: import() from a job, every body once");
        js_gc_unprotect(tv.vm, &p);
        tv_close(&tv, "jobs inside a body: import() from a job");
    }

    /* a graph that fails inside the drain, over the module that is draining */
    mods_reset();
    mod_add("x", "import 'main'; p('x'); throw new Error('boom');");
    mod_add("a", "import('x').catch((e) => p('x: ' + e.message)); drain(); p('a');");
    mod_add("main", "import 'a'; p('main');");
    drain_case("jobs inside a body: another graph fails over this one", "x|x: boom|a|main");
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
    test_reexport_live();
    test_reexport_ns();
    test_barrel();
    test_async_module();
    test_errors();
    test_cross_turn_rounds();
    test_synthetic();
    test_mixed_graph();
    test_load_dedup();
    test_reject_propagation();
    test_canonicalization();
    test_tla_dep_cross_turn();
    test_source_modules_gated();
    test_entry_return();
    test_check_module();
    test_declaration_rules();
    test_functions_at_instantiation();
    test_error_stack();
    test_evaluation_order();
    test_evaluation_across_turns();
    test_jobs_inside_a_body();
    /* parked bodies, cycles and top-level await again with the collector running at every
     * safe point: the rooting of a fiber between instantiation and evaluation */
    g_gc_stress = true;
    test_cycle();
    test_async_module();
    test_tla_dep_cross_turn();
    test_functions_at_instantiation();
    test_error_stack();
    test_evaluation_order();
    test_evaluation_across_turns();
    test_jobs_inside_a_body();
    g_gc_stress = false;
    mods_reset();
    if (checks_failed) {
        fprintf(stderr, "%d/%d module checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d module checks passed\n", checks_run);
    return 0;
}
