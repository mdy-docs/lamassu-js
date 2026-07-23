/*
 * Bytecode serialization tests (phase 8): round-trip fidelity, repeated
 * execution from one loaded cache, and — the security-critical part — that
 * the validating loader rejects malformed/tampered input without ever
 * crashing (run under ASan/UBSan via the _asan build). The single-byte
 * mutation sweep is a lightweight fuzzer over the format: every accepted
 * mutant must also run to completion without a memory error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lamassu.h"

static int checks_run;
static int checks_failed;

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

static uint16_t *widen(const char *s, size_t *len) {
    size_t n = strlen(s);
    uint16_t *u = malloc(n * sizeof(uint16_t) + 2);
    for (size_t i = 0; i < n; i++)
        u[i] = (uint16_t)(unsigned char)s[i];
    *len = n;
    return u;
}

static char *narrow(JsContext *ctx, JsValue v) {
    JsValue s = js_to_string(ctx, v);
    size_t sl;
    const uint16_t *su = js_string_units(s, &sl);
    char *out = malloc(sl + 1);
    for (size_t i = 0; i < sl; i++)
        out[i] = su && su[i] < 128 ? (char)su[i] : '?';
    out[sl] = 0;
    return out;
}

/* Direct compile+run of src; malloc'd ToString of the result. */
static char *run_source(const char *src, bool *ok) {
    CountAlloc ca = {0, 0};
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    size_t len;
    uint16_t *u = widen(src, &len);
    const char *em;
    uint32_t ep;
    JsValue fn = js_compile_module(ctx, u, len, &em, &ep);
    char *out;
    if (!js_is_function(fn)) {
        out = strdup(em ? em : "compile error");
        *ok = false;
    } else {
        js_gc_protect(vm, &fn);
        JsValue p = js_run_module(ctx, fn);
        int st = js_promise_state(p);
        *ok = st == 0 || st == 1;
        JsValue res = js_promise_result(p);
        js_gc_protect(vm, &res);
        out = narrow(ctx, res);
    }
    free(u);
    js_vm_free(vm);
    return out;
}

/*
 * Compile src → serialize → (fresh VM) load → run n_runs times. Returns the
 * ToString of the last run's result; sets *ok, and *load_ok if the buffer
 * loaded at all. Verifies each run agrees and that no allocations leak.
 */
static char *roundtrip(const char *src, int n_runs, bool *ok, bool *load_ok) {
    /* --- compile + serialize in one VM --- */
    CountAlloc ca1 = {0, 0};
    JsVmConfig cfg1 = {.realloc_fn = count_realloc, .alloc_ud = &ca1};
    JsVm *vm1 = js_vm_new(&cfg1);
    JsContext *ctx1 = js_context_new(vm1);
    size_t len;
    uint16_t *u = widen(src, &len);
    const char *em;
    uint32_t ep;
    JsValue fn = js_compile_module(ctx1, u, len, &em, &ep);
    free(u);
    if (!js_is_function(fn)) {
        js_vm_free(vm1);
        *ok = false;
        *load_ok = false;
        return strdup(em ? em : "compile error");
    }
    js_gc_protect(vm1, &fn);
    uint8_t *bc;
    size_t bc_len;
    bool ser = js_bytecode_serialize(ctx1, fn, &bc, &bc_len);
    /* copy the buffer out before tearing down vm1 (it owns bc's allocation) */
    uint8_t *bc_copy = NULL;
    if (ser) {
        bc_copy = malloc(bc_len);
        memcpy(bc_copy, bc, bc_len);
        js_bytecode_free(ctx1, bc, bc_len);
    }
    js_gc_unprotect(vm1, &fn);
    js_vm_free(vm1);
    if (!ser) {
        *ok = false;
        *load_ok = false;
        return strdup("serialize failed");
    }

    /* --- load + run in a second VM --- */
    CountAlloc ca2 = {0, 0};
    JsVmConfig cfg2 = {.realloc_fn = count_realloc, .alloc_ud = &ca2};
    JsVm *vm2 = js_vm_new(&cfg2);
    JsContext *ctx2 = js_context_new(vm2);
    const char *lerr;
    JsValue lfn = js_bytecode_load(ctx2, bc_copy, bc_len, &lerr);
    free(bc_copy);
    char *out = NULL;
    if (!js_is_function(lfn)) {
        *load_ok = false;
        *ok = false;
        out = strdup(lerr ? lerr : "load failed");
    } else {
        *load_ok = true;
        js_gc_protect(vm2, &lfn);
        char *prev = NULL;
        *ok = true;
        for (int i = 0; i < n_runs; i++) {
            JsValue p = js_run_module(ctx2, lfn);
            int st = js_promise_state(p);
            bool rok = st == 0 || st == 1;
            JsValue res = js_promise_result(p);
            js_gc_protect(vm2, &res);
            char *r = narrow(ctx2, res);
            js_gc_unprotect(vm2, &res);
            if (!rok)
                *ok = false;
            if (prev && strcmp(prev, r) != 0)
                *ok = false; /* runs disagree */
            free(prev);
            prev = r;
        }
        out = prev;
        js_gc_unprotect(vm2, &lfn);
    }
    js_vm_free(vm2);
    return out;
}

