/*
 * Module-bytecode tests (phase 8, modules). A tiny in-memory module table
 * holds each module's source; every module is compiled independently to a
 * bytecode buffer (js_bytecode_compile_module), then the graph is loaded,
 * linked, and evaluated from bytecode through a module loader fulfilling
 * with js_bytecode_value buffers — and the result is compared against
 * evaluating the same graph from source. Also checks kind confusion (script
 * vs module) is rejected and that tampered module bytecode is rejected
 * without crashing (ASan/UBSan via the _asan build).
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

/* module table: name -> (widened) source; plus each module's compiled bytecode */
#define MAX_MODS 16
typedef struct {
    char names[MAX_MODS][32];
    uint16_t *src[MAX_MODS];
    size_t src_len[MAX_MODS];
    uint8_t *bc[MAX_MODS];
    size_t bc_len[MAX_MODS];
    int count;
} ModTable;
static ModTable g_mods;

static void mod_add(const char *name, const char *src) {
    int i = g_mods.count++;
    snprintf(g_mods.names[i], 32, "%s", name);
    g_mods.src[i] = to_u16(src, &g_mods.src_len[i]);
    g_mods.bc[i] = NULL;
}

static void mods_reset(void) {
    for (int i = 0; i < g_mods.count; i++) {
        free(g_mods.src[i]);
        free(g_mods.bc[i]);
    }
    g_mods.count = 0;
}

static int mod_find(const uint16_t *spec, size_t spec_len) {
    char name[64];
    size_t n = spec_len < 63 ? spec_len : 63;
    for (size_t i = 0; i < n; i++)
        name[i] = (char)spec[i];
    name[n] = 0;
    for (int i = 0; i < g_mods.count; i++)
        if (strcmp(g_mods.names[i], name) == 0)
            return i;
    return -1;
}

/* ---- loaders ---- */

static JsValue fulfilled_with(JsContext *ctx, JsVm *vm, JsValue v) {
    js_gc_protect(vm, &v);
    JsValue p = js_promise_new(ctx);
    js_resolve(ctx, p, v);
    js_gc_unprotect(vm, &v);
    return p;
}

static JsValue rejected_not_found(JsContext *ctx, JsVm *vm) {
    static const uint16_t msg[] = {'m', 'o', 'd', 'u', 'l', 'e', ' ',
                                   'n', 'o', 't', ' ', 'f', 'o', 'u', 'n', 'd'};
    JsValue reason = js_atom(vm, msg, 16);
    js_gc_protect(vm, &reason);
    JsValue p = js_promise_new(ctx);
    js_reject(ctx, p, reason);
    js_gc_unprotect(vm, &reason);
    return p;
}

/* baseline: fulfill with source text */
static JsValue src_loader(void *ud, JsContext *ctx, const uint16_t *spec, size_t spec_len,
                          const uint16_t *ref, size_t ref_len) {
    JsVm *vm = ud;
    (void)ref;
    (void)ref_len;
    int i = mod_find(spec, spec_len);
    if (i < 0)
        return rejected_not_found(ctx, vm);
    return fulfilled_with(ctx, vm, js_atom(vm, g_mods.src[i], g_mods.src_len[i]));
}

/* bytecode: fulfill with js_bytecode_value buffers; the tamper sweep swaps
 * in a mutated buffer for 'main' via the override */
static const uint8_t *g_override_bc;
static size_t g_override_len;

static JsValue bc_loader(void *ud, JsContext *ctx, const uint16_t *spec, size_t spec_len,
                         const uint16_t *ref, size_t ref_len) {
    JsVm *vm = ud;
    (void)ref;
    (void)ref_len;
    if (g_override_bc && spec_len == 4 && spec[0] == 'm' && spec[1] == 'a' &&
        spec[2] == 'i' && spec[3] == 'n')
        return fulfilled_with(ctx, vm, js_bytecode_value(ctx, g_override_bc, g_override_len));
    int i = mod_find(spec, spec_len);
    if (i < 0 || !g_mods.bc[i])
        return rejected_not_found(ctx, vm);
    return fulfilled_with(ctx, vm, js_bytecode_value(ctx, g_mods.bc[i], g_mods.bc_len[i]));
}

/* Compiles every registered module to its own bytecode buffer (fresh VM each,
 * mirroring independent per-page compilation). Returns false on any failure. */
static bool compile_all(void) {
    for (int i = 0; i < g_mods.count; i++) {
        CountAlloc ca = {0, 0};
        JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca};
        JsVm *vm = js_vm_new(&cfg);
        JsContext *ctx = js_context_new(vm);
        size_t nlen;
        uint16_t *nu = to_u16(g_mods.names[i], &nlen);
        uint8_t *out;
        size_t out_len;
        const char *em;
        uint32_t ep;
        bool ok = js_bytecode_compile_module(ctx, nu, nlen, g_mods.src[i], g_mods.src_len[i],
                                             &out, &out_len, &em, &ep);
        if (ok) {
            g_mods.bc[i] = malloc(out_len);
            memcpy(g_mods.bc[i], out, out_len);
            g_mods.bc_len[i] = out_len;
            js_bytecode_free(ctx, out, out_len);
        } else {
            fprintf(stderr, "compile_module(%s) failed: %s\n", g_mods.names[i], em);
        }
        free(nu);
        js_vm_free(vm);
        if (!ok)
            return false;
    }
    return true;
}

