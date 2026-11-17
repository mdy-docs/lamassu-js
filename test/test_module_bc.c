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

/* set for a second pass of the re-export graphs with a collection at every safe point */
static bool g_gc_stress;

/* where the last graph that failed says its error is from: "module:offset", or "" for nowhere */
static char g_where[96];

/* Evaluates root `name` through `load`, reads export `exp`, returns ToString. */
static char *eval_with(JsModuleLoader load, const char *name, const char *exp, bool *ok) {
    CountAlloc ca = {0, 0};
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca, .gc_stress = g_gc_stress};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    js_set_module_loader(ctx, load, NULL, vm);
    js_enable_source_modules(ctx);
    size_t nlen;
    uint16_t *nu = to_u16(name, &nlen);
    JsValue p = js_eval_module(ctx, nu, nlen);
    js_gc_protect(vm, &p);
    js_run_jobs(ctx);
    int st = js_promise_state(p);
    *ok = st == 1;
    char *out;
    JsValue result = js_promise_result(p);
    if (st != 1) {
        JsStackFrame at;
        g_where[0] = 0;
        if (js_promise_error_stack(ctx, p, &at, 1) == 1) {
            size_t wl, k = 0;
            const uint16_t *wv = js_string_units(at.specifier, &wl);
            for (; k < wl && k < 64; k++)
                g_where[k] = wv[k] < 128 ? (char)wv[k] : '?';
            snprintf(g_where + k, sizeof g_where - k, ":%u", (unsigned)at.pos);
        }
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
        fprintf(stderr, "FAIL root=%s.%s%s\n  expected: %s\n  source:   %s%s\n  bytecode: %s%s\n",
                root, exp, g_gc_stress ? " (gc stress)" : "", expected, s, sok ? "" : " (err)", b,
                bok ? "" : " (err)");
    }
    free(s);
    free(b);
    mods_reset();
}

/* As expect(), for a graph that must not link: from source and from bytecode alike the root's
 * promise rejects with `expected` as the reason. */
static void expect_error(const char *root, const char *expected) {
    checks_run++;
    if (!compile_all()) {
        checks_failed++;
        fprintf(stderr, "FAIL(compile) root=%s\n", root);
        mods_reset();
        return;
    }
    bool sok, bok;
    char *s = eval_with(src_loader, root, "r", &sok);
    char *b = eval_with(bc_loader, root, "r", &bok);
    if (sok || bok || strcmp(s, expected) != 0 || strcmp(s, b) != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL root=%s%s\n  expected error: %s\n  source:   %s%s\n  bytecode: %s%s\n",
                root, g_gc_stress ? " (gc stress)" : "", expected, s, sok ? " (no error)" : "", b,
                bok ? " (no error)" : "");
    }
    free(s);
    free(b);
    mods_reset();
}

/* As expect_error(), and the error is from `where` ("module:offset"), the place V8 points at:
 * the import or the re-export that asked for the name. A cache carries the offsets, so the
 * graph says the same from bytecode. */
static void expect_error_at(const char *root, const char *expected, const char *where) {
    checks_run++;
    if (!compile_all()) {
        checks_failed++;
        fprintf(stderr, "FAIL(compile) root=%s\n", root);
        mods_reset();
        return;
    }
    bool sok, bok;
    char sw[sizeof g_where], bw[sizeof g_where];
    char *s = eval_with(src_loader, root, "r", &sok);
    memcpy(sw, g_where, sizeof sw);
    char *b = eval_with(bc_loader, root, "r", &bok);
    memcpy(bw, g_where, sizeof bw);
    if (sok || bok || strcmp(s, expected) != 0 || strcmp(s, b) != 0 || strcmp(sw, where) != 0 ||
        strcmp(bw, where) != 0) {
        checks_failed++;
        fprintf(stderr,
                "FAIL root=%s%s\n  expected error: %s at %s\n  source:   %s at %s%s\n"
                "  bytecode: %s at %s%s\n",
                root, g_gc_stress ? " (gc stress)" : "", expected, where, s, sw,
                sok ? " (no error)" : "", b, bw, bok ? " (no error)" : "");
    }
    free(s);
    free(b);
    mods_reset();
}

/*
 * Re-exports are the bindings they name, linked as V8 links them (ResolveExport,
 * FinishInstantiate and FetchStarExports of its source-text-module.cc). Every expected string
 * here is what Node 26 (V8 14.6) prints for the same graph.
 */
