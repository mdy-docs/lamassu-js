/*
 * A failed call names its callee: "s.fillet is not a function", "foo is not a function",
 * "a[0] is not a function", "n is not a constructor", as V8 words them; an expression with no
 * readable name stays "value is not a function". The names survive a bytecode-cache round trip.
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

static void to_ascii(JsValue v, char *out, size_t cap) {
    size_t n = 0;
    const uint16_t *u = js_string_units(v, &n);
    size_t k = 0;
    for (size_t i = 0; u && i < n && k + 1 < cap; i++)
        out[k++] = u[i] < 128 ? (char)u[i] : '?';
    out[k] = 0;
}

/* Each case: a body whose last statement throws; the caught message is the completion value. */
static const struct { const char *body; const char *message; } CASES[] = {
    { "const s = { a: 1 }; s.fillet(2);", "s.fillet is not a function" },
    { "const foo = 3; foo();", "foo is not a function" },
    { "const a = { b: { c: 1 } }; a.b.c();", "a.b.c is not a function" },
    { "const a = [1]; a[0]();", "a[0] is not a function" },
    { "const a = {}; a[\"k\"]();", "a[\"k\"] is not a function" },
    { "const a = {}, k = 'x'; a[k]();", "a[k] is not a function" },
    { "const a = {}, k = 1; a[k + 1]();", "a[...] is not a function" },
    { "const f = () => 1; f()();", "f(...) is not a function" },
    { "const g = () => ({}); g().x();", "g(...).x is not a function" },
    { "const o = { go: 1, run: function () { return this.go(); } }; o.run();", "this.go is not a function" },
    { "function h() {} h.call(null);", "h.call is not a function" },
    { "const o = {}; o?.m();", "o?.m is not a function" },
    { "const s = {}, args = [1, 2]; s.f(...args);", "s.f is not a function" },
    { "const n = 3; new n();", "n is not a constructor" },
    { "const m = { K: 1 }; new m.K(...[1]);", "m.K is not a constructor" },
    { "(1 + 2)();", "value is not a function" },
};

static bool message_of(JsContext *ctx, JsValue fn, char *msg, size_t cap) {
    JsVm *vm = js_context_vm(ctx);
    js_gc_protect(vm, &fn);
    JsValue p = js_run_module(ctx, fn);
    js_gc_unprotect(vm, &fn);
    if (js_promise_state(p) != 1) return false;
    to_ascii(js_promise_result(p), msg, cap);
    return true;
}

int main(void) {
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        char src[512], msg[256], what[700];
        snprintf(src, sizeof src, "let msg = 'no error';\ntry { %s } catch (e) { msg = e.name + ': ' + e.message; }\nmsg", CASES[i].body);
        char want[300];
        snprintf(want, sizeof want, "TypeError: %s", CASES[i].message);
        size_t len;
        uint16_t *u = to_u16(src, &len);
        const char *em = NULL; uint32_t ep = 0;
        JsValue fn = js_compile_module(ctx, u, len, &em, &ep);
        CHECK(js_is_function(fn), em ? em : "compiles");
        if (js_is_function(fn)) {
            /* straight from the compiler */
            bool ran = message_of(ctx, fn, msg, sizeof msg);
            snprintf(what, sizeof what, "%s -> \"%s\" (want \"%s\")", CASES[i].body, msg, want);
            CHECK(ran && strcmp(msg, want) == 0, what);
            /* through a bytecode cache */
            uint8_t *bc = NULL; size_t bc_len = 0;
            js_gc_protect(vm, &fn);
            bool ser = js_bytecode_serialize(ctx, fn, &bc, &bc_len);
            js_gc_unprotect(vm, &fn);
            CHECK(ser, "serializes");
            if (ser) {
                const char *lerr = NULL;
                JsValue lfn = js_bytecode_load(ctx, bc, bc_len, &lerr);
                js_bytecode_free(ctx, bc, bc_len);
                CHECK(js_is_function(lfn), lerr ? lerr : "loads");
                if (js_is_function(lfn)) {
                    ran = message_of(ctx, lfn, msg, sizeof msg);
                    snprintf(what, sizeof what, "bytecode: %s -> \"%s\"", CASES[i].body, msg);
                    CHECK(ran && strcmp(msg, want) == 0, what);
                }
            }
        }
        free(u);
    }
    js_vm_free(vm);
    printf("test_callee: %d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
