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

typedef enum { RUN_OK, RUN_COMPILE_ERR, RUN_RUNTIME_ERR, RUN_HARNESS_ERR } RunStatus;

typedef struct {
    JsVmConfig cfg;      /* zeroed = defaults */
    uint64_t fuel;       /* 0 = unlimited */
    double set_global;   /* if set_global_name, predefine this global */
    const char *set_global_name;
} RunOpts;

/* Runs src; returns ToString of the result/error as malloc'd ASCII. */
static RunStatus run_src_opts(const char *src, const RunOpts *opts, char **out) {
    CountAlloc ca = {0, 0};
    JsVmConfig cfg = opts ? opts->cfg : (JsVmConfig){0};
    cfg.realloc_fn = count_realloc;
    cfg.alloc_ud = &ca;
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    if (!vm || !ctx)
        return RUN_HARNESS_ERR;
    if (opts && opts->fuel)
        js_context_set_fuel(ctx, opts->fuel);
    if (opts && opts->set_global_name) {
        JsValue g = js_context_globals(ctx);
        JsValue k = js_atom(vm, (const uint16_t[]){0}, 0);
        (void)k;
        size_t nlen = strlen(opts->set_global_name);
        uint16_t name[64];
        for (size_t i = 0; i < nlen; i++)
            name[i] = (uint16_t)(unsigned char)opts->set_global_name[i];
        JsValue key = js_atom(vm, name, nlen);
        js_gc_protect(vm, &key);
        js_object_set(vm, g, key, js_number(opts->set_global));
        js_gc_unprotect(vm, &key);
    }

    size_t len = strlen(src);
    uint16_t *u = malloc(len * sizeof(uint16_t));
    for (size_t i = 0; i < len; i++)
        u[i] = (uint16_t)(unsigned char)src[i];

    RunStatus status;
    const char *err_msg;
    uint32_t err_pos;
    JsValue fn = js_compile_module(ctx, u, len, &err_msg, &err_pos);
    if (!js_is_function(fn)) {
        *out = strdup(err_msg ? err_msg : "compile error");
        status = RUN_COMPILE_ERR;
    } else {
        js_gc_protect(vm, &fn);
        JsValue result;
        bool ok = js_run_module(ctx, fn, &result);
        js_gc_protect(vm, &result);
        JsValue str = js_to_string(ctx, result);
        size_t slen;
        const uint16_t *sunits = js_string_units(str, &slen);
        char *buf = malloc(slen + 1);
        for (size_t i = 0; i < slen; i++)
            buf[i] = sunits[i] < 128 ? (char)sunits[i] : '?';
        buf[slen] = '\0';
        *out = buf;
        status = ok ? RUN_OK : RUN_RUNTIME_ERR;
        js_gc_unprotect(vm, &result);
        js_gc_unprotect(vm, &fn);
    }
    free(u);
    js_vm_free(vm);
    checks_run++;
    if (ca.net_bytes != 0 || ca.live_allocs != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL leak in: %s (net=%ld allocs=%ld)\n", src,
                ca.net_bytes, ca.live_allocs);
    }
    return status;
}

static void expect_result(const char *src, const char *expected) {
    char *out;
    RunStatus st = run_src_opts(src, NULL, &out);
    checks_run++;
    if (st != RUN_OK) {
        checks_failed++;
        fprintf(stderr, "FAIL expected success\n  source: %s\n  got(%d): %s\n",
                src, (int)st, out);
    } else {
        checks_run++;
        if (strcmp(out, expected) != 0) {
            checks_failed++;
            fprintf(stderr, "FAIL wrong result\n  source:   %s\n  expected: %s\n  actual:   %s\n",
                    src, expected, out);
        }
    }
    free(out);
}

static void expect_error(const char *src, RunStatus want, const char *substr) {
    char *out;
    RunStatus st = run_src_opts(src, NULL, &out);
    checks_run++;
    if (st != want) {
        checks_failed++;
        fprintf(stderr, "FAIL expected status %d got %d\n  source: %s\n  out: %s\n",
                (int)want, (int)st, src, out);
    } else {
        checks_run++;
        if (!strstr(out, substr)) {
            checks_failed++;
            fprintf(stderr, "FAIL wrong error\n  source: %s\n  wanted: %s\n  got:    %s\n",
                    src, substr, out);
        }
    }
    free(out);
}

