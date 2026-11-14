/* Builtin library tests — driven through compile+run, checking ToString of
 * the completion value. Shares the harness shape with test_exec.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lamassu.h"
#include "lamassu_compile.h"

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

/* Runs src, returns malloc'd ASCII of ToString(result); ok=false on error. */
/*
 * UTF-8 <-> UTF-16, because this harness used to do neither.
 *
 * Source went in a byte per code unit — so `'Ä'` reached the engine as two
 * characters, U+00C3 and U+0084 — and results came back with every non-ASCII
 * unit replaced by `?`. Between them, a test COULD NOT EXPRESS a non-ASCII
 * expectation, which is how toLowerCase stayed ASCII-only without a failing
 * test to say so. Fixing the harness is what let the bug be written down.
 */
static uint16_t *utf8_to_utf16(const char *s, size_t len, size_t *out_units) {
    uint16_t *u = malloc((len + 1) * sizeof(uint16_t));
    size_t n = 0;
    for (size_t i = 0; i < len;) {
        unsigned char a = (unsigned char)s[i];
        uint32_t cp;
        size_t width;
        if (a < 0x80) { cp = a; width = 1; }
        else if ((a & 0xE0) == 0xC0) { cp = a & 0x1F; width = 2; }
        else if ((a & 0xF0) == 0xE0) { cp = a & 0x0F; width = 3; }
        else if ((a & 0xF8) == 0xF0) { cp = a & 0x07; width = 4; }
        else { cp = 0xFFFD; width = 1; }
        if (i + width > len) { cp = 0xFFFD; width = 1; }
        for (size_t k = 1; k < width; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += width;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            u[n++] = (uint16_t)(0xD800 + (cp >> 10));
            u[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            u[n++] = (uint16_t)cp;
        }
    }
    *out_units = n;
    return u;
}

static char *utf16_to_utf8(const uint16_t *u, size_t len) {
    char *out = malloc(len * 4 + 1);
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        uint32_t cp = u[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len &&
            u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            cp = 0x10000u + ((cp - 0xD800) << 10) + (u[++i] - 0xDC00);
        }
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = 0;
    return out;
}

static char *run(const char *src, bool stress, bool *ok) {
    CountAlloc ca = {0, 0};
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca, .gc_stress = stress};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    size_t units = 0;
    uint16_t *u = utf8_to_utf16(src, strlen(src), &units);
    const char *em;
    uint32_t ep;
    JsValue fn = js_compile_module(ctx, u, units, &em, &ep);
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
        JsValue s = js_to_string(ctx, res);
        size_t sl;
        const uint16_t *su = js_string_units(s, &sl);
        out = su ? utf16_to_utf8(su, sl) : strdup("");
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

/* Full GC at every allocation is O(heap) and the stdlib gives a big baseline,
 * so blanket stress is prohibitively slow; eq() runs normal, and a curated
 * set of GC-sensitive cases gets eq_s() (normal + stress). */
static void eq(const char *src, const char *expected) {
    eq_mode(src, expected, false);
}
static void eq_s(const char *src, const char *expected) {
    eq_mode(src, expected, false);
    eq_mode(src, expected, true);
}

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

static void test_string(void) {
    eq("'hello'.length;", "5");
    eq("'hello'.charAt(1);", "e");
    eq("'hello'.charCodeAt(0);", "104");
    eq("'hello'.at(-1);", "o");
    eq("'hello world'.indexOf('o');", "4");
    eq("'hello world'.lastIndexOf('o');", "7");
    eq("'hello'.includes('ell');", "true");
    eq("'hello'.startsWith('he');", "true");
    eq("'hello'.endsWith('lo');", "true");
    eq("'hello world'.slice(0, 5);", "hello");
    eq("'hello world'.slice(-5);", "world");
    eq("'hello'.substring(1, 3);", "el");
    eq("'Hello'.toUpperCase();", "HELLO");
    eq("'Hello'.toLowerCase();", "hello");

    /*
     * Case conversion is Unicode's, not ASCII's — these both used to add or
     * subtract 32 over A-Z, which left every one of the following unchanged.
     * Each answer here is what node prints for the same expression.
     */
    eq("'\xc3\x84\xc3\x96\xc3\x9c'.toLowerCase();", "\xc3\xa4\xc3\xb6\xc3\xbc");   /* ÄÖÜ -> äöü */
    eq("'\xc3\xa4\xc3\xb6\xc3\xbc'.toUpperCase();", "\xc3\x84\xc3\x96\xc3\x9c");   /* äöü -> ÄÖÜ */
    eq("'\xc5\x83'.toLowerCase();", "\xc5\x84");                    /* Ń  -> ń, Latin Extended-A */
    eq("'\xe1\xb8\xaa'.toLowerCase();", "\xe1\xb8\xab");              /* Ḫ  -> ḫ, Latin Ext Additional */
    eq("'\xd0\x94'.toLowerCase();", "\xd0\xb4");                    /* Д  -> д, Cyrillic */

    /* Mappings that CHANGE LENGTH: one character becoming two. Unit-by-unit
     * arithmetic cannot express these at all. */
    eq("'\xc3\x9f'.toUpperCase();", "SS");                        /* ß -> SS */
    eq("'stra\xc3\x9f" "e'.toUpperCase();", "STRASSE");
    eq("'\xc4\xb0'.toLowerCase();", "i\xcc\x87");                    /* İ -> i + combining dot */

    /* Final_Sigma: a capital sigma lowercases differently at the end of a word
     * than inside one, which is the only context-dependent rule in the
     * language-independent path. */
    eq("'\xce\x91\xce\xa3'.toLowerCase();", "\xce\xb1\xcf\x82");            /* ΑΣ -> ας */
    eq("'\xce\xa3\xce\x9f\xce\xa6\xce\x9f\xce\xa3'.toLowerCase();", "\xcf\x83\xce\xbf\xcf\x86\xce\xbf\xcf\x82");  /* ΣΟΦΟΣ -> σοφος */
    eq("'\xce\x9f\xce\x94\xce\x9f\xce\xa3 \xce\xa3\xce\xa4\xce\x9f'.toLowerCase();",
       "\xce\xbf\xce\xb4\xce\xbf\xcf\x82 \xcf\x83\xcf\x84\xce\xbf");                  /* ΟΔΟΣ ΣΤΟ -> οδος στο */

    /* An astral character is TWO code units and one character. Mapping the
     * units separately would map a surrogate; these have no case, so they must
     * come back exactly as they went in. */
    eq("'\xf0\x9d\x90\x80'.toLowerCase();", "\xf0\x9d\x90\x80");            /* 𝐀 */
    eq("'\xf0\x92\x80\x80'.toUpperCase();", "\xf0\x92\x80\x80");            /* cuneiform */
    eq("'a\xf0\x92\x80\x80" "B'.toLowerCase();", "a\xf0\x92\x80\x80" "b");
    eq("'  hi  '.trim();", "hi");
    eq("'  hi  '.trimStart() + '|';", "hi  |");
    eq("'ab'.repeat(3);", "ababab");
    eq("'5'.padStart(3, '0');", "005");
    eq("'5'.padEnd(3, '.');", "5..");
    eq("'a-b-c'.split('-').join(',');", "a,b,c");
    eq("'abc'.split('').length;", "3");
    eq("'hello'.replace('l', 'L');", "heLlo");
    eq("'hello'.replaceAll('l', 'L');", "heLLo");
    eq("'a1b2'.replaceAll('1', '$&$&');", "a11b2");
    /* a function replacer gets (match, position, string); its result is converted */
    eq("'aXbXc'.replace('X', (m, i, s) => '[' + m + i + s + ']');", "a[X1aXbXc]bXc");
    eq("'aXbXc'.replaceAll('X', (m, i) => '<' + i + '>');", "a<1>b<3>c");
    eq("'abc'.replaceAll('', (m, i) => i);", "0a1b2c3");
    eq("['abc'.replace('b', () => 5), 'abc'.replace('b', () => undefined), 'abc'.replace('z', () => 'Q'), 'abc'.replace('b')].join('|');",
       "a5c|aundefinedc|abc|aundefinedc");
    eq("let n = 0; 'aaaa'.replaceAll('aa', () => { n++; return ''; }); n;", "2");
    eq("let m; try { 'abc'.replace('b', () => { throw new Error('boom'); }); } catch (e) { m = e.message; } m;", "boom");
    /* GetSubstitution with a string pattern: $` $' too; $n and $<name> stay literal */
    eq("'abcabc'.replace('b', '[$`|$\\'|$$|$1|$<x>]');", "a[a|cabc|$|$1|$<x>]cabc");
    eq("['a.b.c'.replaceAll('.', '$`'), 'xyxy'.replaceAll('xy', \"$'\"), 'ab'.replaceAll('', \"$`$'|\")].join(' ');",
       "aaba.bc xy ab|aab|bab|");
    eq("'foo'.concat('bar', 'baz');", "foobarbaz");
    eq("String.fromCharCode(72, 105);", "Hi");
    eq("String.fromCharCode(65 + 65536);", "A"); /* ToUint16 wraps mod 2**16 */
    eq("String.fromCodePoint(72, 105);", "Hi");
    eq("String.fromCodePoint(0x1F600).length;", "2"); /* astral: a surrogate pair */
    err("String.fromCodePoint(-1);", "RangeError");
    err("String.fromCodePoint(0x110000);", "RangeError"); /* > max code point */
    err("String.fromCodePoint(1.5);", "RangeError"); /* non-integer */
    err("String.fromCodePoint(1e20);", "RangeError"); /* was UB: out-of-range cast */
    err("'x'.repeat(1e20);", "RangeError"); /* was UB: out-of-range cast */
    eq("String(42);", "42");
    eq("String(true);", "true");
    /* `new String(...)`: no boxed-primitive type here, so `new` on a native
     * conversion function just calls it and returns the primitive. */
    eq("new String(42);", "42");
    eq("typeof new String('x');", "string");
    /* A `.length` is UTF-16 code units, and `é` is one of them — the same
     * answer node gives. This asserted 5 for years, because the harness fed
     * source in a byte per unit, so `é` arrived as two characters. */
    eq("'caf\xc3\xa9'.length;", "4");
    /* chaining */
    eq("'  Hello World  '.trim().toLowerCase().split(' ').join('-');", "hello-world");
}

static void test_array(void) {
    eq("[1,2,3].length;", "3");
    eq("let a=[1,2]; a.push(3); a.join(',');", "1,2,3");
    eq("let a=[1,2,3]; a.pop() + '/' + a.length;", "3/2");
    eq("let a=[1,2,3]; a.shift() + '/' + a.join(',');", "1/2,3");
    eq("let a=[2,3]; a.unshift(0,1); a.join(',');", "0,1,2,3");
    eq("[1,2,3,4].slice(1,3).join(',');", "2,3");
    eq("[3,1,2].indexOf(2);", "2");
    eq("[1,2,3].includes(2);", "true");
    eq("[1,2,3].join('-');", "1-2-3");
    eq("[1,2,3].concat([4,5]).join(',');", "1,2,3,4,5");
    eq("[1,2,3].reverse().join(',');", "3,2,1");
    eq("[1,2,3].at(-1);", "3");
    eq("[1,2,3].map(x => x * 2).join(',');", "2,4,6");
    eq("[1,2,3,4].filter(x => x % 2 === 0).join(',');", "2,4");
    eq("[1,2,3,4].reduce((a,b) => a + b, 0);", "10");
    eq("[1,2,3,4].reduce((a,b) => a + b);", "10");
    eq("[1,2,3].some(x => x > 2);", "true");
    eq("[1,2,3].every(x => x > 0);", "true");
    eq("[1,2,3,4].find(x => x > 2);", "3");
    eq("[1,2,3,4].findIndex(x => x > 2);", "2");
    eq("let s=0; [1,2,3].forEach(x => s += x); s;", "6");
    eq("[3,1,2].sort().join(',');", "1,2,3");
    eq("[3,1,2].sort((a,b) => b - a).join(',');", "3,2,1");
    eq("[10,9,1,20].sort().join(',');", "1,10,20,9"); /* default: string sort */
    eq("[10,9,1,20].sort((a,b) => a - b).join(',');", "1,9,10,20");
    eq("[[1,2],[3,[4,5]]].flat().length;", "4"); /* one level: [1,2,3,[4,5]] */
    eq("[[1,2],[3,[4,5]]].flat(2).join(',');", "1,2,3,4,5");
    eq("[1,2,3].fill(0).join(',');", "0,0,0");
    eq("Array.isArray([1,2]);", "true");
    eq("Array.isArray('no');", "false");
    eq("Array.of(1,2,3).join(',');", "1,2,3");
    eq("Array.from('abc').join(',');", "a,b,c");
    eq("Array.from([1,2,3], x => x*x).join(',');", "1,4,9");
    /* map/filter chain */
    eq("[1,2,3,4,5].filter(x => x % 2).map(x => x * 10).join(',');", "10,30,50");
    /* Array as a constructor: callable with or without `new`, same result */
    eq("typeof Array;", "function");
    eq("Array(3).length;", "3");
    eq("new Array(3).length;", "3");
    eq("Array(3)[0];", "undefined");
    eq("Array(1,2,3).join(',');", "1,2,3");
    eq("new Array(1,2,3).join(',');", "1,2,3");
    eq("Array('x').join(',');", "x");
    eq("Array().length;", "0");
    eq("Object.getPrototypeOf(Array(1)) === Array.prototype;", "true");
    err("Array(-1);", "RangeError");
    err("Array(1.5);", "RangeError");
    err("Array(4294967296);", "RangeError");
}

static void test_number(void) {
    /* shortest round-trip digits, nearest the value: the strings V8 prints (each was wrong before:
     * a last digit one too high or a 17th digit, from rounding a rounded string; powers of two
     * at the bottom of a binade need the neighbouring candidate) */
    eq("String(0.09983341664682815);", "0.09983341664682815");
    eq("String(6.557954545454544e-7);", "6.557954545454544e-7");
    eq("String(8.273471959672337e-8);", "8.273471959672337e-8");
    eq("String(9.542280822064698e-288);", "9.542280822064698e-288");
    eq("String(-5.415468657893605e-309);", "-5.415468657893605e-309");
    eq("String(-8.407692237696064e-252);", "-8.407692237696064e-252");
    eq("String(6.784148124086405e-7);", "6.784148124086405e-7");
    eq("String(8.475743555849305e-7);", "8.475743555849305e-7");
    eq("String(7.678447687145631e-239);", "7.678447687145631e-239");
    eq("String(6.142758149716505e-238);", "6.142758149716505e-238");
    eq("String(6.290184345309701e-235);", "6.290184345309701e-235");
    /* parsing is musl's decfloat, not the C library's strtod (Windows' rounds the wrong way next to
     * a power of two), and reads every digit of a long literal: each expected value is Node's */
    eq("7.67844768714563e-239 === 7.678447687145631e-239;", "false");
    eq("6.142758149716504e-238 === 6.142758149716505e-238;", "false");
    eq("String(9007199254740993);", "9007199254740992");                        /* a tie goes to even */
    eq("String(9007199254740993.0000000001);", "9007199254740994");             /* digit 27 breaks the tie */
    eq("String(Number('9007199254740993.0000000001'));", "9007199254740994");
    eq("String(Number('-9007199254740993.00000000000000000000001'));", "-9007199254740994");
    eq("String(0.1000000000000000055511151231257827021181583404541015625);", "0.1"); /* 0.1 exactly */
    eq("String(1.00000000000000011102230246251565404236316680908203125);", "1");      /* 1 + 2^-53: a tie, to even */
    eq("String(1.000000000000000111022302462515654042363166809082031250000000000001);", "1.0000000000000002");
    eq("String(2.4703282292062327e-324);", "0");                                  /* under half the least */
    eq("String(2.4703282292062328e-324);", "5e-324");
    eq("String(1.7976931348623158e308);", "1.7976931348623157e+308");
    eq("String(1.7976931348623159e308);", "Infinity");
    eq("String(Number('1' + '0'.repeat(400) + 'e-400'));", "1");
    eq("String(.5e1);", "5");
    eq("String(0.1 + 0.2);", "0.30000000000000004");
    eq("String(1 / 3);", "0.3333333333333333");
    eq("String(5e-324);", "5e-324");
    eq("String(1.7976931348623157e308);", "1.7976931348623157e+308");
    eq("String(2 ** -1022);", "2.2250738585072014e-308");
    eq("String(2 ** 60);", "1152921504606847000");
    eq("String(1e21);", "1e+21");
    eq("String(123456789012345680000);", "123456789012345680000");
    eq("String(1e-7);", "1e-7");
    eq("String(0.000001);", "0.000001");
    eq("String(-0.0000015);", "-0.0000015");
    eq("String(4.35);", "4.35");
    eq("String(2 / 3 * 1e-300);", "6.666666666666667e-301");
    eq("(3.14159).toFixed(2);", "3.14");
    eq("(0).toFixed(2);", "0.00");
    eq("(255).toString(16);", "ff");
    eq("(255).toString(2);", "11111111");
    eq("(-42).toFixed(0);", "-42");
    eq("(1.005).toFixed(2);", "1.00"); /* binary repr < 1.005 */
    eq("new Number(5) + 1;", "6");
    eq("typeof new Boolean(false);", "boolean");
    eq("Number('42');", "42");
    eq("Number('  3.5  ');", "3.5");
    eq("Number.isInteger(5);", "true");
    eq("Number.isInteger(5.5);", "false");
    eq("Number.isNaN(NaN);", "true");
    eq("Number.isFinite(Infinity);", "false");
    eq("Number.MAX_SAFE_INTEGER;", "9007199254740991");
    eq("parseInt('42px');", "42");
    eq("parseInt('ff', 16);", "255");
    eq("parseInt('0x1F');", "31");
    eq("parseFloat('3.14abc');", "3.14");
    eq("parseInt('   -17  ');", "-17");
    eq("isNaN('abc');", "true");
    eq("isFinite(42);", "true");
}

/* Own keys enumerate as the language orders them: array-index names first, ascending, then the
 * rest in the order they were first set. Object.keys/values/entries, JSON.stringify, spread and
 * Object.assign all walk the same order. */
static void test_key_order(void) {
    eq_s("Object.keys({ zeta: 1, body: 2, lid: 3, alpha: 4 }).join();", "zeta,body,lid,alpha");
    eq_s("const p = (() => { const body = 1, lid = 2; return { body, lid }; })(); Object.keys(p).join();", "body,lid");
    eq_s("Object.keys({ b: 1, 10: 2, a: 3, 2: 4, 0: 5 }).join();", "0,2,10,b,a");
    eq_s("Object.keys({ '-1': 1, '01': 2, '1.5': 3, 4294967295: 4, 4294967294: 5, x: 6 }).join();",
         "4294967294,-1,01,1.5,4294967295,x");
    eq_s("const o = { a: 1, b: 2, c: 3 }; delete o.a; o.a = 4; Object.keys(o).join();", "b,c,a");
    eq_s("const o = { a: 1 }; o[5] = 1; o[3] = 1; delete o[5]; o.z = 1; o[1] = 1; Object.keys(o).join();", "1,3,a,z");
    eq_s("Object.values({ y: 'Y', 1: 'one', x: 'X', 0: 'zero' }).join();", "zero,one,Y,X");
    eq_s("Object.entries({ b: 1, 3: 2 }).map((e) => e.join('=')).join();", "3=2,b=1");
    eq_s("JSON.stringify({ b: 1, 10: 2, a: { z: 1, 9: 2 }, 2: 3 });", "{\"2\":3,\"10\":2,\"b\":1,\"a\":{\"9\":2,\"z\":1}}");
    eq_s("Object.keys({ ...{ b: 1, 7: 2 }, a: 3, 1: 4 }).join();", "1,7,b,a");
    eq_s("Object.keys(Object.assign({ z: 0 }, { 4: 1, y: 2 })).join();", "4,z,y");
    eq_s("const o = JSON.parse('{\"b\":1,\"3\":2,\"a\":3,\"1\":4}'); Object.keys(o).join();", "1,3,b,a");
    /* an array's named properties come after its elements */
    eq_s("const a = [10, 20]; a.x = 1; a.y = 2; Object.keys(a).join() + ':' + Object.values(a).join();", "0,1,x,y:10,20,1,2");
    eq_s("const a = ['p']; a.n = 'q'; Object.entries(a).map((e) => e.join('=')).join();", "0=p,n=q");
    /* a function's own properties too */
    eq_s("function F() {} F.b = 1; F[2] = 1; F.a = 1; Object.keys(F).join();", "2,b,a");
    /* many shuffled integer keys, then the walk after more writes */
    eq_s("const o = {}; for (let i = 0; i < 5000; i++) o[(i * 7919) % 5003] = i; o.s = 1; o[1] = 0;"
         " const k = Object.keys(o); let up = true; for (let i = 1; i < k.length - 1; i++) if (+k[i] <= +k[i - 1]) up = false;"
         " [k.length, k[0], k[k.length - 2], k[k.length - 1], up].join();", "5001,0,5002,s,true");
}

static void test_function(void) {
    /* Function.prototype.call */
    eq_s("function f(a, b) { return this.x + a + b; } f.call({ x: 1 }, 2, 3);", "6");
    eq_s("function g() { return this; } g.call() === undefined;", "true");
    eq_s("function g() { return this; } g.call(null) === null;", "true");
    eq_s("const o = { n: 5, get: function () { return this.n; } }; o.get.call({ n: 7 });", "7");
    eq_s("const a = () => typeof this; a.call({ v: 1 });", "undefined"); /* arrows keep their own this */
    eq_s("Math.max.call(null, 3, 9, 4);", "9");                         /* natives */
    eq_s("Object.keys.call(null, { a: 1, b: 2 }).length;", "2");        /* a native with statics still has call */
    eq_s("Array.prototype.join.call([1, 2], '+');", "1+2");
    eq_s("function f() { return this.k; } f.call.call(f, { k: 2 });", "2");
    eq_s("function fact(n) { return n <= 1 ? 1 : n * fact.call(null, n - 1); } fact.call(null, 10);", "3628800");
    eq_s("const add = function (a, b) { return a + b; }; [[1, 2], [3, 4]].map((p) => add.call(null, p[0], p[1])).join();", "3,7");
    eq_s("function t() { throw new RangeError('no'); } let m = ''; try { t.call(null); } catch (e) { m = e.name + ':' + e.message; } m;", "RangeError:no");
    eq_s("async function af(x) { return x * 2; } await af.call(null, 4);", "8");
    eq_s("typeof (function () {}).call;", "function");
    err("const call = (function () {}).call; call(1);", "TypeError");
    /* Function.prototype.apply */
    eq_s("function f(a, b) { return this.x + a + b; } f.apply({ x: 1 }, [2, 3]);", "6");
    eq_s("function n() { return arguments.length; } n.apply(null) + n.apply(null, null) + n.apply(null, []);", "0");
    eq_s("Math.max.apply(null, [3, 9, 4]);", "9");
    eq_s("function f() { return Array.prototype.join.call(arguments, '-'); } function g() { return f.apply(null, arguments); } g(1, 2, 3);", "1-2-3");
    eq_s("function f(a, b) { return a - b; } const xs = []; for (let i = 0; i < 1000; i++) xs.push(i); Math.max.apply(null, xs) + f.apply(null, [5, 2]);", "1002");
    err("function f() {} f.apply(null, 3);", "TypeError");
    err("function f() {} f.apply(null, { length: 1, 0: 'a' });", "TypeError");
    /* Function.prototype.bind */
    eq_s("function f(a, b) { return this.x + a + b; } const g = f.bind({ x: 1 }, 2); g(3);", "6");
    eq_s("function f(a, b) { return this.x + a + b; } const g = f.bind({ x: 1 }); g(2, 3) + g(10, 10);", "27");
    eq_s("function who() { return this.n; } const o = { n: 1, w: who.bind({ n: 2 }) }; o.w();", "2"); /* bound this wins */
    eq_s("function who() { return this.n; } const b = who.bind({ n: 2 }); b.call({ n: 3 });", "2");
    eq_s("function sum(...a) { return a.reduce((s, v) => s + v, 0); } sum.bind(null, 1, 2).bind(null, 3)(4);", "10");
    eq_s("const pow = Math.pow.bind(null, 2); [1, 2, 3].map((e) => pow(e)).join();", "2,4,8");
    eq_s("const o = { k: 5, get: function () { return this.k; } }; const fs = []; for (let i = 0; i < 300; i++) fs.push(o.get.bind({ k: i })); fs[299]() + fs[0]();", "299");
    eq_s("typeof (function () {}).bind(null);", "function");
    eq_s("async function af(x) { return this.m * x; } await af.bind({ m: 3 })(4);", "12");
    eq_s("function t() { throw new RangeError('b'); } let m = ''; try { t.bind(null)(); } catch (e) { m = e.name; } m;", "RangeError");
    err("const bind = (function () {}).bind; bind(1);", "TypeError");
    /* new on a bound function constructs the target with the presets; the bound this is ignored */
    eq_s("function P(x, y) { this.x = x; this.y = y; } const B = P.bind({ ignored: 1 }, 4); const p = new B(5); [p.x, p.y, p.ignored === undefined].join();", "4,5,true");
    eq_s("function P(a, b, c) { this.s = a + b + c; } const B = P.bind(null, 1).bind(null, 2); new B(3).s;", "6");
    eq_s("function P(x) { this.x = x; } P.prototype.twice = function () { return 2 * this.x; }; const B = P.bind(null); new B(...[21]).twice();", "42");
    err("const B = (() => 1).bind(null); new B();", "TypeError");
    /* properties on function values: statics, constants, caches */
    eq_s("function F(x) { this.x = x; } F.from = (v) => new F(v * 2); F.ORIGIN = new F(0); F.from(3).x + F.ORIGIN.x;", "6");
    eq_s("const f = () => 1; f.tag = 'a'; f.tag + f();", "a1");
    eq_s("function c() { c.n = (c.n || 0) + 1; return c.n; } c(); c(); c();", "3");
    eq_s("function F() {} F.V = 2; ['V' in F, 'nope' in F, 'call' in F, 'prototype' in F].join();", "true,false,true,true");
    eq_s("const f = () => 0; f.a = 1; delete f.a; [f.a === undefined, delete f.never].join();", "true,true");
    eq_s("function F() {} F.b = 2; F.a = 1; Object.keys(F).sort().join() + ':' + Object.values(F).sort().join();", "a,b:1,2");
    eq_s("[Object.keys(function () {}).length, Object.keys(Object).length, Object.entries(() => 0).length].join();", "0,0,0");
    eq_s("function F() {} Object.assign(F, { a: 1, b: 2 }); const c = Object.assign({}, F); F.a + F.b + c.a + c.b;", "6");
    eq_s("function F() {} F.x = 1; [F.hasOwnProperty('x'), F.hasOwnProperty('call'), Object.hasOwn(F, 'x'), Object.hasOwn(F, 'y')].join();", "true,false,true,false");
    eq_s("const f = () => 0; f.a = 1; Object.freeze(f); let m = ''; try { f.a = 2; } catch (e) { m = e.name; } m + f.a + Object.isFrozen(f) + Object.isFrozen(() => 0);", "TypeError1truefalse");
    eq_s("const f = () => 0; Object.seal(f); let m = ''; try { f.z = 1; } catch (e) { m = e.name; } m + Object.isSealed(f);", "TypeErrortrue");
    eq_s("Math.max.tag = 1; Math.max.tag + Math.max(2, 3);", "4");
    eq_s("const k = Object.keys; Object.keys = () => 'mine'; const r = Object.keys({ a: 1 }); Object.keys = k; r + Object.keys({ b: 1 }).join();", "mineb");
    eq_s("function F() {} F.prototype.hi = function () { return 'hi'; }; F.s = 1; new F().hi() + F.s;", "hi1");
    eq_s("function F() {} for (let i = 0; i < 500; i++) F['k' + i] = { v: i }; F.k499.v + Object.keys(F).length;", "999");
    eq_s("const b = (function () {}).bind(null); b.z = 3; b.z;", "3");
    eq_s("const f = () => 1; f.call = 5; f.call;", "5");
    eq_s("const make = () => { const g = () => 0; g.big = []; for (let i = 0; i < 100; i++) g.big.push({ i }); return g; }; let t = 0; for (let j = 0; j < 50; j++) t += make().big[99].i; t;", "4950");
}

static void test_math(void) {
    eq("Math.abs(-5);", "5");
    eq("Math.floor(3.7);", "3");
    eq("Math.ceil(3.2);", "4");
    eq("Math.round(2.5);", "3");
    eq("Math.round(-2.5);", "-2");
    eq("Math.trunc(-3.7);", "-3");
    eq("Math.sign(-8);", "-1");
    eq("Math.sqrt(144);", "12");
    eq("Math.cbrt(27);", "3");
    eq("Math.pow(2, 10);", "1024");
    eq("Math.min(3, 1, 2);", "1");
    eq("Math.max(3, 1, 2);", "3");
    eq("Math.hypot(3, 4);", "5");
    eq("Math.max(...[5, 2, 8, 1]);", "8");
    /* transcendentals: check to a few digits via rounding */
    eq("Math.round(Math.exp(1) * 1000);", "2718");
    eq("Math.round(Math.log(Math.E) * 1000);", "1000");
    eq("Math.round(Math.log2(8) * 1000);", "3000");
    eq("Math.round(Math.log10(1000) * 1000);", "3000");
    eq("Math.round(Math.sin(Math.PI / 2) * 1000);", "1000");
    eq("Math.round(Math.cos(0) * 1000);", "1000");
    eq("Math.round(Math.sin(Math.PI) * 1000);", "0");
    eq("Math.round(Math.atan2(1, 1) * 4 * 1000) === Math.round(Math.PI * 1000);", "true");
    eq("Math.round(Math.tan(Math.PI / 4) * 1000);", "1000");
    /* random is deterministic (fixed seed) and in range */
    eq("let r = Math.random(); r >= 0 && r < 1;", "true");
    eq("Math.round(Math.PI * 100);", "314");
}

static void test_json(void) {
    eq("JSON.stringify(42);", "42");
    eq("JSON.stringify('hi');", "\"hi\"");
    eq("JSON.stringify(true);", "true");
    eq("JSON.stringify(null);", "null");
    eq("JSON.stringify([1,2,3]);", "[1,2,3]");
    eq("JSON.stringify([1,'a',null,true]);", "[1,\"a\",null,true]");
    eq("JSON.stringify({a: 1});", "{\"a\":1}");
    eq("JSON.stringify('a\\nb');", "\"a\\nb\"");
    eq("JSON.stringify([undefined, function(){}]);", "[null,null]");
    eq("JSON.stringify([1,2], null, 2).indexOf('\\n') > 0;", "true");
    /* parse */
    eq("JSON.parse('42');", "42");
    /* JSON.parse errors say what was expected and where, as V8 words them */
    err("JSON.parse('');", "SyntaxError: Unexpected end of JSON input");
    err("JSON.parse('[1,');", "SyntaxError: Unexpected end of JSON input");
    err("JSON.parse('tru');", "SyntaxError: Unexpected end of JSON input");
    err("JSON.parse('[1, x]');", "SyntaxError: Unexpected token 'x', \"[1, x]\" is not valid JSON");
    err("JSON.parse('[1,2,3,4,5,6,7,8,9,10,11,12, x,13,14,15,16,17,18,19,20,21,22,23,24,25]');",
        "SyntaxError: Unexpected token 'x', ...\"10,11,12, x,13,14,15\"... is not valid JSON");
    err("JSON.parse('{\"a\":1,}');", "SyntaxError: Expected double-quoted property name in JSON at position 7 (line 1 column 8)");
    err("JSON.parse('{,}');", "SyntaxError: Expected property name or '}' in JSON at position 1 (line 1 column 2)");
    err("JSON.parse('{\"a\" 1}');", "SyntaxError: Expected ':' after property name in JSON at position 5 (line 1 column 6)");
    err("JSON.parse('[1 2]');", "SyntaxError: Expected ',' or ']' after array element in JSON at position 3 (line 1 column 4)");
    err("JSON.parse('\\n\\n  {\"a\" 1}');", "SyntaxError: Expected ':' after property name in JSON at position 9 (line 3 column 8)");
    err("JSON.parse('\"abc');", "SyntaxError: Unterminated string in JSON at position 4 (line 1 column 5)");
    err("JSON.parse('\"a\\tb\"');", "SyntaxError: Bad control character in string literal in JSON at position 2");
    err("JSON.parse('\"\\\\q\"');", "SyntaxError: Bad escaped character in JSON at position 2");
    err("JSON.parse('01');", "SyntaxError: Unexpected number in JSON at position 1");
    err("JSON.parse('-a');", "SyntaxError: No number after minus sign in JSON at position 1");
    err("JSON.parse('1.e5');", "SyntaxError: Unterminated fractional number in JSON at position 2");
    err("JSON.parse('1e+');", "SyntaxError: Exponent part is missing a number in JSON at position 3");
    err("JSON.parse('1 2');", "SyntaxError: Unexpected non-whitespace character after JSON at position 2 (line 1 column 3)");
    eq("JSON.parse('\"hello\"');", "hello");
    eq("JSON.parse('[1,2,3]').join(',');", "1,2,3");
    eq("JSON.parse('{\"a\":1,\"b\":2}').a;", "1");
    eq("JSON.parse('true');", "true");
    eq("JSON.parse('null') === null;", "true");
    eq("JSON.parse('[{\"x\":10}]')[0].x;", "10");
    eq("JSON.parse('  [ 1 , 2 ] ').length;", "2");
    /* round trip */
    eq("JSON.parse(JSON.stringify({n: 7, s: 'hi', a: [1,2]})).a[1];", "2");
    eq("JSON.stringify(JSON.parse('[1,2,3]'));", "[1,2,3]");
}

static void test_object(void) {
    eq("Object.keys({a:1, b:2}).length;", "2");
    eq("Object.values({a:1, b:2}).reduce((x,y)=>x+y, 0);", "3");
    eq("Object.entries({a:1}).length;", "1");
    eq("Object.entries({a:1})[0][0];", "a");
    eq("let o = Object.assign({}, {a:1}, {b:2}); o.a + o.b;", "3");
    eq("Object.keys([10,20,30]).join(',');", "0,1,2");
    eq("Object.fromEntries([['a',1],['b',2]]).b;", "2");
    eq("Object.hasOwn({x:1}, 'x');", "true");
    eq("Object.hasOwn({x:1}, 'y');", "false");
    /* was UB (out-of-range double->uint32_t cast) and returned true */
    eq("Object.hasOwn([1,2,3], '1e20');", "false");
    eq("[1,2,3].hasOwnProperty('1e20');", "false");
    eq("[1,2,3].hasOwnProperty(1);", "true");
    eq("[1,2,3].hasOwnProperty(5);", "false");
    eq("let o = Object.freeze({a:1}); o.a;", "1");
    /* build object from entries after transform */
    eq("Object.fromEntries(Object.entries({a:1,b:2}).map(([k,v]) => [k, v*10])).a;", "10");
    /* Object as a constructor: callable with or without `new` */
    eq("typeof Object;", "function");
    eq("Object.keys(Object()).length;", "0");
    eq("Object.keys(new Object()).length;", "0");
    eq("Object.keys(Object(null)).length;", "0");
    eq("Object.keys(Object(undefined)).length;", "0");
    eq("let a = [1,2]; Object(a) === a;", "true");
    eq("let o = {x:1}; Object(o) === o;", "true");
    eq("typeof Object.keys;", "function"); /* statics survive the callable rewrite */
    /* Object.prototype: every plain object inherits it, and every other
     * built-in prototype chains up to it in turn. */
    eq("typeof ({}).hasOwnProperty;", "function");
    eq("typeof ({}).toString;", "function");
    eq("typeof ({}).valueOf;", "function");
    eq("({}).toString();", "[object Object]");
    eq("let o = {}; o.valueOf() === o;", "true");
    eq("({x:1}).hasOwnProperty('x');", "true");
    eq("({x:1}).hasOwnProperty('y');", "false");
    eq("[1,2].hasOwnProperty(0);", "true");
    eq("[1,2].hasOwnProperty(5);", "false");
    eq("Object.getPrototypeOf({}) === Object.prototype;", "true");
    eq("Object.getPrototypeOf(Object.prototype);", "null");
    eq("Object.getPrototypeOf(Array.prototype) === Object.prototype;", "true");
    eq("Object.getPrototypeOf(Date.prototype) === Object.prototype;", "true");
    eq("Object.getPrototypeOf(Map.prototype) === Object.prototype;", "true");
    eq("Object.getPrototypeOf(Set.prototype) === Object.prototype;", "true");
    eq("Object.getPrototypeOf(RegExp.prototype) === Object.prototype;", "true");
    eq("function F() {} Object.getPrototypeOf(F.prototype) === Object.prototype;", "true");
    eq("function F() {} new F().hasOwnProperty('x');", "false");
    eq("globalThis.hasOwnProperty('Math');", "true");
    /* own-property semantics still bypass inherited ones */
    eq("Object.keys({}).length;", "0"); /* inherited methods aren't own/enumerable */
}

/* No time zone support: every value is UTC (each getX()/getUTCX() pair and
 * each setX()/setUTCX() pair is literally the same function), so these use
 * explicit millisecond/component/ISO-string forms rather than the host
 * clock. */
static void test_date(void) {
    /* component construction + getters */
    eq("new Date(2024, 0, 15, 10, 30, 45, 500).getFullYear();", "2024");
    eq("new Date(2024, 0, 15).getMonth();", "0");
    eq("new Date(2024, 0, 15).getDate();", "15");
    eq("new Date(2024, 0, 15).getDay();", "1"); /* Monday */
    eq("new Date(2024, 0, 15, 10, 30, 45, 500).getHours();", "10");
    eq("new Date(2024, 0, 15, 10, 30, 45, 500).getMinutes();", "30");
    eq("new Date(2024, 0, 15, 10, 30, 45, 500).getSeconds();", "45");
    eq("new Date(2024, 0, 15, 10, 30, 45, 500).getMilliseconds();", "500");
    eq("new Date(0).getDay();", "4"); /* 1970-01-01 was a Thursday */
    eq("new Date(0).getTimezoneOffset();", "0");
    /* numeric timestamp construction */
    eq("new Date(0).toISOString();", "1970-01-01T00:00:00.000Z");
    eq("new Date(0).getTime();", "0");
    /* month/day overflow normalization (calendar carry, not clamping) */
    eq("new Date(2024, 12, 1).getFullYear();", "2025");
    eq("new Date(2024, 12, 1).getMonth();", "0");
    eq("new Date(2024, 0, 0).getFullYear() + '-' + new Date(2024, 0, 0).getMonth() +"
       " '-' + new Date(2024, 0, 0).getDate();", "2023-11-31");
    /* leap year */
    eq("new Date(2024, 1, 29).getDate();", "29");
    eq("new Date(2024, 1, 29, 12, 0, 0, 0).getTime() -"
       " new Date(2024, 0, 1).getTime();", "5140800000"); /* 59.5 days */
    /* ISO 8601 parsing */
    eq("new Date('2024-03-05T12:00:00.250Z').getTime();", "1709640000250");
    eq("new Date('2024-03-05').toISOString();", "2024-03-05T00:00:00.000Z");
    eq("new Date('2024-03-05T00:00:00+02:00').getTime() -"
       " new Date('2024-03-05T00:00:00Z').getTime();", "-7200000");
    /* Date.now / Date.UTC / Date.parse */
    eq("typeof Date.now();", "number");
    eq("Date.UTC(2024, 0, 1);", "1704067200000");
    eq("Date.parse('1970-01-01T00:00:00.000Z');", "0");
    /* invalid dates */
    eq("isNaN(new Date('not a date').getTime());", "true");
    eq("new Date('not a date').toString();", "Invalid Date");
    err("new Date('bad').toISOString();", "RangeError");
    /* setters (return the new time value; mutate in place) */
    eq("let d = new Date(2024, 0, 1); d.setFullYear(2025); d.getFullYear();", "2025");
    eq("let d = new Date(2024, 0, 1); d.setMonth(5); d.getMonth();", "5");
    eq("let d = new Date(2024, 0, 1); d.setMonth(5); d.getDate();", "1"); /* day untouched */
    eq("let d = new Date(2024, 0, 1); d.setDate(15); d.getDate();", "15");
    eq("let d = new Date(0); d.setHours(5, 30); d.getHours() + '/' + d.getMinutes();",
       "5/30");
    /* implicit ToString coercion (+, template literals) is special-cased,
     * same mechanism as RegExp, since this engine has no general
     * toString/valueOf lookup for objects */
    eq("'' + new Date(0);", "Thu Jan 01 1970 00:00:00 GMT+0000 (UTC)");
    eq("new Date(0).toUTCString();", "Thu, 01 Jan 1970 00:00:00 GMT");
    /* Date instances are ordinary objects: expandos, typeof, toJSON */
    eq("let d = new Date(0); d.custom = 42; d.custom;", "42");
    eq("typeof new Date();", "object");
    eq("new Date(0).toJSON();", "1970-01-01T00:00:00.000Z");
    eq("new Date('bad').toJSON();", "null");
    /* Date -> Date construction copies the time value */
    eq("let a = new Date(12345); let b = new Date(a); b.getTime();", "12345");
}

/* Map: O(n) linear-scan storage (documented in js_mapobj.h, same tradeoff
 * as Array.prototype.sort's O(n^2)); keys() / values() / entries() return
 * real (materialized) arrays rather than a lazy MapIterator, and `for...of`
 * on a Map substitutes an entries() snapshot at JS_OP_ITER_NEW. */
static void test_map(void) {
    /* basic mutators + size */
    eq("let m = new Map(); m.set('a', 1); m.set('b', 2); m.size;", "2");
    eq("let m = new Map(); m.get('missing');", "undefined");
    eq("let m = new Map(); m.set('a', 1); m.get('a');", "1");
    eq("let m = new Map(); m.set('a', 1); m.has('a') + '/' + m.has('z');",
       "true/false");
    eq("let m = new Map(); m.set('a', 1); m.delete('a') + '/' + m.size;", "true/0");
    eq("let m = new Map(); m.delete('missing');", "false");
    eq("let m = new Map(); m.set('a',1); m.set('b',2); m.clear(); m.size;", "0");
    /* set() is chainable and returns the map */
    eq("let m = new Map(); m.set('a', 1).set('b', 2).size;", "2");
    /* re-setting an existing key updates the value without moving it */
    eq("let m = new Map(); m.set('a',1).set('b',2); m.set('a',9);"
       " [...m.keys()].join(',');", "a,b");
    eq("let m = new Map(); m.set('a',1); m.set('a',9); m.get('a');", "9");

    /* SameValueZero key equality */
    eq("let m = new Map(); m.set(NaN, 'x'); m.get(NaN);", "x"); /* NaN === NaN here */
    eq("let m = new Map(); m.set(0, 'z'); m.get(-0);", "z");    /* +0 === -0 */
    eq("let m = new Map(); m.set('a' + '', 1); m.get('a');", "1"); /* strings: by content */
    eq("let m = new Map(); let o = {}; m.set(o, 1); m.get({});", "undefined"); /* objects: by identity */
    eq("let m = new Map(); let o = {}; m.set(o, 1); m.get(o);", "1");
    eq("let m = new Map(); m.set(1, 'num'); m.set('1', 'str'); m.size;", "2"); /* type matters */

    /* construction from an iterable of pairs, and from another Map */
    eq("new Map([['a',1],['b',2]]).size;", "2");
    eq("new Map([['a',1],['b',2]]).get('b');", "2");
    eq("new Map().size;", "0");
    eq("new Map(null).size;", "0");
    eq("let a = new Map([['x',1]]); let b = new Map(a); b.set('x', 2);"
       " a.get('x') + '/' + b.get('x');", "1/2"); /* independent copies */

    /* snapshot views: real, iterable arrays in insertion order */
    eq("[...new Map([['a',1],['b',2]]).keys()].join(',');", "a,b");
    eq("[...new Map([['a',1],['b',2]]).values()].join(',');", "1,2");
    eq("JSON.stringify(new Map([['a',1]]).entries());", "[[\"a\",1]]");

    /* forEach(value, key, map) in insertion order */
    eq("let s = ''; new Map([['a',1],['b',2]]).forEach((v,k) => s += k+v); s;",
       "a1b2");
    /* third forEach callback arg is the map itself */
    eq("let m = new Map([['a',1]]); let seen; m.forEach((v,k,mm) => seen = mm);"
       " seen === m;", "true");

    /* for...of on the Map itself yields [key, value] pairs */
    eq("let out = []; for (const [k, v] of new Map([['a',1],['b',2]]))"
       " out.push(k + ':' + v); out.join(',');", "a:1,b:2");

    /* generic object behavior: expandos, typeof, ToString tag */
    eq("typeof new Map();", "object");
    eq("'' + new Map();", "[object Map]");
    eq("let m = new Map(); m.custom = 5; m.custom;", "5");

    /* error paths */
    err("new Map().forEach(5);", "TypeError: number 5 is not a function");
    err("new Map([1, 2]);", "TypeError: Iterator value 1 is not an entry object");
    err("let s = new Map().set; s();", "TypeError: Method Map.prototype.set called on incompatible receiver undefined");
}

/* Set: same O(n) linear-scan storage and SameValueZero equality as Map
 * (js_setobj.h). keys() === values(); entries() yields [v,v] pairs
 * (spec'd, kept symmetric with Map); `for...of` on a Set yields its values
 * directly (unlike Map, whose default iterator is its entries). */
static void test_set(void) {
    /* basic mutators + size */
    eq("let s = new Set(); s.add(1); s.add(2); s.size;", "2");
    eq("let s = new Set(); s.add(1); s.has(1) + '/' + s.has(9);", "true/false");
    eq("let s = new Set(); s.add(1); s.delete(1) + '/' + s.size;", "true/0");
    eq("let s = new Set(); s.delete('missing');", "false");
    eq("let s = new Set(); s.add(1); s.add(2); s.clear(); s.size;", "0");
    /* add() is chainable; re-adding is a no-op */
    eq("let s = new Set(); s.add(1).add(2).size;", "2");
    eq("let s = new Set(); s.add(1); s.add(1); s.size;", "1");

    /* SameValueZero */
    eq("let s = new Set(); s.add(NaN); s.add(NaN); s.size;", "1");
    eq("let s = new Set(); s.add(NaN); s.has(NaN);", "true");
    eq("let s = new Set(); s.add(0); s.add(-0); s.size;", "1"); /* +0 === -0 */
    eq("let s = new Set(); s.add('a' + ''); s.has('a');", "true"); /* strings: by content */
    eq("let s = new Set(); let o = {}; s.add(o); s.has({});", "false"); /* objects: by identity */
    eq("let s = new Set(); let o = {}; s.add(o); s.has(o);", "true");
    eq("let s = new Set(); s.add(1); s.add('1'); s.size;", "2"); /* type matters */

    /* construction dedupes; from another Set; empty forms */
    eq("new Set([1, 2, 2, 3, 1]).size;", "3");
    eq("[...new Set([1, 2, 2, 3]).values()].join(',');", "1,2,3");
    eq("new Set().size;", "0");
    eq("new Set(null).size;", "0");
    eq("let a = new Set([1]); let b = new Set(a); b.add(2);"
       " a.size + '/' + b.size;", "1/2"); /* independent copies */

    /* keys() is the same view as values(); entries() is [v,v] pairs */
    eq("[...new Set([1,2]).keys()].join(',');", "1,2");
    eq("JSON.stringify(new Set([1,2]).entries());", "[[1,1],[2,2]]");

    /* forEach(value, value, set) in insertion order */
    eq("let s = ''; new Set([1,2]).forEach((v,v2) => s += v+''+v2); s;", "1122");
    eq("let s = new Set([1]); let seen; s.forEach((v,v2,ss) => seen = ss);"
       " seen === s;", "true");

    /* for...of on the Set itself yields values, not [v,v] pairs */
    eq("let out = []; for (const x of new Set([1,2,3])) out.push(x);"
       " out.join(',');", "1,2,3");

    /* generic object behavior */
    eq("typeof new Set();", "object");
    eq("'' + new Set();", "[object Set]");
    eq("let s = new Set(); s.custom = 5; s.custom;", "5");

    /* error paths */
    err("new Set().forEach(5);", "TypeError: number 5 is not a function");
    /* array destructuring takes an iterable apart: a Map or Set by its entries or values */
    eq("const [[k, v]] = new Map([[1, 2]]); k + v;", "3");
    eq("const [a, b] = new Set([4, 5]); a * b;", "20");
    eq("const [c, d] = 'xy'; c + d;", "xy");
    /* refused writes to frozen and sealed arrays, as V8 words the first write */
    err("Object.freeze([1]).push(2);", "TypeError: Cannot add property 1, object is not extensible");
    err("Object.freeze([1, 2]).reverse();", "TypeError: Cannot assign to read only property '0' of object '[object Array]'");
    err("Object.seal([1, 2]).pop();", "TypeError: Cannot delete property '1' of [object Array]");
    eq("Object.seal([]).pop();", "undefined");
    err("Object.freeze([]).pop();", "TypeError: Cannot assign to read only property 'length' of object '[object Array]'");
    err("const a = Object.freeze([1]); a[0] = 2;", "TypeError: Cannot assign to read only property '0' of object '[object Array]'");
    err("const a = Object.freeze([1]); a.length = 0;", "TypeError: Cannot assign to read only property 'length' of object '[object Array]'");
    err("const o = Object.seal({ a: 1 }); o.b = 1;", "TypeError: Cannot add property b, object is not extensible");
    err("const o = Object.freeze({ a: 1 }); delete o.a;", "TypeError: Cannot delete property 'a' of #<Object>");
    err("[1].map('x');", "TypeError: string \"x\" is not a function");
    err("[1].map(null);", "TypeError: object null is not a function");
    err("'a'.repeat(-1);", "RangeError: Invalid count value: -1");
    err("String.fromCodePoint(-1);", "RangeError: Invalid code point -1");
    err("Object.fromEntries([[1, 2], 5]);", "TypeError: Iterator value 5 is not an entry object");
    err("new Promise(5);", "TypeError: Promise resolver 5 is not a function");
    err("new Set(5);", "TypeError: number 5 is not iterable (cannot read property Symbol(Symbol.iterator))");
    err("let a = new Set().add; a();", "TypeError: Method Set.prototype.add called on incompatible receiver undefined");
}

/* GC-sensitive cases: allocation-heavy methods and callbacks that call back
 * into user code (js_call) while building results — exercises rooting. */
static void test_error(void) {
    /* construction, with and without new; message/name/toString */
    eq("const e = new Error('boom'); e.message + '|' + e.name;", "boom|Error");
    eq("const e = Error('boom'); e.message + '|' + e.name;", "boom|Error");
    eq("String(new Error('boom'));", "Error: boom");
    eq("`${new TypeError('bad')}`;", "TypeError: bad");
    eq("new RangeError('r').toString();", "RangeError: r");
    eq("new Error().message === '' && new Error().toString() === 'Error';", "true");
    eq("new Error('x') + '';", "Error: x");
    eq("typeof new Error('x');", "object");
    eq("new Error(42).message;", "42");
    eq("new Error(undefined).message;", "");
    /* the standard subclasses share Error.prototype */
    eq("['TypeError','RangeError','ReferenceError','SyntaxError']"
       ".map(n => Object.getPrototypeOf(new TypeError('x')) === TypeError.prototype).join();",
       "true,true,true,true");
    eq("Object.getPrototypeOf(TypeError.prototype) === Error.prototype;", "true");
    eq("Object.getPrototypeOf(new Error('x')) === Error.prototype;", "true");
    eq("Error.prototype.name + '|' + TypeError.prototype.name + '|' + SyntaxError.prototype.name;",
       "Error|TypeError|SyntaxError");
    eq("new TypeError('x').constructor === TypeError;", "true");
    eq("TypeError.prototype.toString === Error.prototype.toString;", "true");
    /* own message shadows the prototype's; name is inherited but overridable */
    eq("const e = new Error('m'); e.hasOwnProperty('message') + '|' + e.hasOwnProperty('name');",
       "true|false");
    eq("const e = new Error('m'); e.name = 'Custom'; String(e);", "Custom: m");
    eq("const e = new Error('m'); e.message = ''; String(e);", "Error");
    /* toString: either half may be empty */
    eq("const o = { name: '', message: 'only', toString: Error.prototype.toString }; o.toString();",
       "only");
    eq("const o = { toString: Error.prototype.toString }; o.toString();", "Error");
    /* options.cause */
    eq("const c = new Error('inner'); const e = new Error('outer', { cause: c });"
       " e.cause === c;", "true");
    eq("new Error('x', {}).hasOwnProperty('cause');", "false");
    eq("new Error('x', { cause: undefined }).hasOwnProperty('cause');", "true");
    /* throw / catch round-trips the object identity */
    eq("const e = new Error('id'); let got; try { throw e; } catch (x) { got = x; } got === e;",
       "true");
    eq("try { throw new TypeError('t'); } catch (e) { e.name + ': ' + e.message; }", "TypeError: t");
    /* engine-raised errors are Error objects of the matching subclass */
    eq("try { null.x; } catch (e) { e.name; }", "TypeError");
    eq("try { undefinedVariable; } catch (e) { e.name + '|' + e.message; }",
       "ReferenceError|undefinedVariable is not defined");
    eq("try { [].length = -1; } catch (e) { Object.getPrototypeOf(e) === RangeError.prototype; }",
       "true");
    eq("try { new Map([1]); } catch (e) { e.name + '|' + (e.message.length > 0); }", "TypeError|true");
    eq("try { null.x; } catch (e) { typeof e + '|' + (e.message === String(e).slice(11)); }",
       "object|true");
    /* engine errors are worded as V8 words them */
    eq("try { null.x; } catch (e) { '' + e; }",
       "TypeError: Cannot read properties of null (reading 'x')");
    /* rejections from builtins are Error objects too */
    eq("await Promise.resolve().then(() => { null.x; }).catch(e => e.name);", "TypeError");
    eq("await Promise.reject(new SyntaxError('s')).catch(e => e.name + '|' + e.message);",
       "SyntaxError|s");
    eq_s("let s = ''; for (let i = 0; i < 200; i++) { try { null.x; } catch (e) { s = e.name; } } s;",
         "TypeError");
    /* message and cause are own properties V8 does not enumerate; one made later is ordinary */
    eq("JSON.stringify(new Error('j', { cause: 1 }));", "{}");
    eq("const e = new Error('x', { cause: 2 }); [Object.keys(e).length, JSON.stringify({ ...e }),"
       " JSON.stringify(Object.assign({}, e)), e.hasOwnProperty('message'), e.cause].join('|');",
       "0|{}|{}|true|2");
    eq("const e = new Error('x'); e.message = 'y'; const a = Object.keys(e).length;"
       " delete e.message; e.message = 'z'; [a, Object.keys(e).join()].join('|');", "0|message");
    eq("const f = new Error(); f.message = 'later'; f.code = 1; JSON.stringify(f);",
       "{\"message\":\"later\",\"code\":1}");
    eq("let t; try { null.x; } catch (e) { t = e; } JSON.stringify(t);", "{}");
    /* uncaught: the completion is the Error object; the host sees its text */
    err("throw new RangeError('top');", "RangeError: top");
    err("throw new Error('top');", "Error: top");
}

static void test_stress(void) {
    eq_s("'a-b-c-d'.split('-').map(s => s.toUpperCase()).join('');", "ABCD");
    eq_s("'ab'.repeat(20).length;", "40");
    eq_s("'x'.padStart(50, 'ab').length;", "50");
    eq_s("[1,2,3,4,5].map(x => x * x).filter(x => x > 4).join(',');", "9,16,25");
    eq_s("[5,3,8,1,9,2].sort((a,b) => a - b).join(',');", "1,2,3,5,8,9");
    eq_s("let s = 0; [1,2,3,4].forEach(x => s += x); s;", "10");
    eq_s("[1,2,3].reduce((acc, x) => acc.concat([x, x]), []).join(',');", "1,1,2,2,3,3");
    eq_s("JSON.stringify([{a:1},{b:2},{c:3}]);", "[{\"a\":1},{\"b\":2},{\"c\":3}]");
    eq_s("JSON.parse('[{\"x\":[1,2,3]},{\"y\":\"hello\"}]')[0].x[2];", "3");
    eq_s("Object.entries({a:1,b:2,c:3}).map(([k,v]) => k + v).sort().join(',');",
         "a1,b2,c3");
    eq_s("Array.from('hello').map(c => c.charCodeAt(0)).join(',');",
         "104,101,108,108,111");
    eq_s("let acc = []; for (let i = 0; i < 10; i++) acc.push(() => i); "
         "acc.map(f => f()).join('');", "0123456789");
    eq_s("[1,2,3,4,5,6].filter(x => x % 2).map(x => 'n' + x).join(',');", "n1,n3,n5");
    eq_s("'the quick brown fox'.split(' ').sort().join(' ');", "brown fox quick the");
}

static void test_integration(void) {
    /* a small templating-ish pipeline */
    eq("let items = [{name:'apple', qty:3}, {name:'pear', qty:0}, {name:'plum', qty:7}];"
       "items.filter(i => i.qty > 0).map(i => i.name.toUpperCase() + ':' + i.qty).join(', ');",
       "APPLE:3, PLUM:7");
    eq("let data = JSON.parse('{\"users\":[{\"n\":\"al\",\"age\":30},{\"n\":\"bo\",\"age\":25}]}');"
       "data.users.sort((a,b) => a.age - b.age).map(u => u.n).join(',');",
       "bo,al");
    eq("let nums = Array.from({length: 5}, (_, i) => i);" /* array-like unsupported -> empty */
       "nums.length;",
       "0");
    eq("[1,2,3,4,5,6,7,8,9,10].filter(n => n % 2 === 0).reduce((a,b) => a + b, 0);", "30");
    eq("'The quick brown fox'.split(' ').map(w => w.length).join(',');", "3,5,5,3");
    eq("JSON.stringify(Object.keys({x:1,y:2}).sort());", "[\"x\",\"y\"]");
}

/*
 * Object.freeze used to return its argument and enforce nothing, which is
 * worse than not having it: the call succeeds, so guest or host code can
 * believe an object is protected when every write still lands. These check
 * the enforcement at each path that can write, because a freeze honoured on
 * one path and forgotten on another is the same footgun in a smaller box.
 *
 * `try { ... } catch (e) { 'threw' }` is the shape throughout: this engine is
 * strict-mode everywhere, so a refused write is a TypeError, not a silent no-op.
 */
static void test_freeze_seal(void) {
    /* writes, additions and deletions all refused on a frozen object */
    eq("const o = Object.freeze({ a: 1 });"
       "try { o.a = 2; 'no'; } catch (e) { 'threw'; }", "threw");
    eq("const o = Object.freeze({ a: 1 });"
       "try { o.b = 2; 'no'; } catch (e) { 'threw'; }", "threw");
    eq("const o = Object.freeze({ a: 1 });"
       "try { delete o.a; 'no'; } catch (e) { 'threw'; }", "threw");
    eq("const o = Object.freeze({ a: 1 }); o.a;", "1");

    /* sealed keeps existing properties writable but fixes the shape */
    eq("const o = Object.seal({ a: 1 }); o.a = 2; o.a;", "2");
    eq("const o = Object.seal({ a: 1 });"
       "try { o.b = 1; 'no'; } catch (e) { 'threw'; }", "threw");
    eq("const o = Object.seal({ a: 1 });"
       "try { delete o.a; 'no'; } catch (e) { 'threw'; }", "threw");

    /* introspection, including the spec's vacuous answer for primitives */
    eq("Object.isFrozen(Object.freeze({}));", "true");
    eq("Object.isSealed(Object.freeze({}));", "true");
    eq("Object.isSealed(Object.seal({}));", "true");
    eq("Object.isFrozen(Object.seal({}));", "false");
    eq("Object.isFrozen({});", "false");
    eq("Object.isFrozen(1) + ':' + Object.isSealed('s');", "true:true");
    /* idempotent, and returns its argument so `const o = freeze({...})` reads */
    eq("const o = {}; Object.freeze(o) === o && Object.isFrozen(Object.freeze(o));", "true");

    /* arrays: indexed writes and length go through the same gate */
    eq("const a = Object.freeze([1, 2]);"
       "try { a[0] = 9; 'no'; } catch (e) { 'threw'; }", "threw");
    eq("const a = Object.freeze([1, 2]);"
       "try { a.length = 0; 'no'; } catch (e) { 'threw'; }", "threw");
    /* ...and the mutators that write the element buffer directly, which never
     * reach set_property at all */
    eq("const a = Object.freeze([1, 2]);"
       "try { a.push(3); 'no'; } catch (e) { 'threw'; }", "threw");
    eq("const a = Object.freeze([1, 2]);"
       "try { a.pop(); 'no'; } catch (e) { 'threw'; }", "threw");
    eq("const a = Object.freeze([2, 1]);"
       "try { a.sort(); 'no'; } catch (e) { 'threw'; }", "threw");
    eq("const a = Object.freeze([1, 2]);"
       "try { a.reverse(); 'no'; } catch (e) { 'threw'; }", "threw");
    eq("const a = Object.freeze([1, 2]);"
       "try { a.fill(0); 'no'; } catch (e) { 'threw'; }", "threw");
    eq("const a = Object.freeze([1, 2]);"
       "try { a.shift(); 'no'; } catch (e) { 'threw'; }", "threw");
    eq("const a = Object.freeze([1, 2]);"
       "try { a.unshift(0); 'no'; } catch (e) { 'threw'; }", "threw");
    /* a sealed array may rearrange what it has, but not resize */
    eq("const a = Object.seal([2, 1]); a.sort(); a.join(',');", "1,2");
    eq("const a = Object.seal([1, 2]); a[0] = 9; a[0];", "9");
    eq("const a = Object.seal([1, 2]);"
       "try { a.push(3); 'no'; } catch (e) { 'threw'; }", "threw");

    /* The point of the exercise: a host can now harden the prototypes a
     * reused context shares between tenants. */
    eq("Object.freeze(Object.prototype);"
       "try { Object.prototype.polluted = 1; 'no'; } catch (e) { 'threw'; }", "threw");
    eq("Object.freeze(Array.prototype);"
       "try { Array.prototype.map = null; 'no'; } catch (e) { 'threw'; }", "threw");
    eq("Object.freeze(Array.prototype); [1, 2].map(function (v) { return v * 2; }).join(',');",
       "2,4");
}

/* Object.create and instanceof, as V8 gives them (Node's results) */
static void test_object_create_instanceof(void) {
    eq("const base = { greet() { return 'hi ' + this.n; } }; const o = Object.create(base); o.n = 'x';"
       " [o.greet(), Object.getPrototypeOf(o) === base, Object.keys(o).join(), o.hasOwnProperty('greet'),"
       " 'greet' in o, Object.create.length].join();",
       "hi x,true,n,false,true,2");
    eq("const n = Object.create(null); n.k = 1; [Object.getPrototypeOf(n), JSON.stringify(n)].join();",
       ",{\"k\":1}");
    err("Object.create(1);", "TypeError: Object prototype may only be an Object or null: 1");
    err("Object.create();", "TypeError: Object prototype may only be an Object or null: undefined");
    err("Object.create({}, { a: { value: 1 } });",
        "TypeError: Object.create with property descriptors is not supported");
    eq("let c; try { null.x; } catch (e) { c = e; }"
       " [new TypeError() instanceof TypeError, new TypeError() instanceof Error, new Error() instanceof TypeError,"
       " c instanceof TypeError, c instanceof RangeError].join();",
       "true,true,false,true,false");
    eq("[[] instanceof Array, [] instanceof Object, ({}) instanceof Array, new Map() instanceof Map,"
       " new Set() instanceof Map, new Date(0) instanceof Date].join();",
       "true,true,false,true,false,true");
    eq("function P() {} function Q() {} Q.prototype = Object.create(P.prototype);"
       " [new P() instanceof P, new Q() instanceof P, new P() instanceof Q,"
       " Object.create(null) instanceof Object, new P() instanceof P.bind(null)].join();",
       "true,true,false,false,true");
    eq("[(() => 0) instanceof Object, Promise.resolve() instanceof Promise, Promise.resolve() instanceof Object,"
       " 1 instanceof Object, 'x' instanceof Object, null instanceof Object, typeof Promise.prototype.then].join();",
       "true,true,true,false,false,false,function");
    err("({}) instanceof 1;", "TypeError: Right-hand side of 'instanceof' is not an object");
    err("({}) instanceof {};", "TypeError: Right-hand side of 'instanceof' is not callable");
}

/* EvalError, URIError and AggregateError; constructors inheriting Error's statics; V8's
 * Object.prototype.toString tags. Expected values are Node's. */
static void test_error_types(void) {
    eq("[typeof EvalError, typeof URIError, typeof AggregateError, EvalError.length, URIError.length, AggregateError.length].join();",
       "function,function,function,1,1,2");
    eq("[String(new EvalError('a')), String(URIError('b')), new EvalError('a') instanceof Error, new URIError() instanceof TypeError].join('|');",
       "EvalError: a|URIError: b|true|false");
    eq("['EvalError', 'URIError', 'AggregateError'].map(n => Object.getPrototypeOf(globalThis[n].prototype) === Error.prototype && globalThis[n].prototype.name === n && globalThis[n].prototype.constructor === globalThis[n]).join();",
       "true,true,true");
    eq("const a = [1, 2]; const e = new AggregateError(a, 'm', { cause: 3 }); [e.name, e.message, e.errors.join('+'), e.errors !== a, e.cause, String(e), e instanceof Error].join('|');",
       "AggregateError|m|1+2|true|3|AggregateError: m|true");
    eq("const e = new AggregateError([1], 'm', { cause: 2 }); [JSON.stringify(e), Object.keys(e).length, JSON.stringify({ ...e }), e.hasOwnProperty('errors')].join('|');",
       "{}|0|{}|true");
    eq("const e = new AggregateError([]); e.errors = 5; const k = Object.keys(e).length; delete e.errors; e.errors = 6; [k, Object.keys(e).join()].join('|');",
       "0|errors");
    eq("[new AggregateError('ab').errors.join(), new AggregateError(new Set([3, 4])).errors.join(), new AggregateError(new Map([[1, 2]])).errors[0].join(':')].join('|');",
       "a,b|3,4|1:2");
    eq("const e = new AggregateError([]); [e.message, e.hasOwnProperty('message'), AggregateError([7]).errors[0]].join('|');",
       "|false|7");
    eq("let m; try { new AggregateError(); } catch (e) { m = String(e); } m;",
       "TypeError: undefined is not iterable (cannot read property Symbol(Symbol.iterator))");
    eq("let m; try { new AggregateError(5); } catch (e) { m = String(e); } m;",
       "TypeError: number 5 is not iterable (cannot read property Symbol(Symbol.iterator))");
    eq("[Object.getPrototypeOf(TypeError) === Error, Object.getPrototypeOf(AggregateError) === Error, Object.getPrototypeOf(Error) === Object.getPrototypeOf(() => 0)].join();",
       "true,true,true");
    eq("[typeof TypeError.captureStackTrace, URIError.stackTraceLimit, 'captureStackTrace' in RangeError, TypeError.hasOwnProperty('captureStackTrace')].join();",
       "function,10,true,false");
    eq("TypeError.stackTraceLimit = 3; const r = [TypeError.stackTraceLimit, Error.stackTraceLimit]; delete TypeError.stackTraceLimit; r.push(TypeError.stackTraceLimit); r.join();",
       "3,10,10");
    eq("const t = v => Object.prototype.toString.call(v); [t(new Error('x')), t(new AggregateError([])), t([]), t(null), t(undefined), t(1), t('s'), t(true), t(() => 1), t(new Date(0)), t(/a/), t(new Map()), t(new Set()), t(Promise.resolve()), t(Math), t(JSON), t({}), t(Object.create(Error.prototype))].join(' ');",
       "[object Error] [object Error] [object Array] [object Null] [object Undefined] [object Number] [object String] [object Boolean] [object Function] [object Date] [object RegExp] [object Map] [object Set] [object Promise] [object Math] [object JSON] [object Object] [object Object]");
    eq("[String(Math), `${JSON}`, ({}).toString()].join(' ');",
       "[object Math] [object JSON] [object Object]");
    eq("[Object.getPrototypeOf(5) === Number.prototype, Object.getPrototypeOf('s') === String.prototype, Object.getPrototypeOf(Promise.resolve()) === Promise.prototype, (5).constructor === Number, 's'.constructor === String, Promise.resolve().constructor === Promise, typeof Number.prototype.toFixed, typeof String.prototype.slice].join();",
       "true,true,true,true,true,true,function,function");
}

/* encodeURI / decodeURI and their component forms. Expected values are Node's. */
static void test_uri(void) {
    eq("[encodeURIComponent(\"a b&c=d/e?f#g;-_.!~*'()\"), encodeURI(\"a b&c=d/e?f#g;-_.!~*'()\")].join('|');",
       "a%20b%26c%3Dd%2Fe%3Ff%23g%3B-_.!~*'()|a%20b&c=d/e?f#g;-_.!~*'()");
    eq("[encodeURIComponent('\xc3\xa9""\xe2\x82\xac""\xf0\x9f\x98\x80""'), encodeURI('\xc3\xa9""\xe2\x82\xac""\xf0\x9f\x98\x80""')].join('|');",
       "%C3%A9%E2%82%AC%F0%9F%98%80|%C3%A9%E2%82%AC%F0%9F%98%80");
    eq("[decodeURIComponent('%C3%A9%E2%82%AC%F0%9F%98%80') === '\xc3\xa9""\xe2\x82\xac""\xf0\x9f\x98\x80""', decodeURIComponent('a%2Fb%23c%2bd'), decodeURI('a%2Fb%23c%2bd%41%20')].join('|');",
       "true|a/b#c+d|a%2Fb%23c%2bdA ");
    eq("[encodeURIComponent(null), encodeURI(12), decodeURIComponent(undefined), encodeURI.length, decodeURIComponent.name].join('|');",
       "null|12|undefined|1|decodeURIComponent");
    eq("const bad = ['%', '%2', '%G0', '%C3', '%C3%2', '%C3A9', '%80', '%FF', '%C0%80', '%E0%80%80', '%ED%A0%80', '%F4%90%80%80', '%E2%82%2C']; bad.map(x => { try { decodeURIComponent(x); return 'ok'; } catch (e) { return e instanceof URIError ? 'U' : String(e); } }).join('');",
       "UUUUUUUUUUUUU");
    eq("const r = []; for (const x of ['\\uD800', '\\uDC00', 'a\\uD800b']) { try { encodeURIComponent(x); r.push('ok'); } catch (e) { r.push(String(e)); } } r.push(encodeURIComponent('\\uDBFF\\uDFFF')); r.join('|');",
       "URIError: URI malformed|URIError: URI malformed|URIError: URI malformed|%F4%8F%BF%BF");
}

/* A function's `prototype`, as V8 has it (expected values from Node): only constructors have one;
 * it takes any value; it is not deletable, read-only on builtins and frozen functions; the
 * `constructor` it starts with, like every builtin prototype's property, is not enumerable */
/* ToPrimitive: an object's own (or inherited) valueOf/toString, by hint, through every operator and
 * builtin that converts; expectations generated from Node (vm.runInNewContext) */
/* Object.getOwnPropertyNames, and Object.hasOwn/hasOwnProperty agreeing with it; outputs from Node */
static void test_own_property_names(void) {
    eq("const E = (f) => { try { return JSON.stringify(f()); } catch (e) { return e.name + ': ' + e.message; } }; const g = Object.getOwnPropertyNames; const a = [1, 2]; a.x = 1; const f = function h(p, q) {}; f.z = 1; const e = new RangeError('m', { cause: 1 }); const d = new Error('d'); delete d.stack; const r = /a/g; r.k = 1; [E(() => g({ b: 1, a: 2, 2: 0, 1: 0 })), E(() => g(a)), E(() => g('ab')), E(() => g(5)), E(() => g(true)), E(() => g(null)), E(() => g()), E(() => g(f)), E(() => g(() => 1)), E(() => g(async function () {})), E(() => g({ m() {} }.m)), E(() => g(f.bind(null))), E(() => g(Math.max)), E(() => g(f.prototype)), E(() => g(e)), E(() => g(d)), E(() => g(r)), E(() => g(new Map())), E(() => g(Promise.resolve())), E(() => g(Object.freeze({ q: 1 }))), E(() => g(Object.create({ i: 1 }))), E(() => g(Array).slice(0, 4)), E(() => g(Error).slice(0, 4)), g.length, g.name].join('|');",
       "[\"1\",\"2\",\"b\",\"a\"]|[\"0\",\"1\",\"length\",\"x\"]|[\"0\",\"1\",\"length\"]|[]|[]|TypeError: Cannot convert undefined or null to object|TypeError: Cannot convert undefined or null to object|[\"length\",\"name\",\"prototype\",\"z\"]|[\"length\",\"name\"]|[\"length\",\"name\"]|[\"length\",\"name\"]|[\"length\",\"name\"]|[\"length\",\"name\"]|[\"constructor\"]|[\"stack\",\"message\",\"cause\"]|[\"message\"]|[\"lastIndex\",\"k\"]|[]|[]|[\"q\"]|[]|[\"length\",\"name\",\"prototype\",\"isArray\"]|[\"length\",\"name\",\"prototype\",\"captureStackTrace\"]|1|getOwnPropertyNames");
    eq("const E = (f) => { try { return JSON.stringify(f()); } catch (e) { return e.name + ': ' + e.message; } }; const h = Object.hasOwn; const hp = (o, k) => Object.prototype.hasOwnProperty.call(o, k); [h([1, 2], 1), h([1, 2], '01'), h([1, 2], 2), h([], 'length'), hp([], 'length'), h('ab', 1), h('ab', '2'), h('ab', 'length'), h(5, 'x'), E(() => h(null, 'x')), E(() => h(undefined, { toString() { throw new Error('key'); } })), E(() => hp(null, { toString() { throw new Error('key'); } })), h(/a/, 'lastIndex'), h(Object, 'keys'), hp(Array, 'isArray'), h(TypeError, 'captureStackTrace'), h(Error, 'captureStackTrace'), h(() => 1, 'prototype'), h(function () {}, 'prototype'), h(new Error(), 'stack'), h(new Error('x'), 'message'), JSON.stringify(Object.keys('ab')), JSON.stringify(Object.values('ab')), JSON.stringify(Object.entries('ab')), E(() => Object.keys(null)), E(() => Object.values()), E(() => Object.entries(undefined)), JSON.stringify(Object.keys(Error))].join('|');",
       "true|false|false|true|true|true|false|true|false|TypeError: Cannot convert undefined or null to object|TypeError: Cannot convert undefined or null to object|Error: key|true|true|true|false|true|false|true|true|true|[\"0\",\"1\"]|[\"a\",\"b\"]|[[\"0\",\"a\"],[\"1\",\"b\"]]|TypeError: Cannot convert undefined or null to object|TypeError: Cannot convert undefined or null to object|TypeError: Cannot convert undefined or null to object|[\"stackTraceLimit\"]");
    /* a promise holds properties; a builtin's statics can be replaced and deleted; Object.assign
     * copies a string; a sealed object deletes what it lacks; a function's numeric keys */
    eq("const E = (f) => { try { return JSON.stringify(f()); } catch (e) { return e.name + ': ' + e.message; } }; const P = () => Promise.resolve(1); const p = P(); p.x = 1; p[0] = 'z'; [E(() => [p.x, p[0], Object.keys(p), Object.getOwnPropertyNames(p), Object.hasOwn(p, 'x'), 'x' in p, 'then' in p, 'nope' in p]), E(() => [{ ...p }, Object.assign({}, p), JSON.stringify(p), JSON.stringify(P()), JSON.stringify({ q: p })]), E(() => { const q = P(); Object.assign(q, { b: 2 }); q.then = 5; return [q.b, q.then, typeof Promise.prototype.then]; }), E(() => { const q = P(); q.a = 1; delete q.a; return [q.a, Object.keys(q), Object.isFrozen(q), Object.isFrozen(Object.freeze(q))]; }), E(() => { const q = Object.freeze(P()); q.n = 1; }), E(() => { const k = Object.keys; delete Object.keys; const r = [typeof Object.keys, Object.hasOwn(Object, 'keys'), Object.getOwnPropertyNames(Object).includes('keys'), 'keys' in Object]; Object.keys = k; return [r, Object.keys({ a: 1 })]; }), E(() => { const k = Object.isFrozen; Object.isFrozen = 1; const r = [Object.isFrozen, Object.getOwnPropertyNames(Object).filter((n) => n === 'isFrozen').length]; Object.isFrozen = k; return [r, Object.isFrozen({})]; }), E(() => Object.assign({}, 'ab', null, 5, [9])), E(() => { const f = function () {}; f.z = 1; f[0] = 'a'; return [{ ...f }, Object.assign({}, f), f[0], 0 in f]; }), E(() => [delete Object.freeze({}).x, delete Object.seal([1]).x, delete Object.freeze(function () {}).x]), E(() => delete Object.freeze({ x: 1 }).x), E(() => delete [].length)].join('|');",
       "[1,\"z\",[\"0\",\"x\"],[\"0\",\"x\"],true,true,true,false]|[{\"0\":\"z\",\"x\":1},{\"0\":\"z\",\"x\":1},\"{\\\"0\\\":\\\"z\\\",\\\"x\\\":1}\",\"{}\",\"{\\\"q\\\":{\\\"0\\\":\\\"z\\\",\\\"x\\\":1}}\"]|[2,5,\"function\"]|[null,[],false,true]|TypeError: Cannot add property n, object is not extensible|[[\"undefined\",false,false,false],[\"a\"]]|[[1,1],false]|{\"0\":9,\"1\":\"b\"}|[{\"0\":\"a\",\"z\":1},{\"0\":\"a\",\"z\":1},\"a\",true]|[true,true,true]|TypeError: Cannot delete property 'x' of #<Object>|TypeError: Cannot delete property 'length' of [object Array]");
}

static void test_to_primitive(void) {
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; const log = []; const o = { toString() { log.push('t'); return 'S'; }, valueOf() { log.push('v'); return 42; } }; ['' + o, `${o}`, String(o), o + 1, o * 2, -o, +o, Number(o), o < 50, o == 42, o == 'S', log.join('')].join('|');",
       "42|S|S|43|84|-42|42|42|true|true|false|vttvvvvvvvv");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; const t = { toString() { return '7'; } }; const v = { valueOf() { return 5; } }; [t * 3, t + 1, v + 1, `${v}`, String(v), v > 4, t == 7].join('|');",
       "21|71|6|[object Object]|[object Object]|true|true");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; const n = { toString() { return {}; }, valueOf() { return {}; } }; const f = { valueOf() { return {}; }, toString() { return 'fall'; } }; const x = { toString: 5, valueOf: 6 }; [E(() => n + ''), E(() => String(n)), E(() => n * 1), f + 1, E(() => x + ''), E(() => Object.create(null) + '')].join('|');",
       "TypeError: Cannot convert object to primitive value|TypeError: Cannot convert object to primitive value|TypeError: Cannot convert object to primitive value|fall1|TypeError: Cannot convert object to primitive value|TypeError: Cannot convert object to primitive value");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; const th = { toString() { throw new Error('boom'); } }; [E(() => '' + th), E(() => th * 1), E(() => ({})[th]), E(() => `${th}`), E(() => th < 1), E(() => th == 1), E(() => [th].join()), E(() => 'a'.indexOf(th))].join('|');",
       "Error: boom|Error: boom|Error: boom|Error: boom|Error: boom|Error: boom|Error: boom|Error: boom");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; const d = new Date(0); d.toString = () => 'DATE'; d.valueOf = () => 99; [d + 1, d * 1, d < 100, `${d}`, new Date({ valueOf() { return 5; } }).getTime(), new Date({ toString() { return '1970-01-01T00:00:00.007Z'; }, valueOf: undefined }).getTime()].join('|');",
       "DATE1|99|true|DATE|5|7");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; const a = [1, [2, 3], { toString() { return 'x'; } }, null, undefined]; const c = [1, 2]; c.push(c); const j = [1, 2]; j.join = () => 'J'; const k = [1]; k.join = 5; ['' + a, a.join('-'), String(c), c.join('|'), String(j), String(k), Array.prototype.toString.call({ join() { return 'g'; } }), Array.prototype.toString.call({}), E(() => Array.prototype.toString.call(null))].join('|');",
       "1,2,3,x,,|1-2,3-x--|1,2,|1|2||J|[object Array]|g|[object Object]|TypeError: Cannot convert undefined or null to object");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; function f() { return 1; } f.valueOf = () => 10; const g = function () {}; g.toString = () => 'G'; [f * 2, `${g}`, typeof f.valueOf, f.valueOf === Object.prototype.valueOf].join('|');",
       "20|G|function|false");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; const e = new Error('m'); e.toString = () => 'E!'; const e3 = new Error({ toString() { return 'objmsg'; } }); const e4 = new Error('x'); e4.name = { toString() { return 'NM'; } }; const e5 = new Error('q'); e5.name = e5; ['' + e, e3.message, String(e4), E(() => String(e5)), Error.prototype.toString.call({ name: 'N', message: 'M' }), Error.prototype.toString.call({})].join('|');",
       "E!|objmsg|NM: x|RangeError: Maximum call stack size exceeded|N: M|Error");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; let c = 0; const k = { toString() { c++; return 'a'; } }; const o = { a: 1 }; const r = []; o[k] += 1; r.push(c, o.a); c = 0; o[k]++; r.push(c); c = 0; o[k] = 5; r.push(c); c = 0; r.push(o[k], c); c = 0; const lit = { [k]: 3 }; r.push(c, lit.a); c = 0; r.push(k in o, c); c = 0; delete o[k]; r.push(c, o.a); c = 0; r.push(o.hasOwnProperty(k), c); r.join();",
       "2,2,2,1,5,1,1,3,true,1,1,,false,1");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; const order = []; const o = {}; o[{ toString() { order.push('key'); return 'z'; } }] = (order.push('rhs'), 1); const arr = [5, 6, 7]; const m = { [{ toString() { return 'fn'; } }]: function () {} }; [order.join(), arr[{ toString() { return '1'; } }], m.fn.name, E(() => null[{ toString() { throw new Error('no'); } }]).slice(0, 37), E(() => ({ toString() { throw new Error('in'); } }) in 5).slice(0, 51)].join('|');",
       "rhs,key|6|fn|TypeError: Cannot read properties of |TypeError: Cannot use 'in' operator to search for '");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; let c = 0; const ar = [1, 2, 3]; ar.length = { valueOf() { c++; return 1; } }; [c, ar.length].join();",
       "2,1");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; const s = (v) => ({ toString() { return v; } }); const n = (v) => ({ valueOf() { return v; } }); ['abc'.includes(s('b')), 'abcdef'.slice(n(2)), 'ab'.repeat(n(2)), Math.max(n(3), 1), parseInt(s('12')), String.prototype.charAt.call(s('xyz'), 1), 'a'.concat(s('S')), 'a'.padStart(3, s('-')), 'a,b'.split(s(',')).length, 'aXb'.replace(s('X'), '-'), String.fromCharCode(n(65)), isNaN(n(1)), [3, 4, 5].indexOf(4, n(1)), [1, 2, 3].slice(n(1)).join(), 'abc'.at(n(-1)), Math.pow(n(2), n(3)), (255).toString(n(16)), Number(s('0x10'))].join('|');",
       "true|cdef|abab|3|12|y|aS|--a|2|a-b|A|false|1|2,3|c|8|ff|16");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; const log = []; const n = (v) => ({ valueOf() { log.push(v); return v; } }); [Math.max(NaN, n(1), n(2)), Math.min(n(3), NaN), log.join()].join('|');",
       "NaN|NaN|1,2,3");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; [[{ toString() { return 'b'; } }, { toString() { return 'a'; } }].sort().map(String).join(), [3, 1, 2].sort((x, y) => ({ valueOf() { return x - y; } })).join()].join('|');",
       "a,b|1,2,3");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; [(5).valueOf() + 1, typeof (5).valueOf(), E(() => Number.prototype.toString.call({})), E(() => Number.prototype.valueOf.call('1')), E(() => String.prototype.toString.call({})), E(() => String.prototype.valueOf.call(5)), 'ab'.valueOf(), true.toString(), true.valueOf(), E(() => Boolean.prototype.toString.call(1)), String(Number.prototype), String(Boolean.prototype), String(String.prototype) === '', Object.getPrototypeOf(false) === Boolean.prototype].join('|');",
       "6|number|TypeError: Number.prototype.toString requires that 'this' be a Number|TypeError: Number.prototype.valueOf requires that 'this' be a Number|TypeError: String.prototype.toString requires that 'this' be a String|TypeError: String.prototype.valueOf requires that 'this' be a String|ab|true|true|TypeError: Boolean.prototype.toString requires that 'this' be a Boolean|0|false|true|true");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; [E(() => Object.prototype.valueOf.call(null)), 'ab'.hasOwnProperty('length'), 'ab'.hasOwnProperty(1), 'ab'.hasOwnProperty('01'), 'ab'.hasOwnProperty(2), (5).hasOwnProperty('x'), E(() => Object.prototype.hasOwnProperty.call(null, 'x')), RegExp.prototype.toString.call({ source: { toString() { return 'S'; } }, flags: 'g' }), /a/gi.toString(), typeof (1).hasOwnProperty].join('|');",
       "TypeError: Cannot convert undefined or null to object|true|true|false|false|false|TypeError: Cannot convert undefined or null to object|/S/g|/a/gi|function");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; const p = Promise.resolve(1); [String(p), `${p}`, E(() => p * 1), JSON.stringify({ a: { toString() { return 'x'; } } }), Object.keys(Object.fromEntries([[{ toString() { return 'k'; } }, 1]])).join()].join('|');",
       "[object Promise]|[object Promise]|NaN|{\"a\":{}}|k");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; [E(() => { const r = { toString() { return '' + r; } }; return '' + r; }), E(() => { let x = []; for (let i = 0; i < 100; i++) x = [x]; return String(x).length; })].join('|');",
       "RangeError: Maximum call stack size exceeded|0");

}

