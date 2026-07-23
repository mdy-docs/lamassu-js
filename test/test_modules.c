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

static void tv_open(TestVm *tv, JsModuleCanonicalizer canon) {
    tv->ca.net_bytes = 0;
    tv->ca.live_allocs = 0;
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &tv->ca};
    tv->vm = js_vm_new(&cfg);
    tv->ctx = js_context_new(tv->vm);
    g_pending.count = 0;
    js_set_module_loader(tv->ctx, loader, canon, tv->vm);
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

    static const uint16_t spec[] = {'m', 'a', 'i', 'n'};
    JsValue p = js_eval_module(tv.ctx, spec, 4);
    js_gc_protect(tv.vm, &p);
    js_gc_protect(tv.vm, &g_defer_promise);
    js_gc_protect(tv.vm, &g_defer_value);

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
    mods_reset();
    if (checks_failed) {
        fprintf(stderr, "%d/%d module checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d module checks passed\n", checks_run);
    return 0;
}