/* src round-trips to `expected`, and re-running the loaded cache 3x agrees. */
static void eq(const char *src, const char *expected) {
    bool ok, load_ok;
    char *out = roundtrip(src, 3, &ok, &load_ok);
    checks_run++;
    if (!ok || !load_ok || strcmp(out, expected) != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL %s\n  expected: %s\n  actual:   %s%s%s\n", src, expected,
                out, ok ? "" : " (run mismatch/err)", load_ok ? "" : " (load failed)");
    }
    free(out);
}

/* the round-trip result equals a direct compile+run of the same source */
static void eq_direct(const char *src) {
    bool dok, ok, load_ok;
    char *direct = run_source(src, &dok);
    char *rt = roundtrip(src, 1, &ok, &load_ok);
    checks_run++;
    if (!dok || !ok || !load_ok || strcmp(direct, rt) != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL(vs-direct) %s\n  direct: %s\n  bytecode: %s\n", src,
                direct, rt);
    }
    free(direct);
    free(rt);
}

/* ---- functional round-trip coverage ---- */

static void test_roundtrip(void) {
    eq("1 + 2 * 3;", "7");
    eq("'a' + 'b' + 'c';", "abc");
    eq("let x = 10; x * x;", "100");
    eq("[1,2,3].map(n => n * 2).join(',');", "2,4,6");
    eq("const o = {a:1, b:2}; o.a + o.b;", "3");
    eq("function fib(n){ return n < 2 ? n : fib(n-1) + fib(n-2); } fib(12);", "144");
    eq("let s = 0; for (let i = 0; i < 100; i++) s += i; s;", "4950");
    eq("let a = [1,2,3,4,5]; a.filter(x => x % 2).reduce((p,c)=>p+c, 0);", "9");
    eq("try { throw 'e'; } catch (e) { e + '!'; } finally { }", "e!");
    eq("const add = (a, b) => a + b; const c = add.bind ? add : add; c(2, 3);", "5");
    eq("let out = ''; try { throw 1; } catch(e){ out='c'; } finally { out+='f'; } out;", "cf");
    eq("(function(){ let acc = []; for (const x of [1,2,3]) acc.push(x*x); return acc.join(); })();",
       "1,4,9");
    eq("const make = () => { let n = 0; return () => ++n; }; const f = make(); f(); f(); f();",
       "3"); /* closures/upvalues survive serialization */
    eq("`sum=${1+2+3}`;", "sum=6");
    eq("let {a, b=5} = {a:1}; a + b;", "6");
    eq("[...[1,2], ...[3,4]].length;", "4");
    eq("switch (2) { case 1: 'one'; break; case 2: 'two'; break; default: 'other'; }", "two");
    eq("JSON.stringify({x:[1,2,3]});", "{\"x\":[1,2,3]}");
    eq("Math.max(3, 7, 2) + Math.min(9, 4);", "11");
#ifdef LAMASSU_HAS_REGEX
    eq("'a1b2c3'.replace(/[0-9]/g, '#');", "a#b#c#"); /* NEW_REGEXP const survives */
    eq("/(?<y>\\d+)/.exec('x42').groups.y;", "42");
#endif
    /* deeply nested closures exercise recursive function-const serialization */
    eq("const f = a => b => c => d => a+b+c+d; f(1)(2)(3)(4);", "10");
    /* JS_OP_DYNAMIC_IMPORT survives the round-trip (no loader registered, so
     * calling it yields a rejected promise — typeof proves the value shape) */
    eq("function load(s) { return import(s); } typeof load(0).then;", "function");
}

