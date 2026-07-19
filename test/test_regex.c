/* RegExp integration tests (third_party/baru-re binding layer) — driven
 * through compile+run, checking ToString of the completion value. Shares the
 * harness shape with test_builtins.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsvm.h"

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

/* Runs src, returns malloc'd ASCII of ToString(result); ok=false on error
 * (compile or runtime; the error text is returned either way). */
static char *run(const char *src, bool stress, bool *ok) {
    CountAlloc ca = {0, 0};
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca, .gc_stress = stress};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    size_t len = strlen(src);
    uint16_t *u = malloc(len * sizeof(uint16_t));
    for (size_t i = 0; i < len; i++)
        u[i] = (uint16_t)(unsigned char)src[i];
    const char *em;
    uint32_t ep;
    JsValue fn = js_compile_module(ctx, u, len, &em, &ep);
    char *out;
    if (!js_is_function(fn)) {
        out = strdup(em ? em : "compile error");
        *ok = false;
    } else {
        js_gc_protect(vm, &fn);
        JsValue res;
        *ok = js_run_module(ctx, fn, &res);
        js_gc_protect(vm, &res);
        JsValue s = js_to_string(ctx, res);
        size_t sl;
        const uint16_t *su = js_string_units(s, &sl);
        out = malloc(sl + 1);
        for (size_t i = 0; i < sl; i++)
            out[i] = su && su[i] < 128 ? (char)su[i] : '?';
        out[sl] = 0;
    }
    free(u);
    js_vm_free(vm);
    checks_run++;
    if (ca.net_bytes != 0 || ca.live_allocs != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL leak: %s (net=%ld allocs=%ld)\n", src, ca.net_bytes,
                ca.live_allocs);
    }
    return out;
}

static void eq_mode(const char *src, const char *expected, bool stress) {
    bool ok;
    char *out = run(src, stress, &ok);
    checks_run++;
    if (!ok || strcmp(out, expected) != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL%s %s\n  expected: %s\n  actual:   %s%s\n",
                stress ? " (stress)" : "", src, expected, out, ok ? "" : " (error)");
    }
    free(out);
}

/* gc_stress collects at every allocation and a compiled pattern is a large
 * cell, so blanket stress is slow; eq() runs normal, GC-sensitive cases get
 * eq_s() (normal + stress). */
static void eq(const char *src, const char *expected) {
    eq_mode(src, expected, false);
}
static void eq_s(const char *src, const char *expected) {
    eq_mode(src, expected, false);
    eq_mode(src, expected, true);
}

/* expects an error (compile or thrown) whose text starts with `prefix` */
static void err(const char *src, const char *prefix) {
    bool ok;
    char *out = run(src, false, &ok);
    checks_run++;
    if (ok || strncmp(out, prefix, strlen(prefix)) != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL %s\n  expected error: %s...\n  actual:   %s%s\n", src,
                prefix, out, ok ? " (no error)" : "");
    }
    free(out);
}

static void test_literal_and_props(void) {
    eq("/ab+c/.source;", "ab+c");
    eq("/ab/gi.flags;", "gi");
    eq("/ab/ig.flags;", "gi"); /* normalized order */
    eq("/ab/dgimsuy.flags;", "dgimsuy");
    eq("/a/g.global;", "true");
    eq("/a/.global;", "false");
    eq("/a/i.ignoreCase;", "true");
    eq("/a/m.multiline;", "true");
    eq("/a/s.dotAll;", "true");
    eq("/a/y.sticky;", "true");
    eq("/a/u.unicode;", "true");
    eq("/a/v.unicodeSets;", "true");
    eq("/a/d.hasIndices;", "true");
    eq("/a/g.lastIndex;", "0");
    eq("/ab/gi.toString();", "/ab/gi");
    eq("'' + /ab/g;", "/ab/g");
    eq("typeof /a/;", "object");
    eq("typeof /a/.exec;", "function");
    eq("Object.keys(/a/g).length;", "0"); /* built-ins are not own props */
    eq("const r = /a/; r.custom = 7; r.custom;", "7"); /* expandos allowed */
    eq("const r = /a/g; r.lastIndex = 3; r.lastIndex;", "3");
    err("'use strict'; /a/.source = 'x';", "TypeError: cannot assign to read-only");
    err("const r = /(/;", "SyntaxError:"); /* bad pattern: early error */
    err("const r = /a/gg;", "SyntaxError: invalid regular expression flags");
    err("const r = /a/uv;", "SyntaxError: invalid regular expression flags");
}