static void test_arithmetic(void) {
    expect_result("1 + 2 * 3;", "7");
    expect_result("(1 + 2) * 3;", "9");
    expect_result("7 % 3;", "1");
    expect_result("2 ** 10;", "1024");
    expect_result("10 / 4;", "2.5");
    expect_result("1 / 0;", "Infinity");
    expect_result("-1 / 0;", "-Infinity");
    expect_result("0 / 0;", "NaN");
    expect_result("0.1 + 0.2;", "0.30000000000000004");
    expect_result("1 / 3;", "0.3333333333333333");
    expect_result("1e21;", "1e+21");
    expect_result("0.000001;", "0.000001");
    expect_result("1e-7;", "1e-7");
    expect_result("-0;", "0");
    expect_result("1e308 * 10;", "Infinity");
}

static void test_strings(void) {
    expect_result("'a' + 1;", "a1");
    expect_result("1 + '2';", "12");
    expect_result("'x' + null + undefined + true;", "xnullundefinedtrue");
    expect_result("let x = 5; `v=${x + 1}!`;", "v=6!");
    expect_result("`${'a'}${'b'}`;", "ab");
    expect_result("'abc'.length;", "3");
    expect_result("'abc'[1];", "b");
    expect_result("'b' < 'c';", "true");
    expect_result("'abc' < 'abd';", "true");
    expect_result("' 42 ' * 1;", "42");
    expect_result("'0x10' * 1;", "16");
    expect_result("'' * 1;", "0");
    expect_result("'4a' * 1;", "NaN");
    expect_result("-'5';", "-5");
}

static void test_coercions_equality(void) {
    expect_result("'1' == 1;", "true");
    expect_result("'1' === 1;", "false");
    expect_result("null == undefined;", "true");
    expect_result("null === undefined;", "false");
    expect_result("NaN === NaN;", "false");
    expect_result("'a' === 'a';", "true");
    expect_result("0 === -0;", "true");
    expect_result("true == 1;", "true");
    expect_result("[] == '';", "true");
    expect_result("+[];", "0");
    expect_result("+true;", "1");
    expect_result("~0;", "-1");
    expect_result("!0;", "true");
    expect_result("typeof undefined;", "undefined");
    expect_result("typeof 1;", "number");
    expect_result("typeof 'a';", "string");
    expect_result("typeof {};", "object");
    expect_result("typeof null;", "object");
    expect_result("typeof not_declared_anywhere;", "undefined");
    expect_result("void 42;", "undefined");
}

static void test_bitwise(void) {
    expect_result("(5 & 3) + (5 | 3) + (5 ^ 3);", "14");
    expect_result("1 << 3;", "8");
    expect_result("-16 >> 2;", "-4");
    expect_result("-16 >>> 28;", "15");
    expect_result("2 ** 0.5 > 1.414 && 2 ** 0.5 < 1.415;", "true");
}

static void test_scoping(void) {
    expect_result("let a = 1; { let a = 2; } a;", "1");
    expect_result("let a = 1; { let b = a + 1; a = b; } a;", "2");
    expect_result("const c = 41; c + 1;", "42");
    expect_error("{ x; let x = 1; }", RUN_RUNTIME_ERR, "before initialization");
    expect_error("const c = 1; c = 2;", RUN_COMPILE_ERR, "constant");
    expect_error("let d = 1; let d = 2;", RUN_COMPILE_ERR, "already been declared");
    expect_error("x_undeclared = 1;", RUN_RUNTIME_ERR, "is not defined");
    expect_error("missing_global + 1;", RUN_RUNTIME_ERR, "missing_global is not defined");
}

static void test_control_flow(void) {
    expect_result("let s = 0; let i = 0; while (i < 5) { s += i; i++; } s;", "10");
    expect_result("let s = 0; for (let i = 0; i < 10; i++) s += i; s;", "45");
    expect_result("let n = 0; do { n++; } while (n < 3) n;", "3");
    expect_result("let n = 0; for (let i = 0; i < 10; i++) {"
                  " if (i % 2) continue; if (i > 6) break; n += i; } n;", "12");
    expect_result("let x = 5; let r = ''; if (x > 3) r = 'big'; else r = 'small'; r;",
                  "big");
    expect_result("outer: { let done = false; for (let i = 0; i < 3; i++) {"
                  " for (let j = 0; j < 3; j++) { if (j == 1) continue; } } } 'ok';",
                  "ok");
    expect_result("let r = 0; outer: for (let i = 0; i < 5; i++) {"
                  " for (let j = 0; j < 5; j++) { if (i * j > 3) break outer; r++; } } r;",
                  "9");
    expect_result("let t = ''; switch (2) { case 1: t += 'a'; case 2: t += 'b';"
                  " case 3: t += 'c'; break; default: t += 'd'; } t;", "bc");
    expect_result("let t = ''; switch ('1') { case 1: t = 'num'; break;"
                  " default: t = 'none'; } t;", "none");
    expect_result("let s = 0; for (const v of [1, 2, 3]) s += v; s;", "6");
    expect_result("let r = ''; for (const c of 'ab') r += c + '.'; r;", "a.b.");
    expect_result("let s = ''; for (const [k, v] of [['a', 1], ['b', 2]])"
                  " s += k + v; s;", "a1b2");
}