static void test_reexports(void) {
    /* one binding by every route: by name, renamed, through `export *` once and twice, in a
     * namespace and in a re-exported namespace; `export *` leaves `default` out */
    mod_add("c", "export let x = 1; export function inc() { x++; } export let late;"
                 "export function setLate(v) { late = v; } export default 'cdef';");
    mod_add("b", "export { x, inc } from 'c'; export { x as y } from 'c'; export * from 'c';"
                 "export * as cns from 'c'; export const own = 'own';");
    mod_add("star", "export * from 'c';");
    mod_add("star2", "export * from 'star'; export { late as renamed } from 'star';");
    mod_add("main", "import * as b from 'b'; import * as s from 'star'; import * as s2 from 'star2';"
                    "import { x as sx, late as slate } from 'star';"
                    "import { x as s2x, renamed } from 'star2'; import { x as bx, y as by } from 'b';"
                    "const now = () => [b.x, b.y, s.x, s2.x, sx, s2x, bx, by, b.cns.x].join('');"
                    "const out = [Object.keys(b), Object.keys(s), Object.keys(s2), now()];"
                    "b.inc(); out.push(now()); s.inc(); s2.inc(); out.push(now());"
                    "s.setLate('L'); out.push([s.late, s2.late, s2.renamed, slate, renamed].join(''));"
                    "out.push('default' in s, 'default' in b, b.inc === s.inc, s2.inc === s.inc);"
                    "export const r = out.join('|');");
    expect("main", "r", "cns,inc,late,own,setLate,x,y|inc,late,setLate,x|inc,late,renamed,setLate,x|"
                        "111111111|222222222|444444444|LLLLL|false|false|true|true");

    /* `default` re-exported by name, and an import re-exported by an export list */
    mod_add("c", "let d = 'd0'; export { d as default }; export let x = 1;"
                 "export function set(v) { d = v; x = v; }");
    mod_add("m", "import dflt, { x } from 'c'; export { dflt, x as xx }; export { default } from 'c';"
                 "export { default as dd, x as default2 } from 'c';");
    mod_add("main", "import * as m from 'm'; import * as c from 'c'; import md, { dflt, xx, dd } from 'm';"
                    "const now = () => [m.dflt, m.xx, m.default, m.dd, m.default2, md, dflt, xx, dd].join(' ');"
                    "const out = [Object.keys(m), now()]; c.set('S'); out.push(now());"
                    "export const r = out.join('|');");
    expect("main", "r", "dd,default,default2,dflt,xx|d0 1 d0 d0 1 d0 d0 1 d0|S S S S S S S S S");

    /* `export default function f() {}` exports the binding f: an assignment to f shows */
    mod_add("a", "export default function f() { return 1; } export { f as g };"
                 "export function change() { f = 5; }");
    mod_add("b", "export { default as a } from 'a';");
    mod_add("c", "export { g as a } from 'a';");
    mod_add("d", "export * from 'b'; export * from 'c';");
    mod_add("main", "import d, { g, change } from 'a'; import * as ns from 'd'; import * as na from 'a';"
                    "const out = [typeof d, typeof g, Object.keys(ns), Object.keys(na).join()];"
                    "change(); out.push(d, g, ns.a, na.default); export const r = out.join(' ');");
    expect("main", "r", "function function a change,default,g 5 5 5 5");

    /* a name two `export *` bring as different bindings is no export: left out of the
     * namespace, a SyntaxError to import */
    mod_add("p", "export let x = 'p'; export let onlyp = 1;");
    mod_add("q", "export let x = 'q'; export let onlyq = 2;");
    mod_add("m", "export * from 'p'; export * from 'q';");
    mod_add("main", "import * as m from 'm'; export const r = [Object.keys(m), String(m.x), 'x' in m].join('|');");
    expect("main", "r", "onlyp,onlyq|undefined|false");
    mod_add("p", "export let x = 'p';");
    mod_add("q", "export let x = 'q';");
    mod_add("m", "export * from 'p'; export * from 'q';");
    mod_add("main", "import { x } from 'm'; export const r = x;");
    expect_error("main", "SyntaxError: The requested module 'm' contains conflicting star exports for name 'x'");
    mod_add("p", "export let x = 'p';");
    mod_add("q", "export let x = 'q';");
    mod_add("m", "export * from 'p'; export * from 'q';");
    mod_add("n", "export { x } from 'm';");
    mod_add("main", "import * as n from 'n'; export const r = Object.keys(n).join();");
    expect_error("main", "SyntaxError: The requested module 'm' contains conflicting star exports for name 'x'");

    /* the module's own export and its named re-export win over what `export *` brings */
    mod_add("p", "export let x = 'p'; export let y = 'py'; export let z = 'pz';");
    mod_add("q", "export let y = 'qy'; export let z = 'qz';");
    mod_add("m", "export * from 'p'; export let x = 'local'; export { y } from 'q'; export * from 'q';");
    mod_add("main", "import * as m from 'm'; import { x, y } from 'm';"
                    "export const r = [Object.keys(m), m.x, m.y, String(m.z), x, y].join('|');");
    expect("main", "r", "x,y|local|qy|undefined|local|qy");

    /* one binding reached twice does not conflict: through two modules, under two names of
     * its own (`export { x as a, x as b }`) */
    mod_add("c", "export let x = 1; export function inc() { x++; }");
    mod_add("e", "export * from 'c';");
    mod_add("f", "export * from 'c';");
    mod_add("m", "export * from 'e'; export * from 'f';");
    mod_add("main", "import * as m from 'm'; import { x } from 'm'; const out = [Object.keys(m), m.x, x];"
                    "m.inc(); out.push(m.x, x); export const r = out.join('|');");
    expect("main", "r", "inc,x|1|1|2|2");
    mod_add("c", "let x = 1; export { x as a, x as b }; export function inc() { x++; }");
    mod_add("e", "export { a as z } from 'c';");
    mod_add("f", "export { b as z } from 'c';");
    mod_add("m", "export * from 'e'; export * from 'f';");
    mod_add("main", "import * as m from 'm'; import * as c from 'c'; const out = [Object.keys(m), String(m.z)];"
                    "c.inc(); out.push(String(m.z)); export const r = out.join('|');");
    expect("main", "r", "z|1|2");
    mod_add("c", "export let x = 1; export function inc() { return ++x; }");
    mod_add("s", "export * from 'c';");
    mod_add("r", "export { x as y } from 's';");
    mod_add("r2", "export { y as z } from 'r'; export * from 'r';");
    mod_add("main", "import * as r2 from 'r2'; import { z, y } from 'r2'; import { inc } from 's';"
                    "const out = [Object.keys(r2).join(','), r2.z, r2.y, z, y]; inc();"
                    "out.push(r2.z, r2.y, z, y); export const r = out.join(' ');");
    expect("main", "r", "y,z 1 1 1 1 2 2 2 2");

    /* a module has one namespace binding (V8's namespace cell): re-exported by two modules,
     * as `export * as` or as an imported namespace in an export list, it is one binding; the
     * namespaces of two modules under one name conflict */
    mod_add("c", "export let x = 1;");
    mod_add("e", "export * as z from 'c'; import * as w from 'c'; export { w };");
    mod_add("f", "import * as z from 'c'; export { z }; export * as w from 'c';");
    mod_add("m", "export * from 'e'; export * from 'f';");
    mod_add("main", "import * as m from 'm'; import * as e from 'e'; import * as c from 'c'; import { z, w } from 'm';"
                    "export const r = [Object.keys(m), Object.keys(e), e.z === c, e.w === c, m.z === c, z === w].join('|');");
    expect("main", "r", "w,z|w,z|true|true|true|true");
    mod_add("c", "export let x = 1;");
    mod_add("d", "export let x = 1;");
    mod_add("e", "export * as z from 'c';");
    mod_add("f", "export * as z from 'd';");
    mod_add("m", "export * from 'e'; export * from 'f';");
    mod_add("main", "import * as m from 'm'; export const r = '[' + Object.keys(m) + ']';");
    expect("main", "r", "[]");
    mod_add("c", "export let x = 1;");
    mod_add("d", "export let x = 1;");
    mod_add("e", "export * as z from 'c';");
    mod_add("f", "import * as z from 'd'; export { z };");
    mod_add("m", "export * from 'e'; export * from 'f';");
    mod_add("main", "import { z } from 'm'; export const r = typeof z;");
    expect_error("main", "SyntaxError: The requested module 'm' contains conflicting star exports for name 'z'");

    /* the link errors, worded as V8's and naming the module request they came through */
    mod_add("c", "export let x = 1;");
    mod_add("m", "export * from 'c';");
    mod_add("main", "import { nope } from 'm'; export const r = nope;");
    expect_error("main", "SyntaxError: The requested module 'm' does not provide an export named 'nope'");
    mod_add("c", "export let x = 1;");
    mod_add("m", "export { nope } from 'c';");
    mod_add("main", "import * as m from 'm'; export const r = Object.keys(m).join();");
    expect_error("main", "SyntaxError: The requested module 'c' does not provide an export named 'nope'");
    mod_add("a", "export { x } from 'b';");
    mod_add("b", "export { x } from 'a';");
    mod_add("main", "import * as a from 'a'; export const r = Object.keys(a).join();");
    expect_error("main", "SyntaxError: Detected cycle while resolving name 'x' in 'b'");
    mod_add("c", "export default 1; export let x = 1;");
    mod_add("m", "export * from 'c';");
    mod_add("main", "import d from 'm'; export const r = d;");
    expect_error("main", "SyntaxError: The requested module 'm' does not provide an export named 'default'");
    mod_add("u", "export let other = 1;");
    mod_add("s", "export * from 'u';");
    mod_add("t", "export let x = 't';");
    mod_add("m", "export * from 's'; export * from 't';");
    mod_add("main", "import { x } from 'm'; import { x as x2 } from 'u'; export const r = x + x2;");
    expect_error("main", "SyntaxError: The requested module 'u' does not provide an export named 'x'");

    /* where each is from, as V8 points: the name in the import or the re-export that asked for
     * it, in the module that holds it (Node prints that line above the error) */
    mod_add("c", "export let x = 1;");
    mod_add("main", "import { x,\n  nope as q } from 'c'; export const r = q;");
    expect_error_at("main", "SyntaxError: The requested module 'c' does not provide an export named 'nope'",
                    "main:14");
    mod_add("c", "export let x = 1;");
    mod_add("main", "import d, { x } from 'c'; export const r = d;"); /* the binding, for a default */
    expect_error_at("main", "SyntaxError: The requested module 'c' does not provide an export named 'default'",
                    "main:7");
    mod_add("c", "export let x = 1;");
    mod_add("main", "import { default as d } from 'c'; export const r = d;");
    expect_error_at("main", "SyntaxError: The requested module 'c' does not provide an export named 'default'",
                    "main:9");
    mod_add("c", "export let x = 1;");
    mod_add("main", "export { x as y, nope as r } from 'c';");
    expect_error_at("main", "SyntaxError: The requested module 'c' does not provide an export named 'nope'",
                    "main:17");
    mod_add("c", "export let x = 1;"); /* imported and exported again: the import */
    mod_add("main", "import { x, nope } from 'c';\nexport { nope as r };");
    expect_error_at("main", "SyntaxError: The requested module 'c' does not provide an export named 'nope'",
                    "main:12");
    mod_add("c", "export let x = 1;"); /* another module's re-export, though main asked */
    mod_add("m", "// m\n  export { nope } from 'c';");
    mod_add("main", "import { nope } from 'm'; export const r = nope;");
    expect_error_at("main", "SyntaxError: The requested module 'c' does not provide an export named 'nope'",
                    "m:16");
    mod_add("c", "export let x = 1;"); /* ... and with nobody asking: every re-export is resolved */
    mod_add("m", "export { nope } from 'c';");
    mod_add("main", "import 'm'; export const r = 1;");
    expect_error_at("main", "SyntaxError: The requested module 'c' does not provide an export named 'nope'",
                    "m:9");
    mod_add("p", "export let x = 'p';");
    mod_add("q", "export let x = 'q';");
    mod_add("s", "export * from 'p'; export * from 'q';");
    mod_add("main", "import { x } from 's'; export const r = x;");
    expect_error_at("main", "SyntaxError: The requested module 's' contains conflicting star exports for name 'x'",
                    "main:9");
    mod_add("p", "export let x = 'p';"); /* through one more `export *`: its `*` */
    mod_add("q", "export let x = 'q';");
    mod_add("s", "export * from 'p'; export * from 'q';");
    mod_add("m", "\n   export * from 's';");
    mod_add("main", "import { x } from 'm'; export const r = x;");
    expect_error_at("main", "SyntaxError: The requested module 's' contains conflicting star exports for name 'x'",
                    "m:11");
    mod_add("a", "export { x } from 'b';");
    mod_add("b", "export { x } from 'a';");
    mod_add("main", "import { x } from 'a'; export const r = x;");
    expect_error_at("main", "SyntaxError: Detected cycle while resolving name 'x' in 'b'", "a:9");

    /* V8 as it is: a conflict one `export *` further down is no conflict for the namespace
     * (the name stays, from the module that has it), yet importing the name reports it */
    mod_add("p", "export let x = 'p';");
    mod_add("q", "export let x = 'q';");
    mod_add("s", "export * from 'p'; export * from 'q';");
    mod_add("t", "export let x = 't'; export let t = 1;");
    mod_add("m", "export * from 's'; export * from 't';");
    mod_add("main", "import * as m from 'm'; export const r = Object.keys(m) + ' ' + String(m.x);");
    expect("main", "r", "t,x t");
    mod_add("p", "export let x = 'p';");
    mod_add("q", "export let x = 'q';");
    mod_add("s", "export * from 'p'; export * from 'q';");
    mod_add("t", "export let x = 't'; export let t = 1;");
    mod_add("m", "export * from 't'; export * from 's';");
    mod_add("main", "import { x } from 'm'; export const r = x;");
    expect_error("main", "SyntaxError: The requested module 's' contains conflicting star exports for name 'x'");
    /* ... unless the namespace was made first, in a module linked before this one */
    mod_add("p", "export let x = 'p';");
    mod_add("q", "export let x = 'q';");
    mod_add("s", "export * from 'p'; export * from 'q';");
    mod_add("t", "export let x = 't'; export let t = 1;");
    mod_add("m", "export * from 't'; export * from 's';");
    mod_add("mid", "import * as ns from 'm'; export const k = Object.keys(ns).join();");
    mod_add("main", "import { x } from 'm'; import { k } from 'mid'; export const r = x + ' ' + k;");
    expect("main", "r", "t t,x");
    /* a module whose linking failed is linked again by the next import (V8 does; Node keeps
     * the failed job, so its third line repeats the first) */
    mod_add("p", "export let x = 'p';");
    mod_add("q", "export let x = 'q';");
    mod_add("s", "export * from 'p'; export * from 'q';");
    mod_add("t", "export let x = 't'; export let t = 1;");
    mod_add("m", "export * from 't'; export * from 's';");
    mod_add("n1", "import { x } from 'm'; export const r = x;");
    mod_add("n2", "import { x } from 'm'; export const r = x;");
    mod_add("main", "import 'm'; const out = [];"
                    "try { out.push((await import('n1')).r); } catch (e) { out.push(e.message); }"
                    "out.push(Object.keys(await import('m')).join());"
                    "try { out.push((await import('n2')).r); } catch (e) { out.push(e.message); }"
                    "try { out.push((await import('n1')).r); } catch (e) { out.push(e.message); }"
                    "export const r = out.join('|');");
    expect("main", "r", "The requested module 's' contains conflicting star exports for name 'x'|t,x|t|t");
    /* and fails again where the error is still there: never half linked */
    mod_add("c", "export let x = 1;");
    mod_add("bad", "import { nope } from 'c'; export let y = nope;");
    mod_add("bad2", "export * from 'bad'; export let z = 1;");
    mod_add("main", "const out = [];"
                    "for (const s of ['bad', 'bad', 'bad2', 'bad2', 'c']) {"
                    "  try { out.push(s + ' ok ' + Object.keys(await import(s))); }"
                    "  catch (e) { out.push(s + ' ' + e.name + ': ' + e.message); } }"
                    "export const r = out.join('|');");
    expect("main", "r", "bad SyntaxError: The requested module 'c' does not provide an export named 'nope'|"
                        "bad SyntaxError: The requested module 'c' does not provide an export named 'nope'|"
                        "bad2 SyntaxError: The requested module 'c' does not provide an export named 'nope'|"
                        "bad2 SyntaxError: The requested module 'c' does not provide an export named 'nope'|"
                        "c ok x");
    /* a module that threw stays failed, for the importer of its names too */
    mod_add("a", "export let x = 1; throw new Error('boom');");
    mod_add("b", "import { x } from 'a'; export let y = x;");
    mod_add("main", "const out = [];"
                    "for (const s of ['a', 'b', 'a']) { try { await import(s); } catch (e) { out.push(e.message); } }"
                    "export const r = out.join('|');");
    expect("main", "r", "boom|boom|boom");
    /* a module linked later binds to values as they are by then, and stays live */
    mod_add("c", "export let x = 1; export function inc() { x++; }");
    mod_add("m", "export * from 'c'; export { x as y } from 'c';");
    mod_add("main", "import * as c from 'c'; c.inc(); const m = await import('m'); const out = [m.x, m.y];"
                    "c.inc(); out.push(m.x, m.y); export const r = out.join('|');");
    expect("main", "r", "2|2|3|3");

    /* cycles of `export *`: every namespace is complete, a module may star-export itself */
    mod_add("a", "export * from 'b'; export let ax = 'ax'; export function setA(v) { ax = v; }");
    mod_add("b", "export * from 'a'; export let bx = 'bx';");
    mod_add("main", "import * as a from 'a'; import * as b from 'b'; import { ax } from 'b';"
                    "const out = [Object.keys(a), Object.keys(b), b.ax, ax]; a.setA('new');"
                    "out.push(b.ax, a.ax, ax); export const r = out.join('|');");
    expect("main", "r", "ax,bx,setA|ax,bx,setA|ax|ax|new|new|new");
    mod_add("a", "export * from 'b'; export let a1 = 1;");
    mod_add("b", "export * from 'c'; export let b1 = 1;");
    mod_add("c", "export * from 'a'; export let c1 = 1; export * from 'd';");
    mod_add("d", "export let d1 = 1;");
    mod_add("main", "import * as c from 'c'; import * as b from 'b'; import * as a from 'a'; import { a1, d1 } from 'b';"
                    "export const r = [Object.keys(a), Object.keys(b), Object.keys(c)].join('|') + a1 + d1;");
    expect("main", "r", "a1,b1,c1,d1|a1,b1,c1,d1|a1,b1,c1,d111");
    mod_add("m", "export * from 'm'; export let x = 1; export { x as y } from 'm'; export function inc() { x++; }");
    mod_add("main", "import * as m from 'm'; const out = [Object.keys(m).join(','), m.x, m.y]; m.inc();"
                    "out.push(m.x, m.y); export const r = out.join(' ');");
    expect("main", "r", "inc,x,y 1 1 2 2");

    /* a re-exported name read in a cycle before its declaration ran: the TDZ error, naming
     * the local name of an import and the property of a namespace */
    mod_add("a", "import { v, w } from 'b'; export const out = [];"
                 "try { out.push('a sees v: ' + v); } catch (e) { out.push(e.name + ': ' + e.message); }"
                 "try { out.push('a sees w: ' + w); } catch (e) { out.push(e.name + ': ' + e.message); }"
                 "export let v0 = 'av'; export { v0 as w0 };");
    mod_add("b", "export { v0 as v } from 'a'; export { w0 as w } from 'a';");
    mod_add("main", "import * as b from 'b'; import { out } from 'a'; out.push('main: ' + b.v + ' ' + b.w);"
                    "export const r = out.join('|');");
    expect("main", "r", "ReferenceError: Cannot access 'v' before initialization|"
                        "ReferenceError: Cannot access 'w' before initialization|main: av av");
    mod_add("a", "import { v as lv } from 'b'; import * as b from 'b'; export const out = [];"
                 "try { out.push('a sees lv: ' + lv); } catch (e) { out.push(e.name + ': ' + e.message); }"
                 "try { out.push('a sees b.v: ' + b.v); } catch (e) { out.push(e.name + ': ' + e.message); }"
                 "export let v0 = 'av';");
    mod_add("b", "export { v0 as v } from 'a';");
    mod_add("main", "import * as b from 'b'; import { out } from 'a'; out.push('main: ' + b.v);"
                    "export const r = out.join('|');");
    expect("main", "r", "ReferenceError: Cannot access 'lv' before initialization|"
                        "ReferenceError: Cannot access 'v' before initialization|main: av");
    mod_add("a", "import * as b from 'b'; export const out = [];"
                 "try { out.push('a sees b.v: ' + b.v); } catch (e) { out.push(e.name + ': ' + e.message); }"
                 "out.push('in: ' + ('v' in b) + ' ' + ('out' in b)); export let v = 'av';");
    mod_add("b", "export * from 'a'; import 'a';");
    mod_add("main", "import * as b from 'b'; import { v } from 'b'; import { out } from 'a';"
                    "out.push('main: ' + b.v + ' ' + v + ' ' + Object.keys(b)); export const r = out.join('|');");
    expect("main", "r", "ReferenceError: Cannot access 'v' before initialization|in: true true|main: av av out,v");

    /* the order of linking inside one module is V8's: imports by local name (not in source
     * order), then the `export ... from` declarations in source order. It decides which of
     * two errors is the one reported. */
    mod_add("g", "export let g = 1;");
    mod_add("h", "export let h = 1;");
    mod_add("main", "import { zz } from 'g'; import { aa } from 'h'; export const r = 1;");
    expect_error("main", "SyntaxError: The requested module 'h' does not provide an export named 'aa'");
    mod_add("g", "export let g = 1;");
    mod_add("h", "export let h = 1;");
    mod_add("main", "import { zz as b } from 'g'; import { aa as a, bb as ab } from 'h'; export const r = 1;");
    expect_error("main", "SyntaxError: The requested module 'h' does not provide an export named 'aa'");
    mod_add("g", "export let g = 1;");
    mod_add("h", "export let h = 1;");
    mod_add("main", "import { zz as ab } from 'g'; import { aa as a } from 'h'; export const r = 1;");
    expect_error("main", "SyntaxError: The requested module 'h' does not provide an export named 'aa'");
    mod_add("g", "export let g = 1;");
    mod_add("h", "export let h = 1;");
    mod_add("main", "export { nope } from 'g'; import { bad } from 'h'; export const r = 1;");
    expect_error("main", "SyntaxError: The requested module 'h' does not provide an export named 'bad'");
    mod_add("g", "export let g = 1;");
    mod_add("h", "export let h = 1;");
    mod_add("main", "export { q } from 'g'; export { p } from 'h'; export const r = 1;");
    expect_error("main", "SyntaxError: The requested module 'g' does not provide an export named 'q'");
    mod_add("g", "export let g = 1;");
    mod_add("h", "export let h = 1;");
    mod_add("main", "import { g as z } from 'g'; export { z as y }; export { nope } from 'h';"
                    "import { bad as zz } from 'h'; export const r = 1;");
    expect_error("main", "SyntaxError: The requested module 'h' does not provide an export named 'bad'");

    /* and the namespaces a module imports are made in the order of their local names, which
     * shows where two modules in a cycle of `export *` each get a name the other may not:
     * the one whose namespace is made first loses it. `export * as` counts under a hidden
     * name, ".ns-export" and its number among the module's: after `$`, before any letter,
     * and ".ns-export10" before ".ns-export2". */
    static const char *const ring[][2] = {
        {"a", "export * from 'c'; export * from 'd';"},
        {"c", "export * from 'a'; export * from 'b';"},
        {"b", "export let k = 2;"},
        {"d", "export let k = 1;"},
        {"g", "export let g = 1;"},
    };
    static const char *const order[][3] = {
        {"import * as b from 'a'; import * as a from 'c'; export const r = Object.keys(b) + '|' + Object.keys(a);",
         NULL, "k|"},
        {"import * as a from 'a'; import * as b from 'c'; export const r = Object.keys(a) + '|' + Object.keys(b);",
         NULL, "|k"},
        {"import { zz, q } from 'm'; export const r = Object.keys(zz) + '|' + Object.keys(q);",
         "import * as q from 'c'; export * as zz from 'a'; export { q };", "|k"},
        {"import { zz, q } from 'm'; export const r = Object.keys(zz) + '|' + Object.keys(q);",
         "import * as $q from 'c'; export * as zz from 'a'; export { $q as q };", "k|"},
        {"import { za, zc } from 'm'; export const r = Object.keys(za) + '|' + Object.keys(zc);",
         "export * as g0 from 'g'; export * as g1 from 'g'; export * as zc from 'c';"
         "export * as h0 from 'g'; export * as h1 from 'g'; export * as h2 from 'g'; export * as h3 from 'g';"
         "export * as h4 from 'g'; export * as h5 from 'g'; export * as h6 from 'g'; export * as za from 'a';",
         "|k"},
    };
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        for (size_t k = 0; k < sizeof ring / sizeof ring[0]; k++)
            mod_add(ring[k][0], ring[k][1]);
        if (order[i][1])
            mod_add("m", order[i][1]);
        mod_add("main", order[i][0]);
        expect("main", "r", order[i][2]);
    }
}