static void test_constructor(void) {
    eq("RegExp('ab+c').source;", "ab+c");
    eq("RegExp('a', 'gi').flags;", "gi");
    eq("RegExp().source;", "(?:)");
    eq("RegExp().test('');", "true");
    eq("RegExp('').toString();", "/(?:)/");
    eq("RegExp(/ab/gi).flags;", "gi");       /* clone keeps flags */
    eq("RegExp(/ab/gi, 'm').flags;", "m");   /* explicit flags win */
    eq("RegExp(/ab/g).source;", "ab");
    eq("RegExp(123).source;", "123");        /* ToString coercion */
    err("RegExp('(');", "SyntaxError:");
    err("RegExp('a', 'q');", "SyntaxError: invalid regular expression flags");
    err("RegExp('a', 'gg');", "SyntaxError: invalid regular expression flags");
    /* `new RegExp(...)`: natives are factories, so `new` behaves like a call */
    eq("new RegExp('ab+c').source;", "ab+c");
    eq("new RegExp('a', 'gi').flags;", "gi");
    eq("new RegExp('x').test('yxz');", "true");
}

static void test_exec_and_test(void) {
    eq("/b/.exec('abc').index;", "1");
    eq("/b/.exec('abc')[0];", "b");
    eq("/b/.exec('abc').input;", "abc");
    eq("/x/.exec('abc');", "null");
    eq("/(a)(c)?/.exec('ab').length;", "3");
    eq("'' + /(a)(c)?/.exec('ab');", "a,a,"); /* unmatched group -> undefined */
    eq("/(\\d+)-(\\d+)/.exec('a 12-34')[2];", "34");
    eq_s("/(?<y>\\d{4})-(?<m>\\d{2})/.exec('2026-07').groups.y;", "2026");
    eq("/(?<y>\\d4)/.exec('x');", "null");
    eq("/(\\d)/.exec('x7').groups;", "undefined");     /* no named groups */
    eq("/ab+c/.test('xabbc');", "true");
    eq("/ab+c/.test('xac');", "false");
    eq("/^a/.test('ba');", "false");
    err("const e = /a/.exec; e('x');", "TypeError: exec called on a non-RegExp");
}

static void test_last_index(void) {
    eq("const r = /a/g; r.exec('aab'); r.lastIndex;", "1");
    eq("const r = /a/g; r.exec('aab'); r.exec('aab').index;", "1");
    eq("const r = /a/g; r.exec('ab'); r.exec('ab'); r.lastIndex;", "0");
    eq("const r = /a/g; r.lastIndex = 99; r.exec('aaa');", "null");
    eq("const r = /a/; r.exec('aa'); r.lastIndex;", "0"); /* non-global: untouched */
    eq("const r = /a/y; r.test('ba');", "false");          /* sticky anchors */
    eq("const r = /a/y; r.lastIndex = 1; r.test('ba');", "true");
    eq("const r = /a/y; r.lastIndex = 1; r.test('ba'); r.lastIndex;", "2");
    eq("const r = /a/g; r.test('xay'); r.lastIndex;", "2"); /* test tracks too */
}

