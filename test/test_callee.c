/*
 * A failed call names its callee: "s.fillet is not a function", "foo is not a function",
 * "a[0] is not a function", "n is not a constructor", as V8 words them; an expression with no
 * readable name shows its value, as V8 does ("3 is not a function"). The names survive a
 * bytecode-cache round trip. Literals read as V8 prints them ("null is not a function").
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
    { "function h() {} h.bind(null)()();", "h.bind(...)(...) is not a function" },
    { "const o = {}; o?.m();", "o?.m is not a function" },
    { "const s = {}, args = [1, 2]; s.f(...args);", "s.f is not a function" },
    { "const n = 3; new n();", "n is not a constructor" },
    { "const m = { K: 1 }; new m.K(...[1]);", "m.K is not a constructor" },
    { "(1 + 2)();", "3 is not a function" },
    { "null();", "null is not a function" },
    { "\"abc\"();", "\"abc\" is not a function" },
    { "(0x10)();", "16 is not a function" },
    { "const f = () => 1; new f();", "f is not a constructor" },
    /* the same table names what was iterated, destructured or read too early */
    { "const o = {}; for (const k of o) {}", "o is not iterable" },
    { "const o = { p: 1 }; for (const k of o.p) {}", "o.p is not iterable" },
    { "const o = {}; const [x] = o;", "o is not iterable" },
    { "const [x] = 5;", "5 is not iterable" },
    { "const n = 5; [...n];", "n is not iterable" },
    { "const n = 5; Math.max(...n);", "n is not iterable" },
    { "const [[a]] = [5];", "5 is not iterable" },
    { "let o; const { a } = o;", "Cannot destructure property 'a' of 'o' as it is undefined." },
    { "const o = {}; const { a } = o.b;", "Cannot destructure property 'a' of 'o.b' as it is undefined." },
    { "const { a } = null;", "Cannot destructure property 'a' of 'null' as it is null." },
    { "function f({ a }) {} f();", "Cannot destructure property 'a' of 'undefined' as it is undefined." },
    { "const { a: { b } } = {};", "Cannot read properties of undefined (reading 'b')" },
    { "let x = 1; ({ x } = null);", "Cannot destructure property 'x' of 'null' as it is null." },
    { "null.x;", "Cannot read properties of null (reading 'x')" },
    { "const a = []; a[0].b;", "Cannot read properties of undefined (reading 'b')" },
    { "const o = {}; o.a.b = 1;", "Cannot set properties of undefined (setting 'b')" },
    { "const o = {}; o.a[0] = 1;", "Cannot set properties of undefined (setting '0')" },
    { "const c = 1; c = 2;", "Assignment to constant variable." },
    { "function f() { return K; } f(); const K = 1;", "ReferenceError: Cannot access 'K' before initialization" },
    { "const g = () => K2; g(); const K2 = 1;", "ReferenceError: Cannot access 'K2' before initialization" },
    { "const q = q + 1;", "ReferenceError: Cannot access 'q' before initialization" },
    { "{ h(); let v = 1; function h() { return v; } }", "ReferenceError: Cannot access 'v' before initialization" },
    /* writes before the declaration has run: direct, compound, destructuring, from a closure */
    { "x = 1; let x;", "ReferenceError: Cannot access 'x' before initialization" },
    { "x2++; let x2 = 0;", "ReferenceError: Cannot access 'x2' before initialization" },
    { "{ [d] = [1]; let d; }", "ReferenceError: Cannot access 'd' before initialization" },
    { "{ ({ d2 } = { d2: 1 }); let d2; }", "ReferenceError: Cannot access 'd2' before initialization" },
    { "for (d3 of [1]); let d3;", "ReferenceError: Cannot access 'd3' before initialization" },
    { "function g() { n = 5; } g(); let n = 0;", "ReferenceError: Cannot access 'n' before initialization" },
    { "const s = () => { m = 1; }; s(); let m;", "ReferenceError: Cannot access 'm' before initialization" },
    /* a const in the TDZ: the ReferenceError comes before the TypeError */
    { "c = 1; const c = 2;", "ReferenceError: Cannot access 'c' before initialization" },
    { "function f() { c2 = 1; } f(); const c2 = 2;", "ReferenceError: Cannot access 'c2' before initialization" },
    /* a later case of a switch skips the declaration in an earlier one */
    { "switch (1) { case 0: let a = 1; break; case 1: a = 2; }", "ReferenceError: Cannot access 'a' before initialization" },
    { "switch (1) { case 0: let b = 1; break; case 1: b; }", "ReferenceError: Cannot access 'b' before initialization" },
    { "switch (1) { case 0: let e = 1; break; case 1: (() => { e = 2; })(); }", "ReferenceError: Cannot access 'e' before initialization" },
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
        /* a TypeError unless the case names its kind */
        bool named = strncmp(CASES[i].message, "ReferenceError: ", 16) == 0;
        snprintf(want, sizeof want, "%s%s", named ? "" : "TypeError: ", CASES[i].message);
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