static void test_graphs(void) {
    /* functions' source text, from source and from each module's cache alike: an exported
     * declaration's from `function` or `async`, an anonymous default's too */
    mod_add("srcs", "export default function () { return 1 }\n"
                    "export async function g(a) { return a }\n"
                    "export const ar = (x) => x * 2;");
    mod_add("main", "import d, { g, ar } from 'srcs'; export const r = [d, g, ar].join('|');");
    expect("main", "r", "function () { return 1 }|async function g(a) { return a }|(x) => x * 2");

    /* e.stack across modules, from source and from the cache alike: each frame's module, async
     * frames (V8's `at async`), an inferred name (Node gives these, the importer's columns
     * shifted for its './stk.mjs') */
    mod_add("stk", "export async function af() { await null; return new Error('a'); }\n"
                   "export async function outer() { return await af(); }\n"
                   "export const x = {}; x.y = function () { return new Error(); };");
    mod_add("main", "import { outer, x } from 'stk'; const e = await outer();"
                    " export const r = e.stack + '|' + x.y().stack;");
    expect("main", "r", "Error: a\n    at af (stk:1:49)\n    at async outer (stk:2:40)\n    at async main:1:43"
                        "|Error\n    at x.y (stk:3:49)\n    at main:1:93");

    /* basic import/export */
    mod_add("util", "export const g = 'hi'; export function up(s){ return s.toUpperCase(); }");
    mod_add("main", "import { g, up } from 'util'; export const r = up(g);");
    expect("main", "r", "HI");
    mods_reset();
    mod_add("main", "function f() { return import.meta; } export const r = [import.meta.url, f() === import.meta, Object.getPrototypeOf(import.meta)].join();");
    expect("main", "r", "main,true,");
    mods_reset();
    mod_add("main", "const tag = (s, ...v) => s.raw.join('|') + ':' + s.map(String).join('|') + ':' + v.join() + ':' + Object.isFrozen(s);"
                    " const g = (s) => s; const f = () => tag`a${1}\\xg${2}`;"
                    " export const r = f() + ':' + (g`q` !== g`q`) + ':' + String.raw`x\\ny`;");
    expect("main", "r", "a|\\xg|:a|undefined|:1,2:true:true:x\\ny");
    mods_reset();
    mod_add("main", "const big = 2n ** 100n; let i = 0n; i++; export const r = [big, -12345678901234567890n, 0n, i, typeof big].join();");
    expect("main", "r", "1267650600228229401496703205376,-12345678901234567890,0,1,bigint");

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

    /* a cached module keeps its let/const exports in the TDZ until they run (a cycle reads
     * them early), its export lists, and its namespace's name order */
    mod_add("a", "import { early } from 'b'; export const r = early; export let y = 1; let w = 2; export { w as ww };");
    mod_add("b", "import * as ns from 'a'; import { y } from 'a';"
                 "const t = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } };"
                 "export const early = t(() => y) + '|' + t(() => Object.keys(ns));");
    mod_add("main", "import { r } from 'a'; import * as a from 'a'; export const r2 = r + '|' + Object.keys(a) + '|' + a.ww;");
    expect("main", "r2", "ReferenceError: Cannot access 'y' before initialization|"
                         "ReferenceError: Cannot access 'r' before initialization|r,ww,y|2");
    mod_add("k", "export const k = 1;");
    mod_add("main", "import { k } from 'k'; export { k as k2 }; import * as me from 'main'; export function r() { return me.k2; }");
    mod_add("root", "import { r } from 'main'; export const out = r();");
    expect("root", "out", "1");

    /* and its functions exist before any body runs, so the module running first can call them */
    mod_add("fa", "import { early } from 'fb'; export const r = early;"
                  "export function f() { return 'f'; } export default function () { return 'd'; }");
    mod_add("fb", "import d, { f } from 'fa'; export const early = f() + d();");
    mod_add("fmain", "import { r } from 'fa'; export const out = r;");
    expect("fmain", "out", "fd");
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

/* Every byte of `bc` past the magic x three mutations, each loaded as 'main' and evaluated in
 * a fresh registry; returns the number of cases run. */
static int mutation_sweep(const uint8_t *bc, size_t len) {
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
    return total;
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
    checks_run++;
    if (mutation_sweep(bc, len) == 0) {
        checks_failed++;
        fprintf(stderr, "FAIL: module mutation sweep ran no cases\n");
    }
    mods_reset();

    /* the same over a module with every kind of link record: a namespace import, a hidden one,
     * each kind of re-export, a second name of an export, exports that start in the TDZ and
     * ones that do not. Its dependency loads untouched. */
    mod_add("c", "export let x = 1; export function inc() { x++; }");
    mod_add("main", "export * from 'c'; export { x as y } from 'c'; export * as ns from 'c';"
                    "import * as w from 'c'; import { inc as bump } from 'c'; export { w, bump };"
                    "let q = 1; export { q as a, q as b }; export default function f() {}"
                    "export function g() { return q; } bump(); export const r = w.x + g();");
    checks_run++;
    if (!compile_all()) {
        checks_failed++;
        fprintf(stderr, "FAIL: could not compile the re-exporting module for the tamper test\n");
        mods_reset();
        return;
    }
    {
        CountAlloc ca = {0, 0};
        JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca};
        JsVm *vm = js_vm_new(&cfg);
        JsContext *ctx = js_context_new(vm);
        js_set_module_loader(ctx, bc_loader, NULL, vm);
        checks_run++;
        if (!try_module_bc(ctx, vm, g_mods.bc[1], g_mods.bc_len[1])) {
            checks_failed++;
            fprintf(stderr, "FAIL: valid re-exporting module buffer failed to load/eval\n");
        }
        js_vm_free(vm);
    }
    checks_run++;
    if (mutation_sweep(g_mods.bc[1], g_mods.bc_len[1]) == 0) {
        checks_failed++;
        fprintf(stderr, "FAIL: re-export mutation sweep ran no cases\n");
    }
    mods_reset();
}

int main(void) {
    test_graphs();
    test_reexports();
    /* linking again with the collector running at every safe point: the bindings, the mirrors
     * of live re-exports and the namespaces made while linking stay rooted */
    g_gc_stress = true;
    test_reexports();
    g_gc_stress = false;
    test_kind_and_tamper();
    if (checks_failed) {
        fprintf(stderr, "%d/%d module-bytecode checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d module-bytecode checks passed\n", checks_run);
    return 0;
}