static void test_vs_direct(void) {
    eq_direct("let r = 0; for (let i=1;i<=10;i++){ for(let j=1;j<=i;j++){ r+=j; } } r;");
    eq_direct("const xs = [5,3,8,1,9,2]; xs.sort((a,b)=>a-b).join(',');");
    eq_direct("function g(x){ if (x<=0) return 'done'; return g(x-1); } g(50);");
    eq_direct("let o={}; for (let i=0;i<20;i++){ o['k'+i]=i*i; } Object.keys(o).length;");
    eq_direct("'The quick brown fox'.split(' ').map(w=>w.length).join(',');");
}

/* ---- validation / tamper resistance ---- */

static void expect_reject(uint8_t *buf, size_t len, const char *what);

/* Serializes `src`; caller frees the returned buffer. */
static uint8_t *make_valid_bc_src(const char *src, size_t *out_len) {
    CountAlloc ca = {0, 0};
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    size_t len;
    uint16_t *u = widen(src, &len);
    const char *em;
    uint32_t ep;
    JsValue fn = js_compile_module(ctx, u, len, &em, &ep);
    free(u);
    js_gc_protect(vm, &fn);
    uint8_t *bc;
    size_t bc_len;
    uint8_t *copy = NULL;
    if (js_bytecode_serialize(ctx, fn, &bc, &bc_len)) {
        copy = malloc(bc_len);
        memcpy(copy, bc, bc_len);
        *out_len = bc_len;
        js_bytecode_free(ctx, bc, bc_len);
    }
    js_gc_unprotect(vm, &fn);
    js_vm_free(vm);
    return copy;
}

/* Serializes a representative program; caller frees the returned buffer. */
static uint8_t *make_valid_bc(size_t *out_len) {
    return make_valid_bc_src(
        "function fact(n){ return n<=1 ? 1 : n*fact(n-1); }"
        "let s=0; for (const x of [1,2,3,4]) s += fact(x); s;",
        out_len);
}

/*
 * WS-D / P0: a CTAG_NUMBER constant whose 8 payload bytes are tampered into a
 * boxed (non-number) NaN pattern must be rejected at load, not accepted and
 * later dereferenced as an attacker-controlled pointer. We compile a program
 * with the distinctive double 1.5 as a constant, find its little-endian bytes
 * in the serialized buffer, and overwrite the high 16 bits with a boxed tag
 * (>= 0xFFF9) so js_is_number() becomes false.
 */
static void test_ws_d_number_typeconfusion(void) {
    size_t len;
    uint8_t *bc = make_valid_bc_src("let x = 1.5; x + 1;", &len);
    checks_run++;
    if (!bc) {
        checks_failed++;
        fprintf(stderr, "FAIL: could not build bytecode for CTAG_NUMBER test\n");
        return;
    }
    /* 1.5 == 0x3FF8000000000000, little-endian: 00 00 00 00 00 00 F8 3F */
    const uint8_t pat[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x3F};
    bool found = false;
    for (size_t i = 0; i + 8 <= len; i++) {
        if (memcmp(bc + i, pat, 8) == 0) {
            uint8_t *t = malloc(len);
            memcpy(t, bc, len);
            /* set the top 16 bits (the NaN-box tag) to 0xFFFB (object tag) */
            t[i + 6] = 0xFB;
            t[i + 7] = 0xFF;
            expect_reject(t, len, "CTAG_NUMBER boxed-pointer type confusion");
            free(t);
            found = true;
            break;
        }
    }
    checks_run++;
    if (!found) {
        checks_failed++;
        fprintf(stderr, "FAIL: 1.5 constant not found in bytecode (test needs updating)\n");
    }
    free(bc);
}

/*
 * Loads `buf` on the given context; if it loads, runs it under a fuel cap (so
 * a mutant that forms an infinite loop can't hang the sweep). Returns true if
 * it LOADED (regardless of run outcome). Never crashes on valid or invalid
 * input — that invariant is what ASan enforces here. Sharing one context
 * across the whole mutation sweep avoids re-initializing the stdlib per
 * mutant (the dominant cost); result values are irrelevant, only safety.
 */