static void test_match(void) {
    eq("'a@b c@d'.match(/\\w@\\w/g).length;", "2");
    eq("'a@b c@d'.match(/\\w@\\w/g)[1];", "c@d");
    eq("'abc'.match(/x/g);", "null");
    eq("'abc'.match(/x/);", "null");
    eq("'abc'.match(/(b)/).index;", "1");
    eq("'abc'.match(/(b)/)[1];", "b");
    eq("'abc'.match('b')[0];", "b");   /* string arg -> pattern */
    eq("'a.c'.match('a.c').index;", "0"); /* and it is a regex, not literal */
    eq("'abc'.match()[0];", "");       /* undefined -> empty pattern */
    eq("'aaa'.match(/a*?/g).length;", "4"); /* empty matches advance */
    eq_s("'x1y22'.match(/\\d+/g).join('|');", "1|22");
}

static void test_match_all(void) {
    eq("'a1b2'.matchAll(/\\d/g).length;", "2");
    eq("'a1b2'.matchAll(/\\d/g)[1].index;", "3");
    eq("'a1b2'.matchAll(/(\\d)/g)[0][1];", "1");
    eq_s("'x@y z@w'.matchAll(/(?<l>\\w)@/g)[1].groups.l;", "z");
    eq("'abc'.matchAll(/x/g).length;", "0");
    eq("'a1'.matchAll('\\\\d').length;", "1"); /* string arg gets /g */
    err("'a1'.matchAll(/\\d/);", "TypeError: matchAll must be called with a global RegExp");
}

static void test_search(void) {
    eq("'cost: $42'.search(/\\d+/);", "7");
    eq("'abc'.search(/x/);", "-1");
    eq("'abc'.search('b');", "1");
    eq("'abc'.search();", "0");
}

static void test_replace(void) {
    eq("'2026-07-18'.replace(/(\\d+)-(\\d+)-(\\d+)/, '$3/$2/$1');", "18/07/2026");
    eq("'a-b-c'.replace(/-/g, '+');", "a+b+c");
    eq("'a-b-c'.replace(/-/, '+');", "a+b-c"); /* non-global: first only */
    eq("'abcd'.replace(/bc/, '<$&>');", "a<bc>d");
    eq("'abcd'.replace(/bc/, '($`|$\\')');", "a(a|d)d");
    eq("'ab'.replace(/b/, '$$');", "a$");
    eq("'ab'.replace(/b/, '$9');", "a$9");     /* out-of-range: literal */
    eq_s("'john@x'.replace(/(?<u>\\w+)@/, '[$<u>]');", "[john]x");
    eq("'ab'.replace(/(a)|(z)/, '[$2]');", "[]b"); /* unmatched group -> empty */
    eq("'abc'.replace(/x*/g, '-');", "-a-b-c-");   /* empty match handling */
    eq_s("'a1'.replace(/(\\d)/, (m, p1, i, s) => m + p1 + i + s);", "a111a1");
    eq_s("'The Quick'.replace(/\\w+/g, w => w.toUpperCase());", "THE QUICK");
    eq_s("'x@y'.replace(/(?<h>\\w)$/, (m, p1, i, s, g) => g.h);", "x@y");
    eq("'aXbXc'.replaceAll(/X/g, '-');", "a-b-c");
    err("'a'.replaceAll(/a/, 'b');", "TypeError: replaceAll must be called with a global RegExp");
    err("'ab'.replace(/b/g, () => { throw 'boom'; });", "boom"); /* propagates */
}

static void test_split(void) {
    eq("'a1b22c'.split(/\\d+/).join('|');", "a|b|c");
    eq("'a1b'.split(/(\\d)/).join('|');", "a|1|b"); /* captures included */
    eq("'ab'.split(/b/).length;", "2");
    eq("'ab'.split(/b/)[1];", "");
    eq("'abc'.split(/x/)[0];", "abc");
    eq("''.split(/x/).length;", "1");
    eq("''.split(/(?:)/).length;", "0"); /* empty subject, matching pattern */
    eq("'abc'.split(/(?:)/).join('|');", "a|b|c");
    eq_s("'1a2b3'.split(/[ab]/).join(',');", "1,2,3");
}