static void test_objects_arrays(void) {
    expect_result("let o = {a: 1, ['b' + 1]: 2}; o.a + o.b1;", "3");
    expect_result("let o = {x: {y: 1}}; o.x.y += 2; o.x.y;", "3");
    expect_result("let a = [1, 2, 3]; a[1] = 5; a[0] + a[1] + a.length;", "9");
    expect_result("let a = []; a[3] = 1; a.length;", "4");
    expect_result("let a = [1, ...[2, 3], 4]; a.length + '' + a[2];", "43");
    expect_result("let o = {...{a: 1, b: 2}, c: 3}; o.a + o.b + o.c;", "6");
    expect_result("let o = {a: 1}; delete o.a; typeof o.a;", "undefined");
    expect_result("let o = {a: 1}; 'a' in o;", "true");
    expect_result("let o = {a: 1}; 'b' in o;", "false");
    expect_result("0 in [7];", "true");
    expect_result("[1, 2] + '';", "1,2");
    expect_result("[[1, 2], 3] + '';", "1,2,3");
    expect_result("let a = [1]; a.length = 0; a.length;", "0");
    expect_result("let o = {}; o[1] = 'x'; o['1'];", "x");
    expect_result("let n = {a: {b: {c: 42}}}; n['a']['b'].c;", "42");
    expect_result("let shorthand = 7; let o = {shorthand}; o.shorthand;", "7");
}

static void test_destructuring(void) {
    expect_result("let [a, , b = 10] = [1, 2]; a + b;", "11");
    expect_result("let {p, q: {r} = {r: 5}} = {p: 2}; p + r;", "7");
    expect_result("let a, b; [a, b] = [1, 2]; a + b;", "3");
    expect_result("let o = {}; [o.x, o.y] = [3, 4]; o.x * o.y;", "12");
    expect_result("let {m = 1, n = 2} = {m: 10}; m + n;", "12");
    expect_result("let [[x], [y]] = [[1], [2]]; x + y;", "3");
}

static void test_logical_optional(void) {
    expect_result("null ?? 'd';", "d");
    expect_result("0 ?? 'd';", "0");
    expect_result("0 || 'x';", "x");
    expect_result("'' && 'y';", "");
    expect_result("let o = null; typeof o?.a;", "undefined");
    expect_result("let o = null; o?.a.b.c;", "undefined");
    expect_result("let o = {a: {b: 2}}; o?.a?.b;", "2");
    expect_result("let v = null; v ?\?= 5; v;", "5");
    expect_result("let v = 1; v ?\?= 5; v;", "1");
    expect_result("let v = 1; v &&= 7; v;", "7");
    expect_result("let v = 0; v ||= 9; v;", "9");
    expect_result("let o = {n: null}; o.n ?\?= 3; o.n;", "3");
    expect_result("let o = {n: 5}; o['n'] &&= 6; o.n;", "6");
    expect_result("true ? 'y' : 'n';", "y");
    expect_result("let x = (1, 2, 3); x;", "3");
}

static void test_update_ops(void) {
    expect_result("let i = 5; i++ + i;", "11");
    expect_result("let i = 5; ++i + i;", "12");
    expect_result("let i = 5; i--; i;", "4");
    expect_result("let o = {n: 5}; o.n++ + o.n;", "11");
    expect_result("let o = {n: 5}; ++o.n + o.n;", "12");
    expect_result("let a = [5]; a[0]++ + a[0];", "11");
    expect_result("let s = '5'; s++; s;", "6");
}