static void test_prototype(void) {
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; function F() {} [F.hasOwnProperty('prototype'), typeof F.prototype, F.prototype.constructor === F, F.prototype === F.prototype, JSON.stringify(Object.keys(F.prototype)), JSON.stringify(F.prototype)].join();",
       "true,object,true,true,[],{}");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; const o = { m() {}, async a() {}, p: function () {} }; async function af() {} [(() => {}).hasOwnProperty('prototype'), o.m.hasOwnProperty('prototype'), o.a.hasOwnProperty('prototype'), o.p.hasOwnProperty('prototype'), af.hasOwnProperty('prototype'), typeof (() => {}).prototype, 'prototype' in o.m].join();",
       "false,false,false,true,false,undefined,false");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; [Math.max, [].push, Object.keys, parseInt, (function () {}).bind(null), Map, Error, TypeError, Object, Array, Promise].map((f) => f.hasOwnProperty('prototype') + ':' + ('prototype' in f)).join();",
       "false:false,false:false,false:false,false:false,false:false,true:true,true:true,true:true,true:true,true:true,true:true");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; const o = { m() {} }; const k = 'c'; const p = { [k]() {} }; async function af() {} const ar = () => {}; [E(() => new ar()), E(() => new o.m()), E(() => new p.c()), E(() => new af()), E(() => new Math.max()), E(() => new (o.m.bind(null))())].join('|');",
       "TypeError: ar is not a constructor|TypeError: o.m is not a constructor|TypeError: p.c is not a constructor|TypeError: af is not a constructor|TypeError: Math.max is not a constructor|TypeError: o.m.bind(...) is not a constructor");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; function P() {} P.prototype = 5; [P.prototype, P.hasOwnProperty('prototype'), Object.getPrototypeOf(new P()) === Object.prototype, E(() => ({}) instanceof P), 5 instanceof P].join('|');",
       "5|true|true|TypeError: Function has non-object prototype '5' in instanceof check|false");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; function N() {} N.prototype = null; function U() {} U.prototype = undefined; [N.prototype, Object.getPrototypeOf(new N()) === Object.prototype, U.prototype, Object.getPrototypeOf(new U()) === Object.prototype].join();",
       ",true,,true");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; const a = () => {}; [E(() => ({}) instanceof a), E(() => 1 instanceof a), (a.prototype = Object.prototype, ({}) instanceof a), a.hasOwnProperty('prototype'), JSON.stringify(Object.keys(a)), delete a.prototype, a.hasOwnProperty('prototype')].join('|');",
       "TypeError: Function has non-object prototype 'undefined' in instanceof check|false|true|true|[\"prototype\"]|true|false");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; function F() {} [E(() => delete F.prototype), E(() => delete Map.prototype), E(() => { Map.prototype = {}; }), E(() => { Error.prototype = {}; }), E(() => { Number.prototype = {}; })].join('|');",
       "TypeError: Cannot delete property 'prototype' of function F() {}|TypeError: Cannot delete property 'prototype' of function Map() { [native code] }|TypeError: Cannot assign to read only property 'prototype' of function 'function Map() { [native code] }'|TypeError: Cannot assign to read only property 'prototype' of function 'function Error() { [native code] }'|TypeError: Cannot assign to read only property 'prototype' of function 'function Number() { [native code] }'");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; function F() {} Object.freeze(F); function G() {} Object.seal(G); G.prototype = { q: 1 }; [E(() => { F.prototype = {}; }), typeof F.prototype, G.prototype.q].join('|');",
       "TypeError: Cannot assign to read only property 'prototype' of function 'function F() {}'|object|1");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; function F() { return function g() {}; } function G() { return Promise.resolve(1); } function H() { this.k = 1; return 3; } [typeof new F(), new G() instanceof Promise, new H().k].join();",
       "function,true,1");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; function F(a, b) { this.s = a + b; } const B = F.bind(null, 1); const x = new B(2); [x.s, x instanceof F, x instanceof B, B.hasOwnProperty('prototype'), Object.getPrototypeOf(x) === F.prototype].join();",
       "3,true,true,false,true");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; function F() {} const p = F.prototype; delete p.constructor; p.constructor = F; function G() {} G.prototype.constructor = 1; [JSON.stringify(Object.keys(p)), JSON.stringify(Object.keys(G.prototype))].join();",
       "[\"constructor\"],[]");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; function F() {} const p = F.prototype; for (let i = 0; i < 40; i++) p['k' + i] = i; for (let i = 0; i < 30; i++) delete p['k' + i]; p[3] = 'x'; p.z = 1; Object.keys(p).join() + ':' + (p.constructor === F);",
       "3,k30,k31,k32,k33,k34,k35,k36,k37,k38,k39,z:true");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; Map.prototype.zz = 1; const g = Map.prototype.get; Map.prototype.get = function () { return 9; }; JSON.stringify(Object.keys(Map.prototype)) + new Map().get(1);",
       "[\"zz\"]9");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; [Map, Set, Date, RegExp, Error, TypeError, Array, Object, Promise, Number, String, Boolean].map((C) => Object.keys(C.prototype).length + ':' + (C.prototype.constructor === C)).join() + '|' + Object.keys(Math).length + Object.keys(JSON).length + JSON.stringify(Object.keys(Error));",
       "0:true,0:true,0:true,0:true,0:true,0:true,0:true,0:true,0:true,0:true,0:true,0:true|00[\"stackTraceLimit\"]");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; globalThis.myG = 1; Object.keys(globalThis).filter((k) => k !== 'print').join();",
       "myG");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; const e = new Error('m', { cause: 1 }); const f = new Error('m'); delete f.message; f.message = 'n'; const g = new Error('m'); g.message = 'n'; [JSON.stringify(Object.keys(e)), JSON.stringify(e), JSON.stringify(Object.keys(f)), JSON.stringify(Object.keys(g)) + g.message, JSON.stringify(Object.keys(new AggregateError([1])))].join('|');",
       "[]|{}|[\"message\"]|[]n|[]");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; function A(n) { this.n = n; } A.prototype.hi = function () { return 'hi ' + this.n; }; function B(n) { A.call(this, n); } B.prototype = Object.create(A.prototype); B.prototype.constructor = B; const b = new B(3); [b.hi(), b instanceof A, b instanceof B, b.constructor === B, JSON.stringify(Object.keys(B.prototype)), JSON.stringify(b), JSON.stringify({ ...A.prototype })].join('|');",
       "hi 3|true|true|true|[\"constructor\"]|{\"n\":3}|{}");
    eq("const E = (f) => { try { return String(f()); } catch (e) { return e.name + ': ' + e.message; } }; let r; new Promise((res) => { r = res; }); [E(() => new r()), E(() => new (Promise.resolve(1).then)()), typeof Boolean.prototype, Boolean.prototype.constructor === Boolean].join('|');",
       "TypeError: r is not a constructor|TypeError: Promise.resolve(...).then is not a constructor|object|true");

}

int main(void) {
    test_freeze_seal();
    test_string();
    test_array();
    test_number();
    test_function();
    test_key_order();
    test_math();
    test_json();
    test_object();
    test_object_create_instanceof();
    test_date();
    test_map();
    test_set();
    test_error();
    test_error_types();
    test_uri();
    test_prototype();
    test_to_primitive();
    test_own_property_names();
    test_stress();
    test_integration();
    if (checks_failed) {
        fprintf(stderr, "%d/%d builtin checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d builtin checks passed\n", checks_run);
    return 0;
}