/* Evaluates root `name` through `load`, reads export `exp`, returns ToString. */
static char *eval_with(JsModuleLoader load, const char *name, const char *exp, bool *ok) {
    CountAlloc ca = {0, 0};
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    js_set_module_loader(ctx, load, NULL, vm);
    js_enable_source_modules(ctx);
    size_t nlen;
    uint16_t *nu = to_u16(name, &nlen);
    JsValue p = js_eval_module(ctx, nu, nlen);
    js_gc_protect(vm, &p);
    int st = js_promise_state(p);
    *ok = st == 1;
    char *out;
    JsValue result = js_promise_result(p);
    if (st != 1) {
        JsValue s = js_to_string(ctx, result);
        size_t sl;
        const uint16_t *sv = js_string_units(s, &sl);
        out = malloc(sl + 1);
        for (size_t k = 0; k < sl; k++)
            out[k] = sv && sv[k] < 128 ? (char)sv[k] : '?';
        out[sl] = 0;
    } else {
        size_t elen;
        uint16_t *eu = to_u16(exp, &elen);
        JsValue v = js_module_get_export(ctx, result, eu, elen);
        free(eu);
        js_gc_protect(vm, &v);
        JsValue s = js_to_string(ctx, v);
        size_t sl;
        const uint16_t *sv = js_string_units(s, &sl);
        out = malloc(sl + 1);
        for (size_t k = 0; k < sl; k++)
            out[k] = sv && sv[k] < 128 ? (char)sv[k] : '?';
        out[sl] = 0;
        js_gc_unprotect(vm, &v);
    }
    js_gc_unprotect(vm, &p);
    free(nu);
    js_vm_free(vm);
    checks_run++;
    if (ca.net_bytes != 0 || ca.live_allocs != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL leak: %s (net=%ld allocs=%ld)\n", name, ca.net_bytes,
                ca.live_allocs);
    }
    return out;
}

/*
 * The core assertion: bytecode evaluation of `root` reads `exp` == `expected`,
 * AND agrees with source evaluation of the same graph. Compiles the graph to
 * bytecode first. Caller has registered modules; this resets the table after.
 */
static void expect(const char *root, const char *exp, const char *expected) {
    checks_run++;
    if (!compile_all()) {
        checks_failed++;
        fprintf(stderr, "FAIL(compile) root=%s\n", root);
        mods_reset();
        return;
    }
    bool sok, bok;
    char *s = eval_with(src_loader, root, exp, &sok);
    char *b = eval_with(bc_loader, root, exp, &bok);
    if (!sok || !bok || strcmp(s, expected) != 0 || strcmp(s, b) != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL root=%s.%s\n  expected: %s\n  source:   %s%s\n  bytecode: %s%s\n",
                root, exp, expected, s, sok ? "" : " (err)", b, bok ? "" : " (err)");
    }
    free(s);
    free(b);
    mods_reset();
}

static void test_graphs(void) {
    /* basic import/export */
    mod_add("util", "export const g = 'hi'; export function up(s){ return s.toUpperCase(); }");
    mod_add("main", "import { g, up } from 'util'; export const r = up(g);");
    expect("main", "r", "HI");

    /* live bindings: a counter mutated in the exporter is seen by the importer */
    mod_add("counter", "let n = 0; export function tick(){ return ++n; }");
    mod_add("main", "import { tick } from 'counter'; export const r = '' + tick() + tick() + tick();");
    expect("main", "r", "123");

    /* default export/import */
    mod_add("d", "export default 42;");
    mod_add("main", "import x from 'd'; export const r = x + 1;");
    expect("main", "r", "43");

    /* namespace import */
    mod_add("ns", "export const a = 1; export const b = 2;");
    mod_add("main", "import * as m from 'ns'; export const r = m.a + m.b;");
    expect("main", "r", "3");

    /* diamond: main -> {left,right} -> base (base evaluated once) */
    mod_add("base", "export let hits = 0; export function bump(){ return ++hits; }");
    mod_add("left", "import { bump } from 'base'; export const L = bump();");
    mod_add("right", "import { bump } from 'base'; export const R = bump();");
    mod_add("main", "import { L } from 'left'; import { R } from 'right'; export const r = '' + L + R;");
    expect("main", "r", "12");

    /* cycle with hoisted functions (mutual recursion across modules) */
    mod_add("even", "import { odd } from 'odd'; export function even(n){ return n === 0 ? true : odd(n-1); }");
    mod_add("odd", "import { even } from 'even'; export function odd(n){ return n === 0 ? false : even(n-1); }");
    mod_add("main", "import { even } from 'even'; export const r = '' + even(10) + even(7);");
    expect("main", "r", "truefalse");

    /* export * from (re-export all) */
    mod_add("inner", "export const a = 'A'; export const b = 'B';");
    mod_add("mid", "export * from 'inner';");
    mod_add("main", "import { a, b } from 'mid'; export const r = a + b;");
    expect("main", "r", "AB");

    /* named re-export: export { x as y } from */
    mod_add("inner", "export const x = 'X';");
    mod_add("mid", "export { x as y } from 'inner';");
    mod_add("main", "import { y } from 'mid'; export const r = y + '!';");
    expect("main", "r", "X!");

    /* deeper values survive: closures, arrays, regex-in-module */
    mod_add("lib",
            "export const make = () => { let n = 0; return () => ++n; };"
            "export function fmt(xs){ return xs.map(x => x*x).join(','); }");
    mod_add("main",
            "import { make, fmt } from 'lib';"
            "const f = make(); f(); f();"
            "export const r = fmt([1,2,3]) + '|' + f();");
    expect("main", "r", "1,4,9|3");
}