static void test_errors(void) {
    expect_error("null.x;", RUN_RUNTIME_ERR, "TypeError");
    expect_error("undefined.foo;", RUN_RUNTIME_ERR, "reading 'foo'");
    expect_error("let o; o.a.b;", RUN_RUNTIME_ERR, "TypeError");
    expect_error("throw 'boom';", RUN_RUNTIME_ERR, "boom");
    expect_error("let x = 1; x.y = 2;", RUN_RUNTIME_ERR, "primitive");
    expect_error("1 in 2;", RUN_RUNTIME_ERR, "requires an object");
    expect_error("for (const x of 42) {}", RUN_RUNTIME_ERR, "not iterable");
    /* phase gates still ahead */
    expect_error("let r = /a/;", RUN_COMPILE_ERR, "phase 10");
    expect_error("import 'm';", RUN_COMPILE_ERR, "module loader");
    expect_error("let x = await y;", RUN_RUNTIME_ERR, "y is not defined");
}

static void test_functions(void) {
    expect_result("function add(a, b) { return a + b; } add(2, 3);", "5");
    expect_result("const sq = x => x * x; sq(6);", "36");
    expect_result("let f = function(n) { return n + 1; }; f(9);", "10");
    expect_result("(x => x + 1)(41);", "42");
    expect_result("function f() { return; } typeof f();", "undefined");
    expect_result("function f() {} typeof f;", "function");
    /* default and rest params */
    expect_result("function g(a, b = 10) { return a + b; } g(5);", "15");
    expect_result("function g(a, b = 10) { return a + b; } g(5, 6);", "11");
    expect_result("function sum(...xs) { let s = 0; for (const x of xs) s += x; return s; }"
                  " sum(1, 2, 3, 4);", "10");
    expect_result("function h(a, ...rest) { return a + '/' + rest.length; } h(1, 2, 3);",
                  "1/2");
    /* destructuring params */
    expect_result("function p({x, y}) { return x + y; } p({x: 3, y: 4});", "7");
    expect_result("function q([a, b]) { return a * b; } q([5, 6]);", "30");
    /* recursion */
    expect_result("function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); } fact(5);", "120");
    expect_result("function fib(n) { return n < 2 ? n : fib(n-1) + fib(n-2); } fib(10);", "55");
    /* hoisting: callable before its declaration */
    expect_result("let r = early(); function early() { return 7; } r;", "7");
    /* methods */
    expect_result("let o = {n: 3, get() { return this.n; }}; o.get();", "3");
    expect_result("let o = {v: 10, inc() { this.v++; return this.v; }}; o.inc(); o.inc();",
                  "12");
    /* spread call */
    expect_result("function add3(a, b, c) { return a + b + c; } add3(...[1, 2, 3]);", "6");
}

static void test_closures(void) {
    expect_result("function counter() { let n = 0; return () => ++n; }"
                  " let c = counter(); c(); c(); c();", "3");
    expect_result("function make(x) { return () => x; } let f = make(42); f();", "42");
    expect_result("function adder(a) { return b => a + b; } adder(10)(5);", "15");
    /* independent closure instances */
    expect_result("function mk() { let n = 0; return () => ++n; }"
                  " let a = mk(), b = mk(); a(); a(); b(); a() + '/' + b();", "3/2");
    /* per-iteration loop bindings */
    expect_result("let fns = []; for (let i = 0; i < 3; i++) fns[i] = () => i;"
                  " fns[0]() + '' + fns[1]() + fns[2]();", "012");
    expect_result("let fns = []; for (const v of [10, 20, 30]) fns[fns.length] = () => v;"
                  " fns[0]() + '/' + fns[2]();", "10/30");
    /* shared upvalue between two closures */
    expect_result("function pair() { let n = 0; return [() => ++n, () => n]; }"
                  " let [inc, get] = pair(); inc(); inc(); get();", "2");
    /* nested closures capturing multiple levels */
    expect_result("function a(x) { return function b(y) { return function c(z)"
                  " { return x + y + z; }; }; } a(1)(2)(3);", "6");
    /* arrow captures lexical this */
    expect_result("let o = {v: 5, f() { let g = () => this.v; return g(); }}; o.f();", "5");
}

