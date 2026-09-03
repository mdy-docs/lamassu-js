#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lamassu.h"
#include "lamassu_compile.h"

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
    if (!vm || !ctx) {
        /* Set *out and release the VM even here: a heap_limit low enough to
         * starve context creation used to leave the caller reading an
         * uninitialized pointer. */
        *out = strdup("harness: could not create vm/context");
        js_vm_free(vm);
        return RUN_HARNESS_ERR;
    }
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
        JsValue p = js_run_module(ctx, fn);
        int st = js_promise_state(p);
        bool ok = st == 0 || st == 1;
        JsValue result = js_promise_result(p);
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
    /* out-of-[0, UINT32_MAX] lengths must be rejected before the double ->
     * uint32_t cast, not fed to it (that cast is UB out of range) */
    expect_error("let a = [1]; a.length = -1;", RUN_RUNTIME_ERR, "RangeError");
    expect_error("let a = [1]; a.length = 1e20;", RUN_RUNTIME_ERR, "RangeError");
    expect_error("let a = [1]; a.length = 1.5;", RUN_RUNTIME_ERR, "RangeError");
    /* same UB class in computed property access (value_to_index) — a huge
     * or negative numeric key just isn't a valid index, not a crash */
    expect_result("let a = [1, 2, 3]; a[1e20];", "undefined");
    expect_result("let a = [1, 2, 3]; a[-1];", "undefined");
    expect_result("let a = []; a[1e20] = 'x'; a.length;", "0");
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
    /* regex landed (phase 10): a literal now evaluates to an object */
    expect_result("let r = /a+/; r.source;", "a+");
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

static void test_new(void) {
    /* basic construction: fresh object, `this` bound, fields visible */
    expect_result("function P(x, y) { this.x = x; this.y = y; }"
                  " let p = new P(3, 4); p.x + '/' + p.y;", "3/4");
    /* no-parens form */
    expect_result("function E() { this.tag = 'e'; } new E().tag;", "e");
    /* each instance gets its own object */
    expect_result("function P(x) { this.x = x; } let a = new P(1), b = new P(2);"
                  " a === b;", "false");
    expect_result("function P(x) { this.x = x; } let a = new P(1), b = new P(2);"
                  " a.x + '/' + b.x;", "1/2");
    /* fn.prototype: auto-created, shared, writable, backs inherited lookups */
    expect_result("function F() {} typeof F.prototype;", "object");
    expect_result("function F() {} F.prototype === F.prototype;", "true");
    expect_result("function F() {} F.prototype.constructor === F;", "true");
    expect_result("function C() { this.n = 0; }"
                  " C.prototype.inc = function() { this.n++; return this; };"
                  " new C().inc().inc().inc().n;", "3");
    expect_result("function C() {} C.prototype.greet = function() { return 'hi'; };"
                  " let a = new C(), b = new C(); a.greet() + b.greet();", "hihi");
    /* own property shadows an inherited one */
    expect_result("function C() {} C.prototype.v = 1;"
                  " let a = new C(); a.v = 2; a.v + '/' + new C().v;", "2/1");
    /* prototype-chain inheritance across constructors */
    expect_result("function Animal(n) { this.name = n; }"
                  " Animal.prototype.speak = function() { return this.name + ' speaks'; };"
                  " function Dog(n) { this.name = n; } Dog.prototype = new Animal('proto');"
                  " new Dog('Rex').speak();", "Rex speaks");
    /* constructor return-value override: object wins, primitive is discarded */
    expect_result("function W() { this.a = 1; return {b: 2}; }"
                  " let w = new W(); (w.a === undefined) + '/' + w.b;", "true/2");
    expect_result("function P() { this.a = 42; return 99; }"
                  " let r = new P(); r.a + '/' + (typeof r);", "42/object");
    /* member-expression callees and chaining */
    expect_result("let ns = {}; ns.P = function(x) { this.x = x; };"
                  " new ns.P(7).x;", "7");
    expect_result("function C() { this.n = 1; } new C().n + 1;", "2");
    /* spread constructor args */
    expect_result("function P(a, b, c) { this.s = a + b + c; }"
                  " new P(...[1, 2, 3]).s;", "6");
    /* not constructible */
    expect_error("let f = x => x; new f();", RUN_RUNTIME_ERR, "not a constructor");
    expect_error("async function f() {} new f();", RUN_RUNTIME_ERR, "not a constructor");
    expect_error("new (5)();", RUN_RUNTIME_ERR, "not a constructor");
    expect_error("new NoSuchThing();", RUN_RUNTIME_ERR, "is not defined");
    /* assigning a non-object to .prototype is a harmless no-op */
    expect_result("function F() {} F.prototype = 5; typeof F.prototype;", "object");
}

