/*
 * Dynamic import() tests. The same in-memory async-loader harness as
 * test_modules.c, exercising import() from module top level (awaited),
 * promise chaining off the returned value, rejection for unknown
 * specifiers, identity/dedup against static imports of the same module,
 * and — the load-bearing case — a cross-turn import() from a PLAIN
 * (non-module) script, where the referrer is empty and the loader promise
 * settles on a later host turn with a quiescent job queue in between.
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

static uint16_t *to_u16(const char *s, size_t *len) {
    size_t n = strlen(s);
    uint16_t *u = malloc((n + 1) * sizeof(uint16_t));
    for (size_t i = 0; i < n; i++)
        u[i] = (uint16_t)(unsigned char)s[i];
    u[n] = 0;
    *len = n;
    return u;
}

/* ---- module table + loader (immediate or deferred) ---- */

#define MAX_MODS 8
typedef struct {
    char names[MAX_MODS][32];
    uint16_t *sources[MAX_MODS];
    size_t source_lens[MAX_MODS];
    bool deferred[MAX_MODS];
    int loads[MAX_MODS];
    int count;
} ModTable;
static ModTable g_mods;

static void mod_add2(const char *name, const char *src, bool deferred) {
    int i = g_mods.count++;
    snprintf(g_mods.names[i], 32, "%s", name);
    g_mods.sources[i] = to_u16(src, &g_mods.source_lens[i]);
    g_mods.deferred[i] = deferred;
    g_mods.loads[i] = 0;
}
static void mod_add(const char *name, const char *src) {
    mod_add2(name, src, false);
}

static void mods_reset(void) {
    for (int i = 0; i < g_mods.count; i++)
        free(g_mods.sources[i]);
    g_mods.count = 0;
}

static int mod_find(const char *name) {
    for (int i = 0; i < g_mods.count; i++)
        if (strcmp(g_mods.names[i], name) == 0)
            return i;
    return -1;
}

typedef struct {
    JsVm *vm;
    JsValue promises[MAX_MODS];
    int table_idx[MAX_MODS];
    int count;
} Pending;
static Pending g_pending;

static void settle_all(JsContext *ctx) {
    while (g_pending.count > 0) {
        int i = --g_pending.count;
        int t = g_pending.table_idx[i];
        JsValue src = js_atom(g_pending.vm, g_mods.sources[t], g_mods.source_lens[t]);
        js_resolve(ctx, g_pending.promises[i], src);
        js_gc_unprotect(g_pending.vm, &g_pending.promises[i]);
        js_run_jobs(ctx);
    }
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
    JsValue p = js_promise_new(ctx);
    if (idx < 0) {
        static const uint16_t msg[] = {'m', 'o', 'd', 'u', 'l', 'e', ' ',
                                       'n', 'o', 't', ' ', 'f', 'o', 'u', 'n', 'd'};
        JsValue reason = js_atom(vm, msg, 16);
        js_gc_protect(vm, &reason);
        js_reject(ctx, p, reason);
        js_gc_unprotect(vm, &reason);
        return p;
    }
    g_mods.loads[idx]++;
    if (g_mods.deferred[idx]) {
        int i = g_pending.count++;
        g_pending.vm = vm;
        g_pending.promises[i] = p;
        g_pending.table_idx[i] = idx;
        js_gc_protect(vm, &g_pending.promises[i]);
        return p;
    }
    JsValue src = js_atom(vm, g_mods.sources[idx], g_mods.source_lens[idx]);
    js_gc_protect(vm, &src);
    js_resolve(ctx, p, src);
    js_gc_unprotect(vm, &src);
    return p;
}

/* ---- harness ---- */

static char *value_to_cstr(JsContext *ctx, JsVm *vm, JsValue v) {
    js_gc_protect(vm, &v);
    JsValue s = js_to_string(ctx, v);
    js_gc_unprotect(vm, &v);
    size_t sl;
    const uint16_t *su = js_string_units(s, &sl);
    char *out = malloc(sl + 1);
    for (size_t i = 0; i < sl; i++)
        out[i] = su && su[i] < 128 ? (char)su[i] : '?';
    out[sl] = 0;
    return out;
}