static void test_exceptions(void) {
    expect_result("let r; try { throw 'x'; } catch (e) { r = 'caught ' + e; } r;", "caught x");
    expect_result("let r = 0; try { r = 1; } catch { r = 2; } r;", "1");
    expect_result("let log = ''; try { log += 'a'; throw 1; } catch { log += 'b'; }"
                  " finally { log += 'c'; } log;", "abc");
    expect_result("let log = ''; try { log += 'a'; } finally { log += 'b'; } log;", "ab");
    /* finally runs on return */
    expect_result("function f() { try { return 'r'; } finally { } } f();", "r");
    expect_result("let log = ''; function f() { try { return 1; } finally { log += 'f'; } }"
                  " f(); log;", "f");
    /* exception propagates across a call */
    expect_result("function boom() { throw 'deep'; } let r;"
                  " try { boom(); } catch (e) { r = e; } r;", "deep");
    /* runtime TypeError is catchable */
    expect_result("let r; try { null.x; } catch (e) { r = typeof e === 'string'; } r;", "true");
    /* finally runs even when catch rethrows */
    expect_result("let log = ''; try { try { throw 1; } catch { log += 'c'; throw 2; }"
                  " finally { log += 'f'; } } catch (e) { log += 'o' + e; } log;", "cfo2");
    /* nested try, inner catches */
    expect_result("let r = ''; try { try { throw 'a'; } catch { r += '1'; } r += '2'; }"
                  " catch { r += 'X'; } r;", "12");
    /* break through finally */
    expect_result("let log = ''; for (let i = 0; i < 3; i++) {"
                  " try { if (i === 1) break; log += i; } finally { log += 'f'; } } log;",
                  "0ff");
    /* throw an object */
    expect_result("let r; try { throw {code: 42}; } catch (e) { r = e.code; } r;", "42");
    /* Error-like propagation with message */
    expect_error("throw 'top-level';", RUN_RUNTIME_ERR, "top-level");
}

static void test_fuel(void) {
    RunOpts opts = {0};
    opts.fuel = 10000;
    char *out;
    RunStatus st = run_src_opts("while (true) {}", &opts, &out);
    CHECK(st == RUN_RUNTIME_ERR);
    CHECK(strstr(out, "budget") != NULL);
    free(out);

    /* enough fuel: completes */
    opts.fuel = 1000000;
    st = run_src_opts("let s = 0; for (let i = 0; i < 100; i++) s += i; s;", &opts, &out);
    CHECK(st == RUN_OK);
    CHECK(strcmp(out, "4950") == 0);
    free(out);
}

static void test_heap_limit(void) {
    RunOpts opts = {0};
    opts.cfg.heap_limit = 200 * 1024;
    char *out;
    RunStatus st = run_src_opts(
        "let s = 'x'; while (true) { s = s + s; }", &opts, &out);
    CHECK(st == RUN_RUNTIME_ERR);
    CHECK(strstr(out, "memory") != NULL || strstr(out, "too long") != NULL);
    free(out);
}

static void test_gc_stress_run(void) {
    RunOpts opts = {0};
    opts.cfg.gc_stress = true;
    char *out;
    RunStatus st = run_src_opts(
        "let parts = []; for (let i = 0; i < 50; i++) { parts[i] = 'v' + i; }"
        "let all = ''; for (const p of parts) all += p + ';';"
        "all.length;", &opts, &out);
    CHECK(st == RUN_OK);
    CHECK(strcmp(out, "190") == 0); /* 10*2 + 40*3 chars + 50 semicolons */
    free(out);
}

static void test_host_globals(void) {
    RunOpts opts = {0};
    opts.set_global_name = "answer";
    opts.set_global = 42;
    char *out;
    RunStatus st = run_src_opts("answer / 2;", &opts, &out);
    CHECK(st == RUN_OK);
    CHECK(strcmp(out, "21") == 0);
    free(out);

    st = run_src_opts("answer = answer + 1; answer;", &opts, &out);
    CHECK(st == RUN_OK);
    CHECK(strcmp(out, "43") == 0);
    free(out);
}

static void test_completion_value(void) {
    expect_result("1; 2; 3;", "3");
    expect_result("let x = 9;", "undefined"); /* declarations don't update it */
    expect_result("42; let y = 1;", "42");
    expect_result("if (true) { 'taken'; }", "taken");
}

int main(void) {
    test_arithmetic();
    test_strings();
    test_coercions_equality();
    test_bitwise();
    test_scoping();
    test_control_flow();
    test_objects_arrays();
    test_destructuring();
    test_logical_optional();
    test_update_ops();
    test_errors();
    test_functions();
    test_closures();
    test_exceptions();
    test_fuel();
    test_heap_limit();
    test_gc_stress_run();
    test_host_globals();
    test_completion_value();

    if (checks_failed) {
        fprintf(stderr, "%d/%d exec checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d exec checks passed\n", checks_run);
    return 0;
}