/* Array/RegExp/Date/Map/Set instance methods are reached purely through a
 * real, script-visible X.prototype and a genuine [[Prototype]] chain (the
 * same mechanism `new Foo()` uses) — no hidden per-kind method table. */
static void test_real_prototypes(void) {
    expect_result("typeof Array.prototype;", "object");
    expect_result("Object.getPrototypeOf([]) === Array.prototype;", "true");
    expect_result("[].constructor === Array;", "true");
    expect_result("Array.prototype.push === [].push;", "true");
    /* arrays built deep inside other builtins get the real prototype too */
    expect_result("Object.getPrototypeOf([1,2].map(x => x)) === Array.prototype;", "true");
    expect_result("Object.getPrototypeOf(JSON.parse('[1]')) === Array.prototype;", "true");
    /* monkey-patching the real prototype reaches existing instances */
    expect_result("Array.prototype.double = function() { return this.map(x => x * 2); };"
                  " [1,2,3].double().join(',');", "2,4,6");
    expect_result("let a = [1]; Array.prototype.tag = 'x'; a.tag;", "x");
    /* Map/Set: same deal */
    expect_result("Object.getPrototypeOf(new Map()) === Map.prototype;", "true");
    expect_result("new Map().constructor === Map;", "true");
    expect_result("Map.prototype.get === new Map().get;", "true");
    expect_result("Object.getPrototypeOf(new Set()) === Set.prototype;", "true");
    expect_result("new Set().constructor === Set;", "true");
    /* Object.setPrototypeOf rewires lookup for plain objects too */
    expect_result("function Base() {} Base.prototype.hi = function() { return 'hi'; };"
                  " let o = {}; Object.setPrototypeOf(o, Base.prototype); o.hi();", "hi");
    expect_result("function Base() {} let o = {};"
                  " Object.setPrototypeOf(o, Base.prototype);"
                  " Object.getPrototypeOf(o) === Base.prototype;", "true");
    /* getPrototypeOf on values with no [[Prototype]] slot: null, not a crash */
    expect_result("Object.getPrototypeOf(5);", "null");
    expect_result("Object.getPrototypeOf(function(){});", "null");
    /* user-defined `new` prototype chains are unaffected by any of this */
    expect_result("function Animal(n) { this.name = n; }"
                  " Animal.prototype.speak = function() { return this.name + ' speaks'; };"
                  " new Animal('Rex').speak();", "Rex speaks");
    expect_error("Object.getPrototypeOf(null);", RUN_RUNTIME_ERR, "TypeError");
    expect_error("Object.setPrototypeOf({}, 5);", RUN_RUNTIME_ERR, "TypeError");
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
    /* runtime TypeError is catchable, and is a real TypeError object */
    expect_result("let r; try { null.x; } catch (e) { r = typeof e === 'object' && e.name === 'TypeError'"
                  " && e.message === \"cannot read properties of undefined or null (reading 'x')\"; } r;",
                  "true");
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

/*
 * Concatenation continues the left operand's FNV fold instead of rehashing the
 * whole result (js_units_hash_from). That is only sound if a built string
 * hashes identically to the same text written as a literal — otherwise it
 * would fail to intern, and property lookup, Map and Set would all quietly
 * miss. These check the hash through its consumers rather than directly.
 */
static void test_concat_hash_interning(void) {
    expect_result("('ab' + 'c') === 'abc'", "true");
    expect_result("('a' + 'b' + 'c') === ('ab' + 'c')", "true");
    expect_result("('' + 'z') === 'z'", "true");
    expect_result("('z' + '') === 'z'", "true");
    /* property keys */
    expect_result("const o = { abc: 42 }; o['ab' + 'c']", "42");
    expect_result("const o = {}; o['x' + 'y'] = 7; o.xy", "7");
    /* Map and Set hash strings the same way */
    expect_result("const m = new Map(); m.set('hello', 1); m.get('hel' + 'lo')", "1");
    expect_result("const s = new Set(['one']); s.has('o' + 'ne')", "true");
    /* a long string built by repeated append, against the same text built
     * in different-sized pieces */
    expect_result("let a = ''; for (let i = 0; i < 500; i++) a += 'ab';"
                  "let b = ''; for (let i = 0; i < 250; i++) b += 'abab';"
                  "a === b",
                  "true");
    expect_result("let a = ''; for (let i = 0; i < 500; i++) a += 'ab';"
                  "let b = ''; for (let i = 0; i < 250; i++) b += 'abab';"
                  "const o = {}; o[a] = 'yes'; o[b]",
                  "yes");
    /* non-ASCII code units fold the same */
    expect_result("const o = {}; o['\u00e9' + '\u00fc'] = 1; o['\u00e9\u00fc']", "1");
}

/* Builds prefix + n copies of unit + suffix into a malloc'd string. */
static char *repeat_src(const char *prefix, const char *unit, int n, const char *suffix) {
    size_t pl = strlen(prefix), ul = strlen(unit), sl = strlen(suffix);
    char *b = malloc(pl + (size_t)n * ul + sl + 1);
    memcpy(b, prefix, pl);
    char *p = b + pl;
    for (int i = 0; i < n; i++) {
        memcpy(p, unit, ul);
        p += ul;
    }
    memcpy(p, suffix, sl);
    p[sl] = '\0';
    return b;
}

/*
 * Regressions for docs/runtime-audit-plan.md. Under the _asan build these also
 * assert no ASan use-after-free / UBSan report fired along the way.
 */
static void test_audit_regressions(void) {
    char *out, *src;
    RunStatus st;

    /* WS-C: deep '**' / nested 'new' hit the parser depth guard cleanly
     * (previously overflowed the C stack). */
    src = repeat_src("1", "**1", 400, ";");
    st = run_src_opts(src, NULL, &out);
    CHECK(st == RUN_COMPILE_ERR);
    CHECK(strstr(out, "nesting too deep") != NULL);
    free(out);
    free(src);
    src = repeat_src("", "new ", 400, "F();");
    st = run_src_opts(src, NULL, &out);
    CHECK(st == RUN_COMPILE_ERR);
    free(out);
    free(src);
    /* right-associative ** still evaluates correctly */
    st = run_src_opts("2 ** 3 ** 2;", NULL, &out);
    CHECK(st == RUN_OK);
    CHECK(strcmp(out, "512") == 0);
    free(out);

    /* WS-C: JSON.parse nesting is depth-capped and catchable, not a crash. */
    src = repeat_src("try { JSON.parse('", "[", 400, "'); 'ok'; } catch (e) { 'caught'; }");
    st = run_src_opts(src, NULL, &out);
    CHECK(st == RUN_OK);
    CHECK(strcmp(out, "caught") == 0);
    free(out);
    free(src);

    /* WS-C: native-mediated recursion throws RangeError instead of SIGSEGV. */
    st = run_src_opts("function f() { [1].forEach(f); } f();", NULL, &out);
    CHECK(st == RUN_RUNTIME_ERR);
    CHECK(strstr(out, "call stack") != NULL);
    free(out);

    /* P0: toFixed no longer overflows its digit buffer at the max precision. */
    st = run_src_opts("(0).toFixed(100).length;", NULL, &out);
    CHECK(st == RUN_OK);
    CHECK(strcmp(out, "102") == 0); /* "0." + 100 zeros */
    free(out);

    /* P0: sibling-scope scratch slot no longer aliases a live local. */
    st = run_src_opts("let o = {p:0};"
                      "{ let a = 99; o.p++; }"
                      "{ let b = 1; let c = 42; o.p++; c; }",
                      NULL, &out);
    CHECK(st == RUN_OK);
    CHECK(strcmp(out, "42") == 0);
    free(out);

    /* WS-A: numeric-cast corpus — no UB (UBSan build asserts), correct values. */
    st = run_src_opts("parseInt('100', Infinity);", NULL, &out);
    CHECK(st == RUN_OK);
    CHECK(strcmp(out, "100") == 0);
    free(out);
    st = run_src_opts("parseInt('10', -1);", NULL, &out); /* ToInt32 -> radix -1 -> NaN */
    CHECK(strcmp(out, "NaN") == 0);
    free(out);
    st = run_src_opts("JSON.stringify(5, null, Infinity) === '5' ? 'ok' : 'no';", NULL, &out);
    CHECK(strcmp(out, "ok") == 0);
    free(out);
    st = run_src_opts("let a = [1,2,3]; delete a[1e30]; delete a[-1]; a.length;", NULL, &out);
    CHECK(st == RUN_OK);
    CHECK(strcmp(out, "3") == 0);
    free(out);

    /* P2 spec fixes. */
    st = run_src_opts("Math.round(0.49999999999999994);", NULL, &out);
    CHECK(strcmp(out, "0") == 0);
    free(out);
    st = run_src_opts("1 / Math.round(-0.4);", NULL, &out); /* -0 -> -Infinity */
    CHECK(strcmp(out, "-Infinity") == 0);
    free(out);
    st = run_src_opts("parseFloat('5e');", NULL, &out);
    CHECK(strcmp(out, "5") == 0);
    free(out);
    expect_error("JSON.parse('01');", RUN_RUNTIME_ERR, "JSON");
    expect_error("JSON.parse('1.');", RUN_RUNTIME_ERR, "JSON");
    expect_error("JSON.parse('1e');", RUN_RUNTIME_ERR, "JSON");
    /* for-of continue: a closure captured before `continue` keeps its own
     * per-iteration binding. */
    st = run_src_opts("let fns = [];"
                      "for (const x of [1,2,3]) { fns.push(() => x); if (x < 3) continue; }"
                      "fns.map(f => f()).join(',');",
                      NULL, &out);
    CHECK(strcmp(out, "1,2,3") == 0);
    free(out);

    /* Map/Set value-keyed hash index: correctness incl. -0/NaN/object keys. */
    st = run_src_opts("let m = new Map(); for (let i = 0; i < 500; i++) m.set(i, i*2);"
                      "m.delete(100); m.get(200) + ',' + m.has(100) + ',' + m.size;",
                      NULL, &out);
    CHECK(st == RUN_OK);
    CHECK(strcmp(out, "400,false,499") == 0);
    free(out);
    st = run_src_opts("let m = new Map(); m.set(-0, 'z'); m.set(NaN, 'n');"
                      "m.get(0) + m.get(NaN) + ',' + m.has(-0);",
                      NULL, &out);
    CHECK(strcmp(out, "zn,true") == 0);
    free(out);
    st = run_src_opts("let s = new Set(); for (let i = 0; i < 200; i++) s.add(i % 10); s.size;",
                      NULL, &out);
    CHECK(strcmp(out, "10") == 0);
    free(out);

    /* WS-B: UAF sites, exercised under gc_stress (ASan build is the assertion). */
    RunOpts gs = {0};
    gs.cfg.gc_stress = true;
    st = run_src_opts("let o = {...'abcdef'}; Object.keys(o).length;", &gs, &out);
    CHECK(st == RUN_OK);
    CHECK(strcmp(out, "6") == 0);
    free(out);
    st = run_src_opts("Object.entries([10, 20, 30]).map(e => e[0] + ':' + e[1]).join(',');", &gs, &out);
    CHECK(st == RUN_OK);
    CHECK(strcmp(out, "0:10,1:20,2:30") == 0);
    free(out);
    st = run_src_opts("let a = [5,4,3,2,1];"
                      "a.sort((x, y) => { if (a.length > 2) a.length = 2; return x - y; });"
                      "'survived';",
                      &gs, &out);
    CHECK(st == RUN_OK);
    CHECK(strcmp(out, "survived") == 0);
    free(out);
    /* Default (comparator-less) sort compares elements by their string form.
     * Converting the second operand allocates, which is a GC safe point, so
     * the first operand's freshly-made string has to be rooted across it —
     * it was not, and the comparison then read a collected cell. Numbers make
     * every comparison allocate two strings, which is what exposes it. */
    st = run_src_opts("let a = [];"
                      "for (let i = 0; i < 40; i++) a.push(40 - i);"
                      "a.sort(); a[0] + ',' + a[39];",
                      &gs, &out);
    CHECK(st == RUN_OK);
    CHECK(strcmp(out, "1,9") == 0);
    free(out);

    /* A fiber is reachable from nothing between its allocation and the moment
     * the caller installs it as ctx->fiber, yet setting it up allocates: the
     * rest-parameter array is built while the fiber's own stack is half filled
     * in. Async calls spawn a fiber on that path, so this collected the fiber
     * mid-setup and read it back. */
    st = run_src_opts("async function f(...args) { return args.length; } f(1,2,3); 'ok';",
                      &gs, &out);
    CHECK(st == RUN_OK);
    CHECK(strcmp(out, "ok") == 0);
    free(out);

    /* WS-E: heap_limit now bounds bulk (non-cell) growth, not just cell
     * headers — a large array build is stopped instead of overrunning. */
    RunOpts hl = {0};
    hl.cfg.heap_limit = 200 * 1024;
    st = run_src_opts("let a = []; for (let i = 0; i < 1000000; i++) a[i] = i; a.length;", &hl, &out);
    CHECK(st == RUN_RUNTIME_ERR);
    CHECK(strstr(out, "memory") != NULL);
    free(out);
}

/* Regressions for the P2/P3 spec-lite, hardening, and latent-overflow items. */
static void test_audit_p2_p3(void) {
    /* ** requires a parenthesized unary left operand. */
    expect_error("-2 ** 2;", RUN_COMPILE_ERR, "**");
    expect_error("typeof x ** 2;", RUN_COMPILE_ERR, "**");
    expect_result("(-2) ** 2;", "4");
    expect_result("2 ** -2;", "0.25");
    expect_result("2 ** 3 ** 2;", "512"); /* right-assoc */

    /* Math.hypot: Inf/NaN priority and overflow-safe scaling. */
    expect_result("Math.hypot(3, 4);", "5");
    expect_result("Math.hypot(Infinity, NaN);", "Infinity");
    expect_result("Math.hypot(NaN, 2);", "NaN");
    expect_result("Math.hypot();", "0");
    expect_result("Math.hypot(1e200, 1e200) < Infinity;", "true"); /* no overflow */

    /* Number.prototype.toString(radix) no longer truncates large integers. */
    expect_result("(2 ** 100).toString(2).length;", "101");
    expect_result("(255).toString(16);", "ff");

    /* JSON.stringify throws on a cycle; skips undefined/function values. */
    expect_error("let a = {}; a.self = a; JSON.stringify(a);", RUN_RUNTIME_ERR, "circular");
    expect_error("let a = []; a.push(a); JSON.stringify(a);", RUN_RUNTIME_ERR, "circular");
    expect_result("JSON.stringify({b: 1, c: function(){}, d: undefined}) === '{\"b\":1}' ? 'ok' : 'no';",
                  "ok");

    /* `in` sees synthesized props. */
    expect_result("'size' in new Map();", "true");
    expect_result("'size' in new Set();", "true");
    expect_result("'source' in /x/;", "true");
    expect_result("'nope' in new Map();", "false");

    /* Date ISO parse range-validates each field. */
    expect_result("isNaN(new Date('2024-01-15T25:00:00Z').getTime());", "true");
    expect_result("isNaN(new Date('2024-01-15T24:00:00Z').getTime());", "false"); /* end-of-day */
    expect_result("isNaN(new Date('2024-13-01').getTime());", "true");
    expect_result("isNaN(new Date('2024-01-15T10:30:00+05:30').getTime());", "false");

    /* Spread call/new still works after the argc-overflow guard (a 16M-element
     * array to trip the cap is impractical here; verify the common path). */
    expect_result("function sum(...xs){ let s=0; for (const x of xs) s+=x; return s; }"
                  "let a=[]; for (let i=0;i<1000;i++) a[i]=i; sum(...a);",
                  "499500");
}

/* A native the guest can call to interrupt itself — stands in for the timer
 * thread or signal handler a host would really use. */
static bool native_stop_now(JsContext *ctx, JsValue this_val, const JsValue *args,
                            int argc, JsValue *result) {
    (void)this_val; (void)args; (void)argc;
    js_vm_interrupt(js_context_vm(ctx));
    *result = js_undefined();
    return true;
}

/* Runs `src` with a `stopNow()` native available. Returns the promise state and
 * writes the stringified result. */
static int run_with_stop(const char *src, char **out, bool clear_after) {
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    static const uint16_t name[] = {'s','t','o','p','N','o','w'};
    js_register_native(ctx, name, 7, native_stop_now, NULL);
    size_t len = strlen(src);
    uint16_t *u = malloc(len * sizeof(uint16_t));
    for (size_t i = 0; i < len; i++)
        u[i] = (uint16_t)(unsigned char)src[i];
    const char *em; uint32_t ep;
    JsValue fn = js_compile_module(ctx, u, len, &em, &ep);
    free(u);
    if (!js_is_function(fn)) { *out = strdup(em ? em : "compile error"); js_vm_free(vm); return -1; }
    js_gc_protect(vm, &fn);
    JsValue p = js_run_module(ctx, fn);
    js_gc_protect(vm, &p);
    js_run_jobs(ctx);
    int st = js_promise_state(p);
    if (clear_after)
        js_vm_clear_interrupt(vm);
    JsValue r = js_promise_result(p);
    js_gc_protect(vm, &r);
    JsValue s = js_to_string(ctx, r);
    size_t sl = 0;
    const uint16_t *su = js_string_units(s, &sl);
    char *buf = malloc(sl + 1);
    for (size_t i = 0; i < sl; i++) buf[i] = su[i] < 128 ? (char)su[i] : '?';
    buf[sl] = '\0';
    *out = buf;
    js_vm_free(vm);
    return st;
}

/*
 * A host maps the engine's three stops to different outcomes (503 for fuel,
 * 504 for a deadline, discard-the-VM for memory), so it must be able to tell
 * them apart without parsing message text. js_error_cause is that answer; a
 * guest-built error that merely says the same words is still JS_CAUSE_GUEST.
 */
static JsErrorCause cause_of(const char *src, uint64_t fuel, size_t heap_limit, bool stop,
                             char **text) {
    JsVmConfig cfg = {.heap_limit = heap_limit};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    static const uint16_t name[] = {'s','t','o','p','N','o','w'};
    js_register_native(ctx, name, 7, native_stop_now, NULL);
    size_t len = strlen(src);
    uint16_t *u = malloc(len * sizeof(uint16_t));
    for (size_t i = 0; i < len; i++)
        u[i] = (uint16_t)(unsigned char)src[i];
    const char *em; uint32_t ep;
    JsValue fn = js_compile_module(ctx, u, len, &em, &ep);
    free(u);
    js_gc_protect(vm, &fn);
    js_context_set_fuel(ctx, fuel);
    JsValue p = js_run_module(ctx, fn);
    js_gc_protect(vm, &p);
    js_run_jobs(ctx);
    if (stop)
        js_vm_clear_interrupt(vm);
    JsValue r = js_promise_result(p);
    js_gc_protect(vm, &r);
    JsErrorCause cause = js_promise_state(p) == 2 ? js_error_cause(ctx, r) : JS_CAUSE_GUEST;
    JsValue s = js_to_string(ctx, r);
    size_t sl = 0;
    const uint16_t *su = js_string_units(s, &sl);
    char *buf = malloc(sl + 1);
    for (size_t i = 0; i < sl; i++) buf[i] = su[i] < 128 ? (char)su[i] : '?';
    buf[sl] = '\0';
    *text = buf;
    js_vm_free(vm);
    return cause;
}

static void test_error_cause(void) {
    char *t;
    CHECK(cause_of("for (;;) {}", 500, 0, false, &t) == JS_CAUSE_BUDGET);
    CHECK(strcmp(t, "RangeError: execution budget exhausted") == 0);
    free(t);
    /* the guest's catch does not launder it */
    CHECK(cause_of("try { for (;;) {} } catch (e) { 'caught'; }", 500, 0, false, &t) == JS_CAUSE_BUDGET);
    free(t);
    CHECK(cause_of("stopNow(); while (true) {} 'never';", 0, 0, true, &t) == JS_CAUSE_INTERRUPT);
    CHECK(strcmp(t, "Error: execution interrupted") == 0);
    free(t);
    CHECK(cause_of("const a = []; for (;;) a.push({ k: 'v' + a.length, xs: [1, 2, 3] });",
                   0, 256 * 1024, false, &t) == JS_CAUSE_OOM);
    free(t);
    /* guest-raised, even with the same words */
    CHECK(cause_of("throw new RangeError('execution budget exhausted');", 0, 0, false, &t) == JS_CAUSE_GUEST);
    free(t);
    CHECK(cause_of("throw new Error('execution interrupted');", 0, 0, false, &t) == JS_CAUSE_GUEST);
    free(t);
    CHECK(cause_of("throw 'out of memory';", 0, 0, false, &t) == JS_CAUSE_GUEST);
    free(t);
    CHECK(cause_of("throw new Error('mine');", 0, 0, false, &t) == JS_CAUSE_GUEST);
    free(t);
    /* an ordinary engine error is a guest-visible mistake, not a stop */
    CHECK(cause_of("null.x;", 0, 0, false, &t) == JS_CAUSE_GUEST);
    free(t);
    /* a fulfilled run has no cause to speak of */
    CHECK(cause_of("1 + 1;", 0, 0, false, &t) == JS_CAUSE_GUEST);
    free(t);
    /* a host-built string with the same text is not the engine's prebuilt
     * OOM value: strings are classified by identity, never by text */
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    static const uint16_t oom[] = {'o','u','t',' ','o','f',' ','m','e','m','o','r','y'};
    JsValue s = js_string_new(vm, oom, 13);
    js_gc_protect(vm, &s);
    CHECK(js_error_cause(ctx, s) == JS_CAUSE_GUEST);
    CHECK(js_error_cause(ctx, js_number(1)) == JS_CAUSE_GUEST);
    CHECK(js_error_cause(ctx, js_undefined()) == JS_CAUSE_GUEST);
    js_vm_free(vm);
}

/*
 * js_vm_interrupt is the only bound on wall-clock: fuel counts instructions,
 * and a host cannot always map an acceptable running time onto a count. The
 * stop must also be unswallowable, or a guest defeats it with try/catch the
 * way it would defeat any ordinary throw.
 */
static void test_interrupt(void) {
    char *out;
    /* a bare infinite loop stops */
    int st = run_with_stop("stopNow(); while (true) {} 'never';", &out, false);
    CHECK(st == 2);
    CHECK(strstr(out, "interrupted") != NULL);
    free(out);

    /* ...and stays stopped through a catch, because the flag does not clear */
    st = run_with_stop("stopNow(); while (true) { try { 1; } catch (e) {} } 'never';",
                       &out, false);
    CHECK(st == 2);
    CHECK(strstr(out, "interrupted") != NULL);
    free(out);

    /* a finally block cannot outrun it either */
    st = run_with_stop("stopNow(); try { while (true) {} } finally { while (true) {} } 'never';",
                       &out, false);
    CHECK(st == 2);
    free(out);

    /* The drain stops too. The flag is set from INSIDE a job here, which is the
     * case that matters: a chain re-queueing itself would otherwise spin in
     * js_run_jobs forever, allocating a fiber per turn, with the module long
     * since finished and the host still blocked inside the call. */
    st = run_with_stop("let n = 0;"
                       "function f() { n++; if (n === 3) stopNow();"
                       "               Promise.resolve().then(f); }"
                       "Promise.resolve().then(f); 'queued';",
                       &out, false);
    CHECK(st == 1);
    CHECK(strcmp(out, "queued") == 0);
    free(out);

    /* a large sort is the longest a guest can stay inside one native, so it
     * polls the flag rather than making an interrupt wait for it */
    st = run_with_stop("let a = []; for (let i = 0; i < 4000; i++) a.push(4000 - i);"
                       "a.sort(function (x, y) { stopNow(); return x - y; }); 'never';",
                       &out, false);
    CHECK(st == 2);
    CHECK(strstr(out, "interrupted") != NULL);
    free(out);

    /* uninterrupted code is unaffected */
    st = run_with_stop("let n = 0; for (let i = 0; i < 1000; i++) n += i; n;", &out, false);
    CHECK(st == 1);
    CHECK(strcmp(out, "499500") == 0);
    free(out);
}

/*
 * The one error the engine must never fail to describe is the one saying
 * allocation failed — building its message allocates too. Under a tight heap
 * limit the rejection must still carry a reason; it used to come back as bare
 * `undefined`, which told a host something broke but not what.
 */
static void test_oom_is_reported(void) {
    /* Above the floor a context needs to exist at all — below that the harness
     * cannot even build a VM, which tests nothing. */
    static const size_t limits[] = {320 * 1024, 512 * 1024, 1024 * 1024, 4096 * 1024};
    for (size_t i = 0; i < sizeof limits / sizeof limits[0]; i++) {
        RunOpts o = {0};
        o.cfg.heap_limit = limits[i];
        char *out = NULL;
        RunStatus st = run_src_opts(
            "let a = []; for (let i = 0; i < 200000; i++) a.push({ k: 'v' + i }); a.length;",
            &o, &out);
        if (st == RUN_HARNESS_ERR) /* limit too small to construct a context */
            continue;
        checks_run++;
        if (st != RUN_RUNTIME_ERR) { /* limit generous enough to finish */
            free(out);
            continue;
        }
        checks_run++;
        if (!out || strcmp(out, "undefined") == 0) {
            checks_failed++;
            fprintf(stderr, "FAIL: OOM at heap_limit=%zu rejected with no reason\n",
                    limits[i]);
        }
        free(out);
    }
}

/*
 * The regex engine used to call libc directly, so none of its memory -- the
 * compiled program, the parser's AST, class buffers, the per-match context --
 * was counted in bytes_live or refused by heap_limit. A guest could hold regex
 * memory entirely outside its configured budget. It now allocates through the
 * VM, so the cap applies to it like anything else.
 */
static void test_regex_memory_is_capped(void) {
    RunOpts o = {0};
    o.cfg.heap_limit = 700 * 1024;
    char *out = NULL;
    RunStatus st = run_src_opts(
        "let rs = [];"
        "for (let i = 0; i < 64; i++) rs.push(new RegExp('(a|b|c){1,200}'.repeat(20)));"
        "rs.length;",
        &o, &out);
    CHECK(st == RUN_RUNTIME_ERR);
    CHECK(out && strstr(out, "memory") != NULL);
    free(out);

    /* The cap must not break ordinary patterns, and the ReDoS step budget --
     * the other thing standing between a guest and an unbounded match -- has
     * to survive being routed through a different allocator. */
    o.cfg.heap_limit = 4 * 1024 * 1024;
    st = run_src_opts("let caught = 0;"
                      "try { /(a+)+$/.test('a'.repeat(200) + 'X'); } catch (e) { caught = 1; }"
                      "caught + ':' + 'xabbc'.replace(/ab+c/g, function (m) { return m.length; });",
                      &o, &out);
    CHECK(st == RUN_OK);
    CHECK(out && strcmp(out, "1:x4") == 0);
    free(out);
}

/*
 * heap_limit is a cap on what the VM may hold, so nothing should ever push the
 * accounted total past it -- not even transiently, and not even the collector's
 * own bookkeeping. The mark stack used to: it grew during marking, at the one
 * moment the limit could not be enforced, so a guest holding many small cells
 * pushed the total over by the size of the stack needed to trace them (12.5% of
 * the cap when measured, 20% worst case). It is now sized before marking, from
 * the cell count, where a refusal is an ordinary out-of-memory.
 *
 * This allocator watches every call rather than sampling at the end, because
 * the overshoot was transient -- a peak during a collection, gone by the time
 * anything on the outside could look.
 */
typedef struct {
    size_t live;
    size_t limit;
    size_t worst;   /* highest live seen */
} CapWatch;

static void *cap_watch_realloc(void *ud, void *ptr, size_t old_size, size_t new_size) {
    CapWatch *w = ud;
    if (new_size == 0) {
        if (ptr) { w->live -= old_size; free(ptr); }
        return NULL;
    }
    void *p = realloc(ptr, new_size);
    if (!p)
        return NULL;
    w->live = w->live - old_size + new_size;
    if (w->live > w->worst)
        w->worst = w->live;
    return p;
}

static void expect_within_heap_limit(const char *label, const char *src, size_t limit) {
    CapWatch w = {0, limit, 0};
    JsVmConfig cfg = {0};
    cfg.realloc_fn = cap_watch_realloc;
    cfg.alloc_ud = &w;
    cfg.heap_limit = limit;
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    if (!vm || !ctx) { js_vm_free(vm); return; }
    size_t len = strlen(src);
    uint16_t *u = malloc(len * sizeof(uint16_t));
    for (size_t i = 0; i < len; i++)
        u[i] = (uint16_t)(unsigned char)src[i];
    const char *em; uint32_t ep;
    JsValue fn = js_compile_module(ctx, u, len, &em, &ep);
    free(u);
    if (js_is_function(fn)) {
        js_gc_protect(vm, &fn);
        JsValue p = js_run_module(ctx, fn);
        js_gc_protect(vm, &p);
        js_run_jobs(ctx);
    }
    js_vm_free(vm);
    checks_run++;
    if (w.worst > limit) {
        checks_failed++;
        fprintf(stderr, "FAIL heap_limit exceeded (%s): peak %zu > limit %zu (+%zu)\n",
                label, w.worst, limit, w.worst - limit);
    }
}

static void test_heap_limit_is_a_hard_cap(void) {
    const size_t L = 2u * 1024 * 1024;
    /* Cell-heavy workloads are the ones that grow the mark stack. */
    expect_within_heap_limit("many objects",
        "let a = []; for (let i = 0; i < 1000000; i++) a.push({ v: i }); a.length;", L);
    expect_within_heap_limit("closures",
        "let f = []; for (let i = 0; i < 300000; i++) f.push(function () { return i; }); f.length;", L);
    expect_within_heap_limit("property churn",
        "let o = {}; for (let i = 0; i < 500000; i++) o['k' + i] = i; 1;", L);
    expect_within_heap_limit("array growth",
        "let a = []; for (let i = 0; i < 1000000; i++) a.push(i); a.length;", L);
    expect_within_heap_limit("string growth",
        "let s = 'x'; for (let i = 0; i < 40; i++) { s = s + s; } s.length;", L);
    /* A tight limit exercises the same paths with the collector under pressure. */
    expect_within_heap_limit("many objects (tight)",
        "let a = []; for (let i = 0; i < 1000000; i++) a.push({ v: i }); a.length;",
        512u * 1024);
}

int main(void) {
    test_heap_limit_is_a_hard_cap();
    test_regex_memory_is_capped();
    test_interrupt();
    test_error_cause();
    test_oom_is_reported();
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
    test_new();
    test_real_prototypes();
    test_closures();
    test_exceptions();
    test_fuel();
    test_heap_limit();
    test_gc_stress_run();
    test_host_globals();
    test_completion_value();
    test_concat_hash_interning();
    test_audit_regressions();
    test_audit_p2_p3();

    if (checks_failed) {
        fprintf(stderr, "%d/%d exec checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d exec checks passed\n", checks_run);
    return 0;
}
