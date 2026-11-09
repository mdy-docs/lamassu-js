/*
 * Host objects: js_host_object_new gives guest code an ordinary object that owns a host pointer;
 * the class finalizer runs once when the collector frees it and for every survivor when the VM is
 * freed; js_host_object_ptr checks the class; external bytes pace collection.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lamassu.h"
#include "lamassu_compile.h"

static int checks_run, checks_failed;
#define CHECK(cond, what)                                                       \
    do {                                                                        \
        checks_run++;                                                           \
        if (!(cond)) {                                                          \
            checks_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, what);      \
        }                                                                       \
    } while (0)

static uint16_t *to_u16(const char *s, size_t *len) {
    size_t n = strlen(s);
    uint16_t *u = malloc((n + 1) * sizeof(uint16_t));
    for (size_t i = 0; i < n; i++)
        u[i] = (uint16_t)(unsigned char)s[i];
    u[n] = 0;
    *len = n;
    return u;
}

static bool str_is(JsValue v, const char *ascii) {
    size_t n;
    const uint16_t *u = js_string_units(v, &n);
    if (!u || n != strlen(ascii))
        return false;
    for (size_t i = 0; i < n; i++)
        if (u[i] != (uint16_t)(unsigned char)ascii[i])
            return false;
    return true;
}

static JsValue atom(JsVm *vm, const char *s) {
    size_t n;
    uint16_t *u = to_u16(s, &n);
    JsValue v = js_atom(vm, u, n);
    free(u);
    return v;
}

/* A resource: a heap int. The finalizer frees it and counts, and checks it is not run twice. */
static int g_made, g_finalized, g_double_free, g_most_live;
static void finalize_box(void *ptr, void *ud) {
    int *counter = ud;
    int *box = ptr;
    if (*box < 0) g_double_free++;
    *box = -1;
    free(box);
    (*counter)++;
}
static JsHostClass g_box = { "Box", finalize_box, &g_finalized };
static int g_other_finalized;
static JsHostClass g_other = { "Other", finalize_box, &g_other_finalized };
static size_t g_external = 0; /* bytes each new box claims */

/* box(n): a Box holding n */
static bool native_box(JsContext *ctx, JsValue this_val, const JsValue *args, int argc, JsValue *result) {
    (void)this_val;
    int *p = malloc(sizeof *p);
    *p = argc > 0 && js_is_number(args[0]) ? (int)js_get_number(args[0]) : 0;
    JsValue v = js_host_object_new(ctx, &g_box, p, g_external);
    if (js_is_undefined(v)) { free(p); *result = js_error_new_ascii(ctx, JS_ERROR_RANGE, "out of memory"); return false; }
    g_made++;
    int live = g_made - g_finalized;
    if (live > g_most_live) g_most_live = live;
    *result = v;
    return true;
}
/* other(): a host object of another class */
static bool native_other(JsContext *ctx, JsValue this_val, const JsValue *args, int argc, JsValue *result) {
    (void)this_val; (void)args; (void)argc;
    int *p = malloc(sizeof *p);
    *p = 7;
    *result = js_host_object_new(ctx, &g_other, p, 0);
    return true;
}
/* unbox(v): the Box's number, or -1 for anything that is not a Box */
static bool native_unbox(JsContext *ctx, JsValue this_val, const JsValue *args, int argc, JsValue *result) {
    (void)ctx; (void)this_val;
    int *p = argc > 0 ? js_host_object_ptr(args[0], &g_box) : NULL;
    *result = js_number(p ? *p : -1);
    return true;
}

static JsValue run(JsContext *ctx, const char *src) {
    JsVm *vm = js_context_vm(ctx);
    size_t len;
    uint16_t *u = to_u16(src, &len);
    const char *em = NULL;
    uint32_t ep = 0;
    JsValue fn = js_compile_module(ctx, u, len, &em, &ep);
    free(u);
    CHECK(js_is_function(fn), em ? em : "compiles");
    if (!js_is_function(fn)) return js_undefined();
    js_gc_protect(vm, &fn);
    JsValue p = js_run_module(ctx, fn);
    js_gc_unprotect(vm, &fn);
    CHECK(js_promise_state(p) == 1, "runs");
    return js_promise_result(p);
}

static void install(JsContext *ctx) {
    JsVm *vm = js_context_vm(ctx);
    JsValue globals = js_context_globals(ctx);
    js_object_set(vm, globals, atom(vm, "box"), js_native_new(ctx, "box", native_box, NULL));
    js_object_set(vm, globals, atom(vm, "other"), js_native_new(ctx, "other", native_other, NULL));
    js_object_set(vm, globals, atom(vm, "unbox"), js_native_new(ctx, "unbox", native_unbox, NULL));
}

