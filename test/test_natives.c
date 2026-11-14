/*
 * The public native-function API: js_native_new as a value (a method on an object, with `this`),
 * js_native_userdata inside a native (including a nested call), and the error constructors
 * js_error_new_ascii / js_error_new_utf16 (kind, name, message, long messages, catchable by guest code).
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

/* a method: returns this.base + arg0 + (userdata as a number) */
static int g_ud_a = 100, g_ud_b = 1000;
static int g_inner_ud = 0;
static bool native_add(JsContext *ctx, JsValue this_val, const JsValue *args, int argc, JsValue *result) {
    JsVm *vm = js_context_vm(ctx);
    double base = js_is_object(this_val) ? js_get_number(js_object_get(vm, this_val, atom(vm, "base"))) : 0;
    double arg = argc > 0 && js_is_number(args[0]) ? js_get_number(args[0]) : 0;
    int *ud = js_native_userdata(ctx);
    *result = js_number(base + arg + (ud ? *ud : 0));
    return true;
}
/* a native that calls back into a guest function while recording whose userdata it sees afterwards */
static bool native_outer(JsContext *ctx, JsValue this_val, const JsValue *args, int argc, JsValue *result) {
    (void)this_val;
    int *ud_before = js_native_userdata(ctx);
    JsValue out;
    if (argc > 0 && js_is_function(args[0]))
        js_call(ctx, args[0], js_undefined(), NULL, 0, &out);
    int *ud_after = js_native_userdata(ctx);
    g_inner_ud = (ud_before == ud_after && ud_after) ? *ud_after : -1;
    *result = js_undefined();
    return true;
}
/* a native that throws a RangeError with a long message */
static bool native_fail(JsContext *ctx, JsValue this_val, const JsValue *args, int argc, JsValue *result) {
    (void)this_val; (void)args; (void)argc;
    char msg[400];
    memset(msg, 'x', sizeof msg - 1);
    msg[sizeof msg - 1] = 0;
    memcpy(msg, "too far: ", 9);
    *result = js_error_new_ascii(ctx, JS_ERROR_RANGE, msg);
    return false;
}

/* a host constructor: sets this.v = arg0, and records whether it was called through `new` */
static int g_construct_calls;
static bool native_point(JsContext *ctx, JsValue this_val, const JsValue *args, int argc, JsValue *result) {
    JsVm *vm = js_context_vm(ctx);
    if (js_native_is_construct_call(ctx)) {
        g_construct_calls++;
        js_object_set(vm, this_val, atom(vm, "v"), argc > 0 ? args[0] : js_undefined());
    }
    *result = js_undefined();
    return true;
}
/* a host constructor that returns an object of its own */
static bool native_make(JsContext *ctx, JsValue this_val, const JsValue *args, int argc, JsValue *result) {
    (void)this_val; (void)args; (void)argc;
    JsValue o = js_object_new(ctx);
    js_object_set(js_context_vm(ctx), o, atom(js_context_vm(ctx), "own"), js_bool(true));
    *result = o;
    return true;
}

/* A host native is a constructor with a `prototype`, as through V8's embedding API, unless the
 * host turns that off */
static void test_native_constructors(void) {
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    JsValue globals = js_context_globals(ctx);
    js_object_set(vm, globals, atom(vm, "Point"), js_native_new(ctx, "Point", native_point, NULL));
    js_object_set(vm, globals, atom(vm, "Make"), js_native_new(ctx, "Make", native_make, NULL));
    JsValue plain = js_native_new(ctx, "plain", native_point, NULL);
    js_gc_protect(vm, &plain);
    CHECK(js_native_set_constructor(ctx, plain, false), "a host native can stop being a constructor");
    js_object_set(vm, globals, atom(vm, "plain"), plain);
    JsValue maxv = js_object_get(vm, js_object_get(vm, globals, atom(vm, "Math")), atom(vm, "max"));
    CHECK(!js_native_set_constructor(ctx, maxv, true), "a builtin's kind is not the host's to change");

    const char *src =
        "Point.prototype.norm = function () { return this.v * 2; };\n"
        "const p = new Point(21);\n"
        "Point(5);\n"
        "let e1 = '';\n"
        "try { new plain(); } catch (e) { e1 = e.message; }\n"
        "[p.norm(), p instanceof Point, p.constructor === Point, Object.keys(Point.prototype).join(),\n"
        " Point.hasOwnProperty('prototype'), plain.hasOwnProperty('prototype'), 'prototype' in plain,\n"
        " e1, new Make().own === true, new Make() instanceof Make].join('|')";
    size_t len;
    uint16_t *u = to_u16(src, &len);
    const char *em = NULL; uint32_t ep = 0;
    JsValue fn = js_compile_module(ctx, u, len, &em, &ep);
    CHECK(js_is_function(fn), em ? em : "compiles");
    js_gc_protect(vm, &fn);
    JsValue p = js_run_module(ctx, fn);
    CHECK(js_promise_state(p) == 1, "runs");
    CHECK(str_is(js_promise_result(p), "42|true|true|norm|true|false|false|plain is not a constructor|true|false"),
          "new on host natives");
    CHECK(g_construct_calls == 1, "js_native_is_construct_call tells new from a call");
    CHECK(!js_native_is_construct_call(ctx), "no construct call outside a native");
    js_gc_unprotect(vm, &fn);
    js_gc_unprotect(vm, &plain);
    free(u);
    js_vm_free(vm);
}