/* Evaluates root module 'main' (in the table), settles deferred loads, and
 * returns export `exp` (or the rejection reason) as a C string. */
static char *eval_main_export(const char *exp, bool *ok, long *leak) {
    CountAlloc ca = {0, 0};
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    g_pending.count = 0;
    js_set_module_loader(ctx, loader, NULL, vm);
    js_enable_source_modules(ctx);
    static const uint16_t spec[] = {'m', 'a', 'i', 'n'};
    JsValue p = js_eval_module(ctx, spec, 4);
    js_gc_protect(vm, &p);
    settle_all(ctx);
    int st = js_promise_state(p);
    *ok = st == 1;
    char *out;
    if (st == 1) {
        size_t elen;
        uint16_t *eu = to_u16(exp, &elen);
        JsValue v = js_module_get_export(ctx, js_promise_result(p), eu, elen);
        free(eu);
        out = value_to_cstr(ctx, vm, v);
    } else {
        out = value_to_cstr(ctx, vm, js_promise_result(p));
    }
    js_gc_unprotect(vm, &p);
    js_vm_free(vm);
    *leak = ca.net_bytes + ca.live_allocs;
    return out;
}

static void eq_main(const char *root_src, const char *exp, const char *expected) {
    mod_add("main", root_src);
    bool ok;
    long leak;
    char *out = eval_main_export(exp, &ok, &leak);
    checks_run += 2;
    if (!ok || strcmp(out, expected) != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL %s\n  root: %s\n  expected: %s\n  actual: %s%s\n",
                exp, root_src, expected, out, ok ? "" : " (error)");
    }
    if (leak != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL leak: %s\n", root_src);
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

/* ---- module-context import() ---- */

static void test_module_import(void) {
    /* awaited namespace */
    mod_add("lib", "export const value = 'v';");
    eq_main("const ns = await import('lib'); export const out = ns.value + '!';", "out", "v!");

    /* .then() chaining off the returned promise */
    mod_add("lib", "export const value = 'chained';");
    eq_main("export const out = await import('lib').then(ns => ns.value);", "out", "chained");

    /* default export through the namespace */
    mod_add("lib", "export default 41; export const extra = 1;");
    eq_main("const ns = await import('lib');"
            "export const out = ns.default + ns.extra;",
            "out", "42");

    /* rejection: unknown specifier surfaces the loader's reason */
    eq_main("export const out = await import('missing').catch(e => 'caught:' + e);",
            "out", "caught:module not found");

    /* the options argument parses, evaluates, and is ignored */
    mod_add("lib", "export const value = 'opt';");
    eq_main("let evald = '';"
            "export const out = await import('lib', (evald = 'x', { assert: { type: 'js' } }))"
            ".then(ns => ns.value + evald);",
            "out", "optx");

    /* specifier is ToString'd */
    mod_add("7", "export const value = 'seven';");
    eq_main("export const out = await import(7).then(ns => ns.value);", "out", "seven");
}

/* static + dynamic import of the same specifier: one module, one load */
static void test_identity_dedup(void) {
    mod_add("counter",
            "export let n = 0;"
            "export function inc() { n = n + 1; }"
            "export function get() { return n; }");
    mod_add("main",
            "import { inc } from 'counter';"
            "inc();"
            "const ns = await import('counter');"
            "ns.inc();"
            "export const out = ns.get();");
    bool ok;
    long leak;
    char *out = eval_main_export("out", &ok, &leak);
    check(ok && strcmp(out, "2") == 0, "identity: dynamic ns shares the static module's state");
    check(g_mods.loads[mod_find("counter")] == 1, "identity: loader fired once for counter");
    check(leak == 0, "identity: no leak");
    free(out);
    mods_reset();
}

/* ---- plain-script import() (empty referrer), genuinely cross-turn ---- */

static void test_plain_script_cross_turn(void) {
    mod_add2("slow", "export const v = 'arrived';", true /* deferred */);

    CountAlloc ca = {0, 0};
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    g_pending.count = 0;
    js_set_module_loader(ctx, loader, NULL, vm);
    js_enable_source_modules(ctx);

    /* predefine OUT so strict-mode assignment works */
    static const uint16_t k_out[] = {'O', 'U', 'T'};
    JsValue outkey = js_atom(vm, k_out, 3);
    js_gc_protect(vm, &outkey);
    js_object_set(vm, js_context_globals(ctx), outkey, js_undefined());

    const char *src = "import('slow').then(ns => OUT = ns.v); 'kicked';";
    size_t len;
    uint16_t *u = to_u16(src, &len);
    const char *em;
    uint32_t ep;
    JsValue fn = js_compile_module(ctx, u, len, &em, &ep);
    free(u);
    js_gc_protect(vm, &fn);
    JsValue p = js_run_module(ctx, fn);
    js_gc_protect(vm, &p);

    /* the script itself completed; the load is parked on the host */
    check(js_promise_state(p) == 1 && g_pending.count == 1 && !js_has_pending_jobs(ctx),
          "plain-script: script done, load pending, queue quiescent");
    JsValue outv = js_object_get(vm, js_context_globals(ctx), outkey);
    check(js_is_undefined(outv), "plain-script: OUT unset before the host settles");

    js_gc_collect(vm); /* the parked load survives a full GC */
    settle_all(ctx);

    outv = js_object_get(vm, js_context_globals(ctx), outkey);
    char *out = value_to_cstr(ctx, vm, outv);
    check(strcmp(out, "arrived") == 0, "plain-script: import() resolved cross-turn");
    check(g_mods.loads[mod_find("slow")] == 1, "plain-script: one load");
    free(out);

    js_gc_unprotect(vm, &p);
    js_gc_unprotect(vm, &fn);
    js_gc_unprotect(vm, &outkey);
    js_vm_free(vm);
    check(ca.net_bytes == 0 && ca.live_allocs == 0, "plain-script: no leak");
    mods_reset();
}

/* import() inside a TLA module body whose load itself needs a fresh turn */
static void test_module_cross_turn(void) {
    mod_add2("slow", "export const v = 'late';", true);
    mod_add("main", "const ns = await import('slow'); export const out = ns.v + '!';");

    CountAlloc ca = {0, 0};
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    g_pending.count = 0;
    js_set_module_loader(ctx, loader, NULL, vm);
    js_enable_source_modules(ctx);
    static const uint16_t spec[] = {'m', 'a', 'i', 'n'};
    JsValue p = js_eval_module(ctx, spec, 4);
    js_gc_protect(vm, &p);

    check(js_promise_state(p) == 0 && g_pending.count == 1 && !js_has_pending_jobs(ctx),
          "module cross-turn: root pending on the parked dynamic import");
    settle_all(ctx);
    check(js_promise_state(p) == 1, "module cross-turn: fulfilled after settle");
    if (js_promise_state(p) == 1) {
        static const uint16_t eout[] = {'o', 'u', 't'};
        char *out = value_to_cstr(ctx, vm, js_module_get_export(ctx, js_promise_result(p), eout, 3));
        check(strcmp(out, "late!") == 0, "module cross-turn: awaited value observed");
        free(out);
    }
    js_gc_unprotect(vm, &p);
    js_vm_free(vm);
    check(ca.net_bytes == 0 && ca.live_allocs == 0, "module cross-turn: no leak");
    mods_reset();
}

int main(void) {
    test_module_import();
    test_identity_dedup();
    test_plain_script_cross_turn();
    test_module_cross_turn();
    mods_reset();
    if (checks_failed) {
        fprintf(stderr, "%d/%d dynamic-import checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d dynamic-import checks passed\n", checks_run);
    return 0;
}