static void test_flags_semantics(void) {
    eq("/AB/i.test('ab');", "true");
    eq("/^b/m.test('a\\nb');", "true");
    eq("/^b/.test('a\\nb');", "false");
    eq("/a.b/s.test('a\\nb');", "true");
    eq("/a.b/.test('a\\nb');", "false");
    eq("'\\uD83D\\uDE00'.match(/./gu).length;", "1"); /* code-point steps */
    eq("'\\uD83D\\uDE00'.match(/./g).length;", "2");  /* unit steps without u */
    eq("/\\p{L}/u.test('a');", "true");
    eq("/\\p{L}/u.test('1');", "false");
    eq("/(?<=\\$)\\d+/.exec('$42')[0];", "42"); /* lookbehind */
    eq("/(\\w+) \\1/.test('hi hi');", "true");  /* backreference */
    eq("/(a)/d.exec('xa').indices[1].join(',');", "1,2");
    eq("/(a)/d.exec('xa').indices[0].join(',');", "1,2");
    eq_s("/(?<g>a)/d.exec('xa').indices.groups.g.join(',');", "1,2");
    eq("/(a)|(b)/d.exec('b').indices[1];", "undefined");
}

static void test_step_budget(void) {
    /* Catastrophic backtracking must surface as a catchable RangeError, not
     * a hang: /(a+)+$/ over a few hundred a's + b previously hung the VM
     * (verified before the engine's step budget existed — the fail cache
     * does not defuse this shape). Each of these completes in milliseconds
     * or the suite itself hangs. */
    err("/(a+)+$/.test('a'.repeat(200) + 'b');",
        "RangeError: regular expression step budget exhausted");
    err("('a'.repeat(200) + 'b').match(/(a+)+$/);",
        "RangeError: regular expression step budget exhausted");
    err("('a'.repeat(200) + 'b').search(/(a+)+$/);",
        "RangeError: regular expression step budget exhausted");
    err("('a'.repeat(200) + 'b').replace(/(a+)+$/, 'x');",
        "RangeError: regular expression step budget exhausted");
    err("('a'.repeat(200) + 'b').split(/(a+)+$/);",
        "RangeError: regular expression step budget exhausted");
    /* the error is catchable and the VM stays usable afterwards */
    eq("let msg = ''; try { /(a+)+$/.test('a'.repeat(200) + 'b'); }"
       "catch (e) { msg = e.slice(0, 10); } msg + '|' + /a/.test('a');",
       "RangeError|true");
    /* legitimate work stays comfortably inside the budget */
    eq("/(a+)+$/.test('a'.repeat(200));", "true"); /* same pattern, matching input */
    eq("('ab'.repeat(2000)).match(/ab/g).length;", "2000");
    eq("('x'.repeat(5000) + 'needle').search(/needle/);", "5000");
}

static void test_lifecycle(void) {
    /* transient patterns are collected: far more than the live cap in total */
    eq("for (let i = 0; i < 100; i++) RegExp('x' + i); 'ok';", "ok");
    /* the live-pattern cap trips when they are all kept reachable */
    err("const a = []; for (let i = 0; i < 70; i++) a.push(RegExp('x' + i)); 1;",
        "RangeError: too many live regular expressions");
    /* a literal creates a fresh object per evaluation */
    eq("let last = null, diff = false;"
       "for (let i = 0; i < 3; i++) { const r = /a/; diff = diff || r === last; last = r; }"
       "diff;", "false");
    eq_s("const r = /(\\w)/; r.exec('hi')[1];", "h");
}

int main(void) {
    test_literal_and_props();
    test_constructor();
    test_exec_and_test();
    test_last_index();
    test_match();
    test_match_all();
    test_search();
    test_replace();
    test_split();
    test_flags_semantics();
    test_step_budget();
    test_lifecycle();
    if (checks_failed) {
        fprintf(stderr, "%d/%d regex checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d regex checks passed\n", checks_run);
    return 0;
}
