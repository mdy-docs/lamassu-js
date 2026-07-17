#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsvm.h"

static int checks_run;
static int checks_failed;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks_run++;                                                        \
        if (!(cond)) {                                                       \
            checks_failed++;                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

/* Counting allocator: proves exact old_size accounting and zero leaks. */
typedef struct {
    long net_bytes;
    long live_allocs;
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

static JsVm *new_vm(CountAlloc *ca, bool stress) {
    memset(ca, 0, sizeof *ca);
    JsVmConfig cfg = {
        .realloc_fn = count_realloc,
        .alloc_ud = ca,
        .gc_stress = stress,
    };
    return js_vm_new(&cfg);
}

static void check_no_leaks(CountAlloc *ca) {
    CHECK(ca->net_bytes == 0);
    CHECK(ca->live_allocs == 0);
}

/* ASCII -> UTF-16 helper (tests are host code; the engine never sees char*). */
static const uint16_t *U(const char *ascii) {
    static uint16_t bufs[8][256];
    static int rot;
    uint16_t *b = bufs[rot = (rot + 1) % 8];
    size_t i = 0;
    for (; ascii[i] && i < 255; i++)
        b[i] = (uint16_t)(unsigned char)ascii[i];
    b[i] = 0;
    return b;
}

static void test_values(void) {
    JsValue v = js_number(3.5);
    CHECK(js_is_number(v));
    CHECK(js_get_number(v) == 3.5);
    CHECK(!js_is_string(v) && !js_is_object(v) && !js_is_bool(v));
    CHECK(!js_is_undefined(v) && !js_is_null(v));

    CHECK(js_get_number(js_number(-1.5)) == -1.5);
    CHECK(js_get_number(js_number(0.0)) == 0.0);
    CHECK(signbit(js_get_number(js_number(-0.0))));
    CHECK(js_get_number(js_number(1e308)) == 1e308);
    CHECK(js_get_number(js_number(-1e308)) == -1e308);
    CHECK(js_is_number(js_number(INFINITY)));
    CHECK(js_get_number(js_number(-INFINITY)) == -INFINITY);

    JsValue nan = js_number(0.0 / 0.0);
    CHECK(js_is_number(nan));
    CHECK(isnan(js_get_number(nan)));
    CHECK(nan.bits == JS_CANONICAL_NAN); /* canonicalized */

    CHECK(js_is_undefined(js_undefined()));
    CHECK(js_is_null(js_null()));
    CHECK(!js_is_number(js_undefined()));
    CHECK(!js_is_number(js_null()));

    CHECK(js_is_bool(js_bool(true)) && js_get_bool(js_bool(true)));
    CHECK(js_is_bool(js_bool(false)) && !js_get_bool(js_bool(false)));

    CHECK(js_same_value(js_number(2.0), js_number(2.0)));
    CHECK(!js_same_value(js_number(2.0), js_number(3.0)));
    CHECK(!js_same_value(js_undefined(), js_null()));
}

static void test_strings(void) {
    CountAlloc ca;
    JsVm *vm = new_vm(&ca, false);
    CHECK(vm != NULL);

    JsValue s1 = js_string_new(vm, U("hello"), 5);
    CHECK(js_is_string(s1));
    CHECK(js_string_length(s1) == 5);
    size_t len;
    const uint16_t *units = js_string_units(s1, &len);
    CHECK(len == 5 && units && units[0] == 'h' && units[4] == 'o');

    JsValue s2 = js_string_new(vm, U("hello"), 5);
    CHECK(js_string_equals(s1, s2));
    CHECK(!js_same_value(s1, s2)); /* uninterned: distinct cells */
    CHECK(!js_string_equals(s1, js_string_new(vm, U("hellp"), 5)));
    CHECK(!js_string_equals(s1, js_string_new(vm, U("hell"), 4)));
    CHECK(!js_string_equals(s1, js_number(5)));

    JsValue a1 = js_atom(vm, U("hello"), 5);
    JsValue a2 = js_atom(vm, U("hello"), 5);
    CHECK(js_is_string(a1));
    CHECK(js_same_value(a1, a2)); /* interned: same cell */
    CHECK(js_string_equals(a1, s1));

    JsValue empty = js_string_new(vm, NULL, 0);
    CHECK(js_is_string(empty) && js_string_length(empty) == 0);
    CHECK(js_string_equals(empty, js_atom(vm, NULL, 0)));

    /* Embedded NUL unit: length is authoritative, no terminator anywhere. */
    uint16_t raw[3] = {0x0041, 0x0000, 0x0042};
    JsValue nulstr = js_string_new(vm, raw, 3);
    CHECK(js_string_length(nulstr) == 3);
    units = js_string_units(nulstr, &len);
    CHECK(len == 3 && units[1] == 0 && units[2] == 0x0042);

    CHECK(js_string_units(js_number(1), &len) == NULL && len == 0);

    js_vm_free(vm);
    check_no_leaks(&ca);
}

static void test_objects(void) {
    CountAlloc ca;
    JsVm *vm = new_vm(&ca, false);

    JsValue o = js_object_new(vm);
    CHECK(js_is_object(o));
    CHECK(js_object_size(o) == 0);

    JsValue kx = js_atom(vm, U("x"), 1);
    CHECK(js_object_set(vm, o, kx, js_number(42)));
    CHECK(js_object_size(o) == 1);
    CHECK(js_get_number(js_object_get(vm, o, kx)) == 42);

    /* Lookup with a fresh uninterned key string finds the same property. */
    JsValue kx2 = js_string_new(vm, U("x"), 1);
    CHECK(js_get_number(js_object_get(vm, o, kx2)) == 42);

    /* Setting through an uninterned key interns it. */
    JsValue ky = js_string_new(vm, U("y"), 1);
    CHECK(js_object_set(vm, o, ky, js_bool(true)));
    CHECK(js_get_bool(js_object_get(vm, o, js_atom(vm, U("y"), 1))));

    CHECK(js_is_undefined(js_object_get(vm, o, js_atom(vm, U("missing"), 7))));
    /* A key that was never interned can't name a property. */
    CHECK(js_is_undefined(js_object_get(vm, o, js_string_new(vm, U("nope"), 4))));

    CHECK(js_object_set(vm, o, kx, js_number(43))); /* overwrite */
    CHECK(js_get_number(js_object_get(vm, o, kx)) == 43);
    CHECK(js_object_size(o) == 2);

    CHECK(js_object_delete(vm, o, kx));
    CHECK(js_is_undefined(js_object_get(vm, o, kx)));
    CHECK(!js_object_delete(vm, o, kx)); /* already gone */
    CHECK(js_object_size(o) == 1);

    /* Bad arguments. */
    CHECK(!js_object_set(vm, js_number(1), kx, js_number(2)));
    CHECK(!js_object_set(vm, o, js_number(1), js_number(2)));
    CHECK(js_is_undefined(js_object_get(vm, js_null(), kx)));
    CHECK(js_object_size(js_number(7)) == 0);

    /* Growth + tombstone churn: insert, delete half, reinsert. */
    char name[32];
    for (int i = 0; i < 500; i++) {
        snprintf(name, sizeof name, "k%d", i);
        JsValue k = js_atom(vm, U(name), strlen(name));
        CHECK(js_object_set(vm, o, k, js_number(i)));
    }
    for (int i = 0; i < 500; i += 2) {
        snprintf(name, sizeof name, "k%d", i);
        CHECK(js_object_delete(vm, o, js_atom(vm, U(name), strlen(name))));
    }
    for (int i = 0; i < 500; i += 2) {
        snprintf(name, sizeof name, "k%d", i);
        JsValue k = js_atom(vm, U(name), strlen(name));
        CHECK(js_object_set(vm, o, k, js_number(i + 1000)));
    }
    for (int i = 0; i < 500; i++) {
        snprintf(name, sizeof name, "k%d", i);
        JsValue got = js_object_get(vm, o, js_atom(vm, U(name), strlen(name)));
        CHECK(js_get_number(got) == (i % 2 == 0 ? i + 1000 : i));
    }
    CHECK(js_object_size(o) == 501); /* 500 + "y" */

    js_vm_free(vm);
    check_no_leaks(&ca);
}

static void test_gc_basic(void) {
    CountAlloc ca;
    JsVm *vm = new_vm(&ca, false);
    CHECK(js_gc_live_cells(vm) == 0);

    for (int i = 0; i < 10; i++)
        js_string_new(vm, U("garbage"), 7);
    CHECK(js_gc_live_cells(vm) == 10);
    js_gc_collect(vm);
    CHECK(js_gc_live_cells(vm) == 0);

    JsValue s = js_string_new(vm, U("keep"), 4);
    CHECK(js_gc_protect(vm, &s));
    js_string_new(vm, U("junk"), 4);
    js_gc_collect(vm);
    CHECK(js_gc_live_cells(vm) == 1);
    CHECK(js_string_equals(s, js_string_new(vm, U("keep"), 4)));
    js_gc_unprotect(vm, &s);
    js_gc_collect(vm);
    CHECK(js_gc_live_cells(vm) == 0);

    js_vm_free(vm);
    check_no_leaks(&ca);
}

static void test_gc_object_graph(void) {
    CountAlloc ca;
    JsVm *vm = new_vm(&ca, false);

    JsValue a = js_object_new(vm);
    js_gc_protect(vm, &a);
    JsValue b = js_object_new(vm);
    js_gc_protect(vm, &b);
    JsValue k = js_atom(vm, U("child"), 5);
    js_gc_protect(vm, &k);
    JsValue v = js_string_new(vm, U("payload"), 7);
    js_gc_protect(vm, &v);

    CHECK(js_object_set(vm, a, k, b));
    CHECK(js_object_set(vm, b, k, v));
    js_gc_unprotect(vm, &b);
    js_gc_unprotect(vm, &k);
    js_gc_unprotect(vm, &v);

    /* Everything reachable from a: a, b, atom "child", string "payload". */
    js_gc_collect(vm);
    CHECK(js_gc_live_cells(vm) == 4);
    JsValue got = js_object_get(vm, js_object_get(vm, a, js_atom(vm, U("child"), 5)),
                                js_atom(vm, U("child"), 5));
    CHECK(js_string_equals(got, js_string_new(vm, U("payload"), 7)));

    /* Sever b: only a and the key atom (still a's key) stay. */
    CHECK(js_object_set(vm, a, js_atom(vm, U("child"), 5), js_undefined()));
    js_gc_collect(vm);
    CHECK(js_gc_live_cells(vm) == 2);

    js_gc_unprotect(vm, &a);
    js_gc_collect(vm);
    CHECK(js_gc_live_cells(vm) == 0);

    js_vm_free(vm);
    check_no_leaks(&ca);
}

static void test_gc_cycle(void) {
    CountAlloc ca;
    JsVm *vm = new_vm(&ca, false);

    JsValue a = js_object_new(vm);
    js_gc_protect(vm, &a);
    JsValue b = js_object_new(vm);
    js_gc_protect(vm, &b);
    JsValue k = js_atom(vm, U("next"), 4);
    js_gc_protect(vm, &k);

    CHECK(js_object_set(vm, a, k, b));
    CHECK(js_object_set(vm, b, k, a)); /* cycle */

    js_gc_unprotect(vm, &a);
    js_gc_unprotect(vm, &b);
    js_gc_unprotect(vm, &k);
    js_gc_collect(vm);
    CHECK(js_gc_live_cells(vm) == 0); /* mark-sweep eats cycles */

    js_vm_free(vm);
    check_no_leaks(&ca);
}

static void test_atoms_weak(void) {
    CountAlloc ca;
    JsVm *vm = new_vm(&ca, false);

    /* An unreferenced atom dies at collection and can be re-interned. */
    js_atom(vm, U("ephemeral"), 9);
    CHECK(js_gc_live_cells(vm) == 1);
    js_gc_collect(vm);
    CHECK(js_gc_live_cells(vm) == 0);
    JsValue again = js_atom(vm, U("ephemeral"), 9);
    CHECK(js_is_string(again));
    CHECK(js_gc_live_cells(vm) == 1);

    /* An atom in use as a property key survives and stays canonical. */
    JsValue o = js_object_new(vm);
    js_gc_protect(vm, &o);
    JsValue kept = js_atom(vm, U("kept"), 4);
    js_gc_protect(vm, &kept);
    CHECK(js_object_set(vm, o, kept, js_number(1)));
    js_gc_unprotect(vm, &kept);
    js_gc_collect(vm);
    CHECK(js_same_value(js_atom(vm, U("kept"), 4), kept));
    CHECK(js_get_number(js_object_get(vm, o, js_atom(vm, U("kept"), 4))) == 1);

    js_gc_unprotect(vm, &o);
    js_vm_free(vm);
    check_no_leaks(&ca);
}

static void test_context(void) {
    CountAlloc ca;
    JsVm *vm = new_vm(&ca, false);

    JsContext *ctx = js_context_new(vm);
    CHECK(ctx != NULL);
    JsValue g = js_context_globals(ctx);
    CHECK(js_is_object(g));

    /* Context creation installs the whole stdlib, so the live-cell count is
     * a large fixed baseline; assert relative deltas instead. */
    js_gc_collect(vm);
    size_t base = js_gc_live_cells(vm);
    CHECK(base > 50); /* stdlib present */

    JsValue k = js_atom(vm, U("answer"), 6);
    js_gc_protect(vm, &k);
    CHECK(js_object_set(vm, g, k, js_number(42)));
    js_gc_unprotect(vm, &k);

    js_gc_collect(vm);
    CHECK(js_gc_live_cells(vm) == base + 1); /* + the "answer" atom */
    CHECK(js_get_number(js_object_get(vm, js_context_globals(ctx),
                                      js_atom(vm, U("answer"), 6))) == 42);

    JsContext *ctx2 = js_context_new(vm);
    CHECK(ctx2 != NULL);
    js_context_free(ctx);
    js_gc_collect(vm);
    /* ctx2's stdlib remains; shared interned atoms mean this is <= base+1. */
    CHECK(js_gc_live_cells(vm) >= base);

    js_context_free(ctx2);
    js_gc_collect(vm);
    CHECK(js_gc_live_cells(vm) == 0);

    js_vm_free(vm);
    check_no_leaks(&ca);
}

/* gc_stress collects at every safe point: the rooting contract under fire. */
static void test_gc_stress(void) {
    CountAlloc ca;
    JsVm *vm = new_vm(&ca, true);

    JsValue o = js_object_new(vm);
    CHECK(js_is_object(o));
    js_gc_protect(vm, &o);

    char name[32], val[32];
    for (int i = 0; i < 200; i++) {
        snprintf(name, sizeof name, "k%d", i);
        snprintf(val, sizeof val, "v%d", i);
        JsValue k = js_atom(vm, U(name), strlen(name));
        CHECK(js_is_string(k));
        js_gc_protect(vm, &k);
        JsValue v = js_string_new(vm, U(val), strlen(val));
        CHECK(js_is_string(v));
        js_gc_protect(vm, &v);
        CHECK(js_object_set(vm, o, k, v));
        js_gc_unprotect(vm, &v);
        js_gc_unprotect(vm, &k);
    }

    CHECK(js_object_size(o) == 200);
    for (int i = 0; i < 200; i++) {
        snprintf(name, sizeof name, "k%d", i);
        snprintf(val, sizeof val, "v%d", i);
        JsValue k = js_atom(vm, U(name), strlen(name));
        js_gc_protect(vm, &k);
        JsValue got = js_object_get(vm, o, k);
        JsValue want = js_string_new(vm, U(val), strlen(val));
        CHECK(js_string_equals(got, want));
        js_gc_unprotect(vm, &k);
    }
    js_gc_collect(vm); /* flush the last iteration's garbage `want` string */
    CHECK(js_gc_live_cells(vm) == 401); /* o + 200 keys + 200 values */

    js_gc_unprotect(vm, &o);
    js_gc_collect(vm);
    CHECK(js_gc_live_cells(vm) == 0);

    js_vm_free(vm);
    check_no_leaks(&ca);
}

static void test_gc_auto_threshold(void) {
    CountAlloc ca;
    memset(&ca, 0, sizeof ca);
    JsVmConfig cfg = {
        .realloc_fn = count_realloc,
        .alloc_ud = &ca,
        .gc_threshold = 2048, /* tiny: force automatic collections */
    };
    JsVm *vm = js_vm_new(&cfg);

    JsValue keep = js_string_new(vm, U("survivor"), 8);
    js_gc_protect(vm, &keep);
    for (int i = 0; i < 2000; i++)
        js_string_new(vm, U("shortlived-garbage-string"), 25);
    /* Auto-GC must have run many times; garbage can't all be live. */
    CHECK(js_gc_live_cells(vm) < 2000);
    CHECK(js_string_equals(keep, js_string_new(vm, U("survivor"), 8)));
    js_gc_unprotect(vm, &keep);

    js_vm_free(vm);
    check_no_leaks(&ca);
}

static void test_vm_lifecycle(void) {
    CountAlloc ca;
    JsVm *vm = new_vm(&ca, false);
    CHECK(vm != NULL);
    CHECK(js_vm_allocated_bytes(vm) > 0);
    js_vm_free(vm);
    check_no_leaks(&ca);

    /* NULL config: libc default allocator. */
    JsVm *vm2 = js_vm_new(NULL);
    CHECK(vm2 != NULL);
    JsValue s = js_string_new(vm2, U("hi"), 2);
    CHECK(js_string_length(s) == 2);
    js_vm_free(vm2);

    js_vm_free(NULL); /* harmless */
}

int main(void) {
    test_values();
    test_strings();
    test_objects();
    test_gc_basic();
    test_gc_object_graph();
    test_gc_cycle();
    test_atoms_weak();
    test_context();
    test_gc_stress();
    test_gc_auto_threshold();
    test_vm_lifecycle();

    if (checks_failed) {
        fprintf(stderr, "%d/%d checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d checks passed\n", checks_run);
    return 0;
}
