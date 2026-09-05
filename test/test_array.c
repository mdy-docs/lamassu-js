/*
 * The host's view of an array.
 *
 * An array is an object to js_is_object, but its elements and its length live
 * outside the property map — so js_object_get("length") answers `undefined`
 * and js_object_size answers 0, for a perfectly good array. A host that only
 * had the object functions could neither read what a guest produced nor build
 * a list to hand it, and would have to serialise instead, which is the cost
 * these exist to remove.
 *
 * So the checks below are mostly about the SEAM: what the guest makes, the
 * host reads; what the host makes, the guest sees; and the guarantees the
 * object path already gives — freeze, seal, the bound on a one-shot gap —
 * still hold when reached this way.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lamassu.h"
#include "lamassu_compile.h"

static int checks_run;
static int checks_failed;

static void ok(const char *what, bool passed) {
    checks_run++;
    if (!passed) {
        checks_failed++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

static uint16_t *widen(const char *s, size_t *len) {
    size_t n = strlen(s);
    uint16_t *u = malloc(n * sizeof(uint16_t) + 2);
    for (size_t i = 0; i < n; i++)
        u[i] = (uint16_t)(unsigned char)s[i];
    *len = n;
    return u;
}

static JsValue key(JsVm *vm, const char *s) {
    size_t n = 0;
    uint16_t *u = widen(s, &n);
    JsValue v = js_atom(vm, u, n);
    free(u);
    return v;
}

/* Compile and run `source`, returning its completion value. */
static JsValue eval(JsContext *ctx, JsVm *vm, const char *source) {
    size_t n = 0;
    uint16_t *u = widen(source, &n);
    const char *err = NULL;
    uint32_t pos = 0;
    JsValue fn = js_compile_module(ctx, u, n, &err, &pos);
    free(u);
    if (js_is_undefined(fn)) {
        fprintf(stderr, "compile failed: %s\n", err ? err : "?");
        return js_undefined();
    }
    js_gc_protect(vm, &fn);
    JsValue p = js_run_module(ctx, fn);
    js_gc_protect(vm, &p);
    js_run_jobs(ctx);
    JsValue result = js_promise_state(p) == 1 ? js_promise_result(p) : js_undefined();
    js_gc_unprotect(vm, &p);
    js_gc_unprotect(vm, &fn);
    return result;
}

/* ---- what the guest made, the host reads ---------------------------------- */

static void test_reading(void) {
    JsVmConfig cfg = {0};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);

    JsValue arr = eval(ctx, vm, "([10, 'two', true])");
    js_gc_protect(vm, &arr);

    ok("a guest array is an object", js_is_object(arr));
    ok("…and says so", js_is_array(arr));
    ok("length is readable", js_array_length(arr) == 3);
    ok("a number element", js_is_number(js_array_get(arr, 0)) &&
                           js_get_number(js_array_get(arr, 0)) == 10);
    ok("a string element", js_is_string(js_array_get(arr, 1)));
    ok("a boolean element", js_is_bool(js_array_get(arr, 2)) &&
                            js_get_bool(js_array_get(arr, 2)));
    ok("past the end is undefined", js_is_undefined(js_array_get(arr, 3)));

    /* The old way, and why these functions exist. */
    ok("js_object_get cannot see length",
       js_is_undefined(js_object_get(vm, arr, key(vm, "length"))));
    ok("js_object_size cannot see the elements", js_object_size(arr) == 0);

    JsValue plain = eval(ctx, vm, "({a: 1})");
    js_gc_protect(vm, &plain);
    ok("a plain object is not an array", !js_is_array(plain));
    ok("…and reading it as one is empty rather than wrong",
       js_array_length(plain) == 0 && js_is_undefined(js_array_get(plain, 0)));
    ok("a non-object is not an array", !js_is_array(js_number(1)));

    /* Nested, which is the shape real data has. */
    JsValue pairs = eval(ctx, vm, "([[0, 'a'], [1, 'b']])");
    js_gc_protect(vm, &pairs);
    ok("an array of arrays", js_array_length(pairs) == 2 &&
                             js_is_array(js_array_get(pairs, 0)) &&
                             js_array_length(js_array_get(pairs, 0)) == 2);

    js_gc_unprotect(vm, &pairs);
    js_gc_unprotect(vm, &plain);
    js_gc_unprotect(vm, &arr);
    js_context_free(ctx);
    js_vm_free(vm);
}

/* ---- what the host made, the guest sees ----------------------------------- */