/* 1. Guest code sees an ordinary object; the class guards the pointer; garbage is finalized. */
static void test_basics(void) {
    g_made = g_finalized = g_other_finalized = g_double_free = 0;
    g_external = 0;
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    install(ctx);
    JsValue r = run(ctx,
        "const keep = box(42);\n"
        "keep.label = 'kept';\n"
        "globalThis.kept = keep;\n"
        "for (let i = 0; i < 1000; i++) box(i);\n"           /* garbage */
        "const o = other();\n"
        "[typeof keep, unbox(keep), keep.label, Object.keys(keep).join(), JSON.stringify(keep),\n"
        " unbox(o), unbox({}), unbox(3), keep.hasOwnProperty('label'), Object.getPrototypeOf(keep) === Object.prototype]");
    js_gc_protect(vm, &r);
    CHECK(js_is_array(r) && js_array_length(r) == 10, "result array");
    if (js_is_array(r) && js_array_length(r) == 10) {
        CHECK(str_is(js_array_get(r, 0), "object"), "typeof is object");
        CHECK(js_get_number(js_array_get(r, 1)) == 42, "the native reads its pointer back");
        CHECK(str_is(js_array_get(r, 2), "kept"), "expando properties");
        CHECK(str_is(js_array_get(r, 3), "label"), "Object.keys sees only the expando");
        CHECK(str_is(js_array_get(r, 4), "{\"label\":\"kept\"}"), "JSON sees an ordinary object");
        CHECK(js_get_number(js_array_get(r, 5)) == -1, "another class is not a Box");
        CHECK(js_get_number(js_array_get(r, 6)) == -1, "a plain object is not a Box");
        CHECK(js_get_number(js_array_get(r, 7)) == -1, "a number is not a Box");
        CHECK(js_get_bool(js_array_get(r, 8)), "hasOwnProperty");
        CHECK(js_get_bool(js_array_get(r, 9)), "Object.prototype");
    }
    js_gc_unprotect(vm, &r);
    js_gc_collect(vm);
    CHECK(g_made == 1001, "made 1001 boxes");
    CHECK(g_finalized == 1000, "every unreachable box finalized after a collection; the kept one lives");
    CHECK(g_other_finalized == 1, "the other-class object (a dead local) finalized");
    /* the kept one is still usable */
    JsValue k = js_object_get(vm, js_context_globals(ctx), atom(vm, "kept"));
    int *kp = js_host_object_ptr(k, &g_box);
    CHECK(kp && *kp == 42, "survivor's pointer intact");
    CHECK(js_host_object_ptr(k, &g_other) == NULL, "wrong class gives NULL");
    js_vm_free(vm);
    CHECK(g_finalized == 1001, "freeing the VM finalizes the survivors");
    CHECK(g_double_free == 0, "no finalizer runs twice");
}

/* 2. External bytes pace collection: big host memory, tiny engine memory, collected as it goes. */
static void test_pacing(void) {
    g_made = g_finalized = g_double_free = g_most_live = 0;
    g_external = 4u << 20; /* each box claims 4 MB of host memory */
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    install(ctx);
    run(ctx, "for (let i = 0; i < 2000; i++) box(i);");
    CHECK(g_made == 2000, "made 2000 boxes");
    CHECK(g_finalized > 1900, "collected during the run, not only at the end");
    CHECK(g_most_live < 200, "host memory never piled up (a few cycles' worth live at most)");
    CHECK(js_gc_external_bytes(vm) == (size_t)(g_made - g_finalized) * g_external, "external bytes track the live boxes");
    /* set_external moves the account */
    JsValue v;
    native_box(ctx, js_undefined(), NULL, 0, &v);
    js_gc_protect(vm, &v);
    size_t before = js_gc_external_bytes(vm);
    CHECK(js_host_object_set_external(vm, v, &g_box, 10), "set_external on a Box");
    CHECK(js_gc_external_bytes(vm) == before - g_external + 10, "account updated");
    CHECK(!js_host_object_set_external(vm, v, &g_other, 10), "set_external checks the class");
    js_gc_unprotect(vm, &v);
    js_gc_collect(vm);
    CHECK(js_gc_external_bytes(vm) == 0, "nothing held once everything is collected");
    js_vm_free(vm);
    CHECK(g_finalized == g_made, "every box finalized");
    CHECK(g_double_free == 0, "no finalizer runs twice");
    g_external = 0;
}

int main(void) {
    test_basics();
    test_pacing();
    printf("test_host: %d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