/* ---- kind confusion + tamper resistance ---- */

/* Loads a buffer as module 'main' (deps via bc_loader) and evaluates;
 * returns true if the completion fulfilled. Fuel-capped so a mutant that
 * loops can't hang. Never crashes (ASan-enforced). */
static bool try_module_bc(JsContext *ctx, JsVm *vm, const uint8_t *buf, size_t len) {
    static const uint16_t spec[] = {'m', 'a', 'i', 'n'};
    g_override_bc = buf;
    g_override_len = len;
    js_context_set_fuel(ctx, 2000000);
    JsValue p = js_eval_module(ctx, spec, 4);
    js_gc_protect(vm, &p);
    bool ok = js_promise_state(p) == 1;
    js_gc_unprotect(vm, &p);
    js_context_set_fuel(ctx, 0);
    g_override_bc = NULL;
    return ok;
}

static void test_kind_and_tamper(void) {
    /* Build one self-contained module buffer (no deps) for the sweep. */
    mod_add("main",
            "export function fib(n){ return n<2 ? n : fib(n-1)+fib(n-2); }"
            "let s = 0; for (const x of [1,2,3,4,5]) s += fib(x); export const r = s;");
    if (!compile_all()) {
        checks_failed++;
        checks_run++;
        fprintf(stderr, "FAIL: could not compile module for tamper test\n");
        mods_reset();
        return;
    }
    uint8_t *bc = g_mods.bc[0];
    size_t len = g_mods.bc_len[0];

    /* kind confusion: a module buffer must be rejected by the script loader */
    {
        CountAlloc ca = {0, 0};
        JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca};
        JsVm *vm = js_vm_new(&cfg);
        JsContext *ctx = js_context_new(vm);
        const char *em;
        JsValue fn = js_bytecode_load(ctx, bc, len, &em);
        checks_run++;
        if (js_is_function(fn)) {
            checks_failed++;
            fprintf(stderr, "FAIL: script loader accepted a module buffer\n");
        }
        js_vm_free(vm);
    }

    /* a valid module buffer evaluates */
    {
        CountAlloc ca = {0, 0};
        JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca};
        JsVm *vm = js_vm_new(&cfg);
        JsContext *ctx = js_context_new(vm);
        js_set_module_loader(ctx, bc_loader, NULL, vm);
        checks_run++;
        if (!try_module_bc(ctx, vm, bc, len)) {
            checks_failed++;
            fprintf(stderr, "FAIL: valid module buffer failed to load/eval\n");
        }
        js_vm_free(vm);
    }

    /*
     * Mutation sweep over the module buffer: every byte (past the magic) x
     * three mutations, each loaded+evaluated in a fresh registry (the module
     * cache is keyed by specifier, so reusing one context would just replay
     * the first load). The loader must be memory-safe on arbitrary input and
     * never emit a module that is unsafe to run — the point being that the
     * import-index bounds and module-opcode gating hold under corruption.
     * ASan/UBSan enforces it.
     */
    const uint8_t muts[3] = {0xA5, 0x00, 0xFF};
    int total = 0;
    uint8_t *t = malloc(len);
    for (size_t i = 4; i < len; i++) {
        for (int mi = 0; mi < 3; mi++) {
            CountAlloc ca = {0, 0};
            JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca};
            JsVm *vm = js_vm_new(&cfg);
            JsContext *ctx = js_context_new(vm);
            js_set_module_loader(ctx, bc_loader, NULL, vm);
            memcpy(t, bc, len);
            t[i] = mi == 0 ? (uint8_t)(bc[i] ^ 0xA5) : muts[mi];
            (void)try_module_bc(ctx, vm, t, len);
            total++;
            js_vm_free(vm);
        }
    }
    free(t);
    checks_run++;
    if (total == 0) {
        checks_failed++;
        fprintf(stderr, "FAIL: module mutation sweep ran no cases\n");
    }
    mods_reset();
}

int main(void) {
    test_graphs();
    test_kind_and_tamper();
    if (checks_failed) {
        fprintf(stderr, "%d/%d module-bytecode checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d module-bytecode checks passed\n", checks_run);
    return 0;
}
