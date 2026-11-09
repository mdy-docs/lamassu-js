/*
 * Module eviction: a changed dependency is evicted together with its importer, the next
 * evaluation sees the new source, unrelated modules stay, in-flight modules cannot be evicted,
 * and a closure from the evicted graph still runs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lamassu.h"
#include "lamassu_compile.h"

static int checks_run, checks_failed;
#define CHECK(cond, what) do { checks_run++; if (!(cond)) { checks_failed++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

static uint16_t *to_u16(const char *s, size_t *len) {
    size_t n = strlen(s);
    uint16_t *u = malloc((n + 1) * sizeof(uint16_t));
    for (size_t i = 0; i < n; i++) u[i] = (uint16_t)(unsigned char)s[i];
    u[n] = 0; *len = n; return u;
}
static bool str_is(JsValue v, const char *ascii) {
    size_t n; const uint16_t *u = js_string_units(v, &n);
    if (!u || n != strlen(ascii)) return false;
    for (size_t i = 0; i < n; i++) if (u[i] != (uint16_t)(unsigned char)ascii[i]) return false;
    return true;
}

typedef struct { const char *name; const char *src; } Mod;
static Mod g_mods[4];
static int g_nmods;
static int g_loads;

static JsValue settled(JsContext *ctx, JsVm *vm, JsValue v, bool ok) {
    js_gc_protect(vm, &v);
    JsValue p = js_promise_new(ctx);
    if (ok) js_resolve(ctx, p, v); else js_reject(ctx, p, v);
    js_gc_unprotect(vm, &v);
    return p;
}
static JsValue loader(void *ud, JsContext *ctx, const uint16_t *spec, size_t spec_len, const uint16_t *ref, size_t ref_len) {
    JsVm *vm = ud; (void)ref; (void)ref_len;
    g_loads++;
    for (int i = 0; i < g_nmods; i++) {
        size_t n = strlen(g_mods[i].name);
        if (n != spec_len) continue;
        bool same = true;
        for (size_t k = 0; k < n && same; k++) same = spec[k] == (uint16_t)g_mods[i].name[k];
        if (!same) continue;
        size_t sl; uint16_t *su = to_u16(g_mods[i].src, &sl);
        JsValue v = js_atom(vm, su, sl); free(su);
        return settled(ctx, vm, v, true);
    }
    size_t ml; uint16_t *mu = to_u16("module not found", &ml);
    JsValue reason = js_atom(vm, mu, ml); free(mu);
    return settled(ctx, vm, reason, false);
}

static JsValue eval_root(JsContext *ctx, JsVm *vm) {
    static const uint16_t root[] = { 'm', 'a', 'i', 'n', '.', 'j', 's' };
    JsValue p = js_eval_module(ctx, root, 7);
    js_gc_protect(vm, &p);
    js_run_jobs(ctx);
    js_gc_unprotect(vm, &p);
    return p;
}
static JsValue export_of(JsContext *ctx, JsValue p, const char *name) {
    size_t n; uint16_t *u = to_u16(name, &n);
    JsValue v = js_module_get_export(ctx, js_promise_result(p), u, n);
    free(u);
    return v;
}
static bool evict(JsContext *ctx, const char *spec) {
    size_t n; uint16_t *u = to_u16(spec, &n);
    bool ok = js_context_evict_module(ctx, u, n);
    free(u);
    return ok;
}

int main(void) {
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    js_enable_source_modules(ctx);
    js_set_module_loader(ctx, loader, NULL, vm);

    g_nmods = 0;
    g_mods[g_nmods++] = (Mod){ "main.js", "import { lid } from 'lid.js';\nimport { keep } from 'lib.js';\nexport const v = lid() + keep;\nexport const f = () => lid();\n" };
    g_mods[g_nmods++] = (Mod){ "lid.js", "export function lid() { return 'old'; }\n" };
    g_mods[g_nmods++] = (Mod){ "lib.js", "export const keep = '!';\n" };
    JsValue p1 = eval_root(ctx, vm);
    js_gc_protect(vm, &p1);
    CHECK(js_promise_state(p1) == 1, "first graph evaluates");
    CHECK(str_is(export_of(ctx, p1, "v"), "old!"), "first value");
    CHECK(js_context_module_count(ctx) == 3, "three modules registered");
    JsValue old_f = export_of(ctx, p1, "f");
    js_gc_protect(vm, &old_f);

    /* same sources again: the cached graph comes back, nothing is loaded */
    int loads_before = g_loads;
    JsValue p2 = eval_root(ctx, vm);
    CHECK(js_promise_state(p2) == 1 && g_loads == loads_before, "a second evaluation is served from the registry");

    /* the dependency changes: evicting it takes main.js along, lib.js stays */
    g_mods[1].src = "export function lid() { return 'new'; }\n";
    CHECK(evict(ctx, "lid.js"), "evict lid.js");
    CHECK(js_context_module_count(ctx) == 1, "lid.js and its importer are gone, lib.js stays");
    CHECK(str_is(js_context_module_specifier(ctx, 0), "lib.js"), "the survivor is lib.js");
    CHECK(!evict(ctx, "lid.js"), "evicting twice reports false");
    CHECK(!evict(ctx, "nope.js"), "unknown module reports false");
    loads_before = g_loads;
    JsValue p3 = eval_root(ctx, vm);
    CHECK(js_promise_state(p3) == 1, "re-evaluates");
    CHECK(str_is(export_of(ctx, p3, "v"), "new!"), "new source in effect");
    CHECK(g_loads == loads_before + 2, "only main.js and lid.js were loaded again");
    CHECK(js_context_module_count(ctx) == 3, "registry back to three");

    /* a closure from the evicted graph still runs against its own (old) module */
    JsValue out;
    CHECK(js_call(ctx, old_f, js_undefined(), NULL, 0, &out) && str_is(out, "old"), "old closure keeps its module alive");
    js_gc_collect(vm);
    CHECK(js_call(ctx, old_f, js_undefined(), NULL, 0, &out) && str_is(out, "old"), "and survives a collection");

    js_gc_unprotect(vm, &old_f);
    js_gc_unprotect(vm, &p1);
    js_vm_free(vm);
    printf("test_evict: %d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