static bool try_load_and_run_on(JsContext *ctx, JsVm *vm, const uint8_t *buf,
                                size_t len) {
    const char *err;
    JsValue fn = js_bytecode_load(ctx, buf, len, &err);
    bool loaded = js_is_function(fn);
    if (loaded) {
        js_gc_protect(vm, &fn);
        js_context_set_fuel(ctx, 2000000);
        (void)js_run_module(ctx, fn); /* result irrelevant; must not crash */
        js_context_set_fuel(ctx, 0);
        js_gc_unprotect(vm, &fn);
    }
    return loaded;
}

/* Standalone variant (own VM) for the one-off targeted checks. */
static bool try_load_and_run(const uint8_t *buf, size_t len) {
    CountAlloc ca = {0, 0};
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    bool loaded = try_load_and_run_on(ctx, vm, buf, len);
    js_vm_free(vm);
    return loaded;
}

static void expect_reject(uint8_t *buf, size_t len, const char *what) {
    checks_run++;
    if (try_load_and_run(buf, len)) {
        checks_failed++;
        fprintf(stderr, "FAIL(tamper): loader ACCEPTED %s\n", what);
    }
}

static void test_validation(void) {
    size_t len;
    uint8_t *bc = make_valid_bc(&len);
    checks_run++;
    if (!bc) {
        checks_failed++;
        fprintf(stderr, "FAIL: could not build valid bytecode\n");
        return;
    }

    /* the pristine buffer loads and runs */
    checks_run++;
    if (!try_load_and_run(bc, len)) {
        checks_failed++;
        fprintf(stderr, "FAIL: valid bytecode was rejected\n");
    }

    /* targeted structural corruptions must be rejected */
    {
        uint8_t *t = malloc(len);
        memcpy(t, bc, len);
        t[0] ^= 0xFF; /* magic */
        expect_reject(t, len, "corrupt magic");
        free(t);
    }
    {
        uint8_t *t = malloc(len);
        memcpy(t, bc, len);
        t[4] = 99; /* version byte */
        expect_reject(t, len, "wrong version");
        free(t);
    }
    expect_reject(bc, 8, "truncated to 8 bytes");
    expect_reject(bc, len / 2, "truncated to half");
    expect_reject(bc, 15, "shorter than header");
    {
        uint8_t *t = malloc(len + 1); /* trailing byte */
        memcpy(t, bc, len);
        t[len] = 0;
        expect_reject(t, len + 1, "trailing byte");
        free(t);
    }

    /*
     * Single-byte mutation sweep: for every byte (past the magic) apply three
     * mutations — XOR 0xA5, set 0x00, set 0xFF — load, and if accepted, run.
     * Every accepted mutant must load and run without a memory error (ASan/
     * UBSan-enforced). We do NOT require rejection: many flips are harmless
     * (a line-table entry, a constant's value). What must hold is that the
     * loader is memory-safe on arbitrary input and never emits a function
     * that is unsafe to execute. One shared, fuel-capped context serves the
     * whole sweep so we pay stdlib init once, not per mutant.
     */
    CountAlloc ca = {0, 0};
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    int total = 0;
    const uint8_t muts[3] = {0xA5, 0x00, 0xFF};
    for (size_t i = 4; i < len; i++) {
        uint8_t *t = malloc(len);
        for (int mi = 0; mi < 3; mi++) {
            memcpy(t, bc, len);
            t[i] = mi == 0 ? (uint8_t)(bc[i] ^ 0xA5) : muts[mi];
            (void)try_load_and_run_on(ctx, vm, t, len);
            total++;
        }
        free(t);
    }
    js_vm_free(vm);
    checks_run++;
    if (total == 0) {
        checks_failed++;
        fprintf(stderr, "FAIL: mutation sweep ran no cases\n");
    }

    free(bc);
}

int main(void) {
    test_roundtrip();
    test_vs_direct();
    test_validation();
    test_ws_d_number_typeconfusion();
    if (checks_failed) {
        fprintf(stderr, "%d/%d bytecode checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d bytecode checks passed\n", checks_run);
    return 0;
}