static void test_native_values(void) {
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    JsValue globals = js_context_globals(ctx);
    /* an object with a native method, installed through the public API only */
    JsValue obj = js_object_new(ctx);
    js_gc_protect(vm, &obj);
    js_object_set(vm, obj, atom(vm, "base"), js_number(5));
    JsValue add = js_native_new(ctx, "add", native_add, &g_ud_a);
    CHECK(js_is_function(add), "js_native_new gives a function value");
    js_object_set(vm, obj, atom(vm, "add"), add);
    js_object_set(vm, globals, atom(vm, "calc"), obj);
    JsValue add2 = js_native_new(ctx, "add2", native_add, &g_ud_b);
    js_object_set(vm, globals, atom(vm, "add2"), add2);
    JsValue outer = js_native_new(ctx, "outer", native_outer, &g_ud_a);
    js_object_set(vm, globals, atom(vm, "outer"), outer);
    JsValue fail = js_native_new(ctx, "fail", native_fail, NULL);
    js_object_set(vm, globals, atom(vm, "fail"), fail);

    const char *src =
        "const a = calc.add(1);\n"            /* this.base 5 + 1 + ud 100 */
        "const b = add2(1);\n"                /* no this.base, ud 1000 */
        "outer(() => add2(0));\n"             /* nested: inner sees its own ud, outer sees its own again */
        "let caught = null;\n"
        "try { fail(); } catch (e) { caught = e; }\n"
        "[a, b, caught.name, caught.message.length, caught.message.slice(0, 9)]";
    size_t len;
    uint16_t *u = to_u16(src, &len);
    const char *em = NULL; uint32_t ep = 0;
    JsValue fn = js_compile_module(ctx, u, len, &em, &ep);
    CHECK(js_is_function(fn), em ? em : "compiles");
    js_gc_protect(vm, &fn);
    JsValue p = js_run_module(ctx, fn);
    CHECK(js_promise_state(p) == 1, "runs");
    JsValue r = js_promise_result(p);
    CHECK(js_is_array(r) && js_array_length(r) == 5, "result array");
    if (js_is_array(r) && js_array_length(r) == 5) {
        CHECK(js_get_number(js_array_get(r, 0)) == 106, "method sees this and its userdata");
        CHECK(js_get_number(js_array_get(r, 1)) == 1001, "global native sees its own userdata");
        CHECK(str_is(js_array_get(r, 2), "RangeError"), "error kind -> name");
        CHECK(js_get_number(js_array_get(r, 3)) == 399, "long message kept whole");
        CHECK(str_is(js_array_get(r, 4), "too far: "), "message text");
    }
    CHECK(g_inner_ud == 100, "userdata restored after a nested js_call");
    CHECK(js_native_userdata(ctx) == NULL, "no native running outside a call");
    /* the constructors directly */
    JsValue e = js_error_new_ascii(ctx, JS_ERROR_TYPE, "bad");
    js_gc_protect(vm, &e);
    /* `name` lives on the class prototype (js_object_get reads own properties only); ToString shows it */
    CHECK(js_is_object(e) && str_is(js_to_string(ctx, e), "TypeError: bad"), "TypeError name via ToString");
    CHECK(str_is(js_object_get(vm, e, atom(vm, "message")), "bad"), "TypeError message");
    CHECK(js_error_cause(ctx, e) == JS_CAUSE_GUEST, "a host error is a guest-class error");
    static const uint16_t wide[] = { 0x00e9, 0x0074, 0x00e9 }; /* été */
    JsValue e2 = js_error_new_utf16(ctx, JS_ERROR_PLAIN, wide, 3);
    CHECK(js_is_object(e2) && js_string_length(js_object_get(vm, e2, atom(vm, "message"))) == 3, "utf16 message");
    JsValue e3 = js_error_new_ascii(ctx, JS_ERROR_AGGREGATE, "all");
    js_gc_protect(vm, &e3);
    JsValue errs = js_is_object(e3) ? js_object_get(vm, e3, atom(vm, "errors")) : js_undefined();
    CHECK(str_is(js_to_string(ctx, e3), "AggregateError: all") && js_is_array(errs) && js_array_length(errs) == 0,
          "a host AggregateError has an empty errors array");
    JsValue e4 = js_error_new_ascii(ctx, JS_ERROR_URI, "u");
    CHECK(js_is_object(e4) && str_is(js_to_string(ctx, e4), "URIError: u"), "URIError name via ToString");
    js_gc_unprotect(vm, &e3);
    js_gc_unprotect(vm, &e);
    js_gc_unprotect(vm, &fn);
    js_gc_unprotect(vm, &obj);
    free(u);
    js_vm_free(vm);
}

int main(void) {
    test_native_values();
    test_native_constructors();
    printf("test_natives: %d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