static void test_building(void) {
    JsVmConfig cfg = {0};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);

    JsValue arr = js_array_new(ctx, 0);
    js_gc_protect(vm, &arr);
    ok("a host array is an array", js_is_array(arr));
    ok("…and starts empty", js_array_length(arr) == 0);

    ok("push", js_array_push(vm, arr, js_number(1)));
    ok("push again", js_array_push(vm, arr, js_number(2)));
    ok("length follows", js_array_length(arr) == 2);
    ok("set an existing index", js_array_set(vm, arr, 0, js_number(9)));
    ok("…and it reads back", js_get_number(js_array_get(arr, 0)) == 9);

    /* Setting past the end fills the gap, as index assignment does. */
    ok("set past the end", js_array_set(vm, arr, 4, js_number(5)));
    ok("…the gap is filled with undefined",
       js_array_length(arr) == 5 && js_is_undefined(js_array_get(arr, 3)));

    /* The guest must see exactly that. */
    JsValue globals = js_context_globals(ctx);
    js_object_set(vm, globals, key(vm, "fromHost"), arr);
    JsValue seen = eval(ctx, vm, "(Array.isArray(fromHost) + ':' + fromHost.length + ':' + fromHost[0] + ':' + fromHost[3])");
    size_t len = 0;
    const uint16_t *units = js_string_units(seen, &len);
    char got[64] = {0};
    for (size_t i = 0; i < len && i < sizeof got - 1; i++) got[i] = (char)units[i];
    ok("the guest sees a real array", strcmp(got, "true:5:9:undefined") == 0);
    if (strcmp(got, "true:5:9:undefined") != 0)
        fprintf(stderr, "  guest saw: %s\n", got);

    /* A host array is a real array, so the guest's own methods work on it. */
    JsValue mapped = eval(ctx, vm, "(fromHost.filter((x) => x !== undefined).length)");
    ok("…that its own methods accept",
       js_is_number(mapped) && js_get_number(mapped) == 3);

    ok("a reserve is capacity, not length", js_array_length(js_array_new(ctx, 16)) == 0);
    ok("building on a non-array fails",
       !js_array_push(vm, js_number(1), js_number(2)) &&
       !js_array_set(vm, js_number(1), 0, js_number(2)));

    js_gc_unprotect(vm, &arr);
    js_context_free(ctx);
    js_vm_free(vm);
}

/* ---- the guarantees the object path already gives -------------------------- */

static void test_limits(void) {
    JsVmConfig cfg = {0};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);

    /* A host that froze an array to keep guest code out must not be able to
     * defeat its own guarantee by reaching past the property path. */
    JsValue frozen = eval(ctx, vm, "(Object.freeze([1, 2]))");
    js_gc_protect(vm, &frozen);
    ok("frozen refuses a set", !js_array_set(vm, frozen, 0, js_number(9)));
    ok("frozen refuses a push", !js_array_push(vm, frozen, js_number(3)));
    ok("…and is unchanged", js_get_number(js_array_get(frozen, 0)) == 1 &&
                            js_array_length(frozen) == 2);

    JsValue sealed = eval(ctx, vm, "(Object.seal([1, 2]))");
    js_gc_protect(vm, &sealed);
    ok("sealed allows an existing index", js_array_set(vm, sealed, 1, js_number(9)));
    ok("…but not a new one", !js_array_set(vm, sealed, 5, js_number(9)));
    ok("…and not a push", !js_array_push(vm, sealed, js_number(3)));

    /* Every slot up to the index is materialized, so an unbounded index would
     * be an unbounded allocation — the same ceiling the interpreter applies. */
    JsValue arr = js_array_new(ctx, 0);
    js_gc_protect(vm, &arr);
    ok("a gap wider than the limit is refused",
       !js_array_set(vm, arr, 1000000u, js_number(1)));
    ok("…and the array is untouched", js_array_length(arr) == 0);
    ok("a gap inside the limit is allowed", js_array_set(vm, arr, 100, js_number(1)));

    js_gc_unprotect(vm, &arr);
    js_gc_unprotect(vm, &sealed);
    js_gc_unprotect(vm, &frozen);
    js_context_free(ctx);
    js_vm_free(vm);
}

/* ---- growth is a safe point ------------------------------------------------ */

static void test_gc(void) {
    JsVmConfig cfg = {0};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);

    /*
     * Pushing grows the element storage, and growing can collect. The value
     * being pushed is often a freshly allocated cell held nowhere else, so it
     * has to survive the collection its own insertion may trigger. Enough
     * pushes of fresh strings to force many growths, then check they are all
     * still there and still strings.
     */
    JsValue arr = js_array_new(ctx, 0);
    js_gc_protect(vm, &arr);
    for (uint32_t i = 0; i < 2000; i++) {
        uint16_t unit = (uint16_t)('a' + (i % 26));
        JsValue s = js_string_new(vm, &unit, 1);
        if (!js_array_push(vm, arr, s)) {
            ok("push survived growth", false);
            break;
        }
    }
    ok("2000 fresh strings pushed", js_array_length(arr) == 2000);
    bool all_strings = true;
    for (uint32_t i = 0; i < js_array_length(arr); i++)
        if (!js_is_string(js_array_get(arr, i))) { all_strings = false; break; }
    ok("…and every one survived the growths", all_strings);

    js_gc_unprotect(vm, &arr);
    js_context_free(ctx);
    js_vm_free(vm);
}

int main(void) {
    test_reading();
    test_building();
    test_limits();
    test_gc();
    if (checks_failed) {
        fprintf(stderr, "%d/%d array checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d array checks passed\n", checks_run);
    return 0;
}
