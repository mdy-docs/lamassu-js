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
    /* without `new`: the time now as a string, whatever the arguments, as in V8 */
    eq("typeof Date();", "string");
    eq("Date(0) === new Date(0).toString();", "false");
    eq("/^\\w{3} \\w{3} \\d{2} \\d{4} \\d{2}:\\d{2}:\\d{2} GMT[+-]\\d{4}/.test(Date(2000, 1));", "true");
    eq("new Date(0).toJSON();", "1970-01-01T00:00:00.000Z");
    eq("new Date('bad').toJSON();", "null");
    /* Date -> Date construction copies the time value */
    eq("let a = new Date(12345); let b = new Date(a); b.getTime();", "12345");
}

/* Array.prototype.sort and toSorted: V8's TimSort, ported so that the comparator sees the same
 * pairs in the same order. Every expectation here is Node's output for the same source. */
static void test_sort(void) {
    /* undefined elements, what a comparator may return, a comparator that changes or freezes the
     * array, and the calls made for a few short arrays (which differ between sort and toSorted) */
    eq_s(
        "const out = [];\n"
        "const t = (s, f) => { let v; try { v = JSON.stringify(f()); } catch (e) { v = e.constructor.name + ': ' + e.message; } out.push(s + ' => ' + v); };\n"
        "t('undefined last', () => ['z', undefined, 'a'].sort());\n"
        "t('undefined not compared', () => { let n = 0; const a = [3, undefined, 1, undefined, 2].sort((x, y) => (n++, x - y)); return [a, n]; });\n"
        "t('all undefined', () => { let n = 0; return [[undefined, undefined].sort(() => n++), n]; });\n"
        "t('null is compared', () => [null, 'a', undefined, 'z'].sort());\n"
        "t('boolean result', () => [3, 1, 2].sort((x, y) => x > y));\n"
        "t('boolean result 12', () => [5, 1, 4, 2, 8, 0, 9, 3, 7, 6, 11, 10].sort((x, y) => x > y));\n"
        "t('NaN result', () => [3, 1, 2].sort(() => NaN));\n"
        "t('string result', () => [3, 1, 2].sort(() => '-1'));\n"
        "t('object result', () => [3, 1, 2].sort((x, y) => ({ valueOf() { return x - y; } })));\n"
        "t('undefined result', () => [3, 1, 2].sort(() => undefined));\n"
        "t('bigint result', () => [3, 1, 2].sort(() => 1n));\n"
        "t('stable', () => [[1, 'a'], [0, 'b'], [1, 'c'], [0, 'd'], [1, 'e'], [0, 'f']].sort((x, y) => x[0] - y[0]).map((e) => e[1]).join(''));\n"
        "t('frozen', () => { const a = Object.freeze([3, 1, 2]); let n = 0; try { a.sort((x, y) => (n++, x - y)); } catch (e) { return [n, e.message, a]; } return [n, a]; });\n"
        "t('frozen1', () => Object.freeze([1]).sort());\n"
        "t('frozen sorted', () => Object.freeze([1, 2]).sort());\n"
        "t('frozen undefined', () => Object.freeze([undefined, undefined]).sort());\n"
        "t('sealed', () => Object.seal([3, 1, 2]).sort());\n"
        "t('not extensible', () => Object.preventExtensions([3, 1, 2]).sort());\n"
        "t('shortened', () => { const a = [3, 1, 2]; a.sort((x, y) => { a.length = 0; return x - y; }); return a; });\n"
        "t('shortened undefined', () => { const a = [3, 1, 2, undefined, 5]; a.sort((x, y) => { a.length = 1; return x - y; }); return a; });\n"
        "t('grown', () => { const a = [3, 1, 2]; a.sort((x, y) => { a.push(9); return x - y; }); return a; });\n"
        "t('written', () => { const a = [3, 1, 2]; a.sort((x, y) => { a[0] = 7; a[1] = 8; a[2] = 9; return x - y; }); return a; });\n"
        "t('frozen meanwhile', () => { const a = [3, 1, 2]; try { a.sort((x, y) => { Object.freeze(a); return x - y; }); } catch (e) { return [e.message, a]; } return a; });\n"
        "t('shortened and sealed', () => { const a = [3, 1, 2]; try { a.sort((x, y) => { a.length = 1; Object.seal(a); return x - y; }); } catch (e) { return [e.message, a]; } return a; });\n"
        "t('emptied and closed', () => { const a = [3, 1, 2]; try { a.sort((x, y) => { a.length = 0; Object.preventExtensions(a); return x - y; }); } catch (e) { return [e.message, a]; } return a; });\n"
        "t('comparator throws', () => { const a = [3, 1, 2, 0]; try { a.sort((x, y) => { if (x === 2) throw new Error('stop'); return x - y; }); } catch (e) { return [e.message, a]; } return a; });\n"
        "t('bad comparator, one element', () => [1].sort(5));\n"
        "t('bad comparator, none', () => [].sort(null));\n"
        "t('toSorted bad comparator', () => [].toSorted('x'));\n"
        "t('returns the array', () => { const a = [2, 1]; return a.sort() === a; });\n"
        "t('toSorted frozen', () => Object.freeze([3, 1, 2]).toSorted());\n"
        "t('toSorted undefined', () => [3, undefined, 1, undefined].toSorted());\n"
        "t('toSorted one', () => { const a = [undefined]; const b = a.toSorted(); return [b, a === b]; });\n"
        "t('toSorted source shortened', () => { const a = [3, 1, 2]; const b = a.toSorted((x, y) => { a.length = 0; return x - y; }); return [a, b]; });\n"
        "for (const m of ['sort', 'toSorted']) for (const arr of [[5, 3, 4, 1, 2], [1, 2, 3, 4, 5, 6, 7], [7, 6, 5, 4, 3, 2, 1], [2, 1, 4, 3, 6, 5, 8, 7, 9]]) {\n"
        "  const tr = []; const res = arr.slice()[m]((x, y) => (tr.push(x + '' + y), x - y)); out.push(m + ' ' + arr.join('') + ': ' + tr.join(' ') + ' -> ' + res.join(''));\n"
        "}\n"
        "t('default mixed', () => [10, 9, 1, 'b', 'a', null, true, [2], { toString() { return '0'; } }, undefined, NaN, -0, 1e21].sort().map(String));\n"
        "t('default toString throws', () => [1, { toString() { throw new RangeError('ts'); } }].sort());\n"
        "t('default toString order', () => { const log = []; const mk = (s) => ({ toString() { log.push(s); return s; } }); [mk('c'), mk('a'), mk('b')].sort(); return log; });\n"
        "t('default by code unit', () => ['\\u{1F600}', '\\uFFFF', 'a', 'B', '', 'aa', 'a\\u0000'].sort().map((s) => [...s].map((c) => c.codePointAt(0).toString(16)).join('.')));\n"
        "out.join('\\n');",
        "undefined last => [\"a\",\"z\",null]\n"
        "undefined not compared => [[1,2,3,null,null],3]\n"
        "all undefined => [[null,null],0]\n"
        "null is compared => [\"a\",null,\"z\",null]\n"
        "boolean result => [3,1,2]\n"
        "boolean result 12 => [5,1,4,2,8,0,9,3,7,6,11,10]\n"
        "NaN result => [3,1,2]\n"
        "string result => [2,1,3]\n"
        "object result => [1,2,3]\n"
        "undefined result => [3,1,2]\n"
        "bigint result => TypeError: Cannot convert a BigInt value to a number\n"
        "stable => \"bdface\"\n"
        "frozen => [3,\"Cannot assign to read only property '0' of object '[object Array]'\",[3,1,2]]\n"
        "frozen1 => [1]\n"
        "frozen sorted => TypeError: Cannot assign to read only property '0' of object '[object Array]'\n"
        "frozen undefined => TypeError: Cannot assign to read only property '0' of object '[object Array]'\n"
        "sealed => [1,2,3]\n"
        "not extensible => [1,2,3]\n"
        "shortened => [1,2,3]\n"
        "shortened undefined => [1,2,3,5,null]\n"
        "grown => [1,2,3,9,9,9]\n"
        "written => [1,2,3]\n"
        "frozen meanwhile => [\"Cannot assign to read only property '0' of object '[object Array]'\",[3,1,2]]\n"
        "shortened and sealed => [\"Cannot add property 1, object is not extensible\",[1]]\n"
        "emptied and closed => [\"Cannot add property 0, object is not extensible\",[]]\n"
        "comparator throws => [\"stop\",[3,1,2,0]]\n"
        "bad comparator, one element => TypeError: The comparison function must be either a function or undefined: 5\n"
        "bad comparator, none => TypeError: The comparison function must be either a function or undefined: null\n"
        "toSorted bad comparator => TypeError: The comparison function must be either a function or undefined: x\n"
        "returns the array => true\n"
        "toSorted frozen => [1,2,3]\n"
        "toSorted undefined => [1,3,null,null]\n"
        "toSorted one => [[null],false]\n"
        "toSorted source shortened => [[],[1,2,3]]\n"
        "sort 53412: 35 45 43 14 13 24 23 21 -> 12345\n"
        "sort 1234567: 21 32 42 43 53 54 63 65 74 76 -> 1234567\n"
        "sort 7654321: 67 57 56 46 45 36 35 34 25 24 23 15 13 12 -> 1234567\n"
        "sort 214365879: 12 41 42 32 34 63 64 53 56 54 84 86 74 76 78 95 97 98 -> 123456789\n"
        "toSorted 53412: 35 43 45 43 14 13 24 23 21 -> 12345\n"
        "toSorted 1234567: 21 32 43 54 65 76 -> 1234567\n"
        "toSorted 7654321: 67 56 45 34 23 12 -> 1234567\n"
        "toSorted 214365879: 12 41 42 32 34 63 64 53 56 54 84 86 74 76 78 95 97 98 -> 123456789\n"
        "default mixed => [\"0\",\"0\",\"1\",\"10\",\"1e+21\",\"2\",\"9\",\"NaN\",\"a\",\"b\",\"null\",\"true\",\"undefined\"]\n"
        "default toString throws => RangeError: ts\n"
        "default toString order => [\"a\",\"c\",\"b\",\"c\",\"b\",\"a\"]\n"
        "default by code unit => [\"\",\"42\",\"61\",\"61.0\",\"61.61\",\"1f600\",\"ffff\"]");
    /* For each shape of input and each comparator (consistent or not), over lengths that reach
     * every part of the sort (short arrays, forced runs, merges both ways, galloping): the number
     * of comparator calls, a hash of their arguments in order, and a hash of the result. Then the
     * default comparison, by which element's toString runs when. */
    eq(
        "let seed = 12345;\n"
        "const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed; };\n"
        "const mix = (h, v) => (h * 31 + v + 7) % 4294967291;\n"
        "const sizes = [0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 47, 63, 64, 65, 66, 67, 95, 96, 97, 127, 128, 129, 130, 191, 192, 193, 255, 256, 257, 300, 513, 777, 1025, 2049];\n"
        "const patterns = {\n"
        "  random: (n) => { const a = []; for (let i = 0; i < n; i++) a.push(rnd() % 100000); return a; },\n"
        "  reversed: (n) => { const a = []; for (let i = 0; i < n; i++) a.push(n - i); return a; },\n"
        "  dupes: (n) => { const a = []; for (let i = 0; i < n; i++) a.push(rnd() % 7); return a; },\n"
        "  pipe: (n) => { const a = []; for (let i = 0; i < n; i++) a.push(i < n / 2 ? i : n - i); return a; },\n"
        "  runs: (n) => { const a = []; let v = 0; for (let i = 0; i < n; i++) { if (rnd() % 97 === 0) v = rnd() % 1000; a.push(v += rnd() % 3); } return a; },\n"
        "  halves: (n) => { const a = []; for (let i = 0; i < n; i++) a.push(i % 2 ? i : i + n); return a.slice().sort((x, y) => (x < n) - (y < n) || x - y); },\n"
        "  undef: (n) => { const a = []; for (let i = 0; i < n; i++) a.push(rnd() % 5 === 0 ? undefined : rnd() % 1000); return a; },\n"
        "};\n"
        "const cmps = {\n"
        "  num: (x, y) => x.k - y.k,\n"
        "  bool: (x, y) => x.k > y.k,\n"
        "  rand: (x, y) => (rnd() % 3) - 1,\n"
        "  nan: (x, y) => (rnd() % 4 === 0 ? NaN : x.k - y.k),\n"
        "  mod: (x, y) => (x.k % 10) - (y.k % 10),\n"
        "};\n"
        "const out = [];\n"
        "for (const method of ['sort', 'toSorted']) for (const pn of Object.keys(patterns)) for (const cn of Object.keys(cmps)) {\n"
        "  let calls = 0, th = 1, rh = 1;\n"
        "  for (const n of sizes) {\n"
        "    const src = patterns[pn](n).map((k, i) => (k === undefined ? undefined : { k, i }));\n"
        "    const f = cmps[cn];\n"
        "    const res = src[method]((x, y) => { calls++; th = mix(mix(th, x.i), y.i); return f(x, y); });\n"
        "    for (const e of res) rh = mix(rh, e === undefined ? 99999 : e.i);\n"
        "    rh = mix(rh, res.length);\n"
        "  }\n"
        "  out.push([method, pn, cn, calls, th, rh].join(' '));\n"
        "}\n"
        "for (const pn of Object.keys(patterns)) {\n"
        "  let calls = 0, th = 1, rh = 1;\n"
        "  for (const n of sizes) {\n"
        "    if (n > 600) continue;\n"
        "    const src = patterns[pn](n).map((k, i) => (k === undefined ? undefined : { toString() { calls++; th = mix(th, i); return String(k); }, i }));\n"
        "    const res = n % 2 ? src.sort() : src.toSorted();\n"
        "    for (const e of res) rh = mix(rh, e === undefined ? 99999 : e.i);\n"
        "  }\n"
        "  out.push(['default', pn, calls, th, rh].join(' '));\n"
        "}\n"
        "out.join('\\n');",
        "sort random num 56810 730088599 30580866\n"
        "sort random bool 7324 995148665 2090816780\n"
        "sort random rand 44476 815588985 1205633445\n"
        "sort random nan 7677 1439088697 2090816780\n"
        "sort random mod 40752 1095701540 897248003\n"
        "sort reversed num 7329 3040471094 2091300176\n"
        "sort reversed bool 7324 995148665 2090816780\n"
        "sort reversed rand 45467 727495962 2849010768\n"
        "sort reversed nan 8454 2323124058 3580124658\n"
        "sort reversed mod 45793 3001103488 3627064391\n"
        "sort dupes num 43715 2837645695 380268077\n"
        "sort dupes bool 7324 995148665 2090816780\n"
        "sort dupes rand 45199 1709961053 3803001664\n"
        "sort dupes nan 7586 1712812489 2090816780\n"
        "sort dupes mod 43667 373073533 3973807583\n"
        "sort pipe num 14869 883552903 2490409898\n"
        "sort pipe bool 7324 995148665 2090816780\n"
        "sort pipe rand 44062 1655816303 4154359189\n"
        "sort pipe nan 7533 3490026873 2090816780\n"
        "sort pipe mod 45698 1072131242 3024909391\n"
        "sort runs num 12486 1601186845 2328476552\n"
        "sort runs bool 7324 995148665 2090816780\n"
        "sort runs rand 45022 2730195158 178544126\n"
        "sort runs nan 7324 995148665 2090816780\n"
        "sort runs mod 45581 1557128487 1439245500\n"
        "sort halves num 8303 3487615985 729895302\n"
        "sort halves bool 7324 995148665 2090816780\n"
        "sort halves rand 44104 2827425397 3846446760\n"
        "sort halves nan 7324 995148665 2090816780\n"
        "sort halves mod 41061 291722902 760182173\n"
        "sort undef num 43432 1061745980 528432544\n"
        "sort undef bool 5878 3083606021 3245699572\n"
        "sort undef rand 35432 2375836883 3104094828\n"
        "sort undef nan 6127 649363454 571861533\n"
        "sort undef mod 32121 1289650049 3029890631\n"
        "toSorted random num 56828 3926175046 1941380698\n"
        "toSorted random bool 7320 3210799056 2090816780\n"
        "toSorted random rand 44914 2422324739 2212423327\n"
        "toSorted random nan 7592 630017801 2090816780\n"
        "toSorted random mod 40715 1341059661 2173790635\n"
        "toSorted reversed num 7320 3210799056 2091300176\n"
        "toSorted reversed bool 7320 3210799056 2090816780\n"
        "toSorted reversed rand 44777 1783882714 479619578\n"
        "toSorted reversed nan 7843 1757391292 4185866086\n"
        "toSorted reversed mod 45784 38350239 3627064391\n"
        "toSorted dupes num 43759 3419908691 58915375\n"
        "toSorted dupes bool 7320 3210799056 2090816780\n"
        "toSorted dupes rand 43927 1560093885 3144807621\n"
        "toSorted dupes nan 7823 1888778111 2090816780\n"
        "toSorted dupes mod 43826 2302086264 1574622881\n"
        "toSorted pipe num 14868 24037519 2490409898\n"
        "toSorted pipe bool 7320 3210799056 2090816780\n"
        "toSorted pipe rand 45076 2161149406 422293530\n"
        "toSorted pipe nan 7804 763983302 1514217576\n"
        "toSorted pipe mod 45697 299888309 3024909391\n"
        "toSorted runs num 13708 4040909794 1686151799\n"
        "toSorted runs bool 7320 3210799056 2090816780\n"
        "toSorted runs rand 44553 3270972179 1146587608\n"
        "toSorted runs nan 7320 3210799056 2090816780\n"
        "toSorted runs mod 45623 788960384 1059630785\n"
        "toSorted halves num 8304 4072311890 729895302\n"
        "toSorted halves bool 7320 3210799056 2090816780\n"
        "toSorted halves rand 45095 2907818229 2754553026\n"
        "toSorted halves nan 7320 3210799056 2090816780\n"
        "toSorted halves mod 41063 386479695 760182173\n"
        "toSorted undef num 43571 525770822 3120171555\n"
        "toSorted undef bool 5837 1861060868 838033985\n"
        "toSorted undef rand 35806 3449042436 1794401180\n"
        "toSorted undef nan 6086 3882918335 1340808099\n"
        "toSorted undef mod 32062 2492430040 308717531\n"
        "default random 43004 2254417216 2893858223\n"
        "default reversed 14590 3282800613 1227411368\n"
        "default dupes 38496 878909247 3590177027\n"
        "default pipe 24752 69834148 3184426017\n"
        "default runs 17450 778990069 1153986278\n"
        "default halves 21928 448326348 286124465\n"
        "default undef 32826 2934722745 2981346906");
    /* the same over a few lengths with a collection at every allocation */
    eq_mode(
        "let seed = 12345;\n"
        "const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed; };\n"
        "const mix = (h, v) => (h * 31 + v + 7) % 4294967291;\n"
        "const sizes = [5, 9, 66, 130];\n"
        "const patterns = {\n"
        "  random: (n) => { const a = []; for (let i = 0; i < n; i++) a.push(rnd() % 100000); return a; },\n"
        "  reversed: (n) => { const a = []; for (let i = 0; i < n; i++) a.push(n - i); return a; },\n"
        "  dupes: (n) => { const a = []; for (let i = 0; i < n; i++) a.push(rnd() % 7); return a; },\n"
        "  pipe: (n) => { const a = []; for (let i = 0; i < n; i++) a.push(i < n / 2 ? i : n - i); return a; },\n"
        "  runs: (n) => { const a = []; let v = 0; for (let i = 0; i < n; i++) { if (rnd() % 97 === 0) v = rnd() % 1000; a.push(v += rnd() % 3); } return a; },\n"
        "  halves: (n) => { const a = []; for (let i = 0; i < n; i++) a.push(i % 2 ? i : i + n); return a.slice().sort((x, y) => (x < n) - (y < n) || x - y); },\n"
        "  undef: (n) => { const a = []; for (let i = 0; i < n; i++) a.push(rnd() % 5 === 0 ? undefined : rnd() % 1000); return a; },\n"
        "};\n"
        "const cmps = {\n"
        "  num: (x, y) => x.k - y.k,\n"
        "  bool: (x, y) => x.k > y.k,\n"
        "  rand: (x, y) => (rnd() % 3) - 1,\n"
        "  nan: (x, y) => (rnd() % 4 === 0 ? NaN : x.k - y.k),\n"
        "  mod: (x, y) => (x.k % 10) - (y.k % 10),\n"
        "};\n"
        "const out = [];\n"
        "for (const method of ['sort', 'toSorted']) for (const pn of Object.keys(patterns)) for (const cn of Object.keys(cmps)) {\n"
        "  let calls = 0, th = 1, rh = 1;\n"
        "  for (const n of sizes) {\n"
        "    const src = patterns[pn](n).map((k, i) => (k === undefined ? undefined : { k, i }));\n"
        "    const f = cmps[cn];\n"
        "    const res = src[method]((x, y) => { calls++; th = mix(mix(th, x.i), y.i); return f(x, y); });\n"
        "    for (const e of res) rh = mix(rh, e === undefined ? 99999 : e.i);\n"
        "    rh = mix(rh, res.length);\n"
        "  }\n"
        "  out.push([method, pn, cn, calls, th, rh].join(' '));\n"
        "}\n"
        "for (const pn of Object.keys(patterns)) {\n"
        "  let calls = 0, th = 1, rh = 1;\n"
        "  for (const n of sizes) {\n"
        "    if (n > 600) continue;\n"
        "    const src = patterns[pn](n).map((k, i) => (k === undefined ? undefined : { toString() { calls++; th = mix(th, i); return String(k); }, i }));\n"
        "    const res = n % 2 ? src.sort() : src.toSorted();\n"
        "    for (const e of res) rh = mix(rh, e === undefined ? 99999 : e.i);\n"
        "  }\n"
        "  out.push(['default', pn, calls, th, rh].join(' '));\n"
        "}\n"
        "out.join('\\n');",
        "sort random num 1093 3287745783 2383881179\n"
        "sort random bool 208 1503592557 1617452584\n"
        "sort random rand 853 1212363074 1299030437\n"
        "sort random nan 208 1503592557 1617452584\n"
        "sort random mod 1008 2211474579 420438951\n"
        "sort reversed num 210 1720842381 534261261\n"
        "sort reversed bool 208 1503592557 1617452584\n"
        "sort reversed rand 871 3926436107 3992138958\n"
        "sort reversed nan 218 1479011448 1617452584\n"
        "sort reversed mod 1042 3877310465 2280936607\n"
        "sort dupes num 1044 2428038750 3707262755\n"
        "sort dupes bool 208 1503592557 1617452584\n"
        "sort dupes rand 952 3239986236 2722977531\n"
        "sort dupes nan 208 1503592557 1617452584\n"
        "sort dupes mod 1056 2895901100 2591046623\n"
        "sort pipe num 411 3427844377 1121268806\n"
        "sort pipe bool 208 1503592557 1617452584\n"
        "sort pipe rand 871 3393200694 310059374\n"
        "sort pipe nan 208 1503592557 1617452584\n"
        "sort pipe mod 1025 3520759587 4167186924\n"
        "sort runs num 208 1503592557 1617452584\n"
        "sort runs bool 208 1503592557 1617452584\n"
        "sort runs rand 962 2467385177 338351646\n"
        "sort runs nan 208 1503592557 1617452584\n"
        "sort runs mod 1027 3920352806 238711617\n"
        "sort halves num 256 2642272285 951561536\n"
        "sort halves bool 208 1503592557 1617452584\n"
        "sort halves rand 895 988557292 1198194173\n"
        "sort halves nan 208 1503592557 1617452584\n"
        "sort halves mod 993 2040898314 2543553892\n"
        "sort undef num 802 3555573526 553842026\n"
        "sort undef bool 169 871857400 675404919\n"
        "sort undef rand 788 3068575709 41371463\n"
        "sort undef nan 154 1496874168 3934183854\n"
        "sort undef mod 761 1037431818 3506690069\n"
        "toSorted random num 1088 2502353980 2957323385\n"
        "toSorted random bool 206 9320322 1617452584\n"
        "toSorted random rand 912 3382419584 2739512336\n"
        "toSorted random nan 226 3885524994 1617452584\n"
        "toSorted random mod 985 3598268258 3763900393\n"
        "toSorted reversed num 206 9320322 534261261\n"
        "toSorted reversed bool 206 9320322 1617452584\n"
        "toSorted reversed rand 899 1173299825 1510066989\n"
        "toSorted reversed nan 206 9320322 1617452584\n"
        "toSorted reversed mod 1038 1802080828 2280936607\n"
        "toSorted dupes num 1039 1052983522 3817497211\n"
        "toSorted dupes bool 206 9320322 1617452584\n"
        "toSorted dupes rand 896 4244514710 3465676136\n"
        "toSorted dupes nan 226 136575689 4105192813\n"
        "toSorted dupes mod 1039 3739931190 3413353641\n"
        "toSorted pipe num 411 3466421608 1121268806\n"
        "toSorted pipe bool 206 9320322 1617452584\n"
        "toSorted pipe rand 896 1730484747 3231277472\n"
        "toSorted pipe nan 214 108695833 1617452584\n"
        "toSorted pipe mod 1025 1712059854 4167186924\n"
        "toSorted runs num 206 9320322 1617452584\n"
        "toSorted runs bool 206 9320322 1617452584\n"
        "toSorted runs rand 811 2952447 974498510\n"
        "toSorted runs nan 206 9320322 1617452584\n"
        "toSorted runs mod 999 441839854 2661536424\n"
        "toSorted halves num 257 1663521798 951561536\n"
        "toSorted halves bool 206 9320322 1617452584\n"
        "toSorted halves rand 928 22346987 1425088753\n"
        "toSorted halves nan 206 9320322 1617452584\n"
        "toSorted halves mod 994 2780719556 2543553892\n"
        "toSorted undef num 820 2216737971 1371966846\n"
        "toSorted undef bool 156 1307369218 3637077150\n"
        "toSorted undef rand 712 2403666794 2580900601\n"
        "toSorted undef nan 169 2337693633 637641490\n"
        "toSorted undef mod 798 953142951 3906802663\n"
        "default random 2172 1360615710 3305515669\n"
        "default reversed 840 3459508120 1164022076\n"
        "default dupes 2062 340082741 1527482093\n"
        "default pipe 1422 2779676601 823039906\n"
        "default runs 998 841420669 63935783\n"
        "default halves 1412 4123292755 2643197791\n"
        "default undef 1560 3758932003 1648800374", true);
}

/* Map: O(n) linear-scan storage (documented in js_mapobj.h); keys() / values() / entries() return
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

    /* iterators (test_iterators has the protocol), in insertion order */
    eq("[...new Map([['a',1],['b',2]]).keys()].join(',');", "a,b");
    eq("[...new Map([['a',1],['b',2]]).values()].join(',');", "1,2");
    eq("JSON.stringify([...new Map([['a',1]]).entries()]);", "[[\"a\",1]]");
    eq("JSON.stringify(new Map([['a',1]]).entries());", "{}"); /* an iterator, not an array */

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
    eq("JSON.stringify([...new Set([1,2]).entries()]);", "[[1,1],[2,2]]");

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
    eq("let nums = Array.from({length: 5}, (_, i) => i);"
       "nums.join();",
       "0,1,2,3,4");
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
    eq("Object.isFrozen(Object.seal({}));", "true"); /* nothing in it to write: V8 says frozen */
    eq("Object.isFrozen(Object.seal({ a: 1 }));", "false");
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
    eq("const o = Object.create({}, { a: { value: 1 } }); [o.a, Object.keys(o).length].join();", "1,0");
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
/* JSON: toJSON, the replacer, the gap, the circular message, raw JSON, the reviver and its source
 * text context; outputs from Node */
/* Object.setPrototypeOf on functions and promises, and either as a prototype; outputs from Node */
/* Function.prototype is a function, AsyncFunction.prototype an object between it and async
 * functions, and the inherited toString tags; outputs from Node */
/* Map, Set and matchAll iterators; iteration of Maps, Sets and iterators by for-of, spread,
 * destructuring and the natives that take an iterable; Array.from; split's limit. Expected values
 * are Node's (generated from it). */
static void test_iterators(void) {
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('next', () => { const it = new Map([['a', 1], ['b', 2]]).entries(); return [it.next(), it.next(), it.next(), it.next()]; }), t('done shape', () => { const r = new Set().values().next(); return [Object.keys(r), r.value === undefined, r.done]; }), t('done stays', () => { const s = new Set([1]); const it = s.values(); it.next(); it.next(); s.add(2); return [it.next(), [...it]]; }), t('kinds', () => { const m = new Map([[1, 'x']]); const s = new Set(['y']); return [[...m.keys()], [...m.values()], [...m.entries()], [...s.keys()], [...s.values()], [...s.entries()]]; }), t('tags', () => [Object.prototype.toString.call(new Map().keys()), String(new Set().entries()), String('a'.matchAll(/a/g)), `${new Map().values()}`]), t('protos', () => { const mp = Object.getPrototypeOf(new Map().keys()); return [mp === Object.getPrototypeOf(new Map().entries()), mp === Object.getPrototypeOf(new Set().values()), Object.getPrototypeOf(mp) === Object.getPrototypeOf(Object.getPrototypeOf(new Set().values())), Object.getOwnPropertyNames(mp), Object.keys(mp), mp.next.length, mp.next.name, Object.getOwnPropertyNames(new Map().keys())]; })].join(\"\\n\");",
       "next [{\"value\":[\"a\",1],\"done\":false},{\"value\":[\"b\",2],\"done\":false},{\"done\":true},{\"done\":true}]\ndone shape [[\"value\",\"done\"],true,true]\ndone stays [{\"done\":true},[]]\nkinds [[1],[\"x\"],[[1,\"x\"]],[\"y\"],[\"y\"],[[\"y\",\"y\"]]]\ntags [\"[object Map Iterator]\",\"[object Set Iterator]\",\"[object RegExp String Iterator]\",\"[object Map Iterator]\"]\nprotos [true,false,true,[\"next\"],[],0,\"next\",[]]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('chain end', () => Object.getPrototypeOf(Object.getPrototypeOf(Object.getPrototypeOf(new Map().keys()))) === Object.prototype), t('receiver', () => new Map().keys().next.call({})), t('receiver kind', () => new Map().keys().next.call(new Set().values())), t('receiver arr', () => new Set().values().next.call([])), t('receiver re', () => 'a'.matchAll(/a/g).next.call(new Map().keys())), t('wrong method', () => Map.prototype.get.call(new Map().keys()))].join(\"\\n\");",
       "chain end true\nreceiver TypeError: Method Map Iterator.prototype.next called on incompatible receiver #<Object>\nreceiver kind TypeError: Method Map Iterator.prototype.next called on incompatible receiver [object Set Iterator]\nreceiver arr TypeError: Method Set Iterator.prototype.next called on incompatible receiver [object Array]\nreceiver re TypeError: Method %RegExpStringIterator%.prototype.next called on incompatible receiver [object Map Iterator]\nwrong method TypeError: Method Map.prototype.get called on incompatible receiver [object Map Iterator]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('set keys', () => [Set.prototype.keys === Set.prototype.values, Set.prototype.keys.name, Map.prototype.keys === Map.prototype.values]), t('expando', () => { const it = new Set([1]).values(); it.tag = 5; return [it.tag, Object.keys(it), JSON.stringify(it), it instanceof Object]; }), t('patch next', () => { const P = Object.getPrototypeOf(new Set().values()); const orig = P.next; P.next = function () { return { value: 9, done: false }; }; const r = new Set([1]).values().next(); P.next = orig; return [r, [...new Set([2])]]; })].join(\"\\n\");",
       "set keys [true,\"values\",false]\nexpando [5,[\"tag\"],\"{\\\"tag\\\":5}\",true]\npatch next [{\"value\":9,\"done\":false},[2]]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('delete before', () => { const m = new Map([[1, 1], [2, 2], [3, 3]]); const it = m.keys(); const a = it.next().value; m.delete(1); return [a, ...it]; }), t('delete ahead', () => { const m = new Map([[1, 1], [2, 2], [3, 3]]); const it = m.keys(); it.next(); m.delete(2); return [...it]; }), t('delete current', () => { const s = new Set([1, 2, 3]); const out = []; for (const x of s) { out.push(x); s.delete(x); } return [out, s.size]; }), t('add during', () => { const s = new Set([1]); const out = []; for (const x of s) { out.push(x); if (x < 5) s.add(x + 1); } return out; }), t('readd', () => { const s = new Set([1, 2]); const out = []; for (const x of s) { out.push(x); if (x === 1) { s.delete(1); s.add(1); } if (out.length > 5) break; } return out; }), t('clear', () => { const s = new Set([1, 2]); const it = s.values(); it.next(); s.clear(); s.add(9); return [...it]; })].join(\"\\n\");",
       "delete before [1,2,3]\ndelete ahead [3]\ndelete current [[1,2,3],0]\nadd during [1,2,3,4,5]\nreadd [1,2,1,1,1,1]\nclear [9]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('clear all done', () => { const m = new Map([[1, 1]]); const it = m.keys(); m.clear(); return [it.next(), (m.set(2, 2), it.next())]; }), t('two iters', () => { const s = new Set([1, 2, 3, 4]); const a = s.values(), b = s.values(); a.next(); a.next(); b.next(); s.delete(1); s.delete(3); return [[...a], [...b]]; }), t('map forEach live', () => { const m = new Map([['a', 1], ['b', 2], ['c', 3]]); const out = []; m.forEach((v, k) => { out.push(k + v); if (k === 'a') { m.delete('a'); m.set('d', 4); } }); return out; }), t('set forEach live', () => { const s = new Set([1, 2, 3]); const out = []; s.forEach((v) => { out.push(v); if (v === 1) s.delete(1); if (v === 3) s.add(4); }); return out; }), t('forEach throws', () => { const s = new Set([1, 2]); try { s.forEach(() => { throw new Error('x'); }); } catch (e) {} s.delete(1); return [...s]; }), t('other map', () => { const a = new Map([[1, 1], [2, 2]]), b = new Map([[1, 1], [2, 2]]); const it = a.keys(); it.next(); b.delete(1); return [...it]; })].join(\"\\n\");",
       "clear all done [{\"done\":true},{\"done\":true}]\ntwo iters [[4],[2,4]]\nmap forEach live [\"a1\",\"b2\",\"c3\",\"d4\"]\nset forEach live [1,2,3,4]\nforEach throws [2]\nother map [2]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('for of map live', () => { const m = new Map([[1, 'a']]); const out = []; for (const [k, v] of m) { out.push(k + v); if (k < 3) m.set(k + 1, v + 'a'); } return out; })].join(\"\\n\");",
       "for of map live [\"1a\",\"2aa\",\"3aaa\"]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('for of keys', () => { const out = []; for (const k of new Map([['a', 1], ['b', 2]]).keys()) out.push(k); return out; }), t('partial spread', () => { const it = new Set([1, 2, 3]).values(); it.next(); return [[...it], [...it]]; }), t('spread map', () => [...new Map([[1, 2]])]), t('spread call', () => [Math.max(...new Set([3, 9, 2])), Math.min(...new Map([[4, 0]]).keys())]), t('spread iter', () => [0, ...new Set([1, 2]).entries(), 3]), t('array from', () => [Array.from(new Map([[1, 2]])), Array.from(new Set([1, 2]).values()), Array.from(new Map([[1, 2]]).keys(), (k) => k * 10)])].join(\"\\n\");",
       "for of keys [\"a\",\"b\"]\npartial spread [[2,3],[]]\nspread map [[1,2]]\nspread call [9,4]\nspread iter [0,[1,1],[2,2],3]\narray from [[[1,2]],[1,2],[10]]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('matchAll for of', () => { const out = []; for (const m of 'a1b22c333'.matchAll(/\\d+/g)) out.push(m[0] + '@' + m.index); return out; }), t('new Promise.all', () => typeof Promise.all(new Set([1])).then), t('aggregate', () => new AggregateError(new Set([1, 2])).errors), t('aggregate map', () => new AggregateError(new Map([[1, 2]]).keys()).errors)].join(\"\\n\");",
       "matchAll for of [\"1@1\",\"22@3\",\"333@6\"]\nnew Promise.all \"function\"\naggregate [1,2]\naggregate map [1]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('from map', () => { const [[k, v], second] = new Map([['a', 1]]); return [k, v, second]; }), t('from set', () => { const [a, b, c = 'd'] = new Set([1, 2]); return [a, b, c]; }), t('from iter', () => { const it = new Set([1, 2, 3]).values(); const [a] = it; return [a, it.next().value]; }), t('iter rest', () => { const it = new Set([1, 2, 3]).values(); const [a, ...r] = it; return [a, r, it.next()]; }), t('str', () => { const [a, b, ...r] = '\360\237\230\200x\360\237\230\200y'; return [a, b, r]; }), t('str holes', () => { const [, b, , d] = 'a\360\237\230\200bc'; return [b, d]; })].join(\"\\n\");",
       "from map [\"a\",1,null]\nfrom set [1,2,\"d\"]\nfrom iter [1,2]\niter rest [1,[2,3],{\"done\":true}]\nstr [\"\360\237\230\200\",\"x\",[\"\360\237\230\200\",\"y\"]]\nstr holes [\"\360\237\230\200\",\"c\"]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('done sticky', () => { const arr = [1]; const [a, b = arr.push(5), c] = arr; return [a, b, c, arr]; }), t('arr grows', () => { const arr = [1, 2]; const [a, b = 0, c = arr.push(9)] = arr; return [a, b, c]; }), t('holes', () => { const [, , c] = [1, 2, 3]; const [x, , y] = new Set([7, 8, 9]); return [c, x, y]; }), t('rest arr', () => { const [a, ...r] = [1, 2, 3]; const [...all] = []; return [a, r, all]; }), t('nested', () => { const [[a, [b]], { c }] = [new Set([1, 'zq']), { c: 3 }]; return [a, b, c]; }), t('assign', () => { let a = 1, b = 2; [a, b] = [b, a]; let x, y; [x, y] = new Map([['k', 'v']]).keys(); return [a, b, x, y]; })].join(\"\\n\");",
       "done sticky [1,2,null,[1,5]]\narr grows [1,2,3]\nholes [3,7,9]\nrest arr [1,[2,3],[]]\nnested [1,\"z\",3]\nassign [2,1,\"k\",null]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('assign value', () => { let a; const s = new Set([5]); const r = ([a] = s); return [a, r === s]; }), t('params', () => { const f = ([a, b], [c] = 'zq') => [a, b, c]; return [f(new Set([1, 2])), f('ab'), f([3], new Map([[4, 5]]))]; }), t('for of pattern', () => { const out = []; for (const [k, v] of new Map([['a', 1], ['b', 2]])) out.push(k + v); for (const [x, y] of 'ab') out.push(x + y); return out; }), t('default fn', () => { let n = 0; const [a = ++n, b = ++n] = new Set([0]); return [a, b, n]; }), t('not iterable', () => { const [a] = 5; return a; }), t('not iterable obj', () => { const o = {}; const [a] = o; return a; })].join(\"\\n\");",
       "assign value [5,true]\nparams [[1,2,\"z\"],[\"a\",\"b\",\"z\"],[3,null,[4,5]]]\nfor of pattern [\"a1\",\"b2\",\"aundefined\",\"bundefined\"]\ndefault fn [0,1,1]\nnot iterable TypeError: 5 is not iterable\nnot iterable obj TypeError: o is not iterable");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('not iterable null', () => { const [a] = null; return a; }), t('spread not iterable', () => { const o = {}; return [...o]; }), t('spread call not iterable', () => Math.max(...5)), t('for of not iterable', () => { for (const x of 5) return x; })].join(\"\\n\");",
       "not iterable null TypeError: null is not iterable\nspread not iterable TypeError: o is not iterable\nspread call not iterable TypeError: Spread syntax requires ...iterable[Symbol.iterator] to be a function\nfor of not iterable TypeError: 5 is not iterable");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('alike', () => [Array.from({ length: 3, 0: 'x', 2: 'z' }), Array.from({ length: 2 }, (v, i) => i * 2), Array.from({ length: '2', 0: 'q' })]), t('alike length', () => [Array.from({ length: -1 }), Array.from({ length: 2.9 }), Array.from({ length: 'x' }), Array.from({ length: NaN }), Array.from({}), Array.from({ length: { valueOf() { return 1; } } })]), t('primitives', () => [Array.from(5), Array.from(true), Array.from('a\360\237\230\200'), Array.from(function (a, b) {})]), t('str index', () => Array.from('a\360\237\230\200b', (c, i) => c + i)), t('this arg', () => [Array.from([1], function (x) { return this.k + x; }, { k: 10 })]), t('bad fn', () => Array.from([1], 5))].join(\"\\n\");",
       "alike [[\"x\",null,\"z\"],[0,2],[\"q\",null]]\nalike length [[],[null,null],[],[],[],[null]]\nprimitives [[],[],[\"a\",\"\360\237\230\200\"],[null,null]]\nstr index [\"a0\",\"\360\237\230\2001\",\"b2\"]\nthis arg [[11]]\nbad fn TypeError: number 5 is not a function");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('null fn', () => Array.from([1], null)), t('undefined fn', () => Array.from([1], undefined)), t('nullish', () => Array.from(null)), t('nullish u', () => Array.from(undefined)), t('mapfn throws', () => Array.from([1, 2], (x) => { if (x === 2) throw new RangeError('r'); return x; })), t('live array', () => { const a = [1, 2]; return Array.from(a, (x) => { if (a.length < 4) a.push(x * 10); return x; }); })].join(\"\\n\");",
       "null fn TypeError: object null is not a function\nundefined fn [1]\nnullish TypeError: object null is not iterable (cannot read property Symbol(Symbol.iterator))\nnullish u TypeError: undefined is not iterable (cannot read property Symbol(Symbol.iterator))\nmapfn throws RangeError: r\nlive array [1,2,10,20]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('too long', () => Array.from({ length: 2 ** 32 }))].join(\"\\n\");",
       "too long RangeError: Invalid array length");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('fe map', () => [Object.fromEntries(new Map([['a', 1], ['b', 2]])), Object.fromEntries(new Map([[1, 2]]).entries())]), t('fe set', () => Object.fromEntries(new Set([['k', 'v']]))), t('fe alike', () => Object.fromEntries([{ 0: 'k', 1: 'v' }, ['short']])), t('fe str entry', () => Object.fromEntries(['ab'])), t('fe num', () => Object.fromEntries(5)), t('fe null', () => Object.fromEntries(null))].join(\"\\n\");",
       "fe map [{\"a\":1,\"b\":2},{\"1\":2}]\nfe set {\"k\":\"v\"}\nfe alike {\"k\":\"v\"}\nfe str entry TypeError: Iterator value ab is not an entry object\nfe num TypeError: number 5 is not iterable (cannot read property Symbol(Symbol.iterator))\nfe null TypeError: undefined is not iterable");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('fe undef', () => Object.fromEntries()), t('fe str', () => Object.fromEntries('ab')), t('fe key conv', () => Object.fromEntries([[{ toString() { return 'x'; } }, 1], [2, 3]]))].join(\"\\n\");",
       "fe undef TypeError: undefined is not iterable\nfe str TypeError: Iterator value a is not an entry object\nfe key conv {\"2\":3,\"x\":1}");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('map from map', () => { const a = new Map([['x', 1]]); const b = new Map(a); b.set('x', 2); return [a.get('x'), b.get('x'), [...new Map(a.entries())]]; }), t('map alike', () => [[...new Map([{ 0: 'k', 1: 'v' }])], new Map([[1]]).get(1), new Map([[1]]).has(1)]), t('map bad entry', () => new Map([1])), t('map str entry', () => new Map(['ab'])), t('map num', () => new Map(5)), t('map str', () => new Map('ab'))].join(\"\\n\");",
       "map from map [1,2,[[\"x\",1]]]\nmap alike [[[\"k\",\"v\"]],null,true]\nmap bad entry TypeError: Iterator value 1 is not an entry object\nmap str entry TypeError: Iterator value ab is not an entry object\nmap num TypeError: number 5 is not iterable (cannot read property Symbol(Symbol.iterator))\nmap str TypeError: Iterator value a is not an entry object");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('map adder', () => { const o = Map.prototype.set; Map.prototype.set = function (k, v) { return o.call(this, k, v * 10); }; const r = [...new Map([[1, 2]])]; Map.prototype.set = o; return [r, [...new Map([[1, 2]])]]; }), t('map adder bad', () => { const o = Map.prototype.set; Map.prototype.set = 5; try { return new Map([]); } finally { Map.prototype.set = o; } }), t('map adder skip', () => { const o = Map.prototype.set; Map.prototype.set = 5; try { return new Map().size + new Map(null).size; } finally { Map.prototype.set = o; } }), t('set from', () => [[...new Set('aba')], [...new Set(new Set([1, 2]))], [...new Set(new Map([[1, 2]]).keys())], [...new Set(new Map([[1, 2]]))]]), t('set num', () => new Set(5)), t('set adder', () => { const o = Set.prototype.add; Set.prototype.add = undefined; try { return new Set([1]); } finally { Set.prototype.add = o; } })].join(\"\\n\");",
       "map adder [[[1,20]],[[1,2]]]\nmap adder bad TypeError: '5' returned for property 'set' of object '#<Map>' is not a function\nmap adder skip 0\nset from [[\"a\",\"b\"],[1,2],[1],[[1,2]]]\nset num TypeError: number 5 is not iterable (cannot read property Symbol(Symbol.iterator))\nset adder TypeError: 'undefined' returned for property 'add' of object '#<Set>' is not a function");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('set adder call', () => { const o = Set.prototype.add; const seen = []; Set.prototype.add = function (v) { seen.push(v); return o.call(this, v); }; const s = new Set('ab'); Set.prototype.add = o; return [seen, s.size]; })].join(\"\\n\");",
       "set adder call [[\"a\",\"b\"],2]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('split limit', () => ['a,b,c'.split(',', 2), 'a,b,c'.split(',', 0), 'a,b,c'.split(',', -1), 'a,b,c'.split(',', 1.5), 'a,b,c'.split(',', undefined), 'a,b,c'.split(',', '1'), 'a,b,c'.split(',', 2 ** 32 + 1)]), t('split nosep', () => ['abc'.split(undefined, 0), 'abc'.split(undefined, 1), 'abc'.split(), ''.split(',', 0), ''.split(',')]), t('split empty sep', () => ['abc'.split('', 2), 'abc'.split('', 0), 'abc'.split('')]), t('split null sep', () => 'anullb'.split(null, 5)), t('split re', () => ['a1b2c'.split(/\\d/, 2), 'a1b'.split(/(\\d)/, 2), 'a1b'.split(/(\\d)/, 3), 'a1b2c'.split(/(\\d)/, 4), 'abc'.split(/x/, 0), ''.split(/x/, 0), ''.split(/x/, 1), 'ab'.split(/(?:)/, 1)]), t('split re groups', () => 'a12b'.split(/(\\d)(\\d)/, 2))].join(\"\\n\");",
       "split limit [[\"a\",\"b\"],[],[\"a\",\"b\",\"c\"],[\"a\"],[\"a\",\"b\",\"c\"],[\"a\"],[\"a\"]]\nsplit nosep [[],[\"abc\"],[\"abc\"],[],[\"\"]]\nsplit empty sep [[\"a\",\"b\"],[],[\"a\",\"b\",\"c\"]]\nsplit null sep [\"a\",\"b\"]\nsplit re [[\"a\",\"b\"],[\"a\",\"1\"],[\"a\",\"1\",\"b\"],[\"a\",\"1\",\"b\",\"2\"],[],[],[\"\"],[\"a\"]]\nsplit re groups [\"a\",\"1\"]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('split order', () => { const o = []; 'a,b'.split({ toString() { o.push('sep'); return ','; } }, { valueOf() { o.push('lim'); return 1; } }); return o; }), t('split lim throws', () => 'a'.split(',', { valueOf() { throw new TypeError('lim'); } }))].join(\"\\n\");",
       "split order [\"lim\",\"sep\"]\nsplit lim throws TypeError: lim");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('lazy', () => { const it = 'a1b2'.matchAll(/\\d/g); return [it.next().value[0], [...it].length, it.next()]; }), t('last index', () => { const r = /\\d/g; r.lastIndex = 2; return [[...'1a2b3'.matchAll(r)].map((m) => m[0]), r.lastIndex]; }), t('last index big', () => { const r = /\\d/g; r.lastIndex = 99; return [...'1a2'.matchAll(r)]; }), t('string pat', () => [...'a.b.c'.matchAll('.')].map((m) => m.index)), t('empty matches', () => [...'abc'.matchAll(/(?:)/g)].map((m) => m.index)), t('unicode empty', () => [...'\360\237\230\200\360\237\230\200'.matchAll(/(?:)/gu)].map((m) => m.index))].join(\"\\n\");",
       "lazy [\"1\",1,{\"done\":true}]\nlast index [[\"2\",\"3\"],2]\nlast index big []\nstring pat [0,1,2,3,4]\nempty matches [0,1,2,3]\nunicode empty [0,2,4]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t('groups', () => { const [m] = 'k=v'.matchAll(/(?<key>\\w)=(?<val>\\w)/g); return [m.index, m.input, m.groups.key, m.groups.val, m.length]; }), t('non global', () => 'a'.matchAll(/a/)), t('no match', () => [...'abc'.matchAll(/\\d/g)]), t('spread call msgs', () => { const r = []; const u = undefined, o = {}; for (const f of [() => Math.max(...5), () => Math.max(...o), () => Math.max(...u), () => Math.max(...null), () => new Array(...o), () => [...o]]) { try { f(); } catch (e) { r.push(e.message); } } return r; })].join(\"\\n\");",
       "groups [0,\"k=v\",\"k\",\"v\",3]\nnon global TypeError: String.prototype.matchAll called with a non-global RegExp argument\nno match []\nspread call msgs [\"Spread syntax requires ...iterable[Symbol.iterator] to be a function\",\"Spread syntax requires ...iterable[Symbol.iterator] to be a function\",\"u is not iterable (cannot read property undefined)\",\"null is not iterable (cannot read property null)\",\"Spread syntax requires ...iterable[Symbol.iterator] to be a function\",\"o is not iterable\"]");
    /* the live list under a collection at every allocation: iterators dropped mid-way, deletes
     * and clears with several open, forEach and for-of growing the collection */
    eq_s("let out = []; const m = new Map(); for (let i = 0; i < 6; i++) m.set(i, 'v' + i);"
         " for (let j = 0; j < 4; j++) { const it = m.keys(); it.next(); }"
         " const a = m.entries(), b = m.values(); a.next(); a.next(); b.next();"
         " m.delete(0); m.delete(4); out.push(JSON.stringify([...a]), JSON.stringify([...b]));"
         " const s = new Set([1, 2, 3]); const c = s.values(); c.next(); s.clear(); s.add('x');"
         " out.push(JSON.stringify([...c]));"
         " s.forEach((v) => { if (s.size < 5) s.add(v + 'y'); }); out.push(JSON.stringify([...s]));"
         " for (const [k] of m) { if (k === 1) m.set(9, 'v9'); } out.push(m.size);"
         " const [[p, q], r] = new Map([['k', 1]]).entries(); const [u, ...w] = 'a\360\237\230\200b';"
         " out.push(JSON.stringify([p, q, r, u, w, Array.from({ length: 2 }, (x, i) => i), 'a,b,c'.split(',', 2)]));"
         " out.push(JSON.stringify([...'x1y22'.matchAll(/\\d+/g)].map((x) => x[0])));"
         " out.join('|');",
         "[[2,\"v2\"],[3,\"v3\"],[5,\"v5\"]]|[\"v1\",\"v2\",\"v3\",\"v5\"]|[\"x\"]|[\"x\",\"xy\",\"xyy\",\"xyyy\",\"xyyyy\"]|5|[\"k\",1,null,\"a\",[\"\360\237\230\200\",\"b\"],[0,1],[\"a\",\"b\"]]|[\"1\",\"22\"]");
}

static void test_function_prototype(void) {
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const FP = Object.getPrototypeOf(() => 1); const AFP = Object.getPrototypeOf(async function () {}); [t('typeof', () => [typeof FP, FP(), FP(1, 2, 3), FP.length, FP.name, JSON.stringify(FP.name)]), t('proto', () => [Object.getPrototypeOf(FP) === Object.prototype, Object.getPrototypeOf(function () {}) === FP, Object.getPrototypeOf(Math.max) === FP, Object.getPrototypeOf(async () => 1) === FP]), t('tag', () => [Object.prototype.toString.call(FP), FP.toString(), String(FP)]), t('new', () => { const C = FP; return new C(); }), t('own prototype', () => [Object.hasOwn(FP, 'prototype'), FP.prototype, 'prototype' in FP]), t('keys', () => [Object.keys(FP), Object.getOwnPropertyNames(FP).filter((n) => !['arguments', 'caller', 'constructor'].includes(n))])].join(\"\\n\");",
       "typeof [\"function\",null,null,0,\"\",\"\\\"\\\"\"]\nproto [true,true,true,false]\ntag [\"[object Function]\",\"function () { [native code] }\",\"function () { [native code] }\"]\nnew TypeError: C is not a constructor\nown prototype [false,null,false]\nkeys [[],[\"length\",\"name\",\"apply\",\"bind\",\"call\",\"toString\"]]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const FP = Object.getPrototypeOf(() => 1); const AFP = Object.getPrototypeOf(async function () {}); [t('methods', () => [typeof FP.call, FP.call === (() => 1).call, FP.apply === Math.max.apply, typeof FP.bind]), t('call it', () => [FP.call(null, 1), FP.apply(null, [1]), FP.bind(null)()]), t('instanceof', () => { function F() {} F.prototype = FP; return [(() => 1) instanceof F, ({}) instanceof F]; }), t('add method', () => { FP.twice = function () { return this() * 2; }; const r = [(() => 21).twice(), Math.max.twice === FP.twice, Object.hasOwn(FP, 'twice')]; delete FP.twice; return [r, typeof (() => 1).twice]; }), t('setproto FP', () => { const X = { fromX: 'x' }; Object.setPrototypeOf(FP, X); const r = [(() => 1).fromX, typeof (() => 1).hasOwnProperty, FP.fromX, Math.max.fromX]; Object.setPrototypeOf(FP, Object.prototype); return [r, (() => 1).fromX, typeof (() => 1).hasOwnProperty]; }), t('setproto String.prototype', () => { const SP = Object.getPrototypeOf('a'); const X = { fromX: 1 }; Object.setPrototypeOf(SP, X); const r = ['a'.fromX, typeof 'a'.hasOwnProperty]; Object.setPrototypeOf(SP, Object.prototype); return [r, 'a'.fromX]; })].join(\"\\n\");",
       "methods [\"function\",true,true,\"function\"]\ncall it [null,null,null]\ninstanceof [true,false]\nadd method [[42,true,true],\"undefined\"]\nsetproto FP [[\"x\",\"function\",\"x\",\"x\"],null,\"function\"]\nsetproto String.prototype [[1,\"function\"],null]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const FP = Object.getPrototypeOf(() => 1); const AFP = Object.getPrototypeOf(async function () {}); [t('fn hasOwnProperty', () => [(() => 1).hasOwnProperty('name'), FP.hasOwnProperty('call'), FP.hasOwnProperty('length')]), t('callback', () => [[1, 2].map(FP), [3].forEach(FP), Promise.resolve().then(FP) instanceof Promise]), t('freeze FP', () => Object.isFrozen(FP)), t('bound default proto', () => Object.getPrototypeOf((() => 1).bind(null)) === FP), t('afp', () => [typeof AFP, Object.getPrototypeOf(AFP) === FP, Object.getOwnPropertyNames(AFP).filter((n) => n !== 'constructor'), Object.prototype.toString.call(AFP), Object.prototype.toString.call(async () => 1), Object.getPrototypeOf(async () => 1) === AFP, Object.getPrototypeOf(async function () {}.bind(null)) === AFP, Object.keys(AFP)]), t('afp method', () => [Object.getPrototypeOf({ async m() {} }.m) === AFP, typeof (async () => 1).call, (async () => 1).name])].join(\"\\n\");",
       "fn hasOwnProperty [true,true,true]\ncallback [[null,null],null,true]\nfreeze FP false\nbound default proto true\nafp [\"object\",true,[],\"[object AsyncFunction]\",\"[object AsyncFunction]\",true,true,[]]\nafp method [true,\"function\",\"\"]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const FP = Object.getPrototypeOf(() => 1); const AFP = Object.getPrototypeOf(async function () {}); [t('afp call', () => AFP()), t('afp string', () => String(AFP)), t('afp add', () => { AFP.tagged = 1; const r = [(async () => 1).tagged, (() => 1).tagged]; delete AFP.tagged; return r; }), t('tags', () => [Object.prototype.toString.call(Object.create(Promise.prototype)), Object.prototype.toString.call(Object.create(Math)), Object.prototype.toString.call(Object.setPrototypeOf([], Promise.prototype)), Object.prototype.toString.call(Object.setPrototypeOf(async () => 1, null)), Object.prototype.toString.call(Object.setPrototypeOf(() => 1, AFP)), Object.prototype.toString.call(Promise.prototype), Object.prototype.toString.call(Promise.resolve())]), t('async still async', () => (async () => 5)() instanceof Promise)].join(\"\\n\");",
       "afp call TypeError: AFP is not a function\nafp string TypeError: Function.prototype.toString requires that 'this' be a Function\nafp add [1,null]\ntags [\"[object Promise]\",\"[object Math]\",\"[object Promise]\",\"[object Function]\",\"[object AsyncFunction]\",\"[object Promise]\",\"[object Promise]\"]\nasync still async true");
}

static void test_set_prototype(void) {
    eq("const FP = Object.getPrototypeOf(() => 1); const isFn = (x) => { for (let q = Object.getPrototypeOf(x); q !== null; q = Object.getPrototypeOf(q)) if (q === FP) return true; return false; }; const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t(\"cyc\", ()=>{const a={},b={}; Object.setPrototypeOf(a,b); Object.setPrototypeOf(b,a)}), t(\"cyc self\", ()=>{const a={}; Object.setPrototypeOf(a,a)}), t(\"cyc fn\", ()=>{const f=function(){}; const o=Object.create(f); Object.setPrototypeOf(f,o)}), t(\"cyc promise\", ()=>{const p=Promise.resolve(); const o=Object.create(p); Object.setPrototypeOf(p,o)}), t(\"frozen fn\", ()=>{const f=function g(){}; Object.freeze(f); Object.setPrototypeOf(f,{})}), t(\"frozen fn same\", ()=>{const f=function g(){}; Object.freeze(f); return Object.setPrototypeOf(f,Object.getPrototypeOf(f))===f})].join(\"\\n\");",
       "cyc TypeError: Cyclic __proto__ value\ncyc self TypeError: Cyclic __proto__ value\ncyc fn TypeError: Cyclic __proto__ value\ncyc promise TypeError: Cyclic __proto__ value\nfrozen fn TypeError: function g(){} is not extensible\nfrozen fn same true");
    eq("const FP = Object.getPrototypeOf(() => 1); const isFn = (x) => { for (let q = Object.getPrototypeOf(x); q !== null; q = Object.getPrototypeOf(q)) if (q === FP) return true; return false; }; const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t(\"frozen promise\", ()=>{const p=Promise.resolve(); Object.freeze(p); Object.setPrototypeOf(p,{})}), t(\"frozen obj\", ()=>{Object.setPrototypeOf(Object.freeze({}),{})}), t(\"sealed arr\", ()=>{Object.setPrototypeOf(Object.seal([]),{})}), t(\"prim\", ()=>Object.setPrototypeOf(5, {})), t(\"prim bad\", ()=>Object.setPrototypeOf(5, 3)), t(\"fn proto\", ()=>{const f=function(){}; const P={hi(){return \"hi\"}}; Object.setPrototypeOf(f,P); return [f.hi(), typeof f.call, Object.getPrototypeOf(f)===P, isFn(f), f()===undefined]})].join(\"\\n\");",
       "frozen promise TypeError: #<Promise> is not extensible\nfrozen obj TypeError: #<Object> is not extensible\nsealed arr TypeError: [object Array] is not extensible\nprim 5\nprim bad TypeError: Object prototype may only be an Object or null: 3\nfn proto [\"hi\",\"undefined\",true,false,true]");
    eq("const FP = Object.getPrototypeOf(() => 1); const isFn = (x) => { for (let q = Object.getPrototypeOf(x); q !== null; q = Object.getPrototypeOf(q)) if (q === FP) return true; return false; }; const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t(\"fn null\", ()=>{const f=function(){return 7}; Object.setPrototypeOf(f,null); return [typeof f.call, f(), Object.getPrototypeOf(f), \"call\" in f]}), t(\"obj from fn\", ()=>{function g(a,b){} g.s=1; const o=Object.create(g); return [o.s, o.name, o.length, typeof o.call, isFn(o), typeof o, Object.getPrototypeOf(o)===g, \"s\" in o, \"name\" in o]}), t(\"setproto to fn\", ()=>{function g(){} g.k=2; const o=Object.setPrototypeOf({}, g); return [o.k, typeof Object.getPrototypeOf(o)]}), t(\"promise proto\", ()=>{const p=Promise.resolve(1); const P={x:1}; Object.setPrototypeOf(p,P); return [p.x, typeof p.then, p instanceof Promise]}), t(\"obj from promise\", ()=>{const p=Promise.resolve(1); p.z=3; const o=Object.create(p); return [o.z, typeof o.then, o instanceof Promise]}), t(\"ctor proto fn\", ()=>{function F(){} function G(){} G.q=5; F.prototype=G; const o=new F(); return [o.q, o instanceof F, Object.getPrototypeOf(o)===G]})].join(\"\\n\");",
       "fn null [\"undefined\",7,null,false]\nobj from fn [1,\"g\",2,\"function\",true,\"object\",true,true,true]\nsetproto to fn [2,\"function\"]\npromise proto [1,\"undefined\",false]\nobj from promise [3,\"function\",true]\nctor proto fn [5,true,true]");
    eq("const FP = Object.getPrototypeOf(() => 1); const isFn = (x) => { for (let q = Object.getPrototypeOf(x); q !== null; q = Object.getPrototypeOf(q)) if (q === FP) return true; return false; }; const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t(\"instanceof via fn\", ()=>{function F(){} const f=function(){}; Object.setPrototypeOf(f, F.prototype); return [f instanceof F, isFn(f)]}), t(\"subclass statics\", ()=>{function A(){} A.make=()=>\"m\"; function B(){} Object.setPrototypeOf(B,A); return [B.make(), Object.getPrototypeOf(B)===A, isFn(B), B.name]}), t(\"native proto\", ()=>{const P={z:1}; const m = Math.min; Object.setPrototypeOf(m,P); return [m.z, typeof m.call, m(1,2)]}), t(\"error ctor\", ()=>{const P={w:1}; Object.setPrototypeOf(Error,P); return [Error.w, TypeError.w, typeof TypeError.captureStackTrace, String(new TypeError(\"x\"))]}), t(\"arr proto\", ()=>{const a=[1]; Object.setPrototypeOf(a, null); return [typeof a.push, Array.isArray(a), a.length]}), t(\"getproto chain\", ()=>{function A(){} function B(){} Object.setPrototypeOf(B,A); const o=Object.create(B); return [Object.getPrototypeOf(Object.getPrototypeOf(o))===A]})].join(\"\\n\");",
       "instanceof via fn [true,false]\nsubclass statics [\"m\",true,true,\"B\"]\nnative proto [1,\"undefined\",1]\nerror ctor [1,1,\"function\",\"TypeError: x\"]\narr proto [\"undefined\",true,1]\ngetproto chain [true]");
    eq("const FP = Object.getPrototypeOf(() => 1); const isFn = (x) => { for (let q = Object.getPrototypeOf(x); q !== null; q = Object.getPrototypeOf(q)) if (q === FP) return true; return false; }; const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t(\"create fn bad\", ()=>Object.create(5)), t(\"setproto undefined\", ()=>Object.setPrototypeOf({}, undefined)), t(\"default fn proto same\", ()=>[Object.getPrototypeOf(function(){})===Object.getPrototypeOf(()=>1), Object.getPrototypeOf(TypeError)===Error, Object.getPrototypeOf(Promise.resolve())===Promise.prototype]), t(\"bound proto\", ()=>{function A(){} function B(){} Object.setPrototypeOf(B,A); return [Object.getPrototypeOf(B.bind(null))===A, Object.getPrototypeOf(TypeError.bind(null))===Error]}), t(\"method chain\", ()=>{const base={greet(){return \"hello\"}}; const mid=Object.create(base); const f=function(){}; Object.setPrototypeOf(f, mid); const o=Object.create(f); return [o.greet(), \"greet\" in o, f.greet()]}), t(\"err via fn proto\", ()=>{const e=new Error(\"m\"); const f=function named(){}; Object.setPrototypeOf(e, f); return [e.name, e.message]})].join(\"\\n\");",
       "create fn bad TypeError: Object prototype may only be an Object or null: 5\nsetproto undefined TypeError: Object prototype may only be an Object or null: undefined\ndefault fn proto same [true,true,true]\nbound proto [true,true]\nmethod chain [\"hello\",true,\"hello\"]\nerr via fn proto [\"named\",\"m\"]");
    eq("const FP = Object.getPrototypeOf(() => 1); const isFn = (x) => { for (let q = Object.getPrototypeOf(x); q !== null; q = Object.getPrototypeOf(q)) if (q === FP) return true; return false; }; const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; [t(\"reset\", ()=>{const f=function(){}; Object.setPrototypeOf(f,null); Object.setPrototypeOf(f,FP); return typeof f.call})].join(\"\\n\");",
       "reset \"function\"");
    eq_s("const junk = () => { let a = []; for (let i = 0; i < 50; i++) a.push({ s: 'x' + i }); return a.length; }; const f = function () {}; Object.setPrototypeOf(f, { tag: 'fp' }); const p = Promise.resolve(); Object.setPrototypeOf(p, { tag: 'pp' }); const o = Object.create(f); junk(); junk(); [f.tag, p.tag, o.tag, Object.getPrototypeOf(f).tag].join();",
       "fp,pp,fp,fp");
}

static void test_json_v8(void) {
    /* U+2028 and U+2029 may stand raw in a string literal (ES2019), so JSON text is a subset */
    eq("['a\342\200\250b'.length, 'x\342\200\251'.charCodeAt(1), JSON.parse('\"\342\200\250\"').length].join();", "3,8233,1");
    eq("const show = (label, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = 'throw ' + e.constructor.name + ': ' + e.message; } return label + ' ' + s; }; [show('toJSON', () => [JSON.stringify({ toJSON(k) { return 'K=' + k; } }), JSON.stringify({ x: { toJSON(k) { return 'K=' + k; } } }), JSON.stringify([{ toJSON(k) { return 'K=' + k; } }])]), show('date', () => JSON.stringify({ d: new Date(0), a: [new Date(86400000)] })), show('replacer fn', () => JSON.stringify({ a: 1, b: { c: 2 }, d: [3, 'x'] }, function (k, v) { return typeof v === 'number' ? v * 10 : v; })), show('replacer this', () => JSON.stringify({ a: 1 }, function (k, v) { return k === '' ? Object.keys(this).join() + ':' + JSON.stringify(v) : v; })), show('replacer drop', () => JSON.stringify({ a: 1, b: 2, c: [1, 2] }, (k, v) => (k === 'b' || k === '1' ? undefined : v))), show('replacer array', () => [JSON.stringify({ b: 1, a: 2, 3: 4 }, ['a', 3, 'a', 'zz', {}, true]), JSON.stringify([{ a: 1, b: 2 }], ['b']), JSON.stringify({ a: { a: 1, b: 2 }, b: 3 }, ['a'])])].join(\"\\n\");",
       "toJSON [\"\\\"K=\\\"\",\"{\\\"x\\\":\\\"K=x\\\"}\",\"[\\\"K=0\\\"]\"]\ndate \"{\\\"d\\\":\\\"1970-01-01T00:00:00.000Z\\\",\\\"a\\\":[\\\"1970-01-02T00:00:00.000Z\\\"]}\"\nreplacer fn \"{\\\"a\\\":10,\\\"b\\\":{\\\"c\\\":20},\\\"d\\\":[30,\\\"x\\\"]}\"\nreplacer this \"\\\":{\\\\\\\"a\\\\\\\":1}\\\"\"\nreplacer drop \"{\\\"a\\\":1,\\\"c\\\":[1,null]}\"\nreplacer array [\"{\\\"a\\\":2,\\\"3\\\":4}\",\"[{\\\"b\\\":2}]\",\"{\\\"a\\\":{\\\"a\\\":1}}\"]");
    eq("const show = (label, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = 'throw ' + e.constructor.name + ': ' + e.message; } return label + ' ' + s; }; [show('replacer other', () => [JSON.stringify({ a: 1 }, 5), JSON.stringify({ a: 1 }, 'x'), JSON.stringify({ a: 1 }, null)]), show('gap', () => [JSON.stringify({ a: [1, { b: 2 }], c: {}, d: [] }, null, 2), JSON.stringify([1], null, '--'), JSON.stringify({ a: 1 }, null, 'abcdefghijklmn'), JSON.stringify({ a: 1 }, null, 20)]), show('circular', () => { const o = { a: { b: {} } }; o.a.b.c = o; JSON.stringify(o); }), show('circular arr', () => { const a = [{ x: [] }]; a[0].x.push(a); JSON.stringify(a); }), show('circular long', () => { const o = { a: { b: { c: { d: { e: {} } } } } }; o.a.b.c.d.e.f = o; JSON.stringify(o); }), show('circular ctor', () => { function F() {} const o = new F(); o.s = o; JSON.stringify(o); })].join(\"\\n\");",
       "replacer other [\"{\\\"a\\\":1}\",\"{\\\"a\\\":1}\",\"{\\\"a\\\":1}\"]\ngap [\"{\\n  \\\"a\\\": [\\n    1,\\n    {\\n      \\\"b\\\": 2\\n    }\\n  ],\\n  \\\"c\\\": {},\\n  \\\"d\\\": []\\n}\",\"[\\n--1\\n]\",\"{\\nabcdefghij\\\"a\\\": 1\\n}\",\"{\\n          \\\"a\\\": 1\\n}\"]\ncircular throw TypeError: Converting circular structure to JSON\n    --> starting at object with constructor 'Object'\n    |     property 'a' -> object with constructor 'Object'\n    |     property 'b' -> object with constructor 'Object'\n    --- property 'c' closes the circle\ncircular arr throw TypeError: Converting circular structure to JSON\n    --> starting at object with constructor 'Array'\n    |     index 0 -> object with constructor 'Object'\n    |     property 'x' -> object with constructor 'Array'\n    --- index 0 closes the circle\ncircular long throw TypeError: Converting circular structure to JSON\n    --> starting at object with constructor 'Object'\n    |     property 'a' -> object with constructor 'Object'\n    |     property 'b' -> object with constructor 'Object'\n    |     ...\n    |     property 'e' -> object with constructor 'Object'\n    --- property 'f' closes the circle\ncircular ctor throw TypeError: Converting circular structure to JSON\n    --> starting at object with constructor 'F'\n    --- property 's' closes the circle");
    eq("const show = (label, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = 'throw ' + e.constructor.name + ': ' + e.message; } return label + ' ' + s; }; [show('circular nullproto', () => { const o = Object.create(null); o.s = o; JSON.stringify(o); }), show('circular mid', () => { const o = { a: { b: {} } }; o.a.b.c = o.a; JSON.stringify({ root: o }); }), show('not circular repeat', () => { const x = { v: 1 }; return JSON.stringify({ a: x, b: x, c: [x, x] }); }), show('surrogates', () => [JSON.stringify('\\ud800'), JSON.stringify('\\udc00x'), JSON.stringify('\\ud83d\\ude00'), JSON.stringify('a\\udbffb'), JSON.stringify(', \\u0007')]), show('toJSON throws', () => JSON.stringify({ a: { toJSON() { throw new Error('tj'); } } }))].join(\"\\n\");",
       "circular nullproto throw TypeError: Converting circular structure to JSON\n    --> starting at object with constructor 'Object'\n    --- property 's' closes the circle\ncircular mid throw TypeError: Converting circular structure to JSON\n    --> starting at object with constructor 'Object'\n    |     property 'b' -> object with constructor 'Object'\n    --- property 'c' closes the circle\nnot circular repeat \"{\\\"a\\\":{\\\"v\\\":1},\\\"b\\\":{\\\"v\\\":1},\\\"c\\\":[{\\\"v\\\":1},{\\\"v\\\":1}]}\"\nsurrogates [\"\\\"\\\\ud800\\\"\",\"\\\"\\\\udc00x\\\"\",\"\\\"\360\237\230\200\\\"\",\"\\\"a\\\\udbffb\\\"\",\"\\\", \\\\u0007\\\"\"]\ntoJSON throws throw Error: tj");
    eq("const show = (label, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = 'throw ' + e.constructor.name + ': ' + e.message; } return label + ' ' + s; }; [show('replacer throws', () => JSON.stringify({ a: 1 }, () => { throw new RangeError('rp'); })), show('toJSON nonfn', () => JSON.stringify({ toJSON: 5, a: 1 })), show('toJSON on fn', () => { const f = () => 1; f.toJSON = () => 'F'; return JSON.stringify([f, { f }]); }), show('replacer mutates', () => JSON.stringify({ a: 1, b: 2 }, function (k, v) { if (k === 'a') { delete this.b; this.c = 3; } return v; })), show('toJSON returns obj', () => JSON.stringify({ a: { toJSON() { return { z: [1] }; } } })), show('replacer new obj', () => JSON.stringify(1, (k, v) => (k === '' ? { n: v } : v)))].join(\"\\n\");",
       "replacer throws throw RangeError: rp\ntoJSON nonfn \"{\\\"toJSON\\\":5,\\\"a\\\":1}\"\ntoJSON on fn \"[\\\"F\\\",{\\\"f\\\":\\\"F\\\"}]\"\nreplacer mutates \"{\\\"a\\\":1}\"\ntoJSON returns obj \"{\\\"a\\\":{\\\"z\\\":[1]}}\"\nreplacer new obj \"{\\\"n\\\":1}\"");
    eq("const show = (label, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = 'throw ' + e.constructor.name + ': ' + e.message; } return label + ' ' + s; }; [show('undefined top', () => [JSON.stringify(undefined), JSON.stringify(() => 1), JSON.stringify(undefined, (k, v) => 7)]), show('replacer key types', () => { const ks = []; JSON.stringify({ a: [10, { b: 1 }] }, (k, v) => { ks.push(typeof k + ':' + k); return v; }); return ks; }), show('error map set regexp', () => JSON.stringify([new Error('x'), new Map([[1, 2]]), new Set([1]), /a/g, Math, JSON])), show('raw', () => { const r = JSON.rawJSON('1e1000'); return [JSON.stringify({ r, a: [r] }), r.rawJSON, Object.getPrototypeOf(r), Object.isFrozen(r), JSON.isRawJSON(r), JSON.isRawJSON({ rawJSON: '1' }), JSON.isRawJSON(1), Object.keys(r), Object.prototype.toString.call(r)]; }), show('raw bad', () => ['', ' 1', '{}', '[1]', '1x', '1 ', 'tru', '\"a', 12, null, true, '-', '\"\\\\u00e9\"'].map((t) => { try { return JSON.rawJSON(t).rawJSON; } catch (e) { return e.constructor.name + ': ' + e.message; } })), show('raw in replacer', () => JSON.stringify({ a: 1 }, (k, v) => (k === 'a' ? JSON.rawJSON('99') : v)))].join(\"\\n\");",
       "undefined top [null,null,\"7\"]\nreplacer key types [\"string:\",\"string:a\",\"string:0\",\"string:1\",\"string:b\"]\nerror map set regexp \"[{},{},{},{},{},{}]\"\nraw [\"{\\\"r\\\":1e1000,\\\"a\\\":[1e1000]}\",\"1e1000\",null,true,true,false,false,[\"rawJSON\"],\"[object Object]\"]\nraw bad [\"SyntaxError: Invalid value for JSON.rawJSON\",\"SyntaxError: Unexpected token ' ', \\\" 1\\\" is not valid JSON\",\"SyntaxError: Unexpected token '{', \\\"{}\\\" is not valid JSON\",\"SyntaxError: Unexpected token '[', \\\"[1]\\\" is not valid JSON\",\"SyntaxError: Invalid value for JSON.rawJSON\",\"SyntaxError: Invalid value for JSON.rawJSON\",\"SyntaxError: Unexpected end of JSON input\",\"SyntaxError: Unterminated string in JSON at position 2 (line 1 column 3)\",\"12\",\"null\",\"true\",\"SyntaxError: No number after minus sign in JSON at position 1 (line 1 column 2)\",\"\\\"\\\\u00e9\\\"\"]\nraw in replacer \"{\\\"a\\\":99}\"");
    eq("const show = (label, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = 'throw ' + e.constructor.name + ': ' + e.message; } return label + ' ' + s; }; [show('raw lengths', () => [JSON.rawJSON.length, JSON.isRawJSON.length, JSON.rawJSON.name]), show('promise', () => { const p = Promise.resolve(); p.a = 1; return JSON.stringify({ p }); }), show('gopn JSON', () => Object.getOwnPropertyNames(JSON))].join(\"\\n\");",
       "raw lengths [1,1,\"rawJSON\"]\npromise \"{\\\"p\\\":{\\\"a\\\":1}}\"\ngopn JSON [\"parse\",\"stringify\",\"rawJSON\",\"isRawJSON\"]");
    eq("const show = (label, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = 'throw ' + e.constructor.name + ': ' + e.message; } return label + ' ' + s; }; [show('ctx', () => { const log = []; JSON.parse('{\"a\":1.50,\"b\":[2,\"x\",true,null],\"c\":{\"d\":-0},\"e\":\"\\\\u0041\"}', function (k, v, c) { log.push(k + ':' + JSON.stringify(c)); return v; }); return log; }), show('modified', () => { const log = []; JSON.parse('{\"a\":1,\"b\":2,\"c\":3}', function (k, v, c) { if (k === 'a') { this.b = 5; this.c = 3; } log.push(k + ':' + JSON.stringify(c) + ':' + v); return v; }); return log; }), show('replaced arr', () => { const log = []; JSON.parse('[1,[2]]', function (k, v, c) { if (k === '0' && Array.isArray(this) && this.length === 2) this[1] = [9]; log.push(k + ':' + JSON.stringify(c)); return v; }); return log; }), show('moved obj', () => { const log = []; JSON.parse('{\"a\":{\"x\":1},\"b\":{\"x\":2}}', function (k, v, c) { if (k === '' ) return v; if (k === 'x') log.push(JSON.stringify(c)); return v; }); return log; }), show('transform', () => JSON.parse('{\"a\":1,\"b\":[1,2,{\"c\":3}]}', (k, v) => (typeof v === 'number' ? v * 2 : v))), show('delete', () => JSON.parse('{\"a\":1,\"b\":2,\"c\":[1,2,3]}', (k, v) => (k === 'b' || k === '1' ? undefined : v)))].join(\"\\n\");",
       "ctx [\"a:{\\\"source\\\":\\\"1.50\\\"}\",\"0:{\\\"source\\\":\\\"2\\\"}\",\"1:{\\\"source\\\":\\\"\\\\\\\"x\\\\\\\"\\\"}\",\"2:{\\\"source\\\":\\\"true\\\"}\",\"3:{\\\"source\\\":\\\"null\\\"}\",\"b:{}\",\"d:{\\\"source\\\":\\\"-0\\\"}\",\"c:{}\",\"e:{\\\"source\\\":\\\"\\\\\\\"\\\\\\\\u0041\\\\\\\"\\\"}\",\":{}\"]\nmodified [\"a:{\\\"source\\\":\\\"1\\\"}:1\",\"b:{}:5\",\"c:{\\\"source\\\":\\\"3\\\"}:3\",\":{}:[object Object]\"]\nreplaced arr [\"0:{\\\"source\\\":\\\"1\\\"}\",\"0:{}\",\"1:{}\",\":{}\"]\nmoved obj [\"{\\\"source\\\":\\\"1\\\"}\",\"{\\\"source\\\":\\\"2\\\"}\"]\ntransform {\"a\":2,\"b\":[2,4,{\"c\":6}]}\ndelete {\"a\":1,\"c\":[1,null,3]}");
    eq("const show = (label, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = 'throw ' + e.constructor.name + ': ' + e.message; } return label + ' ' + s; }; [show('root', () => JSON.parse('5', (k, v) => [k, v])), show('root this', () => JSON.parse('{\"a\":1}', function (k, v) { return k === '' ? Object.keys(this).concat(Object.keys(v)) : v; })), show('order', () => { const ks = []; JSON.parse('{\"a\":{\"b\":1,\"c\":[1,{\"d\":2}]},\"e\":3}', (k, v) => { ks.push(k); return v; }); return ks; }), show('add key', () => JSON.parse('{\"a\":1}', function (k, v) { if (k === 'a') this.z = 9; return v; })), show('frozen', () => JSON.parse('{\"a\":{\"b\":1}}', function (k, v) { if (k === 'a') return v; if (k === 'b') { Object.freeze(this); return 7; } return v; })), show('frozen delete', () => JSON.parse('{\"a\":{\"b\":1}}', function (k, v) { if (k === 'b') { Object.freeze(this); return undefined; } return v; }))].join(\"\\n\");",
       "root [\"\",5]\nroot this [\"\",\"a\"]\norder [\"b\",\"0\",\"d\",\"1\",\"c\",\"a\",\"e\",\"\"]\nadd key {\"a\":1,\"z\":9}\nfrozen {\"a\":{\"b\":1}}\nfrozen delete {\"a\":{\"b\":1}}");
    eq("const show = (label, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = 'throw ' + e.constructor.name + ': ' + e.message; } return label + ' ' + s; }; [show('reviver throws', () => JSON.parse('[1]', () => { throw new TypeError('rv'); })), show('nonfn reviver', () => JSON.parse('[1]', 5)), show('proto key', () => JSON.parse('{\"__proto__\":1,\"a\":{\"x\":1}}', (k, v) => (k === 'x' ? undefined : v))), show('dup keys', () => { const log = []; JSON.parse('{\"a\":1,\"a\":2}', (k, v, c) => { log.push(k + JSON.stringify(c)); return v; }); return log; }), show('strings src', () => { const log = []; JSON.parse('[\"a\\\\nb\", \"\\\\ud83d\\\\ude00\", 1e2, -0.0]', (k, v, c) => { log.push(c.source); return v; }); return log; }), show('ctx proto', () => { let p; JSON.parse('1', (k, v, c) => { p = [Object.getPrototypeOf(c) === Object.prototype, Object.keys(c)]; return v; }); return p; })].join(\"\\n\");",
       "reviver throws throw TypeError: rv\nnonfn reviver [1]\nproto key {\"__proto__\":1,\"a\":{}}\ndup keys [\"a{\\\"source\\\":\\\"2\\\"}\",\"{}\"]\nstrings src [\"\\\"a\\\\nb\\\"\",\"\\\"\\\\ud83d\\\\ude00\\\"\",\"1e2\",\"-0.0\",null]\nctx proto [true,[\"source\"]]");
    eq("const show = (label, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = 'throw ' + e.constructor.name + ': ' + e.message; } return label + ' ' + s; }; [show('same value replaced', () => { const log = []; JSON.parse('{\"a\":1,\"b\":1}', function (k, v, c) { if (k === 'a') this.b = 1; log.push(k + JSON.stringify(c)); return v; }); return log; }), show('nan', () => { const log = []; JSON.parse('{\"a\":1,\"b\":2}', function (k, v, c) { if (k === 'a') this.b = NaN; log.push(k + JSON.stringify(c)); return v; }); return log; }), show('array grows', () => JSON.parse('[1,2]', function (k, v) { if (k === '0') this.push(3); return v; })), show('array shrinks', () => JSON.parse('[1,2,3]', function (k, v) { if (k === '0') this.length = 1; return v; })), show('roundtrip', () => JSON.parse(JSON.stringify({ d: new Date(0) }), (k, v) => (k === 'd' ? new Date(v).getTime() : v)))].join(\"\\n\");",
       "same value replaced [\"a{\\\"source\\\":\\\"1\\\"}\",\"b{\\\"source\\\":\\\"1\\\"}\",\"{}\"]\nnan [\"a{\\\"source\\\":\\\"1\\\"}\",\"b{}\",\"{}\"]\narray grows [1,2,3]\narray shrinks [1]\nroundtrip {\"d\":0}");
}

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

/* Iterator helpers and the iterator protocol over a script's own iterators (what inherits
 * Iterator.prototype): for-of, spread, destructuring and the natives step it and close it
 * (return()) when they stop early; break/continue/return pop the try handlers they leave; an
 * assignment pattern evaluates its member targets before stepping. Expected values from Node. */
static void test_iterator_helpers(void) {
    eq_s("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('for of', () => { const log = []; const out = []; for (const x of ci(3, log)) out.push(x); return [out, log]; }), t('spread', () => { const log = []; return [[...ci(2, log)], log]; }), t('call spread', () => { const log = []; return [Math.max(...ci(3, log)), log]; }), t('array from', () => { const log = []; return [Array.from(ci(2, log), (x) => x * 10), log]; }), t('set ctor', () => { const log = []; return [[...new Set(ci(3, log))], log]; }), t('destructure', () => { const log = []; const [a, , c] = ci(5, log); return [a, c, log]; })].join(\"\\n\");",
       "for of [[0,1,2],[\"n0\",\"n1\",\"n2\",\"n3\"]]\nspread [[0,1],[\"n0\",\"n1\",\"n2\"]]\ncall spread [2,[\"n0\",\"n1\",\"n2\",\"n3\"]]\narray from [[0,10],[\"n0\",\"n1\",\"n2\"]]\nset ctor [[0,1,2],[\"n0\",\"n1\",\"n2\",\"n3\"]]\ndestructure [0,2,[\"n0\",\"n1\",\"n2\",\"r\"]]");
    eq_s("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('map ctor', () => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => i < 2 ? { value: [i, 'v' + i++], done: false } : { done: true }; return [...new Map(it)]; }), t('fromEntries', () => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => i < 2 ? { value: ['k' + i, i++], done: false } : { done: true }; return Object.fromEntries(it); }), t('aggregate', () => new AggregateError(ci(2, [])).errors), t('promise all', () => typeof Promise.all(ci(2, [])).then), t('rest', () => { const log = []; const [a, ...r] = ci(4, log); return [a, r, log]; }), t('param', () => { const log = []; const f = ([a, b]) => a + b; return [f(ci(9, log)), log]; })].join(\"\\n\");",
       "map ctor [[0,\"v0\"],[1,\"v1\"]]\nfromEntries {\"k0\":0,\"k1\":1}\naggregate [0,1]\npromise all \"function\"\nrest [0,[1,2,3],[\"n0\",\"n1\",\"n2\",\"n3\",\"n4\"]]\nparam [1,[\"n0\",\"n1\",\"r\"]]");
    eq_s("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('plain obj', () => { const o = { next() { return { done: true }; } }; return [...o]; }), t('inherits fn', () => { const f = function () {}; Object.setPrototypeOf(f, Iterator.prototype); f.next = () => ({ done: true }); return [...f]; }), t('next not fn', () => { const it = Object.create(Iterator.prototype); it.next = 1; for (const x of it) return x; }), t('result not obj', () => { const it = Object.create(Iterator.prototype); it.next = () => 1; return [...it]; }), t('done truthy', () => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => ({ done: i++ > 1 ? 'yes' : 0, value: i }); return [...it]; }), t('next read once', () => { const it = Object.create(Iterator.prototype); let i = 0; it.next = function () { this.next = () => ({ done: true }); return i++ < 3 ? { value: i, done: false } : { done: true }; }; return [...it]; })].join(\"\\n\");",
       "plain obj TypeError: o is not iterable\ninherits fn []\nnext not fn TypeError: number 1 is not a function\nresult not obj TypeError: Iterator result 1 is not an object\ndone truthy [1,2]\nnext read once [1,2,3]");
    eq_s("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('break', () => { const log = []; for (const x of ci(5, log)) { if (x === 1) break; } return log; }), t('continue', () => { const log = []; for (const x of ci(3, log)) { if (x < 2) continue; } return log; }), t('return', () => { const log = []; const f = () => { for (const x of ci(5, log)) { if (x === 2) return x; } }; return [f(), log]; }), t('throw body', () => { const log = []; try { for (const x of ci(5, log)) throw new Error('b' + x); } catch (e) { log.push(e.message); } return log; }), t('throw next', () => { const log = []; const it = ci(5, log); it.next = () => { throw new Error('n'); }; try { for (const x of it); } catch (e) { log.push(e.message); } return log; }), t('throw binding', () => { const log = []; try { for (const [a] of ci(5, log)); } catch (e) { log.push(e.constructor.name); } return log; })].join(\"\\n\");",
       "break [\"n0\",\"n1\",\"r\"]\ncontinue [\"n0\",\"n1\",\"n2\",\"n3\"]\nreturn [2,[\"n0\",\"n1\",\"n2\",\"r\"]]\nthrow body [\"n0\",\"r\",\"b0\"]\nthrow next [\"n\"]\nthrow binding [\"n0\",\"r\",\"TypeError\"]");
    eq_s("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('labeled continue', () => { const log = []; outer: for (const a of ci(2, log)) { for (const b of ci(3, log)) { if (b === 1) continue outer; } } return log; }), t('labeled break', () => { const log = []; outer: for (const a of ci(2, log)) { for (const b of ci(3, log)) { break outer; } } return log; }), t('break in finally order', () => { const log = []; for (const x of ci(3, log)) { try { break; } finally { log.push('f'); } } return log; }), t('return through finally', () => { const log = []; const f = () => { try { for (const x of ci(3, log)) { try { return 'v'; } finally { log.push('f1'); } } } finally { log.push('f2'); } }; return [f(), log]; }), t('switch in loop', () => { const log = []; for (const x of ci(3, log)) { switch (x) { case 1: break; default: continue; } break; } return log; }), t('return throws', () => { const log = []; const it = ci(3, log); it.return = () => { throw new Error('r'); }; try { for (const x of it) break; } catch (e) { log.push(e.message); } return log; })].join(\"\\n\");",
       "labeled continue [\"n0\",\"n0\",\"n1\",\"r\",\"n1\",\"n0\",\"n1\",\"r\",\"n2\"]\nlabeled break [\"n0\",\"n0\",\"r\",\"r\"]\nbreak in finally order [\"n0\",\"f\",\"r\"]\nreturn through finally [\"v\",[\"n0\",\"f1\",\"r\",\"f2\"]]\nswitch in loop [\"n0\",\"n1\",\"r\"]\nreturn throws [\"n0\",\"r\"]");
    eq_s("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('return nonobj', () => { const it = ci(3, []); it.return = () => 1; for (const x of it) break; }), t('return null', () => { const log = []; const it = ci(3, log); it.return = null; for (const x of it) break; return log; }), t('return throws on throw', () => { const it = ci(3, []); it.return = () => { throw new Error('r'); }; for (const x of it) throw new Error('body'); }), t('return not fn on throw', () => { const it = ci(3, []); it.return = 7; for (const x of it) throw new Error('body'); }), t('return gets this', () => { const log = []; const it = ci(3, log); it.return = function () { log.push(this === it); return {}; }; for (const x of it) break; return log; }), t('builtin no return', () => { const out = []; for (const x of new Set([1, 2]).values()) { out.push(x); break; } return out; })].join(\"\\n\");",
       "return nonobj TypeError: Iterator result 1 is not an object\nreturn null [\"n0\"]\nreturn throws on throw Error: body\nreturn not fn on throw Error: body\nreturn gets this [\"n0\",true]\nbuiltin no return [1]");
    eq_s("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('closed', () => { const log = []; const [a, b] = ci(5, log); return [a, b, log]; }), t('exhausted', () => { const log = []; const [a, b, c] = ci(2, log); return [a, b, c, log]; }), t('exactly', () => { const log = []; const [a, b] = ci(2, log); return [a, b, log]; }), t('default throws', () => { const log = []; try { const [a, b = (() => { throw new Error('d'); })()] = ci(1, log); } catch (e) { log.push(e.message); } return log; }), t('assign target throws', () => { const log = []; const o = Object.freeze({}); try { [o.x] = ci(3, log); } catch (e) { log.push(e.constructor.name); } return log; }), t('nested', () => { const log = []; const [[a]] = [ci(3, log)]; return [a, log]; })].join(\"\\n\");",
       "closed [0,1,[\"n0\",\"n1\",\"r\"]]\nexhausted [0,1,null,[\"n0\",\"n1\",\"n2\"]]\nexactly [0,1,[\"n0\",\"n1\",\"r\"]]\ndefault throws [\"n0\",\"n1\",\"d\"]\nassign target throws [\"n0\",\"r\",\"TypeError\"]\nnested [0,[\"n0\",\"r\"]]");
    eq_s("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('finally once', () => { let n = 0; const f = () => { try { return 1; } finally { n++; throw new Error('x'); } }; try { f(); } catch (e) {} return n; }), t('finally once break', () => { let n = 0; for (;;) { try { try { break; } finally { n++; throw new Error('y'); } } catch (e) {} break; } return n; }), t('no stale handler', () => { const f = () => { for (;;) { try { break; } catch (e) { return 'stale'; } } throw new Error('outer'); }; try { return f(); } catch (e) { return e.message; } }), t('no stale continue', () => { let i = 0; const f = () => { while (i < 2) { i++; try { continue; } catch (e) { return 'stale'; } } throw new Error('outer'); }; try { return f(); } catch (e) { return e.message; } }), t('catch break finally', () => { const log = []; for (;;) { try { throw 1; } catch (e) { break; } finally { log.push('f'); } } try { throw new Error('z'); } catch (e) { log.push(e.message); } return log; }), t('labeled block', () => { const log = []; a: { try { break a; } finally { log.push('f'); } } try { throw new Error('w'); } catch (e) { log.push(e.message); } return log; })].join(\"\\n\");",
       "finally once 1\nfinally once break 1\nno stale handler \"outer\"\nno stale continue \"outer\"\ncatch break finally [\"f\",\"z\"]\nlabeled block [\"f\",\"w\"]");
    eq_s("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('values', () => { const it = [1, 2].values(); return [it.next(), it.next(), it.next(), it.next()]; }), t('keys entries', () => [[...['a', 'b'].keys()], [...['a', 'b'].entries()]]), t('live', () => { const a = [1]; const out = []; for (const x of a.values()) { out.push(x); if (a.length < 3) a.push(x + 1); } return out; }), t('done stays', () => { const a = [1]; const it = a.values(); it.next(); it.next(); a.push(2); return it.next(); }), t('array like', () => [[...Array.prototype.values.call({ length: 2, 0: 'x', 1: 'y' })], [...Array.prototype.keys.call('abc')], [...Array.prototype.entries.call(5)]]), t('nullish this', () => Array.prototype.values.call(null))].join(\"\\n\");",
       "values [{\"value\":1,\"done\":false},{\"value\":2,\"done\":false},{\"done\":true},{\"done\":true}]\nkeys entries [[0,1],[[0,\"a\"],[1,\"b\"]]]\nlive [1,2,3]\ndone stays {\"done\":true}\narray like [[\"x\",\"y\"],[0,1,2],[]]\nnullish this TypeError: Cannot convert undefined or null to object");
    eq_s("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('map lazy', () => { const log = []; const h = ci(3, log).map((x, i) => { log.push('m' + x + i); return x * 2; }); log.push('made'); const a = h.next(); return [a, log]; }), t('filter', () => [...[1, 2, 3, 4].values().filter((x, i) => x % 2 === 0)]), t('take', () => { const log = []; return [ci(9, log).take(2).toArray(), log]; }), t('take 0', () => { const log = []; return [ci(9, log).take(0).toArray(), log]; }), t('drop', () => { const log = []; return [ci(4, log).drop(2).toArray(), log]; }), t('drop more', () => [...[1, 2].values().drop(5)])].join(\"\\n\");",
       "map lazy [{\"value\":0,\"done\":false},[\"made\",\"n0\",\"m00\"]]\nfilter [2,4]\ntake [[0,1],[\"n0\",\"n1\",\"r\"]]\ntake 0 [[],[\"r\"]]\ndrop [[2,3],[\"n0\",\"n1\",\"n2\",\"n3\",\"n4\"]]\ndrop more []");
    eq_s("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('flatMap', () => [...[1, 2].values().flatMap((x) => [x, x * 10])]), t('flatMap set map', () => [...[1].values().flatMap(() => new Set(['a', 'b'])), ...[1].values().flatMap(() => new Map([[1, 2]]))]), t('flatMap iter', () => [...[1, 2].values().flatMap((x) => ci(x, []))]), t('flatMap string', () => [...[1].values().flatMap(() => 'ab')]), t('concat', () => [...Iterator.concat([1], new Set([2]), ci(2, []))]), t('concat empty', () => [Iterator.concat().next(), String(Iterator.concat())])].join(\"\\n\");",
       "flatMap [1,10,2,20]\nflatMap set map [\"a\",\"b\",[1,2]]\nflatMap iter [0,0,1]\nflatMap string TypeError: Iterator.prototype.flatMap called on non-object\nconcat [1,2,0,1]\nconcat empty [{\"done\":true},\"[object Iterator Helper]\"]");
    eq_s("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('return start', () => { const log = []; const h = ci(3, log).map((x) => x); return [h.return(), h.next(), log]; }), t('return yield', () => { const log = []; const h = ci(3, log).filter(() => true); h.next(); return [h.return(), h.next(), log]; }), t('return done', () => { const log = []; const h = ci(1, log).map((x) => x); h.next(); h.next(); return [h.return(), log]; }), t('callback throws', () => { const log = []; const h = ci(3, log).map(() => { throw new Error('cb'); }); try { h.next(); } catch (e) { log.push(e.message); } return [h.next(), log]; }), t('for of helper break', () => { const log = []; for (const x of ci(5, log).map((v) => v)) break; return log; }), t('reentrant', () => { let h; h = [1, 2].values().map(() => h.next()); try { h.next(); } catch (e) { return [e.message, h.next()]; } })].join(\"\\n\");",
       "return start [{\"done\":true},{\"done\":true},[\"r\"]]\nreturn yield [{\"done\":true},{\"done\":true},[\"n0\",\"r\"]]\nreturn done [{\"done\":true},[\"n0\",\"n1\"]]\ncallback throws [{\"done\":true},[\"n0\",\"r\",\"cb\"]]\nfor of helper break [\"n0\",\"r\"]\nreentrant [\"Generator is already running\",{\"done\":true}]");
    eq_s("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('reduce', () => [[1, 2, 3].values().reduce((a, b) => a + b), [1, 2].values().reduce((a, b, i) => a + b + i, 10)]), t('toArray', () => [new Map([[1, 2]]).entries().toArray(), 'ab'.matchAll(/./g).toArray().map((m) => m[0])]), t('forEach', () => { const out = []; return [[5, 6].values().forEach((v, i) => out.push(v + ':' + i)), out]; }), t('some every find', () => { const log = []; return [ci(5, log).some((x) => x === 1), ci(5, log).every((x) => x < 1), ci(5, log).find((x) => x === 2), [1].values().find((x) => x > 5), log]; }), t('some false', () => { const log = []; return [ci(2, log).some((x) => false), log]; }), t('callback this', () => { let th = 'unset'; [1].values().forEach(function () { th = this; }); return th; })].join(\"\\n\");",
       "reduce [6,14]\ntoArray [[[1,2]],[\"a\",\"b\"]]\nforEach [null,[\"5:0\",\"6:1\"]]\nsome every find [true,false,2,null,[\"n0\",\"n1\",\"r\",\"n0\",\"n1\",\"r\",\"n0\",\"n1\",\"n2\",\"r\"]]\nsome false [false,[\"n0\",\"n1\",\"n2\"]]\ncallback this undefined");
    eq_s("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('basics', () => [typeof Iterator, Iterator.prototype.constructor === Iterator, Object.getPrototypeOf(Object.getPrototypeOf([].values())) === Iterator.prototype, [].values() instanceof Iterator, new Map().keys() instanceof Iterator, Object.keys(Iterator.prototype)]), t('names', () => [Object.getOwnPropertyNames(Iterator.prototype).sort(), Object.getOwnPropertyNames(Object.getPrototypeOf([].values().map((x) => x))).sort(), Iterator.name, Iterator.from.name]), t('from', () => { const a = [1, 2].values(); const c = ci(1, []); return [Iterator.from(a) === a, Iterator.from(c) === c, String(Iterator.from([1])), [...Iterator.from(new Map([[1, 2]]))], [...Iterator.from(new Set([3]))]]; }), t('from wrap', () => { const o = { i: 0, next() { return this.i < 2 ? { value: this.i++, done: false } : { done: true }; } }; const w = Iterator.from(o); return [w === o, w instanceof Iterator, w.map((x) => x * 3).toArray(), String(w)]; }), t('from wrap forwards', () => { const o = { next() { return 7; } }; const w = Iterator.from(o); return [w.next(), w.next()]; }), t('tags', () => [String(Iterator.prototype), String(Object.create(Iterator.prototype)), String([].values()), String([].values().take(1)), Object.prototype.toString.call(Iterator.from('a'))])].join(\"\\n\");",
       "basics [\"function\",true,true,true,true,[]]\nnames [[\"constructor\",\"drop\",\"every\",\"filter\",\"find\",\"flatMap\",\"forEach\",\"map\",\"reduce\",\"some\",\"take\",\"toArray\"],[\"next\",\"return\"],\"Iterator\",\"from\"]\nfrom [true,true,\"[object Array Iterator]\",[[1,2]],[3]]\nfrom wrap [false,true,[0,3],\"[object Iterator]\"]\nfrom wrap forwards [7,7]\ntags [\"[object Iterator]\",\"[object Iterator]\",\"[object Array Iterator]\",\"[object Iterator Helper]\",\"[object String Iterator]\"]");
    eq_s("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('patched next', () => { const it = new Set([1, 2]).values(); it.next = () => ({ done: true }); return [...it]; }), t('patched proto next', () => { const P = Object.getPrototypeOf(new Map().keys()); const orig = P.next; P.next = function () { return { done: true }; }; const r = [...new Map([[1, 2]])]; P.next = orig; return r; }), t('Map Set new', () => { const out = []; try { Map(); } catch (e) { out.push(e.message); } try { Set(); } catch (e) { out.push(e.message); } return out; }), t('Iterator calls', () => { const out = []; try { Iterator(); } catch (e) { out.push(e.message); } try { new Iterator(); } catch (e) { out.push(e.message); } return out; }), t('array values chain', () => [1, 2, 3, 4, 5].values().filter((x) => x % 2).map((x) => x * x).take(2).toArray()), t('generic this', () => Iterator.prototype.toArray.call({ i: 0, next() { return this.i < 2 ? { value: this.i++, done: false } : { done: true }; } }))].join(\"\\n\");",
       "patched next []\npatched proto next []\nMap Set new [\"Constructor Map requires 'new'\",\"Constructor Set requires 'new'\"]\nIterator calls [\"Constructor Iterator requires 'new'\",\"Abstract class Iterator not directly constructable\"]\narray values chain [1,9]\ngeneric this [0,1]");
    eq_s("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('array member', () => { const log = []; const o = () => { log.push('t'); return {}; }; const k = () => { log.push('k'); return 'p'; }; [o().x, o()[k()]] = ci(5, log); return log; }), t('array default', () => { const log = []; const tgt = {}; const o = () => { log.push('t'); return tgt; }; [o().x = (log.push('d'), 9)] = ci(0, log); return [log, tgt.x]; }), t('array rest member', () => { const log = []; const tgt = {}; const o = () => { log.push('t'); return tgt; }; [, ...o().r] = ci(3, log); return [log, tgt.r]; }), t('object default', () => { const log = []; const tgt = {}; const o = () => { log.push('t'); return tgt; }; ({ a: o().x = (log.push('d'), 1), [(log.push('k'), 'b')]: o()['y'] } = { b: 2 }); return [log, tgt]; }), t('target throws closes', () => { const log = []; try { [(() => { throw new Error('t'); })().x] = ci(3, log); } catch (e) { log.push(e.message); } return log; }), t('mixed', () => { const log = []; let a; const o = {}; [a, o.b, [o.c] = [7]] = ci(2, log); return [a, o, log]; })].join(\"\\n\");",
       "array member [\"t\",\"n0\",\"t\",\"k\",\"n1\",\"r\"]\narray default [[\"t\",\"n0\",\"d\"],9]\narray rest member [[\"n0\",\"t\",\"n1\",\"n2\",\"n3\"],[1,2]]\nobject default [[\"t\",\"d\",\"k\",\"t\"],{\"x\":1,\"y\":2}]\ntarget throws closes [\"r\",\"t\"]\nmixed [0,{\"b\":1,\"c\":7},[\"n0\",\"n1\",\"n2\"]]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('map nonobj', () => Iterator.prototype.map.call(5, x => x)), t('map nonfn', () => [1].values().map(5)), t('map nonfn closes', () => { let c = 0; const it = Object.create(Iterator.prototype); it.next = () => ({done:true}); it.return = () => { c++; return {}; }; try { it.map(5); } catch (e) { return [c, e.message]; } }), t('filter nonfn', () => [1].values().filter({})), t('take nan', () => [1].values().take(NaN)), t('take neg', () => [1].values().take(-1))].join(\"\\n\");",
       "map nonobj TypeError: Iterator.prototype.map called on non-object\nmap nonfn TypeError: number 5 is not a function\nmap nonfn closes [1,\"number 5 is not a function\"]\nfilter nonfn TypeError: object is not a function\ntake nan RangeError: NaN must be positive\ntake neg RangeError: -1 must be positive");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('take str', () => [1,2,3].values().take('2').toArray()), t('take undefined', () => [1].values().take()), t('drop neg', () => [1].values().drop(-1)), t('drop inf', () => [1,2].values().drop(Infinity).toArray()), t('reduce empty', () => [].values().reduce((a, b) => a + b)), t('reduce nonfn', () => [].values().reduce(1))].join(\"\\n\");",
       "take str [1,2]\ntake undefined RangeError: undefined must be positive\ndrop neg RangeError: -1 must be positive\ndrop inf []\nreduce empty TypeError: Reduce of a done iterator with no initial value\nreduce nonfn TypeError: number 1 is not a function");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('forEach nonfn', () => [].values().forEach(null)), t('some nonfn', () => [].values().some()), t('every nonfn', () => [].values().every()), t('find nonfn', () => [].values().find()), t('flatMap nonfn', () => [].values().flatMap()), t('flatMap str', () => [1].values().flatMap(x => 'ab').toArray())].join(\"\\n\");",
       "forEach nonfn TypeError: object null is not a function\nsome nonfn TypeError: undefined is not a function\nevery nonfn TypeError: undefined is not a function\nfind nonfn TypeError: undefined is not a function\nflatMap nonfn TypeError: Iterator.prototype.flatMap is not a function\nflatMap str TypeError: Iterator.prototype.flatMap called on non-object");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('flatMap num', () => [1].values().flatMap(x => 5).toArray()), t('flatMap obj', () => [1].values().flatMap(x => ({})).toArray()), t('flatMap objnext', () => [1].values().flatMap(x => ({ i: 0, next() { return this.i++ < 2 ? { value: this.i, done: false } : { done: true }; } })).toArray()), t('nonobj result', () => { const it = Object.create(Iterator.prototype); it.next = () => 5; return it.toArray(); }), t('nonobj result for', () => { const it = Object.create(Iterator.prototype); it.next = () => 5; for (const x of it); }), t('next not fn', () => { const it = Object.create(Iterator.prototype); it.next = 5; return it.toArray(); })].join(\"\\n\");",
       "flatMap num TypeError: Iterator.prototype.flatMap called on non-object\nflatMap obj TypeError: undefined is not a function\nflatMap objnext [1,2]\nnonobj result TypeError: Iterator result 5 is not an object\nnonobj result for TypeError: Iterator result 5 is not an object\nnext not fn TypeError: number 5 is not a function");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('next not fn for', () => { const it = Object.create(Iterator.prototype); it.next = 5; for (const x of it); }), t('next undef for', () => { const it = Object.create(Iterator.prototype); for (const x of it); }), t('helper next bad this', () => { const h = [1].values().map(x => x); return Object.getPrototypeOf(h).next.call({}); }), t('helper return bad this', () => { const h = [1].values().map(x => x); return Object.getPrototypeOf(h).return.call({}); }), t('running', () => { let h; h = [1].values().map(x => h.next()); return h.next(); }), t('running ret', () => { let h; h = [1].values().map(x => h.return()); return h.next(); })].join(\"\\n\");",
       "next not fn for TypeError: number 5 is not a function\nnext undef for TypeError: undefined is not a function\nhelper next bad this TypeError: Method Iterator Helper.prototype.next called on incompatible receiver #<Object>\nhelper return bad this TypeError: Method Iterator Helper.prototype.return called on incompatible receiver #<Object>\nrunning TypeError: Generator is already running\nrunning ret TypeError: Generator is already running");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('from num', () => Iterator.from(5)), t('from obj', () => Iterator.from({}).next()), t('from null', () => Iterator.from(null)), t('from str', () => [Object.prototype.toString.call(Iterator.from('ab')), [...Iterator.from('a\\u{1F600}')]]), t('wrap bad this', () => { const w = Iterator.from({ next() { return 1; } }); return [w.next(), Object.getPrototypeOf(w).next.call({})]; }), t('wrap ret bad this', () => { const w = Iterator.from({ next() { return 1; } }); return Object.getPrototypeOf(w).return.call({}); })].join(\"\\n\");",
       "from num TypeError: Iterator.from called on non-object\nfrom obj TypeError: undefined is not a function\nfrom null TypeError: Iterator.from called on non-object\nfrom str [\"[object String Iterator]\",[\"a\",\"\360\237\230\200\"]]\nwrap bad this TypeError: Method %WrapForValidIteratorPrototype%.next called on incompatible receiver #<Object>\nwrap ret bad this TypeError: Method %WrapForValidIteratorPrototype%.return called on incompatible receiver #<Object>");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('wrap ret', () => { const w = Iterator.from({ next() { return 1; } }); return w.return(); }), t('wrap ret2', () => { const w = Iterator.from({ next() { return 1; }, return() { return 7; } }); return w.return(); }), t('concat str', () => Iterator.concat('ab')), t('concat obj', () => Iterator.concat({})), t('concat num', () => Iterator.concat(1)), t('concat', () => Iterator.concat([1], new Set([2]), new Map([[3, 4]]), [5].values()).toArray())].join(\"\\n\");",
       "wrap ret {\"done\":true}\nwrap ret2 7\nconcat str TypeError: Iterator.concat called on non-object\nconcat obj TypeError: #<Object> is not iterable\nconcat num TypeError: Iterator.concat called on non-object\nconcat [1,2,[3,4],5]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('ctor', () => Iterator()), t('new ctor', () => new Iterator()), t('closes ret nonobj', () => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = () => 5; for (const x of it) break; }), t('closes ret throws', () => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = () => { throw new Error('r'); }; for (const x of it) break; }), t('closes ret notfn', () => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = 5; for (const x of it) break; }), t('closes throw keeps', () => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = () => { throw new Error('r'); }; for (const x of it) throw new Error('body'); })].join(\"\\n\");",
       "ctor TypeError: Constructor Iterator requires 'new'\nnew ctor TypeError: Abstract class Iterator not directly constructable\ncloses ret nonobj TypeError: Iterator result 5 is not an object\ncloses ret throws Error: r\ncloses ret notfn TypeError: number 5 is not a function\ncloses throw keeps Error: body");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t('lengths', () => ['map','filter','take','drop','flatMap','reduce','toArray','forEach','some','every','find'].map(n => n + Iterator.prototype[n].length).concat([Iterator.length, Iterator.from.length, Iterator.concat.length])), t('array values', () => [Object.prototype.toString.call([].values()), [].values().next.name, [].keys.length, Object.getOwnPropertyNames(Object.getPrototypeOf([].values()))]), t('arr alike', () => [...Array.prototype.values.call({ length: 2, 0: 'a', 1: 'b' }), ...Array.prototype.entries.call('xy')]), t('arr next bad', () => [].values().next.call(new Map().keys())), t('str iter next bad', () => Iterator.from('a').next.call([].values())), t('helper tag', () => [String([].values().map(x => x)), String(Iterator.from({next(){}})), String(Iterator.prototype)])].join(\"\\n\");",
       "lengths [\"map1\",\"filter1\",\"take1\",\"drop1\",\"flatMap1\",\"reduce1\",\"toArray0\",\"forEach1\",\"some1\",\"every1\",\"find1\",0,1,0]\narray values [\"[object Array Iterator]\",\"next\",0,[\"next\"]]\narr alike [\"a\",\"b\",[0,\"x\"],[1,\"y\"]]\narr next bad TypeError: Method Array Iterator.prototype.next called on incompatible receiver [object Map Iterator]\nstr iter next bad TypeError: Method String Iterator.prototype.next called on incompatible receiver [object Array Iterator]\nhelper tag [\"[object Iterator Helper]\",\"[object Iterator]\",\"[object Iterator]\"]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t(\"some\", () => mk(()=>5).some(x=>true)), t(\"every\", () => mk(()=>5).every(x=>false)), t(\"find\", () => mk(()=>5).find(x=>true)), t(\"take0\", () => mk(()=>5).take(0).next()), t(\"take1 then\", () => { const h = mk(()=>5).take(1); h.next(); return h.next(); }), t(\"helper return start\", () => mk(()=>5).map(x=>x).return())].join(\"\\n\");",
       "some TypeError: return called on non-object\nevery TypeError: return called on non-object\nfind TypeError: return called on non-object\ntake0 TypeError: return called on non-object\ntake1 then TypeError: return called on non-object\nhelper return start TypeError: return called on non-object");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t(\"helper return yield\", () => { const h = mk(()=>5).map(x=>x); h.next(); return h.return(); }), t(\"flatMap ret inner\", () => { const h = mk(()=>({})).flatMap(x=>mk(()=>5)); h.next(); return h.return(); }), t(\"destr\", () => { const [a] = mk(()=>5); }), t(\"for break\", () => { for (const x of mk(()=>5)) break; }), t(\"for return\", () => { for (const x of mk(()=>5)) return 1; }), t(\"concat ret\", () => { const h = Iterator.concat(mk(()=>5)); h.next(); return h.return(); })].join(\"\\n\");",
       "helper return yield TypeError: return called on non-object\nflatMap ret inner TypeError: return called on non-object\ndestr TypeError: Iterator result 5 is not an object\nfor break TypeError: Iterator result 5 is not an object\nfor return TypeError: Iterator result 5 is not an object\nconcat ret TypeError: return called on non-object");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t(\"some notfn\", () => mk(5).some(x=>true)), t(\"take0 notfn\", () => mk(5).take(0).next()), t(\"destr notfn\", () => { const [a] = mk(5); }), t(\"wrap ret notfn\", () => Iterator.from({next(){}, return: 5}).return()), t(\"arrfrom\", () => Array.from(mk(()=>5), x => { throw new Error(\"m\"); })), t(\"map ctor\", () => new Map(mk(()=>5)))].join(\"\\n\");",
       "some notfn TypeError: number 5 is not a function\ntake0 notfn TypeError: number 5 is not a function\ndestr notfn TypeError: number 5 is not a function\nwrap ret notfn TypeError: '5' returned for property 'return' of object '#<Object>' is not a function\narrfrom Error: m\nmap ctor TypeError: Iterator value 1 is not an entry object");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t(\"map ctor2\", () => { const it = Object.create(Iterator.prototype); it.next=()=>({done:false,value:5}); it.return=()=>{ throw new Error(\"ignored\"); }; return new Map(it); }), t(\"fromEntries\", () => { const it = Object.create(Iterator.prototype); it.next=()=>({done:false,value:5}); let c=0; it.return=()=>{ c++; return {}; }; try { Object.fromEntries(it); } catch(e) { return [e.message, c]; } }), t(\"helper\", () => Map.prototype.get.call([].values().map(x=>x))), t(\"wrap\", () => Map.prototype.get.call(Iterator.from({next(){}}))), t(\"custom\", () => { const o = Object.create(Iterator.prototype); return Map.prototype.get.call(o); }), t(\"str\", () => Map.prototype.get.call(Iterator.from(\"a\")))].join(\"\\n\");",
       "map ctor2 TypeError: Iterator value 5 is not an entry object\nfromEntries [\"Iterator value 5 is not an entry object\",1]\nhelper TypeError: Method Map.prototype.get called on incompatible receiver [object Iterator Helper]\nwrap TypeError: Method Map.prototype.get called on incompatible receiver [object Object]\ncustom TypeError: Method Map.prototype.get called on incompatible receiver [object Object]\nstr TypeError: Method Map.prototype.get called on incompatible receiver [object String Iterator]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t(\"take x\", () => [].values().take(\"x\")), t(\"take obj\", () => [].values().take({})), t(\"take -0.5\", () => [1].values().take(-0.5).toArray()), t(\"take -inf\", () => [].values().take(-Infinity)), t(\"drop nan\", () => [].values().drop(NaN)), t(\"take sym\", () => [].values().take({valueOf(){ throw new Error(\"vo\"); }}))].join(\"\\n\");",
       "take x RangeError: x must be positive\ntake obj RangeError: #<Object> must be positive\ntake -0.5 []\ntake -inf RangeError: -Infinity must be positive\ndrop nan RangeError: NaN must be positive\ntake sym Error: vo");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t(\"ret check\", () => { const it = Object.create(Iterator.prototype); it.next=()=>({done:false,value:1}); it.return=()=>{ throw new Error(\"r\"); }; return it.take(0).next(); }), t(\"concat lazy\", () => { const a=[1]; const c=Iterator.concat(a); a.push(2); return c.toArray(); }), t(\"flatMap close\", () => { const log=[]; const inner={next(){return {done:false,value:1}}, return(){log.push(\"inner\");return {}}}; const outer=Object.create(Iterator.prototype); outer.next=()=>({done:false,value:0}); outer.return=()=>{log.push(\"outer\");return {}}; const h=outer.flatMap(()=>inner); h.next(); h.return(); return log; }), t(\"destr close\", () => { const log=[]; const it=Object.create(Iterator.prototype); let i=0; it.next=()=>({done:false,value:i++}); it.return=()=>{log.push(\"ret\"); return {};}; const [a,b]=it; return [a,b,log]; }), t(\"destr done\", () => { const log=[]; const it=Object.create(Iterator.prototype); let i=0; it.next=()=>(i<1?{done:false,value:i++}:{done:true}); it.return=()=>{log.push(\"ret\"); return {};}; const [a,b]=it; return [a,b,log]; }), t(\"destr rest\", () => { const log=[]; const it=Object.create(Iterator.prototype); let i=0; it.next=()=>(i<3?{done:false,value:i++}:{done:true}); it.return=()=>{log.push(\"ret\"); return {};}; const [a,...b]=it; return [a,b,log]; })].join(\"\\n\");",
       "ret check Error: r\nconcat lazy [1,2]\nflatMap close [\"inner\",\"outer\"]\ndestr close [0,1,[\"ret\"]]\ndestr done [0,null,[]]\ndestr rest [0,[1,2],[]]");
    eq("const t = (l, f) => { let s; try { s = JSON.stringify(f()); } catch (e) { s = e.constructor.name + ': ' + e.message; } return l + ' ' + s; }; const ci = (n, log, ret) => { const it = Object.create(Iterator.prototype); let i = 0; it.next = () => { log.push('n' + i); return i < n ? { value: i++, done: false } : { value: 'end', done: true }; }; if (ret !== false) it.return = () => { log.push('r'); return {}; }; return it; }; const mk = (r) => { const it = Object.create(Iterator.prototype); it.next = () => ({ done: false, value: 1 }); it.return = r; return it; }; [t(\"destr throw\", () => { const log=[]; const it=Object.create(Iterator.prototype); let i=0; it.next=()=>({done:false,value:undefined}); it.return=()=>{log.push(\"ret\"); throw new Error(\"ignored\");}; try { const [a=(()=>{throw new Error(\"d\")})()]=it; } catch(e) { return [e.message, log]; } }), t(\"Map()\", () => Map()), t(\"Set()\", () => Set())].join(\"\\n\");",
       "destr throw [\"d\",[\"ret\"]]\nMap() TypeError: Constructor Map requires 'new'\nSet() TypeError: Constructor Set requires 'new'");
}


/* Array.fromAsync, and promises adopting thenables (Promise.resolve, resolve functions, await, async
 * returns) with resolving functions that act once; expected values from Node */
static void test_from_async(void) {
    eq_s("const out = []; const log = []; const ITER = (vals, tag) => { let i = 0; const it = Object.create(Iterator.prototype); it.next = () => i < vals.length ? { value: vals[i++], done: false } : { value: undefined, done: true }; it.return = () => { log.push('return ' + tag); return {}; }; return it; }; const cases = [ ['arr', () => Array.fromAsync([1, Promise.resolve(2), 3])], ['map', () => Array.fromAsync([1, 2], (x, i) => Promise.resolve(x * 10 + i))], ['set', () => Array.fromAsync(new Set([1, 2]))], ['arraylike', () => Array.fromAsync({ length: 2, 0: 'a', 1: Promise.resolve('b') })], ['string', () => Array.fromAsync('a\\u{1F600}')], ['map throws', () => Array.fromAsync(ITER([1, 2], 'mt'), () => { throw new Error('m'); })], ['map rejects', () => Array.fromAsync(ITER([1, 2], 'mr'), () => Promise.reject(new Error('mr')))], ['bad map', () => Array.fromAsync([1], 's')], ['null', () => Array.fromAsync(null)], ['num', () => Array.fromAsync(5)], ['this', () => Array.fromAsync([1], function (x) { return this.k + x; }, { k: 10 })], ['thenable', () => Array.fromAsync([{ then(r) { r(7); } }])], ['arraylike reject', () => Array.fromAsync({ length: 1, 0: Promise.reject(new Error('al')) })], ['long', () => Array.fromAsync(Array.from({ length: 300 }, (_, i) => Promise.resolve(i))).then((a) => a.length + a[299])], ['thenables', () => Promise.all([Promise.resolve({ then(r) { r(1); } }), (async () => ({ then(r) { r(2); } }))(), new Promise((res) => { res(new Promise(() => {})); res(5); }).then(() => 'x', () => 'y') === undefined])], ['first wins', () => new Promise((res, rej) => { rej(new Error('r')); res(1); }).catch((e) => e.message)], ['then throws', () => Promise.resolve({ then() { throw new Error('t'); } }).catch((e) => e.message)], ['await thenable', async () => await { then(r) { r(9); } }], ['log', () => log], ]; for (const [s, f] of cases) { try { out.push(s + ' => ' + JSON.stringify(await f())); } catch (e) { out.push(s + ' => ' + e.constructor.name + ': ' + e.message); } } out.push([typeof Array.fromAsync, Array.fromAsync.length].join()); out.join('\\n');",
       "arr => [1,2,3]\nmap => [10,21]\nset => [1,2]\narraylike => [\"a\",\"b\"]\nstring => [\"a\",\"\xf0\x9f\x98\x80""\"]\nmap throws => Error: m\nmap rejects => Error: mr\nbad map => TypeError: s is not a function\nnull => TypeError: Cannot read properties of null (reading 'Symbol(Symbol.asyncIterator)')\nnum => []\nthis => [11]\nthenable => [7]\narraylike reject => Error: al\nlong => 599\nthenables => [1,2,false]\nfirst wins => \"r\"\nthen throws => \"t\"\nawait thenable => 9\nlog => [\"return mt\",\"return mr\"]\nfunction,1");
}

/* RegExp's legacy statics and compile; how a message names an object (#<Set>, [object Date],
 * "Error: q"); expected values from Node */
static void test_regexp_legacy(void) {
    eq_s("\"use strict\"; const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; t('initial', () => [RegExp.$1, RegExp.lastMatch, RegExp.input, RegExp.leftContext]); t('names', () => Object.getOwnPropertyNames(RegExp)); t('exec', () => { /(a)(b)?c/.exec(\"xxabcyy\"); return [RegExp.$1, RegExp.$2, RegExp.$3, RegExp.lastMatch, RegExp[\"$&\"], RegExp.lastParen, RegExp[\"$+\"], RegExp.leftContext, RegExp[\"$`\"], RegExp.rightContext, RegExp[\"$'\"], RegExp.input, RegExp.$_]; }); t('fail keeps', () => { /(q)/.exec(\"zz\"); return RegExp.$1; }); t('replace g', () => { \"q1w2\".replace(/(\\d)/g, \"x\"); return [RegExp.$1, RegExp.lastMatch, RegExp.input]; }); t('match', () => { \"abc\".match(/(b)/); return [RegExp.$1]; }); t('test', () => { /x/.test(\"axb\"); return [RegExp.$1, RegExp.lastMatch, RegExp.leftContext]; }); t('set input', () => { RegExp.input = 5; return [RegExp.input, RegExp.$_, typeof RegExp.$_]; }); t('set $1', () => { RegExp.$1 = \"z\"; return RegExp.$1; }); t('set lastMatch', () => { RegExp.lastMatch = \"z\"; return RegExp.lastMatch; }); t('unicode', () => { /(\\u{1F600})/u.exec(\"a\\u{1F600}\"); return [RegExp.$1.length, RegExp.leftContext]; }); t('split', () => { \"a-b\".split(/(-)/); return [RegExp.$1, RegExp.input, RegExp.lastMatch]; }); t('search', () => { \"abcabc\".search(/c/); return [RegExp.lastMatch, RegExp.rightContext]; }); t('matchAll', () => { [...\"a1b2\".matchAll(/\\d/g)]; return [RegExp.lastMatch]; }); t('alt', () => { /(a)|(b)/.exec(\"b\"); return [RegExp.$1, RegExp.$2, RegExp.lastParen]; }); t('eleven', () => { new RegExp(\"(1)(2)(3)(4)(5)(6)(7)(8)(9)(10)(11)\").exec(\"1234567891011\"); return [RegExp.$9, RegExp.lastParen]; }); t('no groups', () => { /a/.exec(\"a\"); return [RegExp.$1, RegExp.lastParen]; }); t('hasOwn', () => [Object.hasOwn(RegExp, \"$1\"), Object.hasOwn(RegExp, \"lastMatch\"), \"$1\" in RegExp, RegExp.propertyIsEnumerable(\"$1\")]); t('desc', () => Object.getOwnPropertyDescriptor(RegExp, \"$1\").enumerable); t('keys', () => Object.keys(RegExp)); t('compile', () => { const r = /a/g; r.lastIndex = 3; const s = r.compile(\"b\", \"i\"); return [s === r, r.source, r.flags, r.lastIndex, r.test(\"B\"), String(r)]; }); t('compile re', () => { const r = /a/g; return r.compile(/c/m).flags; }); t('compile re flags', () => /a/.compile(/c/, \"g\")); t('compile syntax', () => /a/.compile(\"(\", \"g\")); t('compile none', () => String(/a/.compile())); t('compile recv', () => RegExp.prototype.compile.call({}, \"a\")); t('compile len', () => RegExp.prototype.compile.length); t('compile frozen', () => Object.freeze(/a/).compile(\"b\")); t('compile bad flags', () => /a/.compile(\"a\", \"gg\")); t('delete', () => { const r = delete RegExp.$2; return [r, RegExp.$2, Object.hasOwn(RegExp, \"$2\"), Object.getOwnPropertyNames(RegExp).includes(\"$2\")]; }); t('define', () => { Object.defineProperty(RegExp, \"$3\", { value: 7 }); /(x)(y)(z)/.exec(\"xyz\"); return [RegExp.$3, JSON.stringify(Object.getOwnPropertyDescriptor(RegExp, \"$3\"))]; }); out.join(\"\\n\");",
       "initial => [\"\",\"\",\"\",\"\"]\nnames => [\"length\",\"name\",\"prototype\",\"input\",\"$_\",\"lastMatch\",\"$&\",\"lastParen\",\"$+\",\"leftContext\",\"$`\",\"rightContext\",\"$'\",\"$1\",\"$2\",\"$3\",\"$4\",\"$5\",\"$6\",\"$7\",\"$8\",\"$9\",\"escape\"]\nexec => [\"a\",\"b\",\"\",\"abc\",\"abc\",\"b\",\"b\",\"xx\",\"xx\",\"yy\",\"yy\",\"xxabcyy\",\"xxabcyy\"]\nfail keeps => \"a\"\nreplace g => [\"2\",\"2\",\"q1w2\"]\nmatch => [\"b\"]\ntest => [\"\",\"x\",\"a\"]\nset input => [\"5\",\"5\",\"string\"]\nset $1 => \"\"\nset lastMatch => \"x\"\nunicode => [2,\"a\"]\nsplit => [\"-\",\"a-b\",\"-\"]\nsearch => [\"c\",\"abc\"]\nmatchAll => [\"2\"]\nalt => [\"\",\"b\",\"b\"]\neleven => [\"9\",\"11\"]\nno groups => [\"\",\"\"]\nhasOwn => [true,true,true,false]\ndesc => false\nkeys => []\ncompile => [true,\"b\",\"i\",0,true,\"/b/i\"]\ncompile re => \"m\"\ncompile re flags => TypeError: Cannot supply flags when constructing one RegExp from another\ncompile syntax => SyntaxError: Invalid regular expression: /(/g: Unterminated group\ncompile none => \"/(?:)/\"\ncompile recv => TypeError: Method RegExp.prototype.compile called on incompatible receiver #<Object>\ncompile len => 2\ncompile frozen => TypeError: Cannot assign to read only property 'lastIndex' of object '[object RegExp]'\ncompile bad flags => SyntaxError: Invalid flags supplied to RegExp constructor 'gg'\ndelete => [true,null,false,false]\ndefine => [7,\"{\\\"value\\\":7,\\\"writable\\\":false,\\\"enumerable\\\":false,\\\"configurable\\\":true}\"]");
    eq_s("\"use strict\"; const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; const m = (o) => { o.x = 1; Object.freeze(o); try { o.x = 2; } catch (e) { out.push(e.message); } }; m(new Date(0)); m(new Date(NaN)); m(/a/g); m(new Error(\"q\")); m(new TypeError(\"t\")); m(new Set()); m(new WeakMap()); m(Object.create(null)); m([1]); m(Promise.resolve()); m(function f() {}); const e = new Error(\"z\"); e.name = \"N\"; m(e); m(Object.create(Error.prototype)); m(new Map().keys()); function Q() {} m(new Q()); const e2 = new Error(\"m2\"); e2.name = \"\"; m(e2); const e3 = new Error(\"\"); m(e3); out.join(\"\\n\");",
       "Cannot assign to read only property 'x' of object '[object Date]'\nCannot assign to read only property 'x' of object '[object Date]'\nCannot assign to read only property 'x' of object '[object RegExp]'\nCannot assign to read only property 'x' of object 'Error: q'\nCannot assign to read only property 'x' of object 'TypeError: t'\nCannot assign to read only property 'x' of object '#<Set>'\nCannot assign to read only property 'x' of object '#<WeakMap>'\nCannot assign to read only property 'x' of object '[object Object]'\nCannot assign to read only property 'x' of object '[object Array]'\nCannot assign to read only property 'x' of object '#<Promise>'\nCannot assign to read only property 'x' of function 'function f() {}'\nCannot assign to read only property 'x' of object 'N: z'\nCannot assign to read only property 'x' of object 'Error'\nCannot assign to read only property 'x' of object '[object Map Iterator]'\nCannot assign to read only property 'x' of object '#<Q>'\nCannot assign to read only property 'x' of object 'm2'\nCannot assign to read only property 'x' of object 'Error'");
}

/* Numeric separators, `debugger` (a no-op), unicode escapes in identifiers */
static void test_lexical(void) {
    /* tagged templates (strings per site, raw, undefined for an escape that does not cook); Node */
    eq_s("\"use strict\"; const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; const tag = (s, ...v) => [s, s.raw, v]; t('basic', () => tag`a${1}b${2}c`); t('empty', () => tag``); t('only sub', () => tag`${'x'}`); t('raw escapes', () => { const r = tag`x\\nyA`; return [r[0][0], r[1][0]]; }); t('bad escape', () => { const r = tag`\\unicode and \\x and \\01`; return [r[0][0], r[1][0]]; }); t('bad mid', () => { const r = tag`ok${1}\\xg${2}fine`; return r[0].concat(r[1]); }); t('frozen', () => { const r = tag`a`; return [Object.isFrozen(r[0]), Object.isFrozen(r[1]), Object.keys(r[0]), JSON.stringify(Object.getOwnPropertyDescriptor(r[0], 'raw').enumerable)]; }); t('same site', () => { const f = () => tag`q`[0]; return f() === f(); }); t('diff site', () => tag`q`[0] === tag`q`[0]); t('member this', () => { const o = { k: 5, m(s) { return this.k + s[0]; } }; return o.m`x`; }); t('computed member', () => { const o = { k: 6, m(s) { return this.k + s.length; } }; return o['m']`a${1}b`; }); t('String.raw', () => String.raw`a\\nb${1}A`); t('chain', () => { const f = (s) => (s2) => s[0] + s2[0]; return f`a``b`; }); t('crlf', () => { const r = tag`a b`; return [r[0][0], r[1][0]]; }); t('new', () => { function F(s) { this.s = s[0]; } return new F`x`.s; }); t('result', () => (s => s.length)`a${1}b${2}c`); t('not a function', () => { const x = 1; return x`a`; }); out.join(\"\\n\");",
       "basic => [[\"a\",\"b\",\"c\"],[\"a\",\"b\",\"c\"],[1,2]]\nempty => [[\"\"],[\"\"],[]]\nonly sub => [[\"\",\"\"],[\"\",\"\"],[\"x\"]]\nraw escapes => [\"x\\nyA\",\"x\\\\nyA\"]\nbad escape => [null,\"\\\\unicode and \\\\x and \\\\01\"]\nbad mid => [\"ok\",null,\"fine\",\"ok\",\"\\\\xg\",\"fine\"]\nfrozen => [true,true,[\"0\"],\"false\"]\nsame site => true\ndiff site => false\nmember this => \"5x\"\ncomputed member => 8\nString.raw => \"a\\\\nb1A\"\nchain => \"ab\"\ncrlf => [\"a b\",\"a b\"]\nnew => TypeError: Cannot set properties of undefined (setting 's')\nresult => 3\nnot a function => TypeError: x is not a function");
    /* object rest in destructuring, and destructuring null with no property to name; from Node */
    eq_s("\"use strict\"; const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; t('basic', () => { const { a, ...rest } = { a: 1, b: 2, c: 3 }; return [a, rest]; }); t('only rest', () => { const { ...r } = { x: 1, y: [2] }; return r; }); t('computed', () => { const k = 'b'; const { [k]: v, ...r } = { a: 1, b: 2, c: 3 }; return [v, r]; }); t('number key', () => { const { 0: z, ...r } = { 0: 'a', 1: 'b', q: 3 }; return [z, r]; }); t('array src', () => { const { length, ...r } = ['x', 'y']; return [length, r]; }); t('string src', () => { const { 0: f, ...r } = 'abc'; return [f, r]; }); t('null', () => { const { ...r } = null; return r; }); t('undefined', () => { const { a, ...r } = undefined; return r; }); t('null computed', () => { const { ['a']: a, ...r } = null; return r; }); t('assign', () => { let a, r; ({ a, ...r } = { a: 1, b: 2 }); return [a, r]; }); t('assign member', () => { const o = {}; ({ a: o.x, ...o.rest } = { a: 1, b: 2 }); return o; }); t('param', () => (({ a, ...r }) => [a, r])({ a: 1, b: 2, c: 3 })); t('nested', () => { const { p: { q, ...inner }, ...outer } = { p: { q: 1, w: 2 }, z: 3 }; return [q, inner, outer]; }); t('non-enumerable', () => { const o = Object.defineProperty({ a: 1 }, 'hid', { value: 2 }); const { ...r } = o; return r; }); t('proto not copied', () => { const o = Object.create({ inh: 1 }); o.own = 2; const { ...r } = o; return r; }); t('order', () => { const { b, ...r } = { 2: 'x', a: 1, b: 2, 1: 'y', c: 3 }; return Object.keys(r); }); t('key once', () => { let n = 0; const key = { toString() { n++; return 'a'; } }; const { [key]: v, ...r } = { a: 1, b: 2 }; return [v, r, n]; }); t('default', () => { const { a = 5, ...r } = { b: 1 }; return [a, r]; }); t('fresh', () => { const src = { a: 1 }; const { ...r } = src; return r !== src; }); t('number src', () => { const { ...r } = 5; return r; }); t('for of', () => { const out = []; for (const { k, ...v } of [{ k: 1, x: 2 }, { k: 3 }]) out.push([k, v]); return out; }); out.join(\"\\n\");",
       "basic => [1,{\"b\":2,\"c\":3}]\nonly rest => {\"x\":1,\"y\":[2]}\ncomputed => [2,{\"a\":1,\"c\":3}]\nnumber key => [\"a\",{\"1\":\"b\",\"q\":3}]\narray src => [2,{\"0\":\"x\",\"1\":\"y\"}]\nstring src => [\"a\",{\"1\":\"b\",\"2\":\"c\"}]\nnull => TypeError: Cannot destructure 'null' as it is null.\nundefined => TypeError: Cannot destructure property 'a' of 'undefined' as it is undefined.\nnull computed => TypeError: Cannot destructure 'null' as it is null.\nassign => [1,{\"b\":2}]\nassign member => {\"x\":1,\"rest\":{\"b\":2}}\nparam => [1,{\"b\":2,\"c\":3}]\nnested => [1,{\"w\":2},{\"z\":3}]\nnon-enumerable => {\"a\":1}\nproto not copied => {\"own\":2}\norder => [\"1\",\"2\",\"a\",\"c\"]\nkey once => [1,{\"b\":2},1]\ndefault => [5,{\"b\":1}]\nfresh => true\nnumber src => {}\nfor of => [[1,{\"x\":2}],[3,{}]]");
    eq_s("\"use strict\"; const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; t('empty', () => { const {} = null; }); t('computed', () => { const { ['a']: a } = null; }); t('computed2', () => { const k = 'a'; const { [k]: a } = undefined; }); t('named', () => { const { a } = null; }); let x; t('assign only rest', () => { ({ ...x } = null); }); out.join(\"\\n\");",
       "empty => TypeError: Cannot destructure 'null' as it is null.\ncomputed => TypeError: Cannot destructure 'null' as it is null.\ncomputed2 => TypeError: Cannot destructure 'undefined' as it is undefined.\nnamed => TypeError: Cannot destructure property 'a' of 'null' as it is null.\nassign only rest => TypeError: Cannot destructure 'null' as it is null.");
    /* new.target (direct, through arrows, guard), delete with optional chaining; expected from Node */
    eq_s("\"use strict\"; const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; function F() { return { nt: new.target === F, und: new.target === undefined }; } t('new', () => new F()); t('call', () => F()); function G() { const a = () => new.target; const b = () => () => new.target; this.r = [a() === G, b()() === G]; } t('arrow', () => new G().r); t('arrow call', () => { function H() { return (() => new.target)(); } return H() === undefined; }); t('nested fn', () => { function O() { function I() { return new.target; } return I(); } return new O() ; }); t('typeof', () => { function T() { return typeof new.target; } return [typeof new T(), T()]; }); t('method', () => ({ m() { return new.target; } }).m()); t('bound', () => { function B() { return new.target === B; } const bb = B.bind(null); return new bb() instanceof Object; }); t('in param default', () => { function P(x = 1) { return new.target === P; } return P(); }); t('async', () => { async function A() { return new.target; } return typeof A(); }); t('guard', () => { function Must() { if (!new.target) throw new TypeError('use new'); this.ok = 1; } try { Must(); } catch (e) { return [e.message, new Must().ok]; } }); out.join(\"\\n\");",
       "new => {\"nt\":true,\"und\":false}\ncall => {\"nt\":false,\"und\":true}\narrow => [true,true]\narrow call => true\nnested fn => {}\ntypeof => [\"object\",\"undefined\"]\nmethod => undefined\nbound => true\nin param default => false\nasync => \"object\"\nguard => [\"use new\",1]");
    eq_s("\"use strict\"; const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; let a = null, o = { b: { c: 1 }, x: 2 }; t('null', () => delete a?.b); t('undef', () => delete undefined?.b); t('obj', () => [delete o?.x, o]); t('deep null', () => delete a?.b.c); t('deep obj', () => [delete o?.b.c, o]); t('computed', () => { const k = 'x'; const p = { x: 1 }; return [delete p?.[k], p, delete a?.[k]]; }); t('mid', () => [delete o.b?.c, delete o.zz?.c, o]); t('frozen', () => delete Object.freeze({ q: 1 })?.q); t('in expr', () => (delete a?.b) && 1); t('side effect', () => { let n = 0; const r = delete a?.[n++]; return [r, n]; }); out.join(\"\\n\");",
       "null => true\nundef => true\nobj => [true,{\"b\":{\"c\":1}}]\ndeep null => true\ndeep obj => [true,{\"b\":{}}]\ncomputed => [true,{},true]\nmid => [true,true,{\"b\":{}}]\nfrozen => TypeError: Cannot delete property 'q' of #<Object>\nin expr => 1\nside effect => [true,0]");
    eq("[1_000, 0x1_F, 0b1_0, 0o1_7, 1_0.0_5, 1e1_0, .5_5, 9_007_199_254_740_993, 1_2_3_4_5_6_7_8_9_0_1_2_3_4_5_6_7_8_9_0_1].join();", "1000,31,2,15,10.05,10000000000,0.55,9007199254740992,123456789012345680000");
    eq("let n = 0; debugger; if (n === 0) debugger\nn = 1; function f() { debugger; return n + 1; } f();", "2");
    eq("let \\u0061 = 1; let b\\u0063 = 2; let \\u{64}e = 3; const o = { \\u0069f: 4, x\\u0079: 5 }; [a, bc, de, o.if, o.xy, o.\\u0069f, ({ k: 6 }).\\u006b].join();", "1,2,3,4,5,4,6");
    eq("let \\u{1d4b6} = 7; \\u{1d4b6};", "7");
}

/* BigInt: literals, operators (and the mixing, division and size errors), comparisons with numbers
 * and strings, conversions, BigInt(), asIntN/asUintN, ++ and --; expected values from Node. The
 * last check hashes 600 random cases of every operator, against Node's. */
static void test_bigint(void) {
    eq_s("\"use strict\"; const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; t('lit', () => [1n, 0x1Fn, 0b101n, 0o17n, 1_000n, 9007199254740993n].map(String)); t('typeof', () => typeof 1n); t('add', () => 1n + 2n); t('big mul', () => (2n ** 100n).toString()); t('neg', () => (-(2n ** 70n)).toString()); t('sub', () => 5n - 8n); t('div', () => [7n / 2n, -7n / 2n, 7n / -2n, -7n % 2n, 7n % -2n].map(String)); t('div0', () => 1n / 0n); t('mod0', () => 1n % 0n); t('pow neg', () => 2n ** -1n); t('big div', () => [(10n ** 40n) / (10n ** 20n + 7n), (10n ** 40n) % (10n ** 20n + 7n), (2n**200n - 1n) / (2n**100n + 3n)].map(String)); t('mix', () => 1n + 1); t('mix sub', () => 1n * 2); t('concat', () => 'a' + 1n + 'b'); t('unary plus', () => +1n); t('bitnot', () => [~0n, ~5n, ~-6n].map(String)); t('bitwise', () => [5n & 3n, 5n | 3n, 5n ^ 3n, -5n & 3n, -5n | 3n, -5n ^ -3n, (2n**70n) & (2n**70n - 1n), -1n & (2n ** 65n)].map(String)); t('shifts', () => [1n << 70n, -1n >> 3n, -5n >> 1n, 5n >> 1n, 1n << -1n, 8n >> -2n, (2n ** 70n) >> 69n, -(2n**70n) >> 200n].map(String)); t('ushr', () => 1n >>> 0n); t('cmp', () => [1n < 2n, 2n < 1n, 1n == 1, 1n === 1, 1n == '1', 1n < 1.5, 2n > 1.5, 1n < 'x', 1n == 'x', 0n == false, 1n == true, 2n**64n > 1e19, 2n**64n == 18446744073709551616, 1n < NaN, 1n > -Infinity].join()); t('eq', () => [1n === 1n, 2n**80n === 2n**80n, 0n === -0n, Object.is(0n, -0n), [1n].includes(1n), [1n].indexOf(1n)].join()); t('truthy', () => [!!0n, !!1n, 0n ? 'a' : 'b', 0n || 'z'].join()); t('string', () => [String(2n**64n), (255n).toString(16), (-255n).toString(2), (2n**70n).toString(36), `${-0n}`].join()); t('Number', () => [Number(2n**64n), Number(-12345678901234567890123n), Number(2n**1024n), Number(0n)].join()); t('BigInt()', () => [BigInt(10), BigInt(-0), BigInt('0x1f'), BigInt(' 12 '), BigInt(''), BigInt(true), BigInt(2**60), BigInt({valueOf(){return 7}})].map(String)); t('BigInt 1.5', () => BigInt(1.5)); t('BigInt str', () => BigInt('1.5')); t('BigInt undef', () => BigInt(undefined)); t('new BigInt', () => new BigInt(1)); t('asN', () => [BigInt.asIntN(8, 255n), BigInt.asIntN(8, 128n), BigInt.asUintN(8, -1n), BigInt.asUintN(64, -1n), BigInt.asIntN(64, 2n**63n), BigInt.asIntN(0, 5n), BigInt.asUintN(3, 13n), BigInt.asIntN(200, -(2n**199n))].map(String)); t('asN bad', () => BigInt.asIntN(-1, 1n)); t('asN num', () => BigInt.asIntN(8, 1)); t('radix', () => (1n).toString(37)); t('this', () => BigInt.prototype.toString.call(1)); t('json', () => JSON.stringify({ a: 1n })); t('tags', () => [Object.prototype.toString.call(1n), Object.prototype.toString.call(BigInt.prototype)].join()); t('map', () => { const m = new Map([[1n, 'a'], [2n**70n, 'b']]); return [m.get(1n), m.get(2n**70n), new Set([1n, 1n, 2n]).size]; }); t('prop set', () => { (1n).x = 1; }); t('methods', () => [(5n).valueOf() === 5n, 5n.toString(), BigInt.prototype.constructor === BigInt, Object.getPrototypeOf(1n) === BigInt.prototype].join()); t('math', () => Math.max(1n)); t('isNaN', () => isNaN(1n)); t('key', () => ({ 1: 'one' })[1n]); t('sort', () => [3n, 1n, 2n].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0)).map(String)); t('fact', () => { let f = 1n; for (let i = 1n; i <= 30n; i = i + 1n) f = f * i; return f.toString(); }); t('lengths', () => [BigInt.length, BigInt.asIntN.length, (1n).toString.length].join()); out.join(\"\\n\");",
       "lit => [\"1\",\"31\",\"5\",\"15\",\"1000\",\"9007199254740993\"]\ntypeof => \"bigint\"\nadd => TypeError: Do not know how to serialize a BigInt\nbig mul => \"1267650600228229401496703205376\"\nneg => \"-1180591620717411303424\"\nsub => TypeError: Do not know how to serialize a BigInt\ndiv => [\"3\",\"-3\",\"-3\",\"-1\",\"1\"]\ndiv0 => RangeError: Division by zero\nmod0 => RangeError: Division by zero\npow neg => RangeError: Exponent must be positive\nbig div => [\"99999999999999999993\",\"49\",\"1267650600228229401496703205373\"]\nmix => TypeError: Cannot mix BigInt and other types, use explicit conversions\nmix sub => TypeError: Cannot mix BigInt and other types, use explicit conversions\nconcat => \"a1b\"\nunary plus => TypeError: Cannot convert a BigInt value to a number\nbitnot => [\"-1\",\"-6\",\"5\"]\nbitwise => [\"1\",\"7\",\"6\",\"3\",\"-5\",\"6\",\"0\",\"36893488147419103232\"]\nshifts => [\"1180591620717411303424\",\"-1\",\"-3\",\"2\",\"0\",\"32\",\"2\",\"-1\"]\nushr => TypeError: BigInts have no unsigned right shift, use >> instead\ncmp => \"true,false,true,false,true,true,true,false,false,true,true,true,true,false,true\"\neq => \"true,true,true,true,true,0\"\ntruthy => \"false,true,b,z\"\nstring => \"18446744073709551616,ff,-11111111,6x5kxtvuwilukg,0\"\nNumber => \"18446744073709552000,-1.2345678901234568e+22,Infinity,0\"\nBigInt() => [\"10\",\"0\",\"31\",\"12\",\"0\",\"1\",\"1152921504606846976\",\"7\"]\nBigInt 1.5 => RangeError: The number 1.5 cannot be converted to a BigInt because it is not an integer\nBigInt str => SyntaxError: Cannot convert 1.5 to a BigInt\nBigInt undef => TypeError: Cannot convert undefined to a BigInt\nnew BigInt => TypeError: BigInt is not a constructor\nasN => [\"-1\",\"-128\",\"255\",\"18446744073709551615\",\"-9223372036854775808\",\"0\",\"5\",\"-803469022129495137770981046170581301261101496891396417650688\"]\nasN bad => RangeError: Invalid value: not (convertible to) a safe integer\nasN num => TypeError: Cannot convert 1 to a BigInt\nradix => RangeError: toString() radix argument must be between 2 and 36\nthis => TypeError: BigInt.prototype.toString requires that 'this' be a BigInt\njson => TypeError: Do not know how to serialize a BigInt\ntags => \"[object BigInt],[object BigInt]\"\nmap => [\"a\",\"b\",2]\nprop set => TypeError: Cannot create property 'x' on bigint '1'\nmethods => \"true,5,true,true\"\nmath => TypeError: Cannot convert a BigInt value to a number\nisNaN => TypeError: Cannot convert a BigInt value to a number\nkey => \"one\"\nsort => [\"1\",\"2\",\"3\"]\nfact => \"265252859812191058636308480000000\"\nlengths => \"1,2,0\"");
    eq_s("\"use strict\"; const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; t('num', () => { let i = 5; const a = i++; const b = ++i; const c = i--; const d = --i; return [a, b, c, d, i]; }); t('big', () => { let i = 5n; const a = i++; const b = ++i; const c = i--; const d = --i; return [a, b, c, d, i]; }); t('str', () => { let s = '5'; const a = s++; return [a, s, typeof a]; }); t('strpre', () => { let s = '5'; const a = ++s; return [a, s]; }); t('obj', () => { let o = { valueOf() { return 2; } }; const a = o++; return [a, o]; }); t('objbig', () => { let o = { valueOf() { return 2n; } }; const a = o++; return [a, o]; }); t('member', () => { const o = { a: 1n, b: 'x', c: 3 }; const r = [o.a++, ++o.a, o.b++, o.c--, --o.c]; return [r, o]; }); t('computed', () => { const o = [1n, 2]; const k = 0; const r = [o[k]++, ++o[1], o[0]--]; return [r, o]; }); t('undef', () => { let u; const a = u++; return [a, u]; }); t('null', () => { let n = null; return [n++, n]; }); t('for', () => { let s = 0n; for (let i = 0n; i < 5n; i++) s += i; return s; }); t('for num', () => { let s = 0; for (let i = 0; i < 1000; i++) s += i; return s; }); t('const', () => { const c = 1; c++; }); t('neg zero', () => { let z = -0; z++; z--; return Object.is(z, 0); }); t('big neg', () => { let x = -1n; x++; const y = x--; return [x, y]; }); t('closure', () => { let n = 0n; const inc = () => n++; inc(); inc(); return n; }); out.join(\"\\n\");",
       "num => [5,7,7,5,5]\nbig => TypeError: Do not know how to serialize a BigInt\nstr => [5,6,\"number\"]\nstrpre => [6,6]\nobj => [2,3]\nobjbig => TypeError: Do not know how to serialize a BigInt\nmember => TypeError: Do not know how to serialize a BigInt\ncomputed => TypeError: Do not know how to serialize a BigInt\nundef => [null,null]\nnull => [0,1]\nfor => TypeError: Do not know how to serialize a BigInt\nfor num => 499500\nconst => TypeError: Assignment to constant variable.\nneg zero => true\nbig neg => TypeError: Do not know how to serialize a BigInt\nclosure => TypeError: Do not know how to serialize a BigInt");
    /* a BigInt as a property key is its string: a literal key (`{ 5n: 1 }`, in a literal and in
     * a pattern), and an index into an array, a string or `arguments`, as the number is */
    eq_s("\"use strict\"; const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; t('literal', () => ({ 5n: 1, 0x10n: 2, 0b11n: 3, 1_000n: 4, 12345678901234567890123n: 5, 0n: 6 })); t('same key', () => [{ 5n: 1, 5: 2 }, { 5: 1, 5n: 2 }, { 5n: 1, '5': 2, [5]: 3 }]); t('read', () => { const o = { 5n: 'a' }; return [o[5], o['5'], o[5n], 5n in o, Object.keys(o)]; }); t('method', () => { const o = { 5n() { return 7; }, 6n: () => 8, 7n: function () {} }; return [o[5](), o[5].name, String(o[5]), o[6].name, o[7].name]; }); t('pattern', () => { const { 5n: a, 6n: b = 2, 7n: [c, d], ...r } = { 5: 'five', 7: [1, 2], 8: 3 }; return [a, b, c, d, r]; }); t('assign', () => { let a; ({ 5n: a } = { 5: 'five' }); return a; }); t('param', () => { const f = ({ 5n: a }, { 0x10n: b }) => [a, b]; return f({ 5: 9 }, { 16: 8 }); }); t('of array', () => { const r = []; for (const { 1n: a } of [[1, 2], [3, 4]]) r.push(a); try { throw [5, 6]; } catch ({ 0n: e }) { r.push(e); } return r; }); t('index', () => { const a = [1, 2, 3]; const k = 2n; return [a[1n], a[k], a[3n], a[-1n], a[2n ** 64n], 'abc'[1n], 'abc'[3n], 1n in a, 3n in a]; }); t('store', () => { const a = [1, 2]; a[1n] = 9; a[0n] += 5; a[1n]++; a[2n] ||= 6; a[-1n] = 'm'; return [a, a.length, Object.keys(a)]; }); t('own', () => [Object.hasOwn([1, 2], 1n), [1, 2].hasOwnProperty(2n), Object.getOwnPropertyDescriptor([1, 2], 1n).value]); t('arguments', () => (function () { return arguments[1n]; })(1, 2)); t('frozen', () => { Object.freeze([1, 2])[0n] = 5; }); t('wide', () => { const o = {}; o[2n ** 70n] = 'b'; o[4294967295n] = 'c'; return o; }); out.join(\"\\n\");",
         "literal => {\"0\":6,\"3\":3,\"5\":1,\"16\":2,\"1000\":4,\"12345678901234567890123\":5}\nsame key => [{\"5\":2},{\"5\":2},{\"5\":3}]\nread => [\"a\",\"a\",\"a\",true,[\"5\"]]\nmethod => [7,\"5\",\"5n() { return 7; }\",\"6\",\"7\"]\npattern => [\"five\",2,1,2,{\"8\":3}]\nassign => \"five\"\nparam => [9,8]\nof array => [2,4,5]\nindex => [2,3,null,null,null,\"b\",null,true,false]\nstore => [[6,10,6],3,[\"0\",\"1\",\"2\",\"-1\"]]\nown => [true,false,2]\narguments => 2\nfrozen => TypeError: Cannot assign to read only property '0' of object '[object Array]'\nwide => {\"1180591620717411303424\":\"b\",\"4294967295\":\"c\"}");
    /* `__proto__: v` in an object literal sets the prototype (the key a name or a string; an
     * object, a function or null, and any other value is ignored), and is no property; computed,
     * shorthand and as a method it is a property like any other, as it is in a pattern and to
     * JSON.parse. Outputs from Node. */
    eq_s("\"use strict\"; const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; const P = { x: 1, hi() { return 'hi ' + this.n; } }; const gp = Object.getPrototypeOf; t('sets', () => { const o = { __proto__: P, n: 3 }; return [o.x, o.hi(), gp(o) === P, Object.keys(o), Object.getOwnPropertyNames(o), JSON.stringify(o), Object.hasOwn(o, '__proto__'), P.isPrototypeOf(o)]; }); t('string key', () => { const o = { \"__proto__\": P }, q = { '__proto__': P, a: 1 }, e = { \\u005f_proto__: P }; return [gp(o) === P, gp(q) === P, gp(e) === P, Object.keys(q)]; }); t('null', () => { const o = { __proto__: null, a: 1 }; return [gp(o), Object.keys(o), typeof o.toString, 'toString' in o, o.a]; }); t('not an object', () => [1, 's', undefined, true, 5n, NaN].map((v) => { const o = { __proto__: v }; return [gp(o) === Object.prototype, Object.keys(o).length]; })); t('any object', () => { const f = () => 1, a = [1, 2], m = new Map(), d = new Date(0), r = /a/, p = Promise.resolve(1); return [f, a, m, d, r, p, Math, Object.prototype].map((v) => gp({ __proto__: v }) === v); }); t('order', () => { const s = []; const o = { a: s.push('a'), __proto__: (s.push('p'), P), b: s.push('b') }; return [s, Object.keys(o)]; }); t('own wins', () => { const o = { x: 2, __proto__: P }, q = { __proto__: P, x: 3 }; const r = [o.x, q.x]; delete o.x; return [r, o.x, P.x]; }); t('no name', () => [gp({ __proto__: function () {} }).name, gp({ __proto__: () => 1 }).name, gp({ __proto__: function named() {} }).name, { ['__proto__']: function () {} }.__proto__.name]); t('computed', () => { const k = '__proto__'; const o = { ['__proto__']: P }, q = { [k]: 5 }; return [gp(o) === Object.prototype, Object.keys(o), o.__proto__ === P, q[k]]; }); t('shorthand', () => { const __proto__ = P; const o = { __proto__ }; return [gp(o) === Object.prototype, Object.keys(o), o.__proto__ === P]; }); t('method', () => { const o = { __proto__() { return 7; } }; return [gp(o) === Object.prototype, Object.keys(o), o.__proto__(), o.__proto__.name]; }); t('with the others', () => { const __proto__ = 7; const a = { __proto__: P, ['__proto__']: 5 }, b = { __proto__: P, __proto__ }, c = { __proto__, __proto__: P }, d = { __proto__: P, __proto__() { return 2; } }; return [a, b, c].map((o) => [o.x, Object.keys(o), o.__proto__]).concat([[d.x, typeof d.__proto__]]); }); t('spread', () => { const src = { ['__proto__']: P, y: 2 }; const o = { ...src }, q = { ...{ __proto__: P } }, r = { __proto__: P, ...{ y: 2 } }; return [gp(o) === Object.prototype, Object.keys(o), q.x, Object.keys(q), r.x, r.y]; }); t('chain', () => { const o = { __proto__: { __proto__: { __proto__: null, deep: 1 }, mid: 2 }, top: 3 }; let c; for (let i = 0; i < 4; i++) c = { __proto__: c ?? null, depth: i }; return [o.deep, o.mid, o.top, typeof o.toString, c.depth, gp(c).depth, gp(gp(gp(gp(c))))]; }); t('late', () => { const p = {}; const o = { __proto__: p }; p.late = 5; return o.late; }); t('write', () => { const o = { __proto__: P }; o.x = 9; return [o.x, P.x, Object.keys(o)]; }); t('frozen proto', () => { const o = { __proto__: Object.freeze({ x: 1 }) }; o.y = 2; const r = [o.y, Object.isFrozen(o)]; o.x = 2; return r; }); t('frozen', () => { const o = Object.freeze({ __proto__: P, a: 1 }); return [Object.isFrozen(o), o.x, Object.setPrototypeOf(o, P) === o]; }); t('instanceof', () => { function F() {} F.prototype.m = () => 'm'; const o = { __proto__: F.prototype }, e = { __proto__: new Error('x') }, a = { __proto__: [] }; return [o instanceof F, o.m(), o.constructor === F, e instanceof Error, e.message, a instanceof Array, Array.isArray(a)]; }); t('copies', () => { const o = { __proto__: { a: 1 }, b: 2 }; const { a, ...rest } = o; return [Object.entries(o), { ...o }, Object.assign({}, o), a, rest]; }); t('conversions', () => { const o = { __proto__: { toJSON() { return 'J'; }, valueOf() { return 42; }, toString() { return 'T'; } } }; return [JSON.stringify(o), o + 1, `${o}`, o > 41]; }); t('null to string', () => `${{ __proto__: null }}`); t('json', () => { const o = JSON.parse('{\"__proto__\": {\"x\": 1}, \"y\": 2}'); return [o.x, Object.keys(o), gp(o) === Object.prototype, JSON.stringify({ __proto__: P, b: [{ __proto__: null, c: 1 }] })]; }); t('fromEntries', () => { const o = Object.fromEntries([['__proto__', P]]); return [o.x, Object.keys(o)]; }); t('pattern', () => { const { __proto__: a, __proto__: b, ['__proto__']: c } = { ['__proto__']: 5 }; let d, e; ({ __proto__: d, __proto__: e } = { ['__proto__']: 6 }); const f = ({ __proto__: g, __proto__: h }) => [g, h]; return [a, b, c, d, e, f({ ['__proto__']: 7 })]; }); t('async', () => { const f = async () => ({ __proto__: await Promise.resolve(P), y: 2 }); return typeof f; }); t('each call', () => { const mk = (p) => ({ __proto__: p, own: 1 }); return [mk({ k: 1 }).k, mk({ k: 2 }).k, mk(null).k, mk(7).k, typeof mk(7).toString]; }); t('throws', () => ({ a: 1, __proto__: (() => { throw new RangeError('in the value'); })() })); out.join(\"\\n\");",
         "sets => [1,\"hi 3\",true,[\"n\"],[\"n\"],\"{\\\"n\\\":3}\",false,true]\nstring key => [true,true,true,[\"a\"]]\nnull => [null,[\"a\"],\"undefined\",false,1]\nnot an object => [[true,0],[true,0],[true,0],[true,0],[true,0],[true,0]]\nany object => [true,true,true,true,true,true,true,true]\norder => [[\"a\",\"p\",\"b\"],[\"a\",\"b\"]]\nown wins => [[2,3],1,1]\nno name => [\"\",\"\",\"named\",\"__proto__\"]\ncomputed => [true,[\"__proto__\"],true,5]\nshorthand => [true,[\"__proto__\"],true]\nmethod => [true,[\"__proto__\"],7,\"__proto__\"]\nwith the others => [[1,[\"__proto__\"],5],[1,[\"__proto__\"],7],[1,[\"__proto__\"],7],[1,\"function\"]]\nspread => [true,[\"__proto__\",\"y\"],null,[],1,2]\nchain => [1,2,3,\"undefined\",3,2,null]\nlate => 5\nwrite => [9,1,[\"x\"]]\nfrozen proto => TypeError: Cannot assign to read only property 'x' of object '#<Object>'\nfrozen => [true,1,true]\ninstanceof => [true,\"m\",true,true,\"x\",true,false]\ncopies => [[[\"b\",2]],{\"b\":2},{\"b\":2},1,{\"b\":2}]\nconversions => [\"\\\"J\\\"\",43,\"T\",true]\nnull to string => TypeError: Cannot convert object to primitive value\njson => [null,[\"__proto__\",\"y\"],true,\"{\\\"b\\\":[{\\\"c\\\":1}]}\"]\nfromEntries => [null,[\"__proto__\"]]\npattern => [5,5,5,6,6,[7,7]]\nasync => \"function\"\neach call => [1,2,null,null,\"function\"]\nthrows => RangeError: in the value");
    eq("let seed = 12345; const rnd = () => { seed = (Math.imul(seed, 1103515245) + 12345) | 0; return (seed >>> 8) & 0xffff; }; const big = () => { const limbs = rnd() % 30; let v = 0n; for (let i = 0; i < limbs; i++) v = (v << 16n) + BigInt(rnd()); if (rnd() & 1) v = -v; if (rnd() % 7 === 0) v = BigInt(rnd() % 5) - 2n; return v; }; const hash = (h, s) => { for (let i = 0; i < s.length; i++) h = (Math.imul(h, 31) + s.charCodeAt(i)) | 0; return h; }; const show = (f) => { try { return String(f()); } catch (e) { return e.constructor.name; } }; const out = []; for (let blk = 0; blk < 6; blk++) { let h = 0; for (let k = 0; k < 100; k++) { const a = big(), b = big(), s = BigInt(rnd() % 200) - 100n, e = BigInt(rnd() % 8); const res = [a + b, a - b, a * b, show(() => a / b), show(() => a % b), a & b, a | b, a ^ b, ~a, -a, show(() => a << s), show(() => a >> s), show(() => a ** e), a < b, a == b, a > b, BigInt.asIntN(rnd() % 130, a), BigInt.asUintN(rnd() % 130, a), a.toString(2 + rnd() % 35), Number(a), a < Number(b), a == Number(a), BigInt(String(a)) === a]; h = hash(h, res.join(',')); } out.push(blk + ' ' + h); } out.join(\"\\n\"); ",
       "0 1615464223\n1 -956890821\n2 -2109627117\n3 -1775085825\n4 871620854\n5 -1320593969");
}

/* String.prototype.normalize; expected values from Node (every code point in all four forms, and
 * Unicode's NormalizationTest.txt, were checked against Node and pass; too big to keep here) */
static void test_normalize(void) {
    /* the ranges with most of the decompositions, each code point and with two marks after it */
    eq("const hash = (h, s) => { for (let i = 0; i < s.length; i++) h = (Math.imul(h, 31) + s.charCodeAt(i)) | 0; return h; }; let h = 0; for (const [a, b] of [[0, 0x3400], [0xAC00, 0xAD00], [0xF900, 0x10000], [0x1D100, 0x1D800], [0x2F800, 0x2FA20]]) for (let cp = a; cp < b; cp++) { const s = String.fromCodePoint(cp) + '̣́'; for (const f of ['NFC', 'NFD', 'NFKC', 'NFKD']) h = hash(h, s.normalize(f)); } String(h);",
       "-1904547770");
    eq_s("\"use strict\"; const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; t('forms', () => ['ẛ̣', 'ﬁ', '각', '각', 'é', 'Å', 'Ω', 'x̣̂́', '\\uD800́', '́̀a', ''].map((s) => ['NFC', 'NFD', 'NFKC', 'NFKD'].map((f) => [...s.normalize(f)].map((c) => c.codePointAt(0).toString(16)).join(' ')))); t('default', () => ['é'.normalize() === 'é', 'é'.normalize(undefined) === 'é']); t('bad form', () => 'a'.normalize('nfc')); t('null form', () => 'a'.normalize(null)); t('obj form', () => 'é'.normalize({ toString() { return 'NFD'; } }).length); t('null this', () => String.prototype.normalize.call(null)); t('num this', () => String.prototype.normalize.call(123, 'NFKC')); t('same', () => { const s = 'plain ascii'; return s.normalize() === s; }); t('length', () => ''.normalize.length); t('long', () => ('ḍ̇'.repeat(500)).normalize('NFD').length); out.join(\"\\n\");",
       "forms => [[\"1e9b 323\",\"17f 323 307\",\"1e69\",\"73 323 307\"],[\"fb01\",\"fb01\",\"66 69\",\"66 69\"],[\"ac01\",\"1100 1161 11a8\",\"ac01\",\"1100 1161 11a8\"],[\"ac01\",\"1100 1161 11a8\",\"ac01\",\"1100 1161 11a8\"],[\"e9\",\"65 301\",\"e9\",\"65 301\"],[\"c5\",\"41 30a\",\"c5\",\"41 30a\"],[\"3a9\",\"3a9\",\"3a9\",\"3a9\"],[\"78 323 302 301\",\"78 323 302 301\",\"78 323 302 301\",\"78 323 302 301\"],[\"d800 301\",\"d800 301\",\"d800 301\",\"d800 301\"],[\"301 300 61\",\"301 300 61\",\"301 300 61\",\"301 300 61\"],[\"\",\"\",\"\",\"\"]]\ndefault => [true,true]\nbad form => RangeError: The normalization form should be one of NFC, NFD, NFKC, NFKD.\nnull form => RangeError: The normalization form should be one of NFC, NFD, NFKC, NFKD.\nobj form => 2\nnull this => TypeError: String.prototype.normalize called on null or undefined\nnum this => \"123\"\nsame => true\nlength => 0\nlong => 1500");
}

/* Runs the modules STEPS in one VM, in turn, collecting and draining the job queue between them
 * (where a host's event loop would let the collector run and FinalizationRegistry callbacks with
 * it); returns the last one's result. */
static char *run_steps(const char *const *steps, int n, bool stress) {
    CountAlloc ca = {0, 0};
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca, .gc_stress = stress};
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    char *out = NULL;
    for (int i = 0; i < n; i++) {
        size_t units = 0;
        uint16_t *u = utf8_to_utf16(steps[i], strlen(steps[i]), &units);
        const char *em;
        uint32_t ep;
        JsValue fn = js_compile_module(ctx, u, units, &em, &ep);
        free(u);
        if (!js_is_function(fn)) {
            free(out);
            out = strdup(em ? em : "compile error");
            break;
        }
        js_gc_protect(vm, &fn);
        JsValue p = js_run_module(ctx, fn);
        js_gc_protect(vm, &p);
        js_gc_collect(vm);
        js_run_jobs(ctx);
        JsValue res = js_promise_result(p);
        js_gc_protect(vm, &res);
        JsValue s = js_to_string(ctx, res);
        size_t sl;
        const uint16_t *su = js_string_units(s, &sl);
        free(out);
        out = su ? utf16_to_utf8(su, sl) : strdup("");
        js_gc_unprotect(vm, &res);
        js_gc_unprotect(vm, &p);
        js_gc_unprotect(vm, &fn);
    }
    js_vm_free(vm);
    if (ca.net_bytes != 0 || ca.live_allocs != 0) {
        checks_failed++;
        fprintf(stderr, "FAIL leak: %s (net=%ld allocs=%ld)\n", steps[0], ca.net_bytes, ca.live_allocs);
    }
    return out;
}

static void steps_eq(const char *const *steps, int n, const char *expected) {
    for (int stress = 0; stress < 2; stress++) {
        char *out = run_steps(steps, n, stress);
        checks_run++;
        if (!out || strcmp(out, expected) != 0) {
            checks_failed++;
            fprintf(stderr, "FAIL%s %s\n  expected: %s\n  actual:   %s\n", stress ? " (stress)" : "",
                    steps[0], expected, out ? out : "(null)");
        }
        free(out);
    }
}

/* What the collector does to WeakMap, WeakSet, WeakRef and FinalizationRegistry */
static void test_weak(void) {
    /* unreachable keys go, with their values; a reachable key keeps its value; dead targets'
     * held values reach the callback, after the job */
    const char *basic[] = {
        "globalThis.log = []; globalThis.reg = new FinalizationRegistry(h => log.push(h));"
        "(() => { const o = {}; reg.register(o, 'o'); globalThis.ref = new WeakRef(o);"
        "  const k = {}; globalThis.keep = k; reg.register(k, 'k');"
        "  globalThis.wm = new WeakMap([[o, { v: 1 }], [k, { v: 2 }]]);"
        "  globalThis.vref = new WeakRef(wm.get(o)); globalThis.kref = new WeakRef(wm.get(k));"
        "  globalThis.ws = new WeakSet([o, k]); })();"
        "[ref.deref() !== undefined, log.length].join();",
        "[ref.deref(), vref.deref(), kref.deref() === wm.get(keep), wm.get(keep).v, ws.has(keep),"
        " log.join()].join();",
    };
    steps_eq(basic, 2, ",,true,2,true,o");
    /* within one job a WeakRef's target stays (KeepDuringJob), even collecting at every step */
    const char *kept[] = {"const r = new WeakRef({ a: 1 }); let s = 0; for (let i = 0; i < 50; i++) s += [i].length;"
                          "r.deref().a + s;"};
    steps_eq(kept, 1, "51");
    /* an ephemeron: a value that refers to its own key does not keep it */
    const char *cycle[] = {
        "globalThis.wm = new WeakMap(); (() => { const o = {}; wm.set(o, { o }); globalThis.r = new WeakRef(o);"
        "  const p = {}; const q = {}; wm.set(p, q); wm.set(q, p); globalThis.rp = new WeakRef(p); })(); 0;",
        "[r.deref(), rp.deref()].join();",
    };
    steps_eq(cycle, 2, ",");
    /* a chain through two maps lives as long as its first key does */
    const char *chain[] = {
        "const a = new WeakMap(), b = new WeakMap(); globalThis.k1 = {}; const k2 = {}; a.set(k1, k2);"
        "b.set(k2, { deep: 7 }); globalThis.a = a; globalThis.b = b; 0;",
        "b.get(a.get(k1)).deep;",
    };
    steps_eq(chain, 2, "7");
    /* unregister: no callback; a callback that throws does not stop the others; one that queues a
     * job has it run in the same drain */
    const char *unreg[] = {
        "globalThis.log = []; globalThis.tok = {};"
        "globalThis.reg = new FinalizationRegistry(h => { log.push(h); if (h === 'boom') throw new Error('x');"
        "  Promise.resolve().then(() => log.push('then ' + h)); });"
        "(() => { reg.register({}, 'gone', tok); reg.register({}, 'boom'); reg.register({}, 'b'); })();"
        "reg.unregister(tok);",
        "log.slice().sort().join();",
    };
    steps_eq(unreg, 2, "b,boom,then b");
    /* a token held only by the registry is weak: it goes, and unregistering is no longer possible */
    const char *tokens[] = {
        "globalThis.log = []; globalThis.keep = {}; globalThis.reg = new FinalizationRegistry(h => log.push(h));"
        "(() => { reg.register(keep, 'kept', {}); })(); 0;",
        "[reg.unregister({}), log.length].join();",
    };
    steps_eq(tokens, 2, "false,0");
    /* the surface (constructors, methods, errors, lengths, tags), and naming a receiver by its
     * constructor; expected values from Node */
    eq_s("\"use strict\"; const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; for (const C of [WeakMap, WeakSet, WeakRef, FinalizationRegistry]) t(C.name, () => [C.length, Object.getOwnPropertyNames(C.prototype).join(), Object.prototype.toString.call(C.prototype), typeof C]); t('wm key', () => new WeakMap().set(1, 2)); t('ws val', () => new WeakSet().add(1)); t('wr target', () => new WeakRef(1)); t('fr cb', () => new FinalizationRegistry(1)); t('fr target', () => new FinalizationRegistry(() => {}).register(1)); t('fr same', () => { const o = {}; new FinalizationRegistry(() => {}).register(o, o); }); t('fr token', () => new FinalizationRegistry(() => {}).register({}, 1, 2)); t('fr unreg bad', () => new FinalizationRegistry(() => {}).unregister(1)); t('no new', () => WeakMap()); t('no new ref', () => WeakRef({})); t('ctor iter', () => new WeakMap([[1, 2]])); t('ctor entry', () => new WeakMap([1])); t('misses', () => [new WeakMap().get(1), new WeakMap().has(1), new WeakMap().delete(1), new WeakSet().has(1), new WeakSet().delete(1)]); t('wm ops', () => { const k = {}; const m = new WeakMap([[k, 1]]); return [m.get(k), m.has(k), m.delete(k), m.has(k), m.set(k, 2) === m, m.get(k)]; }); t('ws ops', () => { const k = {}; const s = new WeakSet([k]); return [s.has(k), s.add(k) === s, s.delete(k), s.has(k)]; }); t('wr deref', () => { const k = {}; const r = new WeakRef(k); return r.deref() === k; }); t('recv', () => WeakMap.prototype.get.call({}, {})); t('recv ws', () => WeakMap.prototype.get.call(new WeakSet(), {})); t('recv ref', () => WeakRef.prototype.deref.call({})); t('recv fr', () => FinalizationRegistry.prototype.register.call({}, {})); t('unreg', () => { const r = new FinalizationRegistry(() => {}); const tok = {}; r.register({}, 1, tok); r.register({}, 2, tok); return [r.unregister(tok), r.unregister(tok), r.register({}, 1)]; }); t('goi', () => { const m = new WeakMap(); const k = {}; return [m.getOrInsert(k, 1), m.getOrInsert(k, 2), m.getOrInsertComputed({}, (x) => typeof x)]; }); t('goic bad', () => new WeakMap().getOrInsertComputed({}, 1)); t('goic badkey', () => new WeakMap().getOrInsertComputed(1, () => 1)); t('tags', () => [String(new WeakMap()), Object.prototype.toString.call(new WeakRef({})), Object.prototype.toString.call(new FinalizationRegistry(() => {}))]); t('fn keys', () => { const m = new WeakMap(); const f = function () {}; const p = Promise.resolve(); m.set(f, 1); m.set(p, 2); return [m.get(f), m.get(p)]; }); t('many', () => { const m = new WeakMap(); const ks = []; for (let i = 0; i < 1000; i++) { const k = {i}; ks.push(k); m.set(k, i); } for (let i = 0; i < 1000; i += 2) m.delete(ks[i]); let s = 0; for (const k of ks) s += m.has(k) ? m.get(k) : 0; return s; }); t('lengths', () => [WeakMap.prototype.set.length, WeakMap.prototype.getOrInsertComputed.length, FinalizationRegistry.prototype.register.length, FinalizationRegistry.prototype.unregister.length, WeakRef.prototype.deref.length, WeakSet.prototype.add.length]); t('instanceof', () => [new WeakMap() instanceof WeakMap, Object.getPrototypeOf(new WeakRef({})) === WeakRef.prototype, WeakMap.prototype.constructor === WeakMap]); t('expando', () => { const m = new WeakMap(); m.x = 1; return [m.x, Object.keys(m)]; }); t('json', () => JSON.stringify([new WeakMap(), new WeakSet()])); out.join(\"\\n\");",
       "WeakMap => [0,\"constructor,delete,get,set,has,getOrInsert,getOrInsertComputed\",\"[object WeakMap]\",\"function\"]\nWeakSet => [0,\"constructor,delete,has,add\",\"[object WeakSet]\",\"function\"]\nWeakRef => [1,\"constructor,deref\",\"[object WeakRef]\",\"function\"]\nFinalizationRegistry => [1,\"constructor,register,unregister\",\"[object FinalizationRegistry]\",\"function\"]\nwm key => TypeError: Invalid value used as weak map key\nws val => TypeError: Invalid value used in weak set\nwr target => TypeError: WeakRef: invalid target\nfr cb => TypeError: FinalizationRegistry: cleanup must be callable\nfr target => TypeError: FinalizationRegistry.prototype.register: invalid target\nfr same => TypeError: FinalizationRegistry.prototype.register: target and holdings must not be same\nfr token => TypeError: Invalid unregisterToken ('2')\nfr unreg bad => TypeError: Invalid unregisterToken ('1')\nno new => TypeError: Constructor WeakMap requires 'new'\nno new ref => TypeError: Constructor WeakRef requires 'new'\nctor iter => TypeError: Invalid value used as weak map key\nctor entry => TypeError: Iterator value 1 is not an entry object\nmisses => [null,false,false,false,false]\nwm ops => [1,true,true,false,true,2]\nws ops => [true,true,true,false]\nwr deref => true\nrecv => TypeError: Method WeakMap.prototype.get called on incompatible receiver #<Object>\nrecv ws => TypeError: Method WeakMap.prototype.get called on incompatible receiver #<WeakSet>\nrecv ref => TypeError: Method WeakRef.prototype.deref called on incompatible receiver #<Object>\nrecv fr => TypeError: Method FinalizationRegistry.prototype.register called on incompatible receiver #<Object>\nunreg => [true,false,null]\ngoi => [1,1,\"object\"]\ngoic bad => TypeError: WeakMap.prototype.getOrInsertComputed is not a function\ngoic badkey => TypeError: Invalid value used as weak map key\ntags => [\"[object WeakMap]\",\"[object WeakRef]\",\"[object FinalizationRegistry]\"]\nfn keys => [1,2]\nmany => 250000\nlengths => [2,2,2,1,0,1]\ninstanceof => [true,true,true]\nexpando => [1,[\"x\"]]\njson => \"[{},{}]\"");
    eq_s("\"use strict\"; const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; t('recv set', () => Map.prototype.get.call(new Set(), 1)); t('tags', () => [Object.prototype.toString.call(Map.prototype), Object.prototype.toString.call(Object.create(Set.prototype)), Object.prototype.toString.call(Object.create(WeakMap.prototype))]); t('ctor name', () => { function Foo() {} const f = new Foo(); Object.freeze(f); f.x = 1; }); t('null proto', () => { const o = Object.freeze(Object.create(null)); o.x = 1; }); t('math', () => { delete Math.PI; }); out.join(\"\\n\");",
       "recv set => TypeError: Method Map.prototype.get called on incompatible receiver #<Set>\ntags => [\"[object Map]\",\"[object Set]\",\"[object WeakMap]\"]\nctor name => TypeError: Cannot add property x, object is not extensible\nnull proto => TypeError: Cannot add property x, object is not extensible\nmath => TypeError: Cannot delete property 'PI' of #<Object>");
}

/* Object.defineProperty, defineProperties, getOwnPropertyDescriptor(s), preventExtensions,
 * isExtensible, isFrozen/isSealed property by property, read-only builtin constants and globals,
 * inherited read-only properties, Object.assign as [[Set]]. Expected values from Node. */
static void test_descriptors(void) {
    eq_s("\"use strict\"; const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; const D = (o, k) => JSON.stringify(Object.getOwnPropertyDescriptor(o, k)); t('0', () => { const o = {}; Object.defineProperty(o, \"x\", {value: 1}); return [D(o, \"x\"), Object.keys(o), o.x]; }); t('1', () => { const o = {}; Object.defineProperty(o, \"x\", {value: 1}); o.x = 2; }); t('2', () => { const o = {}; Object.defineProperty(o, \"x\", {value: 1}); delete o.x; }); t('3', () => { const o = {}; Object.defineProperty(o, \"x\", {value: 1}); Object.defineProperty(o, \"x\", {value: 2}); }); t('4', () => { const o = {}; Object.defineProperty(o, \"x\", {value: 1}); Object.defineProperty(o, \"x\", {value: 1}); return o.x; }); t('5', () => { const o = {}; Object.defineProperty(o, \"x\", {value: 1, writable: true}); Object.defineProperty(o, \"x\", {value: 2}); return D(o, \"x\"); }); t('6', () => { const o = {}; Object.defineProperty(o, \"x\", {value: 1, writable: true}); Object.defineProperty(o, \"x\", {writable: false}); return D(o, \"x\"); }); t('7', () => { const o = {}; Object.defineProperty(o, \"x\", {value: 1}); Object.defineProperty(o, \"x\", {writable: true}); }); t('8', () => { const o = {}; Object.defineProperty(o, \"x\", {value: 1}); Object.defineProperty(o, \"x\", {enumerable: true}); }); t('9', () => { const o = {}; Object.defineProperty(o, \"x\", {value: 1, configurable: true}); Object.defineProperty(o, \"x\", {enumerable: true, writable: true}); return D(o, \"x\"); }); t('10', () => { const o = {a: 1}; Object.defineProperty(o, \"a\", {enumerable: false}); return [Object.keys(o), JSON.stringify(o), D(o, \"a\"), {...o}]; }); t('11', () => { const o = {a: 1, b: 2}; return D(o, \"a\"); }); t('12', () => D({}, \"zz\")); t('13', () => D([1, 2], 0)); t('14', () => D([1, 2], \"length\")); t('15', () => D(\"ab\", 1)); t('16', () => D(\"ab\", \"length\")); t('17', () => D(function f(a) {}, \"length\")); t('18', () => D(function f(a) {}, \"name\")); t('19', () => D(function f(a) {}, \"prototype\")); t('20', () => D(() => 1, \"prototype\")); t('21', () => D(Math, \"PI\")); t('22', () => D(Number, \"EPSILON\")); t('23', () => D(Object, \"keys\")); t('24', () => D(Object, \"prototype\")); t('25', () => D(/a/g, \"lastIndex\")); t('26', () => D(new Error(\"m\"), \"message\")); t('27', () => D(Object.freeze({a: 1}), \"a\")); t('28', () => D(Object.seal({a: 1}), \"a\")); t('29', () => D(Object.freeze([1]), 0)); t('30', () => D(Object.freeze([1]), \"length\")); t('31', () => D(5, \"x\")); t('32', () => Object.defineProperty(1, \"x\", {})); t('33', () => Object.defineProperty({}, \"x\", 1)); t('34', () => Object.defineProperty({}, \"x\", {get: 1})); t('35', () => Object.defineProperty({}, \"x\", {get() {}, value: 1})); t('36', () => Object.getOwnPropertyDescriptor(null, \"x\")); t('37', () => Object.defineProperties({}, null)); t('38', () => Object.defineProperties(1, {})); t('39', () => Object.create({}, null)); t('40', () => Object.defineProperty(Object.preventExtensions({}), \"x\", {value: 1})); t('41', () => { const o = Object.preventExtensions({a: 1}); o.a = 2; delete o.a; return [o, Object.isExtensible(o), Object.isSealed(o), Object.isFrozen(o)]; }); t('42', () => { const o = Object.preventExtensions({a: 1}); o.b = 2; }); t('43', () => { const o = Object.preventExtensions({}); return [Object.isSealed(o), Object.isFrozen(o)]; }); t('44', () => { const o = {}; Object.defineProperty(o, \"x\", {value: 1}); Object.preventExtensions(o); return [Object.isSealed(o), Object.isFrozen(o)]; }); t('45', () => { const o = {}; Object.defineProperty(o, \"x\", {value: 1, writable: true}); Object.preventExtensions(o); return [Object.isSealed(o), Object.isFrozen(o)]; }); t('46', () => [Object.isExtensible(1), Object.preventExtensions(1), Object.isExtensible({}), Object.isFrozen(1)]); t('47', () => { const a = Object.preventExtensions([1, 2]); a[0] = 5; a.pop(); return a; }); t('48', () => { const a = Object.preventExtensions([1, 2]); a.push(3); }); t('49', () => { const a = Object.preventExtensions([1, 2]); a[2] = 3; }); t('50', () => { const f = Object.preventExtensions(function () {}); f.x = 1; }); t('51', () => Object.isExtensible(Object.preventExtensions(function () {}))); t('52', () => { const o = Object.preventExtensions({}); Object.setPrototypeOf(o, {}); }); t('53', () => { const o = Object.defineProperties({}, {a: {value: 1, enumerable: true}, b: {value: 2}}); return [o, D(o, \"b\")]; }); t('54', () => { const o = Object.create({p: 1}, {a: {value: 1, enumerable: true}}); return [o, o.p]; }); t('55', () => Object.getOwnPropertyDescriptors({a: 1, b: \"x\"})); t('56', () => Object.keys(Object.getOwnPropertyDescriptors([1]))); t('57', () => Object.getOwnPropertyDescriptors(\"ab\")); t('58', () => { const p = {}; Object.defineProperty(p, \"x\", {value: 1}); const c = Object.create(p); c.x = 2; }); t('59', () => { const p = {}; Object.defineProperty(p, \"x\", {value: 1}); const c = Object.create(p); Object.defineProperty(c, \"x\", {value: 2, enumerable: true}); return [c.x, c]; }); t('60', () => { const p = Object.freeze({x: 1}); const c = Object.create(p); return {...c, x: 3}; }); t('61', () => { const f = function g() {}; Object.defineProperty(f, \"name\", {value: \"h\"}); return [f.name, D(f, \"name\"), Object.getOwnPropertyNames(f)]; }); t('62', () => { const f = function g() {}; Object.defineProperty(f, \"name\", {value: \"h\"}); f.name = \"z\"; }); t('63', () => { const f = function g() {}; Object.defineProperty(f, \"name\", {value: \"h\", writable: true}); f.name = \"z\"; return f.name; }); t('64', () => { const f = function g(a, b) {}; Object.defineProperty(f, \"length\", {value: 7}); return [f.length, D(f, \"length\")]; }); t('65', () => { const f = function g() {}; Object.defineProperty(f, \"name\", {value: \"h\"}); delete f.name; return [f.name, Object.getOwnPropertyNames(f)]; }); t('66', () => { const f = function g() {}; Object.defineProperty(f, \"name\", {configurable: false}); delete f.name; }); t('67', () => { function F() {} Object.defineProperty(F, \"prototype\", {writable: false}); F.prototype = {}; }); t('68', () => { function F() {} Object.defineProperty(F, \"prototype\", {writable: false}); return D(F, \"prototype\"); }); t('69', () => { function F() {} const p = {q: 1}; Object.defineProperty(F, \"prototype\", {value: p}); return new F().q; }); t('70', () => { function F() {} Object.defineProperty(F, \"prototype\", {enumerable: true}); }); t('71', () => { delete Math.PI; }); t('72', () => { delete Number.NaN; }); t('73', () => { const a = [1, 2]; Object.defineProperty(a, 0, {value: 9}); return a; }); t('74', () => { const a = [1, 2]; Object.defineProperty(a, \"length\", {value: 1}); return a; }); t('75', () => { const a = [1, 2]; Object.defineProperty(a, 2, {value: 3, writable: true, enumerable: true, configurable: true}); return a; }); t('76', () => { const a = [1, 2]; Object.defineProperty(a, \"foo\", {value: 3}); return [a.foo, Object.keys(a)]; }); t('77', () => { const a = [1, 2]; Object.defineProperty(a, \"length\", {value: -1}); }); t('78', () => { const e = new Error(\"m\"); Object.defineProperty(e, \"stack\", {value: \"S\"}); return [e.stack, D(e, \"stack\")]; }); t('79', () => { const e = new Error(\"m\"); Object.defineProperty(e, \"message\", {enumerable: true}); return Object.keys(e); }); t('80', () => { const r = /a/g; Object.defineProperty(r, \"lastIndex\", {value: 3}); return r.lastIndex; }); t('81', () => { const o = {a: 1}; Object.defineProperty(o, \"a\", {writable: false, configurable: false}); Object.preventExtensions(o); return Object.isFrozen(o); }); t('82', () => { const o = Object.freeze({a: 1}); Object.defineProperty(o, \"a\", {value: 1}); Object.defineProperty(o, \"a\", {value: 2}); }); t('83', () => { const o = Object.seal({a: 1}); Object.defineProperty(o, \"a\", {value: 2}); Object.defineProperty(o, \"a\", {writable: false}); return [D(o, \"a\"), Object.isFrozen(o)]; }); t('84', () => { const o = Object.seal({a: 1}); Object.defineProperty(o, \"a\", {enumerable: false}); }); t('85', () => { const o = {}; Object.defineProperty(o, \"x\", {value: 1}); Object.assign(o, {x: 2}); }); t('86', () => { const o = {}; Object.defineProperty(o, \"x\", {value: 1, writable: true}); return Object.assign(o, {x: 2}).x; }); t('87', () => { const d = Object.create({value: 5, enumerable: true}); const o = Object.defineProperty({}, \"x\", d); return [o.x, Object.keys(o)]; }); t('88', () => { const o = Object.defineProperty({}, \"x\", {value: 1, writable: 0, enumerable: \"y\", configurable: null}); return D(o, \"x\"); }); t('89', () => Object.defineProperty({}, 1.5, {value: 1, enumerable: true})); out.join(\"\\n\");",
       "0 => [\"{\\\"value\\\":1,\\\"writable\\\":false,\\\"enumerable\\\":false,\\\"configurable\\\":false}\",[],1]\n1 => TypeError: Cannot assign to read only property 'x' of object '#<Object>'\n2 => TypeError: Cannot delete property 'x' of #<Object>\n3 => TypeError: Cannot redefine property: x\n4 => 1\n5 => \"{\\\"value\\\":2,\\\"writable\\\":true,\\\"enumerable\\\":false,\\\"configurable\\\":false}\"\n6 => \"{\\\"value\\\":1,\\\"writable\\\":false,\\\"enumerable\\\":false,\\\"configurable\\\":false}\"\n7 => TypeError: Cannot redefine property: x\n8 => TypeError: Cannot redefine property: x\n9 => \"{\\\"value\\\":1,\\\"writable\\\":true,\\\"enumerable\\\":true,\\\"configurable\\\":true}\"\n10 => [[],\"{}\",\"{\\\"value\\\":1,\\\"writable\\\":true,\\\"enumerable\\\":false,\\\"configurable\\\":true}\",{}]\n11 => \"{\\\"value\\\":1,\\\"writable\\\":true,\\\"enumerable\\\":true,\\\"configurable\\\":true}\"\n12 => undefined\n13 => \"{\\\"value\\\":1,\\\"writable\\\":true,\\\"enumerable\\\":true,\\\"configurable\\\":true}\"\n14 => \"{\\\"value\\\":2,\\\"writable\\\":true,\\\"enumerable\\\":false,\\\"configurable\\\":false}\"\n15 => \"{\\\"value\\\":\\\"b\\\",\\\"writable\\\":false,\\\"enumerable\\\":true,\\\"configurable\\\":false}\"\n16 => \"{\\\"value\\\":2,\\\"writable\\\":false,\\\"enumerable\\\":false,\\\"configurable\\\":false}\"\n17 => \"{\\\"value\\\":1,\\\"writable\\\":false,\\\"enumerable\\\":false,\\\"configurable\\\":true}\"\n18 => \"{\\\"value\\\":\\\"f\\\",\\\"writable\\\":false,\\\"enumerable\\\":false,\\\"configurable\\\":true}\"\n19 => \"{\\\"value\\\":{},\\\"writable\\\":true,\\\"enumerable\\\":false,\\\"configurable\\\":false}\"\n20 => undefined\n21 => \"{\\\"value\\\":3.141592653589793,\\\"writable\\\":false,\\\"enumerable\\\":false,\\\"configurable\\\":false}\"\n22 => \"{\\\"value\\\":2.220446049250313e-16,\\\"writable\\\":false,\\\"enumerable\\\":false,\\\"configurable\\\":false}\"\n23 => \"{\\\"writable\\\":true,\\\"enumerable\\\":false,\\\"configurable\\\":true}\"\n24 => \"{\\\"value\\\":{},\\\"writable\\\":false,\\\"enumerable\\\":false,\\\"configurable\\\":false}\"\n25 => \"{\\\"value\\\":0,\\\"writable\\\":true,\\\"enumerable\\\":false,\\\"configurable\\\":false}\"\n26 => \"{\\\"value\\\":\\\"m\\\",\\\"writable\\\":true,\\\"enumerable\\\":false,\\\"configurable\\\":true}\"\n27 => \"{\\\"value\\\":1,\\\"writable\\\":false,\\\"enumerable\\\":true,\\\"configurable\\\":false}\"\n28 => \"{\\\"value\\\":1,\\\"writable\\\":true,\\\"enumerable\\\":true,\\\"configurable\\\":false}\"\n29 => \"{\\\"value\\\":1,\\\"writable\\\":false,\\\"enumerable\\\":true,\\\"configurable\\\":false}\"\n30 => \"{\\\"value\\\":1,\\\"writable\\\":false,\\\"enumerable\\\":false,\\\"configurable\\\":false}\"\n31 => undefined\n32 => TypeError: Object.defineProperty called on non-object\n33 => TypeError: Property description must be an object: 1\n34 => TypeError: Getter must be a function: 1\n35 => TypeError: Invalid property descriptor. Cannot both specify accessors and a value or writable attribute, #<Object>\n36 => TypeError: Cannot convert undefined or null to object\n37 => TypeError: Cannot convert undefined or null to object\n38 => TypeError: Object.defineProperties called on non-object\n39 => TypeError: Cannot convert undefined or null to object\n40 => TypeError: Cannot define property x, object is not extensible\n41 => [{},false,true,true]\n42 => TypeError: Cannot add property b, object is not extensible\n43 => [true,true]\n44 => [true,true]\n45 => [true,false]\n46 => [false,1,true,true]\n47 => [5]\n48 => TypeError: Cannot add property 2, object is not extensible\n49 => TypeError: Cannot add property 2, object is not extensible\n50 => TypeError: Cannot add property x, object is not extensible\n51 => false\n52 => TypeError: #<Object> is not extensible\n53 => [{\"a\":1},\"{\\\"value\\\":2,\\\"writable\\\":false,\\\"enumerable\\\":false,\\\"configurable\\\":false}\"]\n54 => [{\"a\":1},1]\n55 => {\"a\":{\"value\":1,\"writable\":true,\"enumerable\":true,\"configurable\":true},\"b\":{\"value\":\"x\",\"writable\":true,\"enumerable\":true,\"configurable\":true}}\n56 => [\"0\",\"length\"]\n57 => {\"0\":{\"value\":\"a\",\"writable\":false,\"enumerable\":true,\"configurable\":false},\"1\":{\"value\":\"b\",\"writable\":false,\"enumerable\":true,\"configurable\":false},\"length\":{\"value\":2,\"writable\":false,\"enumerable\":false,\"configurable\":false}}\n58 => TypeError: Cannot assign to read only property 'x' of object '#<Object>'\n59 => [2,{\"x\":2}]\n60 => {\"x\":3}\n61 => [\"h\",\"{\\\"value\\\":\\\"h\\\",\\\"writable\\\":false,\\\"enumerable\\\":false,\\\"configurable\\\":true}\",[\"length\",\"name\",\"prototype\"]]\n62 => TypeError: Cannot assign to read only property 'name' of function 'function g() {}'\n63 => \"z\"\n64 => [7,\"{\\\"value\\\":7,\\\"writable\\\":false,\\\"enumerable\\\":false,\\\"configurable\\\":true}\"]\n65 => [\"\",[\"length\",\"prototype\"]]\n66 => TypeError: Cannot delete property 'name' of function g() {}\n67 => TypeError: Cannot assign to read only property 'prototype' of function 'function F() {}'\n68 => \"{\\\"value\\\":{},\\\"writable\\\":false,\\\"enumerable\\\":false,\\\"configurable\\\":false}\"\n69 => 1\n70 => TypeError: Cannot redefine property: prototype\n71 => TypeError: Cannot delete property 'PI' of #<Object>\n72 => TypeError: Cannot delete property 'NaN' of function Number() { [native code] }\n73 => [9,2]\n74 => [1]\n75 => [1,2,3]\n76 => [3,[\"0\",\"1\"]]\n77 => RangeError: Invalid array length\n78 => [\"S\",\"{\\\"value\\\":\\\"S\\\",\\\"writable\\\":false,\\\"enumerable\\\":false,\\\"configurable\\\":true}\"]\n79 => [\"message\"]\n80 => 3\n81 => true\n82 => TypeError: Cannot redefine property: a\n83 => [\"{\\\"value\\\":2,\\\"writable\\\":false,\\\"enumerable\\\":true,\\\"configurable\\\":false}\",true]\n84 => TypeError: Cannot redefine property: a\n85 => TypeError: Cannot assign to read only property 'x' of object '#<Object>'\n86 => 2\n87 => [5,[\"x\"]]\n88 => \"{\\\"value\\\":1,\\\"writable\\\":false,\\\"enumerable\\\":true,\\\"configurable\\\":false}\"\n89 => {\"1.5\":1}");
    eq_s("\"use strict\"; const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; const D = (o, k) => JSON.stringify(Object.getOwnPropertyDescriptor(o, k)); t('0', () => D(globalThis, \"NaN\")); t('1', () => { Object.defineProperty(Math, \"PI\", {value: 3}); }); t('2', () => { Object.defineProperty(Math, \"PI\", {value: Math.PI}); return Math.PI; }); t('3', () => { Object.defineProperty(Object, \"keys\", {value: 1, enumerable: true}); const r = [Object.keys, D(Object, \"keys\")]; return r; }); t('4', () => { Object.defineProperty(globalThis, \"myG\", {value: 5}); return myG; }); t('5', () => { Object.defineProperty(globalThis, \"myG2\", {value: 5}); myG2 = 6; }); t('6', () => { delete globalThis.NaN; }); out.join(\"\\n\");",
       "0 => \"{\\\"value\\\":null,\\\"writable\\\":false,\\\"enumerable\\\":false,\\\"configurable\\\":false}\"\n1 => TypeError: Cannot redefine property: PI\n2 => 3.141592653589793\n3 => [1,\"{\\\"value\\\":1,\\\"writable\\\":true,\\\"enumerable\\\":true,\\\"configurable\\\":true}\"]\n4 => 5\n5 => TypeError: Cannot assign to read only property 'myG2' of object '#<Object>'\n6 => TypeError: Cannot delete property 'NaN' of #<Object>");
}

/* V8 builtins added together: Array (splice, copyWithin, findLast, flatMap, reduceRight, toSorted
 * and the rest), Object.is/groupBy, isPrototypeOf, propertyIsEnumerable, the Set methods,
 * Map.groupBy/getOrInsert, Error.isError, String raw/isWellFormed/the HTML methods, RegExp.escape,
 * toExponential/toPrecision/toFixed, escape/unescape, getYear/setYear. Expected values from Node
 * (generated by the probe scripts these came from). */
static void test_v8_methods(void) {
    eq_s("const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; t('flatMap no fn', () => [1].flatMap(1)); t('findLast no fn', () => [1].findLast(1)); t('findLastIndex no fn', () => [1].findLastIndex()); t('reduceRight empty', () => [].reduceRight((a,b)=>a)); t('reduceRight no fn', () => [1].reduceRight(3)); t('with oob', () => [1,2].with(5, 1)); t('with neg oob', () => [1,2].with(-3, 1)); t('toSorted bad', () => [1,2].toSorted(3)); t('splice frozen del', () => Object.freeze([1,2,3]).splice(0,1)); t('splice frozen ins', () => Object.freeze([1,2,3]).splice(1,0,9)); t('splice frozen noop', () => Object.freeze([1,2,3]).splice(1,0)); t('splice frozen same', () => Object.freeze([1,2,3]).splice(1,1,9)); t('splice sealed del', () => Object.seal([1,2,3]).splice(0,1)); t('splice sealed ins', () => Object.seal([1,2,3]).splice(1,0,9)); t('splice sealed same', () => { const a=Object.seal([1,2,3]); return [a.splice(1,1,9), a]; }); t('splice sealed del end', () => Object.seal([1,2,3]).splice(2,1)); t('splice frozen del end', () => Object.freeze([1,2,3]).splice(2,1)); t('copyWithin frozen', () => Object.freeze([1,2,3]).copyWithin(0,1)); t('copyWithin frozen empty', () => Object.freeze([]).copyWithin(0,1)); t('copyWithin frozen noop', () => Object.freeze([1,2,3]).copyWithin(0,3)); t('copyWithin sealed', () => Object.seal([1,2,3]).copyWithin(0,1)); t('reverse frozen', () => Object.freeze([1]).reverse()); t('toSpliced big', () => [1].toSpliced(0,0,...Array(3))); t('splice', () => { const a=[1,2,3,4,5]; return [a.splice(1,2,'a','b','c'), a]; }); t('splice neg', () => { const a=[1,2,3,4,5]; return [a.splice(-2), a]; }); t('splice none', () => { const a=[1,2,3]; return [a.splice(), a]; }); t('splice undef', () => { const a=[1,2,3]; return [a.splice(undefined), a]; }); t('splice 1 undef', () => { const a=[1,2,3]; return [a.splice(1, undefined), a]; }); t('splice inf', () => { const a=[1,2,3]; return [a.splice(1, Infinity), a]; }); t('splice neg del', () => { const a=[1,2,3]; return [a.splice(1, -1, 7), a]; }); t('copyWithin', () => [1,2,3,4,5].copyWithin(-2, 0, 3)); t('copyWithin2', () => [1,2,3,4,5].copyWithin(1, 0)); t('toSpliced', () => [1,2,3].toSpliced(1,1,'x','y')); t('with', () => [1,2,3].with(-1, 9)); t('toReversed', () => [1,2,3].toReversed()); t('toSorted', () => [3,1,10,2].toSorted()); t('flatMap', () => [1,[2],3].flatMap((x,i)=>[x,[i]])); t('findLast', () => [1,2,3,4].findLast(x=>x%2==1)); t('findLastIndex', () => [1,2,3,4].findLastIndex(x=>x>5)); t('reduceRight', () => [1,2,3].reduceRight((a,b)=>a+'-'+b)); t('reduceRight init', () => [1,2,3].reduceRight((a,b,i,arr)=>a+b+i, 'x')); t('includes NaN', () => [NaN].includes(NaN)); t('indexOf NaN', () => [NaN].indexOf(NaN)); t('includes from', () => [1,2,3].includes(1, 1)); t('lastIndexOf from', () => [1,2,1,2].lastIndexOf(1, 1)); t('forEach push', () => { const a=[1,2]; let n=0; a.forEach(x=>{n++; if(a.length<6) a.push(9)}); return [n, a.length]; }); t('map shrink', () => { const a=[1,2,3]; return a.map((x,i)=>{ if(i==0) a.pop(); return x*2; }); }); t('find shrink', () => { const a=[1,2,3]; const seen=[]; a.find((x,i)=>{ if(i==0) a.pop(); seen.push(x); return false; }); return seen; }); t('findLast mut', () => { const a=[1,2,3]; const seen=[]; a.findLast((x,i)=>{ if(i==2) a.length=1; seen.push(x); return false; }); return seen; }); t('Math.max len', () => [Math.imul.length, Math.clz32.length, Math.f16round.length, Math.sinh.length]); t('lengths', () => [[].splice.length, [].copyWithin.length, [].findLast.length, [].flatMap.length, [].reduceRight.length, [].toSorted.length, [].toSpliced.length, [].with.length, [].toReversed.length]); out.join(\"\\n\");",
       "flatMap no fn => TypeError: number 1 is not a function\nfindLast no fn => TypeError: number 1 is not a function\nfindLastIndex no fn => TypeError: undefined is not a function\nreduceRight empty => TypeError: Reduce of empty array with no initial value\nreduceRight no fn => TypeError: number 3 is not a function\nwith oob => RangeError: Invalid index : 5\nwith neg oob => RangeError: Invalid index : -3\ntoSorted bad => TypeError: The comparison function must be either a function or undefined: 3\nsplice frozen del => TypeError: Cannot assign to read only property '0' of object '[object Array]'\nsplice frozen ins => TypeError: Cannot add property 3, object is not extensible\nsplice frozen noop => TypeError: Cannot assign to read only property 'length' of object '[object Array]'\nsplice frozen same => TypeError: Cannot assign to read only property '1' of object '[object Array]'\nsplice sealed del => TypeError: Cannot delete property '2' of [object Array]\nsplice sealed ins => TypeError: Cannot add property 3, object is not extensible\nsplice sealed same => [[2],[1,9,3]]\nsplice sealed del end => TypeError: Cannot delete property '2' of [object Array]\nsplice frozen del end => TypeError: Cannot delete property '2' of [object Array]\ncopyWithin frozen => TypeError: Cannot assign to read only property '0' of object '[object Array]'\ncopyWithin frozen empty => []\ncopyWithin frozen noop => [1,2,3]\ncopyWithin sealed => [2,3,3]\nreverse frozen => [1]\ntoSpliced big => [null,null,null,1]\nsplice => [[2,3],[1,\"a\",\"b\",\"c\",4,5]]\nsplice neg => [[4,5],[1,2,3]]\nsplice none => [[],[1,2,3]]\nsplice undef => [[1,2,3],[]]\nsplice 1 undef => [[],[1,2,3]]\nsplice inf => [[2,3],[1]]\nsplice neg del => [[],[1,7,2,3]]\ncopyWithin => [1,2,3,1,2]\ncopyWithin2 => [1,1,2,3,4]\ntoSpliced => [1,\"x\",\"y\",3]\nwith => [1,2,9]\ntoReversed => [3,2,1]\ntoSorted => [1,10,2,3]\nflatMap => [1,[0],[2],[1],3,[2]]\nfindLast => 3\nfindLastIndex => -1\nreduceRight => \"3-2-1\"\nreduceRight init => \"x322110\"\nincludes NaN => true\nindexOf NaN => -1\nincludes from => false\nlastIndexOf from => 0\nforEach push => [2,4]\nmap shrink => [2,4,null]\nfind shrink => [1,2,null]\nfindLast mut => [3,null,1]\nMath.max len => [2,1,1,1]\nlengths => [2,2,1,1,1,1,2,2,0]");
    eq_s("const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; t('sort frozen1', () => Object.freeze([1]).sort()); t('sort frozen sorted', () => Object.freeze([1,2]).sort()); t('sort frozen', () => Object.freeze([2,1]).sort()); t('fill frozen1', () => Object.freeze([1]).fill(0)); t('fill frozen empty range', () => Object.freeze([1,2]).fill(0, 2)); t('reverse frozen2', () => Object.freeze([1,2]).reverse()); t('reverse frozen0', () => Object.freeze([]).reverse()); t('sealed splice del middle', () => { const a = Object.seal([1,2,3]); try { a.splice(0,1); } catch (e) { return [e.message, a]; } }); t('frozen splice empty', () => Object.freeze([]).splice(0,0)); t('pop frozen empty', () => Object.freeze([]).pop()); t('sealed splice noop', () => Object.seal([1,2]).splice(0,0)); t('copyWithin frozen same', () => Object.freeze([1,2,3]).copyWithin(0,0)); out.join(\"\\n\");",
       "sort frozen1 => [1]\nsort frozen sorted => TypeError: Cannot assign to read only property '0' of object '[object Array]'\nsort frozen => TypeError: Cannot assign to read only property '0' of object '[object Array]'\nfill frozen1 => TypeError: Cannot assign to read only property '0' of object '[object Array]'\nfill frozen empty range => [1,2]\nreverse frozen2 => TypeError: Cannot assign to read only property '0' of object '[object Array]'\nreverse frozen0 => []\nsealed splice del middle => [\"Cannot delete property '2' of [object Array]\",[2,3,3]]\nfrozen splice empty => TypeError: Cannot assign to read only property 'length' of object '[object Array]'\npop frozen empty => TypeError: Cannot assign to read only property 'length' of object '[object Array]'\nsealed splice noop => []\ncopyWithin frozen same => TypeError: Cannot assign to read only property '0' of object '[object Array]'");
    eq_s("const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; t('is', () => [Object.is(NaN, NaN), Object.is(0, -0), Object.is(-0, -0), Object.is('a', 'a'), Object.is({}, {}), Object.is(), Object.is(undefined, null), Object.is(1, 1)]); function F() {} const f = new F(); t('isProto', () => [F.prototype.isPrototypeOf(f), Object.prototype.isPrototypeOf(f), Array.prototype.isPrototypeOf([]), Object.prototype.isPrototypeOf(1), Object.prototype.isPrototypeOf(() => 1), (() => {}).isPrototypeOf.call(Object.getPrototypeOf(() => 1), () => 2), Object.prototype.isPrototypeOf(Object.create(null)), Error.prototype.isPrototypeOf(new TypeError('x'))]); t('isProto null this prim', () => Object.prototype.isPrototypeOf.call(null, 1)); t('isProto null this obj', () => Object.prototype.isPrototypeOf.call(null, {})); const e = new Error('m'); t('pie', () => [({a: 1}).propertyIsEnumerable('a'), [1].propertyIsEnumerable(0), [1].propertyIsEnumerable('length'), 'ab'.propertyIsEnumerable(1), 'ab'.propertyIsEnumerable('length'), e.propertyIsEnumerable('message'), e.propertyIsEnumerable('stack'), F.propertyIsEnumerable('prototype'), F.propertyIsEnumerable('name'), F.prototype.propertyIsEnumerable('constructor'), Math.propertyIsEnumerable('PI'), Math.propertyIsEnumerable('max'), Object.propertyIsEnumerable('keys'), Array.prototype.propertyIsEnumerable('map'), ({}).propertyIsEnumerable('toString'), /a/.propertyIsEnumerable('lastIndex'), Object.prototype.propertyIsEnumerable.call(5, 'x')]); F.x = 1; t('pie fn prop', () => [F.propertyIsEnumerable('x'), (async () => 1)().propertyIsEnumerable('then')]); t('pie null', () => Object.prototype.propertyIsEnumerable.call(undefined, 'x')); t('lengths', () => [Object.is.length, ({}).isPrototypeOf.length, ({}).propertyIsEnumerable.length]); out.join(\"\\n\");",
       "is => [true,false,true,true,false,true,false,true]\nisProto => [true,true,true,false,true,true,false,true]\nisProto null this prim => false\nisProto null this obj => TypeError: Cannot convert undefined or null to object\npie => [true,true,false,true,false,false,false,false,false,false,false,false,false,false,false,false,false]\npie fn prop => [true,false]\npie null => TypeError: Cannot convert undefined or null to object\nlengths => [2,1,1]");
    eq_s("const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; const S = (...a) => new Set(a); const A = (s) => [...s]; t('not obj', () => S(1).union(1)); t('arr', () => S(1).union([1])); t('size NaN', () => S(1).union({ size: 'x', has() {}, keys() {} })); t('size undefined', () => S(1).union({ has() {}, keys() {} })); t('size neg', () => S(1).union({ size: -1, has() {}, keys() {} })); t('has nf', () => S(1).union({ size: 1, has: 1, keys() {} })); t('keys nf', () => S(1).union({ size: 1, has() {}, keys: 1 })); t('keys not obj', () => S(1).union({ size: 1, has() {}, keys() { return 1; } })); t('next not obj', () => S(1).union({ size: 1, has() {}, keys() { return { next() { return 1; } }; } })); t('next nf', () => S(1).union({ size: 1, has() {}, keys() { return { next: 1 }; } })); t('this not set', () => Set.prototype.union.call({}, S())); t('union', () => A(S(1, 2, -0).union(S(2, 3, 0, NaN)))); t('intersection', () => A(S(1, 2, 3, 4).intersection(S(4, 2, 9)))); t('intersection big', () => A(S(1, 2).intersection(S(4, 2, 9, 1, 0)))); t('difference', () => A(S(1, 2, 3).difference(S(2)))); t('difference big', () => A(S(1, 2).difference(S(2, 5, 6)))); t('symdiff', () => A(S(1, 2, 3).symmetricDifference(S(3, 4, 1, 5)))); t('subset', () => [S(1, 2).isSubsetOf(S(1, 2, 3)), S(1, 4).isSubsetOf(S(1, 2, 3)), S().isSubsetOf(S()), S(1,2,3).isSubsetOf(S(1))]); t('superset', () => [S(1, 2, 3).isSupersetOf(S(1, 2)), S(1, 2).isSupersetOf(S(1, 5)), S(1).isSupersetOf(S(1, 2))]); t('disjoint', () => [S(1, 2).isDisjointFrom(S(3, 4)), S(1, 2).isDisjointFrom(S(2, 4, 5)), S(1, 2, 3).isDisjointFrom(S(3))]); t('map arg', () => A(S(1, 2).union(new Map([[3, 'a']])))); const log = []; const other = { size: 2, has(v) { log.push('has ' + v); return v === 2; }, keys() { log.push('keys'); let i = 0; return { next() { log.push('next'); i++; return i <= 2 ? { value: i * 10, done: false } : { done: true }; }, return() { log.push('return'); return {}; } }; } }; t('order intersection small', () => { log.length = 0; const r = A(S(1, 2).intersection(other)); return [r, log]; }); t('order intersection big', () => { log.length = 0; const r = A(S(1, 2, 3).intersection(other)); return [r, log]; }); t('order superset', () => { log.length = 0; const r = S(1, 2, 3).isSupersetOf(other); return [r, log]; }); t('order disjoint big', () => { log.length = 0; const r = S(1, 2, 10).isDisjointFrom(other); return [r, log]; }); t('order symdiff', () => { log.length = 0; const r = A(S(10).symmetricDifference(other)); return [r, log]; }); t('order difference', () => { log.length = 0; const r = A(S(1, 2).difference(other)); return [r, log]; }); t('order subset', () => { log.length = 0; const r = S(2, 1).isSubsetOf(other); return [r, log]; }); t('mutating has', () => { const s = S(1, 2, 3); const r = s.intersection({ size: 10, has(v) { if (v === 1) { s.delete(2); s.add(4); } return true; }, keys() {} }); return A(r); }); t('size Infinity', () => S(1,2).isSubsetOf({ size: Infinity, has: () => true, keys() {} })); t('lengths', () => ['union','intersection','difference','symmetricDifference','isSubsetOf','isSupersetOf','isDisjointFrom'].map(k => Set.prototype[k].length)); out.join(\"\\n\");",
       "not obj => TypeError: Set.prototype.union argument must be an object\narr => TypeError: The .size property is NaN\nsize NaN => TypeError: The .size property is NaN\nsize undefined => TypeError: The .size property is NaN\nsize neg => RangeError: '-1' is an invalid size\nhas nf => TypeError: string \"has\" is not a function\nkeys nf => TypeError: string \"keys\" is not a function\nkeys not obj => TypeError: Result of the keys method is not an object\nnext not obj => TypeError: Iterator result 1 is not an object\nnext nf => TypeError: string \"next\" is not a function\nthis not set => TypeError: Method Set.prototype.union called on incompatible receiver #<Object>\nunion => [1,2,0,3,null]\nintersection => [4,2]\nintersection big => [1,2]\ndifference => [1,3]\ndifference big => [1]\nsymdiff => [2,4,5]\nsubset => [true,false,true,false]\nsuperset => [true,false,false]\ndisjoint => [true,false,false]\nmap arg => [1,2,3]\norder intersection small => [[2],[\"has 1\",\"has 2\"]]\norder intersection big => [[],[\"keys\",\"next\",\"next\",\"next\"]]\norder superset => [false,[\"keys\",\"next\",\"return\"]]\norder disjoint big => [false,[\"keys\",\"next\",\"return\"]]\norder symdiff => [[20],[\"keys\",\"next\",\"next\",\"next\"]]\norder difference => [[1],[\"has 1\",\"has 2\"]]\norder subset => [false,[\"has 2\",\"has 1\"]]\nmutating has => [1,3,4]\nsize Infinity => true\nlengths => [1,1,1,1,1,1,1]");
    eq_s("const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; t('ogb', () => Object.groupBy([1, 2, 3, 4, 5], (x, i) => x % 2 ? 'odd' : 'even')); t('ogb proto', () => Object.getPrototypeOf(Object.groupBy([], x => x))); t('ogb str', () => Object.groupBy('abca', (c) => c)); t('ogb keys', () => Object.keys(Object.groupBy([3, 1, 2, 'b', 'a'], x => x))); t('ogb idx', () => Object.groupBy([1,2], (x, i) => i)); t('ogb null', () => Object.groupBy(null, x => x)); t('ogb nonit', () => Object.groupBy({}, x => x)); t('ogb nf', () => Object.groupBy([1], 1)); t('ogb throw', () => Object.groupBy([1], () => { throw new Error('boom'); })); t('ogb objkey', () => Object.groupBy([1], () => ({ toString() { return 'k'; } }))); t('mgb', () => [...Map.groupBy([1, 2, 3, -0, 0], x => x === 0 ? -0 : x % 2)].map(([k, v]) => [Object.is(k, -0) ? '-0' : k, v])); t('mgb set', () => [...Map.groupBy(new Set([1, 2]), x => x > 1)]); t('mgb nf', () => Map.groupBy([1])); t('goi', () => { const m = new Map([[1, 'a']]); return [m.getOrInsert(1, 'b'), m.getOrInsert(2, 'c'), [...m]]; }); t('goic', () => { const m = new Map([[1, 'a']]); const l = []; return [m.getOrInsertComputed(1, k => { l.push(k); return 'x'; }), m.getOrInsertComputed(-0, k => { l.push(Object.is(k, -0)); return 'y'; }), l, [...m]]; }); t('goic nf', () => new Map().getOrInsertComputed(1, 2)); t('goic nf has', () => new Map([[1,1]]).getOrInsertComputed(1, 2)); t('goic reenter', () => { const m = new Map(); const v = m.getOrInsertComputed(1, k => { m.set(1, 'inner'); m.set(2, 'z'); return 'outer'; }); return [v, [...m]]; }); t('goi nonmap', () => Map.prototype.getOrInsert.call({}, 1, 2)); t('isError', () => [Error.isError(new Error()), Error.isError(new TypeError()), Error.isError({}), Error.isError(Object.create(Error.prototype)), Error.isError(), Error.isError(new AggregateError([]))]); t('isWellFormed', () => ['abc'.isWellFormed(), 'a\\uD800'.isWellFormed(), '\xf0\x9f\x98\x80""'.isWellFormed(), '\\uDE00\\uD83D'.isWellFormed()]); t('toWellFormed', () => ['a\\uD800b'.toWellFormed(), '\\uDE00x\\uD83D'.toWellFormed(), '\xf0\x9f\x98\x80""'.toWellFormed()].map(s => [...s].map(c => c.codePointAt(0)))); t('trimLeft', () => [' a '.trimLeft(), ' a '.trimRight(), String.prototype.trimLeft === String.prototype.trimStart, String.prototype.trimLeft.name, String.prototype.trimRight.name]); t('raw', () => [String.raw({ raw: ['a', 'b', 'c'] }, 1, 2, 3), String.raw({ raw: 'xyz' }, 1, 2), String.raw({ raw: [] }), String.raw({ raw: { length: 2, 0: 'p', 1: 'q' } }, 9)]); t('raw null', () => String.raw(null)); t('raw noraw', () => String.raw({})); t('wellformed null this', () => String.prototype.isWellFormed.call(null)); t('re escape', () => [RegExp.escape('a.b*c'), RegExp.escape('1abc'), RegExp.escape('-_ \\n,=<>#&!%:;@~\\'`\"'), RegExp.escape(' \\uD800x'), RegExp.escape('^$\\\\.*+?()[]{}|/')]); t('re escape nonstr', () => RegExp.escape(1)); t('pwr', () => { const r = Promise.withResolvers(); return [typeof r.promise.then, typeof r.resolve, typeof r.reject, Object.keys(r)]; }); t('proto msg', () => [TypeError.prototype.hasOwnProperty('message'), TypeError.prototype.message, RangeError.prototype.hasOwnProperty('message'), AggregateError.prototype.hasOwnProperty('message'), TypeError.prototype.propertyIsEnumerable('message')]); t('lengths', () => [Object.groupBy.length, Map.groupBy.length, Map.prototype.getOrInsert.length, Map.prototype.getOrInsertComputed.length, Error.isError.length, ''.isWellFormed.length, ''.toWellFormed.length, String.raw.length, RegExp.escape.length, Promise.try.length, Promise.withResolvers.length]); out.join(\"\\n\");",
       "ogb => {\"odd\":[1,3,5],\"even\":[2,4]}\nogb proto => null\nogb str => {\"a\":[\"a\",\"a\"],\"b\":[\"b\"],\"c\":[\"c\"]}\nogb keys => [\"1\",\"2\",\"3\",\"b\",\"a\"]\nogb idx => {\"0\":[1],\"1\":[2]}\nogb null => TypeError: Object.groupBy called on null or undefined\nogb nonit => TypeError: object is not iterable (cannot read property Symbol(Symbol.iterator))\nogb nf => TypeError: 1 is not a function\nogb throw => Error: boom\nogb objkey => {\"k\":[1]}\nmgb => [[1,[1,3]],[0,[2,0,0]]]\nmgb set => [[false,[1]],[true,[2]]]\nmgb nf => TypeError: undefined is not a function\ngoi => [\"a\",\"c\",[[1,\"a\"],[2,\"c\"]]]\ngoic => [\"a\",\"y\",[false],[[1,\"a\"],[0,\"y\"]]]\ngoic nf => TypeError: Map.prototype.getOrInsertComputed is not a function\ngoic nf has => TypeError: Map.prototype.getOrInsertComputed is not a function\ngoic reenter => [\"outer\",[[1,\"outer\"],[2,\"z\"]]]\ngoi nonmap => TypeError: Method Map.prototype.getOrInsert called on incompatible receiver #<Object>\nisError => [true,true,false,false,false,true]\nisWellFormed => [true,false,true,false]\ntoWellFormed => [[97,65533,98],[65533,120,65533],[128512]]\ntrimLeft => [\"a \",\" a\",true,\"trimStart\",\"trimEnd\"]\nraw => [\"a1b2c\",\"x1y2z\",\"\",\"p9q\"]\nraw null => TypeError: Cannot convert undefined or null to object\nraw noraw => TypeError: Cannot convert undefined or null to object\nwellformed null this => TypeError: String.prototype.isWellFormed called on null or undefined\nre escape => [\"\\\\x61\\\\.b\\\\*c\",\"\\\\x31abc\",\"\\\\x2d_\\\\x20\\\\n\\\\x2c\\\\x3d\\\\x3c\\\\x3e\\\\x23\\\\x26\\\\x21\\\\x25\\\\x3a\\\\x3b\\\\x40\\\\x7e\\\\x27\\\\x60\\\\x22\",\"\\\\x20\\\\ud800x\",\"\\\\^\\\\$\\\\\\\\\\\\.\\\\*\\\\+\\\\?\\\\(\\\\)\\\\[\\\\]\\\\{\\\\}\\\\|\\\\/\"]\nre escape nonstr => TypeError: input argument must be a string\npwr => [\"function\",\"function\",\"function\",[\"promise\",\"resolve\",\"reject\"]]\nproto msg => [true,\"\",true,true,false]\nlengths => [2,2,2,2,1,0,0,1,1,1,0]");
    eq_s("const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; t('trim null', () => String.prototype.trim.call(null)); t('slice undef', () => String.prototype.slice.call(undefined, 1)); t('at num', () => String.prototype.at.call(123, 0)); t('match null', () => String.prototype.match.call(null, /a/)); t('split null', () => String.prototype.split.call(null, ',')); t('toString null', () => String.prototype.toString.call(null)); out.join(\"\\n\");",
       "trim null => TypeError: String.prototype.trim called on null or undefined\nslice undef => TypeError: String.prototype.slice called on null or undefined\nat num => \"1\"\nmatch null => TypeError: String.prototype.match called on null or undefined\nsplit null => TypeError: String.prototype.split called on null or undefined\ntoString null => TypeError: String.prototype.toString requires that 'this' be a String");
    eq_s("const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; t('html', () => ['x'.anchor('a\"b'), 'x'.big(), 'x'.blink(), 'x'.bold(), 'x'.fixed(), 'x'.fontcolor('red'), 'x'.fontsize(7), 'x'.italics(), 'x'.link('u'), 'x'.small(), 'x'.strike(), 'x'.sub(), 'x'.sup(), 'x'.anchor()]); t('html null', () => String.prototype.bold.call(null)); t('escape', () => [escape('a b+c/\xc3\xa4""\xe2\x82\xac""\\u{1F600}@*_-.'), escape(), escape(12)]); t('unescape', () => [unescape('%41%u20AC%zz%u12%4'), unescape('a%2'), unescape(escape('\xc3\xa4""\xe2\x82\xac""\\u{1F600} x'))]); t('getYear', () => [new Date(0).getYear(), new Date(NaN).getYear(), new Date(Date.UTC(2024, 0)).getYear()]); t('setYear', () => { const d = new Date(0); return [d.setYear(99), d.getUTCFullYear(), d.setYear(2005), d.getUTCFullYear(), d.setYear(-1), d.setYear(NaN), new Date(NaN).setYear(50)]; }); t('lengths', () => [''.anchor.length, ''.big.length, ''.link.length, escape.length, unescape.length, new Date().getYear.length, new Date().setYear.length]); out.join(\"\\n\");",
       "html => [\"<a name=\\\"a&quot;b\\\">x</a>\",\"<big>x</big>\",\"<blink>x</blink>\",\"<b>x</b>\",\"<tt>x</tt>\",\"<font color=\\\"red\\\">x</font>\",\"<font size=\\\"7\\\">x</font>\",\"<i>x</i>\",\"<a href=\\\"u\\\">x</a>\",\"<small>x</small>\",\"<strike>x</strike>\",\"<sub>x</sub>\",\"<sup>x</sup>\",\"<a name=\\\"undefined\\\">x</a>\"]\nhtml null => TypeError: String.prototype.bold called on null or undefined\nescape => [\"a%20b+c/%E4%u20AC%uD83D%uDE00@*_-.\",\"undefined\",\"12\"]\nunescape => [\"A\xe2\x82\xac""%zz%u12%4\",\"a%2\",\"\xc3\xa4""\xe2\x82\xac""\xf0\x9f\x98\x80"" x\"]\ngetYear => [70,null,124]\nsetYear => [915148800000,1999,1104537600000,2005,-62198755200000,null,-631152000000]\nlengths => [1,0,1,1,1,0,1]");
    eq_s("const out = []; const t = (s, f) => { let v; try { v = s + ' => ' + JSON.stringify(f()); } catch (e) { v = s + ' => ' + e.constructor.name + ': ' + e.message; } out.push(v); }; t('setMonth nan', () => { const d = new Date(NaN); return [d.setMonth(1), d.getTime()]; }); t('setHours nan', () => { const d = new Date(NaN); return [d.setHours(1), d.getTime()]; }); t('setFullYear nan', () => { const d = new Date(NaN); return [d.setFullYear(2020), d.getTime()]; }); t('setDate nan', () => new Date(NaN).setDate(3)); out.join(\"\\n\");",
       "setMonth nan => [null,null]\nsetHours nan => [null,null]\nsetFullYear nan => [1577836800000,1577836800000]\nsetDate nan => null");
    eq_s("const cases=[[0,0,17],[-0,101,1],[1,50,21],[-1,30,undefined],[0.5,15,3],[1.5,undefined,10],[2.5,undefined,6],[1.005,21,50],[1.45,21,1],[0.1,1.7,17],[0.000001,2,2],[1e-7,-1,1],[123.456,-1,undefined],[1e+21,100,101],[-1e+21,20,6],[9.995,10,50],[999.9999,undefined,0],[1e-300,101,6],[5e-324,101,10],[1.7976931348623157e+308,-1,undefined],[NaN,101,100],[Infinity,100,3],[-Infinity,15,21],[25,50,undefined],[0.000123,undefined,undefined],[1234567890123456800,30,undefined],[9007199254740992,101,17],[5e-7,30,6],[4.35,5,undefined],[0.045,undefined,0],[1.25e-7,21,21],[0.5,2,undefined],[0.05,100,101],[0.25,101,undefined],[2.675,undefined,0],[0.000001,3,0],[999999999999999900000,-1,21],[0.0000035,20,3],[-6.134154796600341e+28,101,10],[-2.8983688354492188e+26,21,21],[26760482788085936,50,50],[3.8101583719253545e-33,1,100],[-7.370188236236572e+36,50,50],[-14392042160.03418,10,3],[7.619290351867676e+26,20,50],[-0.00041489362716674806,101,3],[4.620862603187561e-27,3,6],[9.582369327545166e+29,15,21],[3.102025985717773e-17,3,10],[-3.9670658111572264e+21,undefined,2],[0.00019032955169677736,100,undefined],[-8.583765029907227e-7,-1,100],[6.146221160888671e+38,30,0],[-1.2259859591722488e-41,5,3],[692733764648437.5,1,100],[-0.19724273681640625,3,0],[9.21630859375e+37,3,2],[-1.2398529052734375e-18,21,undefined],[7.152252197265625e-12,21,3],[8.674850463867188e-18,1.7,10],[5.46173095703125e-22,undefined,10],[4.060745239257812e-21,100,100],[-5.503692626953125e-11,21,17],[957823514.9383545,1.7,0],[9.777832031250001e+29,21,100],[-4.779891967773438e+37,21,6],[-7051849365234375,21,50],[-0.7509472370147705,30,undefined],[-3.4517335891723636e-11,50,0],[8095698356.628418,2,1],[-8.466916084289551e-20,15,10],[-93068099021.91162,10,3],[-1.032036542892456e-39,10,101],[-666851.0437011719,100,10],[-9.504977464675904e-36,1,0],[-83346581459045400,21,6],[-9.702622890472412e-26,10,1],[2.0809715986251834e-32,101,50],[9629783630371.094,3,2],[-19851303.100585938,15,21],[0.002081718444824219,15,100],[-0.009453849792480469,50,101],[6.597347417846321e-41,15,101],[-8.577248454093934e-33,100,1],[9.13057029247284e-38,15,undefined],[-1.7834466695785523e-28,20,100],[-1.983873248100281e-36,2,0],[4.1403770446777344e+23,5,17],[8.223873376846314e-35,50,0],[59035992622375490,20,6],[-7.764518260955811e+24,20,50],[-4.535751342773438e+34,1.7,17],[3.582916259765625e+21,100,17],[-3.6054223775863645e-25,30,10],[713836.4315032959,2,3],[-5.6117463111877444e+23,50,undefined],[-9.48834776878357e-23,101,6],[3.7080907821655272e+28,2,101],[1.2932729721069337e+30,undefined,1],[-90.3994083404541,21,10],[-7.902143001556397e-19,20,21],[-3.454122543334961e+37,20,17],[-9.462270736694336e-20,-1,0],[9.97584342956543e+37,50,undefined],[-3.8373279571533204e+24,15,undefined],[-0.000046890735626220705,-1,100],[6.658887863159179e-7,15,6],[2.2608816623687744e-36,3,1],[6.668651103973389e-9,15,21],[8.44733715057373e-15,-1,0],[-5.28573215007782e-31,50,2],[-4.896448254585266e-36,0,17],[482540607452392600,50,undefined],[-3.6005353927612306e+25,1,21],[235920429229736.3,undefined,2],[-90785026.55029297,1.7,0],[0.00009281430244445801,20,21],[-1.4211220294237135e-41,30,1],[6.518616676330567e-19,undefined,0],[-1.7865657806396485e-8,3,0],[-169312953.9489746,50,10],[77051.78260803223,3,2],[8.414216041564941e-17,undefined,1],[-9.03134822845459e-15,15,3],[-1.9950366020202638e+26,undefined,0],[-2.941596508026123e-20,0,1],[-7.008121013641357e+34,1,50],[-2408962249.7558594,50,17],[-4.071025252342224e-30,101,17],[-5.1776713132858276e-36,101,6],[-9.609630703926087e-33,10,50],[0.0008968073129653931,100,2],[-3.450946807861328e-14,100,100],[-7.67862319946289e+30,15,3],[0.000004247634410858154,-1,100],[-7.024128437042236e+29,0,undefined],[-3.291401863098145e+26,30,undefined],[8.484983444213868e+24,undefined,21],[-8.036198616027832e-8,15,100],[-8.476006984710693e-20,5,1],[-5.2235126495361335e-19,15,101],[0.000008562562465667726,3,10],[4.0169501304626464e-13,30,2],[-1.9930481910705566e+28,101,2],[6.283299922943115e-13,1.7,1],[-4.521006345748901e-27,100,6],[1.981043815612793e-12,5,0],[5.353732705116271e-35,3,6],[-2.0453715324401857e-10,50,100],[9.86893951892853e-32,0,101],[-8.247727751731871e-37,50,1],[6.919145584106446e+35,1.7,3],[-1386675834655.7617,10,21],[9.013676643371582e-15,undefined,0],[-4.532194137573242e+35,50,0],[-2.3874616622924805e+29,30,17],[-0.000006049304008483887,20,100],[-2269065380096435500,undefined,100],[7474.83491897583,21,100],[6.609756946563721e+26,101,10],[3.4425854682922365e-16,101,3],[8.21220874786377e+23,-1,1],[-6.599795222282409e-22,100,2],[4.6293699741363527e-8,30,2],[9452.018737792969,5,21],[-8.85356843471527e-29,101,6],[0.003665555715560913,2,17],[7.761059403419495e-27,50,50],[2.9668569564819335e+34,15,50],[-2.825891971588135e+31,30,50],[2.301499843597412e+33,10,100],[47256231307983.4,20,6],[1.9009184837341307e-15,2,101],[8.919882774353029e+23,1.7,undefined],[-3610136508.9416504,21,undefined],[9.39918279647827e+22,undefined,100],[-7.717993259429932e+24,50,undefined],[2.6760339736938476e-12,undefined,3],[-8.630309104919433e+23,20,3],[-4.319521188735962e-27,101,10],[-9.836574792861939e-13,3,50],[-9.170442819595336e-12,1.7,2],[-41713786125183110000,50,10],[-9.064929485321044e+32,100,6],[4.0089607238769534e-9,1,17],[6.198883056640625e-23,undefined,100],[4833393.096923828,30,3],[8.993473052978516e+29,undefined,undefined],[0.0002794055938720703,undefined,21],[-396505355834960.94,0,100],[5.195637345314026e-37,101,100],[-3.2718491554260254e-9,101,0],[1.5599799156188964e-11,101,undefined],[-0.000003568670749664307,1.7,17],[3499181270599.365,0,21],[6.121690273284912e-13,10,3],[878005.7430267334,-1,1],[8.875405788421632e+24,2,10],[-4.5993441343307496e-29,-1,1],[-1.9072574377059939e-38,101,17],[57752.72846221924,101,100],[-3.7941932678222654e+37,1.7,101],[-2.0188236236572266e+33,101,0],[-6.031274199485778e-24,1.7,undefined],[9.88438069820404e-25,10,101],[3.040213584899902e+30,5,50],[7.755422592163086e-7,0,6],[5.5827093124389646e+23,20,6],[-6.910843253135682e-23,30,0],[4.524258971214294e-24,15,101],[1.898834705352783e+27,50,10],[-46026873588562010000,1.7,10],[9.231637120246886e-35,15,undefined],[7.325754165649414e-7,10,101],[82841587066.65039,3,2],[-6942739486694335000,2,2],[6.744917631149292e-24,-1,10],[-1.6779303550720216e-35,5,21],[3235437870.025635,-1,3],[1.2338095903396607e-26,1,1],[9.951325058937073e-32,100,2],[-7.20684953033924e-40,100,17],[5.621233582496643e-22,5,17],[3.8718110322952273e-26,1,10],[0.05635323524475098,5,undefined],[0.09126343727111817,21,undefined],[6.639343500137329e-27,undefined,undefined],[-6.334550380706788e-14,-1,undefined],[941458702087402400,20,2],[52580928802.490234,-1,17],[3.201208710670471e-30,-1,101],[5.20865261554718e-23,100,undefined],[-7.853732109069824e+27,2,0],[4.925465583801269e+27,0,2],[1.2331509590148926e-17,15,6],[9084503650.665283,2,50],[-2.2949475049972538e-34,30,21],[462102.89001464844,101,50],[251288414001464.84,30,100],[7.844915390014648e-12,20,2],[-2.2918033599853514e+33,30,101],[3.3527284860610964e-27,1,50],[-5.366146564483643e-15,2,0],[-0.00009925961494445802,2,101],[4.14616584777832e+37,100,undefined],[73375.41580200195,5,undefined],[-7.71130919456482e-24,1.7,10],[9.541332721710205e-26,0,100],[8410.439491271973,0,101],[6.685395240783692e-18,0,1],[-7.58878231048584e+27,1,21],[1.1931347846984862e+36,21,100],[22851490974426268,101,6],[1.0189175605773925e+23,-1,101],[6524.968147277832,101,101],[890683412.5518799,3,3],[71063923.8357544,0,21],[9.944775700569153e-33,101,10],[9.480696320533753e-27,20,100],[30597496032.714844,5,1],[3.6398130655288694e-29,100,undefined],[-8.449804782867432e+26,2,2],[-5101675987243652000,50,50],[6.272645592689514e-29,15,17],[5.4121613502502445e-9,5,1],[-7.563741207122802e-8,-1,1],[8.230409622192383e+33,0,6],[3.409700989723205e-39,-1,0],[-580004930496.2158,15,101],[5.832611247897148e-41,10,21],[9.173119589686393e-41,100,6],[9.733079075813293e-36,5,101],[-617.518424987793,-1,3],[-0.007564127445220947,101,21],[-7.662196159362793e+35,1,undefined],[-3.2828974723815915e+30,1,17],[6.706059575080872e-36,101,3],[1.2709712982177733e-22,10,1],[-37278079986572.266,1.7,50],[-47463417053.22266,2,10],[-5.766382217407226e-13,10,21],[-5.528584718704224e-23,100,50],[-0.009537508487701416,101,10],[934028387069702100,1,6],[406636953353881.8,undefined,0],[9.006536602973939e-33,undefined,undefined],[-5.047593116760254e+23,21,21],[-71355676651000980000,100,3],[0.0006485223770141602,50,6],[-3.845650553703308e-30,2,50],[8.971290588378906e+27,101,10],[-1.5565866231918334e-35,20,undefined],[5.669003725051879e-38,1.7,2],[4571852684020.996,0,21],[-6.25079870223999e-25,1.7,3],[1876308917999.2676,-1,3],[-7.524535655975341e-20,15,17],[-3.2633578777313236e-33,21,6],[7.68044650554657e-33,50,10],[-4.351620674133301e+28,30,10],[6.45911693572998e+21,-1,3],[9.106097221374511e+30,1.7,3],[6.589808464050293e+23,15,undefined],[521152.0195007324,100,undefined],[7854533195495605000,50,10],[85154294967651.38,21,50],[-1.9146776199340818e+28,100,0],[-8.618893623352051e-13,30,10],[3.1730496883392334e-35,15,17],[5740308761596680,101,6],[9.712781906127929e+36,50,undefined],[-2396287918090820000,15,undefined],[-1.8173688650131227e-38,30,21],[-9.144336581230163e-35,101,100],[-6.671007871627807e-13,1.7,10],[-3.0698204040527343e-25,21,10],[-713300704956054700,3,3],[6.908423900604248e-9,15,6],[6.323612332344055e-29,15,3],[5.388913750648499e-33,0,17],[-1.238226890563965e-11,undefined,10],[8.023169040679931e-22,10,6],[-7.0407485961914065e+37,-1,undefined],[6.292827129364013e+30,20,100],[4.898841977119446e-36,101,10],[-185845851898193340,5,17],[2.4159485101699832e-33,-1,undefined],[9.033187031745911e-33,2,3],[-8.457794189453125,21,3],[-3.1024277210235594e-30,undefined,undefined],[-2.4738889932632446e-28,2,21],[-91420.53127288818,0,1],[6638517379760.742,2,3],[5.0012499094009396e-30,1,101],[-0.00005962803363800049,2,6],[-1725816726684570400,21,1],[-5.308193564414978e-29,2,21],[-1.836196184158325e-8,15,17],[-2.107468843460083e-10,3,1],[-14944171905.517578,5,100],[-1.7693805694580078e+30,30,10],[9.203214645385741e+28,5,21],[4919462203979492,21,6],[-0.0007530794143676758,-1,101],[4.0323805809021e-20,undefined,101],[-954277276992797800,100,17],[-8.751528263092041e+34,undefined,100],[57087421417236330,101,1],[9.009556770324707e-8,10,0],[-3.3076810836791994e+25,2,21],[3.5971879959106444e-7,1.7,6],[1.167035698890686e-31,30,17],[32586693763732910000,5,101],[4.552023410797119e-21,undefined,17],[-7.956349849700927e+36,1,100],[8.803377151489259e+24,50,6],[-7.756833434104919e-25,30,undefined],[7.753395438194276e-31,30,10],[5.809237957000733e+25,3,100],[4.102447032928466e+28,101,1],[3864009380.340576,-1,101],[6.16204023361206e-9,50,10],[-7.47636079788208e-9,-1,2],[541560649.8718262,100,10],[5.182033181190491e-32,0,undefined],[5.8441472053527835e-19,0,17],[9.655125141143798e-21,3,101],[-4.683418273925781e+21,50,2],[-8.543205261230468e-17,1,17],[5.683315992355347e-29,20,100],[0.009682040214538574,-1,3],[0.00030403852462768555,0,1],[-9.938855171203613e+38,30,21],[44.72017288208008,5,21],[-2.0808690786361696e-28,100,101],[-2.544281482696533e+34,100,17],[5.332515239715577e-16,21,17],[-5269.844532012939,30,3],[-0.07029199600219727,101,50],[-1.5503549575805665e+31,101,0],[5.19162654876709e+35,101,1],[-60214090347.29004,21,undefined],[-3.358646631240845e-26,10,undefined],[-0.036761903762817384,3,21],[-4.456639289855958e-16,10,17],[-98217439.65148926,undefined,10],[-6516404.151916504,5,6],[9.97936487197876e-18,50,101],[-9.061097502708435e-35,50,1],[-1.000971794128418e+36,-1,10],[-949130535125732400000,0,100],[6.64156436920166e+38,15,21],[-5.1793044805526735e-33,0,100],[6.770401000976562e+27,30,21],[0.0004478912353515625,0,2],[-577583312988.2812,100,undefined],[2.1362948417663573e+33,1.7,undefined],[-3.6659550666809084e+21,21,101],[6.697413921356202e-16,1.7,21],[78101420402526850,undefined,10]]; const show = (f) => { try { return f(); } catch (e) { return e.constructor.name + \": \" + e.message; } }; const out=[]; for (const [x,d,p] of cases) out.push(String(x) + \" \" + d + \" \" + show(() => x.toFixed(d)) + \" \" + show(() => x.toExponential(d)) + \" \" + p + \" \" + show(() => x.toPrecision(p))); out.join(\"\\n\");",
       "0 0 0 0e+0 17 0.0000000000000000\n0 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 1 0\n1 50 1.00000000000000000000000000000000000000000000000000 1.00000000000000000000000000000000000000000000000000e+0 21 1.00000000000000000000\n-1 30 -1.000000000000000000000000000000 -1.000000000000000000000000000000e+0 undefined -1\n0.5 15 0.500000000000000 5.000000000000000e-1 3 0.500\n1.5 undefined 2 1.5e+0 10 1.500000000\n2.5 undefined 3 2.5e+0 6 2.50000\n1.005 21 1.004999999999999893419 1.004999999999999893419e+0 50 1.0049999999999998934185896359849721193313598632813\n1.45 21 1.449999999999999955591 1.449999999999999955591e+0 1 1\n0.1 1.7 0.1 1.0e-1 17 0.10000000000000001\n0.000001 2 0.00 1.00e-6 2 0.0000010\n1e-7 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 1 1e-7\n123.456 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 undefined 123.456\n1e+21 100 1e+21 1.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000e+21 101 RangeError: toPrecision() argument must be between 1 and 100\n-1e+21 20 -1e+21 -1.00000000000000000000e+21 6 -1.00000e+21\n9.995 10 9.9950000000 9.9950000000e+0 50 9.9949999999999992184029906638897955417633056640625\n999.9999 undefined 1000 9.999999e+2 0 RangeError: toPrecision() argument must be between 1 and 100\n1e-300 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 6 1.00000e-300\n5e-324 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 10 4.940656458e-324\n1.7976931348623157e+308 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 undefined 1.7976931348623157e+308\nNaN 101 RangeError: toFixed() digits argument must be between 0 and 100 NaN 100 NaN\nInfinity 100 Infinity Infinity 3 Infinity\n-Infinity 15 -Infinity -Infinity 21 -Infinity\n25 50 25.00000000000000000000000000000000000000000000000000 2.50000000000000000000000000000000000000000000000000e+1 undefined 25\n0.000123 undefined 0 1.23e-4 undefined 0.000123\n1234567890123456800 30 1234567890123456768.000000000000000000000000000000 1.234567890123456768000000000000e+18 undefined 1234567890123456800\n9007199254740992 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 17 9007199254740992.0\n5e-7 30 0.000000499999999999999977374056 4.999999999999999773740559129431e-7 6 5.00000e-7\n4.35 5 4.35000 4.35000e+0 undefined 4.35\n0.045 undefined 0 4.5e-2 0 RangeError: toPrecision() argument must be between 1 and 100\n1.25e-7 21 0.000000125000000000000 1.249999999999999943435e-7 21 1.24999999999999994344e-7\n0.5 2 0.50 5.00e-1 undefined 0.5\n0.05 100 0.0500000000000000027755575615628913510590791702270507812500000000000000000000000000000000000000000000 5.0000000000000002775557561562891351059079170227050781250000000000000000000000000000000000000000000000e-2 101 RangeError: toPrecision() argument must be between 1 and 100\n0.25 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 undefined 0.25\n2.675 undefined 3 2.675e+0 0 RangeError: toPrecision() argument must be between 1 and 100\n0.000001 3 0.000 1.000e-6 0 RangeError: toPrecision() argument must be between 1 and 100\n999999999999999900000 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 21 999999999999999868928\n0.0000035 20 0.00000350000000000000 3.49999999999999994750e-6 3 0.00000350\n-6.134154796600341e+28 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 10 -6.134154797e+28\n-2.8983688354492188e+26 21 -2.8983688354492188e+26 -2.898368835449218791752e+26 21 -2.89836883544921879175e+26\n26760482788085936 50 26760482788085936.00000000000000000000000000000000000000000000000000 2.67604827880859360000000000000000000000000000000000e+16 50 26760482788085936.000000000000000000000000000000000\n3.8101583719253545e-33 1 0.0 3.8e-33 100 3.810158371925354522139135612772155469945327825952548001852215233825287499304998083615118181777248196e-33\n-7.370188236236572e+36 50 -7.370188236236572e+36 -7.37018823623657183730266162456218828800000000000000e+36 50 -7370188236236571837302661624562188288.0000000000000\n-14392042160.03418 10 -14392042160.0341796875 -1.4392042160e+10 3 -1.44e+10\n7.619290351867676e+26 20 7.619290351867676e+26 7.61929035186767578997e+26 50 761929035186767578997456896.00000000000000000000000\n-0.00041489362716674806 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 3 -0.000415\n4.620862603187561e-27 3 0.000 4.621e-27 6 4.62086e-27\n9.582369327545166e+29 15 9.582369327545166e+29 9.582369327545166e+29 21 9.58236932754516629186e+29\n3.102025985717773e-17 3 0.000 3.102e-17 10 3.102025986e-17\n-3.9670658111572264e+21 undefined -3.9670658111572264e+21 -3.9670658111572264e+21 2 -4.0e+21\n0.00019032955169677736 100 0.0001903295516967773600130325872825665101117920130491256713867187500000000000000000000000000000000000 1.9032955169677736001303258728256651011179201304912567138671875000000000000000000000000000000000000000e-4 undefined 0.00019032955169677736\n-8.583765029907227e-7 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 100 -8.583765029907226676408990746758309597908009891398251056671142578125000000000000000000000000000000000e-7\n6.146221160888671e+38 30 6.146221160888671e+38 6.146221160888671163811426313198e+38 0 RangeError: toPrecision() argument must be between 1 and 100\n-1.2259859591722488e-41 5 -0.00000 -1.22599e-41 3 -1.23e-41\n692733764648437.5 1 692733764648437.5 6.9e+14 100 692733764648437.5000000000000000000000000000000000000000000000000000000000000000000000000000000000000\n-0.19724273681640625 3 -0.197 -1.972e-1 0 RangeError: toPrecision() argument must be between 1 and 100\n9.21630859375e+37 3 9.21630859375e+37 9.216e+37 2 9.2e+37\n-1.2398529052734375e-18 21 -0.000000000000000001240 -1.239852905273437547924e-18 undefined -1.2398529052734375e-18\n7.152252197265625e-12 21 0.000000000007152252197 7.152252197265624835159e-12 3 7.15e-12\n8.674850463867188e-18 1.7 0.0 8.7e-18 10 8.674850464e-18\n5.46173095703125e-22 undefined 0 5.46173095703125e-22 10 5.461730957e-22\n4.060745239257812e-21 100 0.0000000000000000000040607452392578119044866909524703779631697690319387916138213490135910888056969270 4.0607452392578119044866909524703779631697690319387916138213490135910888056969270110130310058593750000e-21 100 4.060745239257811904486690952470377963169769031938791613821349013591088805696927011013031005859375000e-21\n-5.503692626953125e-11 21 -0.000000000055036926270 -5.503692626953125219306e-11 17 -5.5036926269531252e-11\n957823514.9383545 1.7 957823514.9 9.6e+8 0 RangeError: toPrecision() argument must be between 1 and 100\n9.777832031250001e+29 21 9.777832031250001e+29 9.777832031250000825961e+29 100 977783203125000082596051288064.0000000000000000000000000000000000000000000000000000000000000000000000\n-4.779891967773438e+37 21 -4.779891967773438e+37 -4.779891967773437568403e+37 6 -4.77989e+37\n-7051849365234375 21 -7051849365234375.000000000000000000000 -7.051849365234375000000e+15 50 -7051849365234375.0000000000000000000000000000000000\n-0.7509472370147705 30 -0.750947237014770507812500000000 -7.509472370147705078125000000000e-1 undefined -0.7509472370147705\n-3.4517335891723636e-11 50 -0.00000000003451733589172363556974709152341753044813 -3.45173358917236355697470915234175304481256674193901e-11 0 RangeError: toPrecision() argument must be between 1 and 100\n8095698356.628418 2 8095698356.63 8.10e+9 1 8e+9\n-8.466916084289551e-20 15 -0.000000000000000 -8.466916084289551e-20 10 -8.466916084e-20\n-93068099021.91162 10 -93068099021.9116210938 -9.3068099022e+10 3 -9.31e+10\n-1.032036542892456e-39 10 -0.0000000000 -1.0320365429e-39 101 RangeError: toPrecision() argument must be between 1 and 100\n-666851.0437011719 100 -666851.0437011718750000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 -6.6685104370117187500000000000000000000000000000000000000000000000000000000000000000000000000000000000e+5 10 -666851.0437\n-9.504977464675904e-36 1 -0.0 -9.5e-36 0 RangeError: toPrecision() argument must be between 1 and 100\n-83346581459045400 21 -83346581459045408.000000000000000000000 -8.334658145904540800000e+16 6 -8.33466e+16\n-9.702622890472412e-26 10 -0.0000000000 -9.7026228905e-26 1 -1e-25\n2.0809715986251834e-32 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 50 2.0809715986251833567504513763421355088642161148001e-32\n9629783630371.094 3 9629783630371.094 9.630e+12 2 9.6e+12\n-19851303.100585938 15 -19851303.100585937500000 -1.985130310058594e+7 21 -19851303.1005859375000\n0.002081718444824219 15 0.002081718444824 2.081718444824219e-3 100 0.002081718444824218888777878078144567552953958511352539062500000000000000000000000000000000000000000000\n-0.009453849792480469 50 -0.00945384979248046868061106096092771622352302074432 -9.45384979248046868061106096092771622352302074432373e-3 101 RangeError: toPrecision() argument must be between 1 and 100\n6.597347417846321e-41 15 0.000000000000000 6.597347417846321e-41 101 RangeError: toPrecision() argument must be between 1 and 100\n-8.577248454093934e-33 100 -0.0000000000000000000000000000000085772484540939337014114291242263571703509536912746351609540445304150 -8.5772484540939337014114291242263571703509536912746351609540445304149814000208219182466717889212759474e-33 1 -9e-33\n9.13057029247284e-38 15 0.000000000000000 9.130570292472840e-38 undefined 9.13057029247284e-38\n-1.7834466695785523e-28 20 -0.00000000000000000000 -1.78344666957855225684e-28 100 -1.783446669578552256835969723546722704081951860153996788727806965755164054056661204761269345908658579e-28\n-1.983873248100281e-36 2 -0.00 -1.98e-36 0 RangeError: toPrecision() argument must be between 1 and 100\n4.1403770446777344e+23 5 4.1403770446777344e+23 4.14038e+23 17 4.1403770446777344e+23\n8.223873376846314e-35 50 0.00000000000000000000000000000000008223873376846314 8.22387337684631390916735785829961361876661530346164e-35 0 RangeError: toPrecision() argument must be between 1 and 100\n59035992622375490 20 59035992622375488.00000000000000000000 5.90359926223754880000e+16 6 5.90360e+16\n-7.764518260955811e+24 20 -7.764518260955811e+24 -7.76451826095581124205e+24 50 -7764518260955811242049536.0000000000000000000000000\n-4.535751342773438e+34 1.7 -4.535751342773438e+34 -4.5e+34 17 -4.5357513427734377e+34\n3.582916259765625e+21 100 3.582916259765625e+21 3.5829162597656250286080000000000000000000000000000000000000000000000000000000000000000000000000000000e+21 17 3.5829162597656250e+21\n-3.6054223775863645e-25 30 -0.000000000000000000000000360542 -3.605422377586364497210374549181e-25 10 -3.605422378e-25\n713836.4315032959 2 713836.43 7.14e+5 3 7.14e+5\n-5.6117463111877444e+23 50 -5.6117463111877444e+23 -5.61174631118774437150720000000000000000000000000000e+23 undefined -5.6117463111877444e+23\n-9.48834776878357e-23 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 6 -9.48835e-23\n3.7080907821655272e+28 2 3.7080907821655272e+28 3.71e+28 101 RangeError: toPrecision() argument must be between 1 and 100\n1.2932729721069337e+30 undefined 1.2932729721069337e+30 1.2932729721069337e+30 1 1e+30\n-90.3994083404541 21 -90.399408340454101562500 -9.039940834045410156250e+1 10 -90.39940834\n-7.902143001556397e-19 20 -0.00000000000000000079 -7.90214300155639725909e-19 21 -7.90214300155639725909e-19\n-3.454122543334961e+37 20 -3.454122543334961e+37 -3.45412254333496081357e+37 17 -3.4541225433349608e+37\n-9.462270736694336e-20 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 0 RangeError: toPrecision() argument must be between 1 and 100\n9.97584342956543e+37 50 9.97584342956543e+37 9.97584342956542921035752126372611358720000000000000e+37 undefined 9.97584342956543e+37\n-3.8373279571533204e+24 15 -3.8373279571533204e+24 -3.837327957153320e+24 undefined -3.8373279571533204e+24\n-0.000046890735626220705 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 100 -0.00004689073562622070469709315010398142931080656126141548156738281250000000000000000000000000000000000000\n6.658887863159179e-7 15 0.000000665888786 6.658887863159179e-7 6 6.65889e-7\n2.2608816623687744e-36 3 0.000 2.261e-36 1 2e-36\n6.668651103973389e-9 15 0.000000006668651 6.668651103973389e-9 21 6.66865110397338902029e-9\n8.44733715057373e-15 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 0 RangeError: toPrecision() argument must be between 1 and 100\n-5.28573215007782e-31 50 -0.00000000000000000000000000000052857321500778201364 -5.28573215007782013636992725518538257843806625042818e-31 2 -5.3e-31\n-4.896448254585266e-36 0 -0 -5e-36 17 -4.8964482545852661e-36\n482540607452392600 50 482540607452392576.00000000000000000000000000000000000000000000000000 4.82540607452392576000000000000000000000000000000000e+17 undefined 482540607452392600\n-3.6005353927612306e+25 1 -3.6005353927612306e+25 -3.6e+25 21 -3.60053539276123056909e+25\n235920429229736.3 undefined 235920429229736 2.359204292297363e+14 2 2.4e+14\n-90785026.55029297 1.7 -90785026.6 -9.1e+7 0 RangeError: toPrecision() argument must be between 1 and 100\n0.00009281430244445801 20 0.00009281430244445801 9.28143024444580102194e-5 21 0.0000928143024444580102194\n-1.4211220294237135e-41 30 -0.000000000000000000000000000000 -1.421122029423713497955369884257e-41 1 -1e-41\n6.518616676330567e-19 undefined 0 6.518616676330567e-19 0 RangeError: toPrecision() argument must be between 1 and 100\n-1.7865657806396485e-8 3 -0.000 -1.787e-8 0 RangeError: toPrecision() argument must be between 1 and 100\n-169312953.9489746 50 -169312953.94897460937500000000000000000000000000000000000000 -1.69312953948974609375000000000000000000000000000000e+8 10 -169312953.9\n77051.78260803223 3 77051.783 7.705e+4 2 7.7e+4\n8.414216041564941e-17 undefined 0 8.414216041564941e-17 1 8e-17\n-9.03134822845459e-15 15 -0.000000000000009 -9.031348228454591e-15 3 -9.03e-15\n-1.9950366020202638e+26 undefined -1.9950366020202638e+26 -1.9950366020202638e+26 0 RangeError: toPrecision() argument must be between 1 and 100\n-2.941596508026123e-20 0 -0 -3e-20 1 -3e-20\n-7.008121013641357e+34 1 -7.008121013641357e+34 -7.0e+34 50 -70081210136413572632248115737395200.000000000000000\n-2408962249.7558594 50 -2408962249.75585937500000000000000000000000000000000000000000 -2.40896224975585937500000000000000000000000000000000e+9 17 -2408962249.7558594\n-4.071025252342224e-30 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 17 -4.0710252523422238e-30\n-5.1776713132858276e-36 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 6 -5.17767e-36\n-9.609630703926087e-33 10 -0.0000000000 -9.6096307039e-33 50 -9.6096307039260874650742027044950077428011959107013e-33\n0.0008968073129653931 100 0.0008968073129653930837534847597680709441192448139190673828125000000000000000000000000000000000000000 8.9680731296539308375348475976807094411924481391906738281250000000000000000000000000000000000000000000e-4 2 0.00090\n-3.450946807861328e-14 100 -0.0000000000000345094680786132809422789738508631523749279647328425113528282963670790195465087890625000 -3.4509468078613280942278973850863152374927964732842511352828296367079019546508789062500000000000000000e-14 100 -3.450946807861328094227897385086315237492796473284251135282829636707901954650878906250000000000000000e-14\n-7.67862319946289e+30 15 -7.67862319946289e+30 -7.678623199462890e+30 3 -7.68e+30\n0.000004247634410858154 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 100 0.000004247634410858154372498101530863934272019832860678434371948242187500000000000000000000000000000000000\n-7.024128437042236e+29 0 -7.024128437042236e+29 -7e+29 undefined -7.024128437042236e+29\n-3.291401863098145e+26 30 -3.291401863098145e+26 -3.291401863098144711109509120000e+26 undefined -3.291401863098145e+26\n8.484983444213868e+24 undefined 8.484983444213868e+24 8.484983444213868e+24 21 8.48498344421386803479e+24\n-8.036198616027832e-8 15 -0.000000080361986 -8.036198616027832e-8 100 -8.036198616027831792759403371079196531923116708640009164810180664062500000000000000000000000000000000e-8\n-8.476006984710693e-20 5 -0.00000 -8.47601e-20 1 -8e-20\n-5.2235126495361335e-19 15 -0.000000000000000 -5.223512649536133e-19 101 RangeError: toPrecision() argument must be between 1 and 100\n0.000008562562465667726 3 0.000 8.563e-6 10 0.000008562562466\n4.0169501304626464e-13 30 0.000000000000401695013046264638 4.016950130462646375809238254719e-13 2 4.0e-13\n-1.9930481910705566e+28 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 2 -2.0e+28\n6.283299922943115e-13 1.7 0.0 6.3e-13 1 6e-13\n-4.521006345748901e-27 100 -0.0000000000000000000000000045210063457489012565110874779564107394545855396114890958881611726613060104 -4.5210063457489012565110874779564107394545855396114890958881611726613060104479346890116175927687436342e-27 6 -4.52101e-27\n1.981043815612793e-12 5 0.00000 1.98104e-12 0 RangeError: toPrecision() argument must be between 1 and 100\n5.353732705116271e-35 3 0.000 5.354e-35 6 5.35373e-35\n-2.0453715324401857e-10 50 -0.00000000020453715324401856831786154298759511718786 -2.04537153244018568317861542987595117187860083163287e-10 100 -2.045371532440185683178615429875951171878600831632866174913942813873291015625000000000000000000000000e-10\n9.86893951892853e-32 0 0 1e-31 101 RangeError: toPrecision() argument must be between 1 and 100\n-8.247727751731871e-37 50 -0.00000000000000000000000000000000000082477277517319 -8.24772775173187134561085487907382368854167309201759e-37 1 -8e-37\n6.919145584106446e+35 1.7 6.919145584106446e+35 6.9e+35 3 6.92e+35\n-1386675834655.7617 10 -1386675834655.7617187500 -1.3866758347e+12 21 -1386675834655.76171875\n9.013676643371582e-15 undefined 0 9.013676643371582e-15 0 RangeError: toPrecision() argument must be between 1 and 100\n-4.532194137573242e+35 50 -4.532194137573242e+35 -4.53219413757324187543281137174446080000000000000000e+35 0 RangeError: toPrecision() argument must be between 1 and 100\n-2.3874616622924805e+29 30 -2.3874616622924805e+29 -2.387461662292480475127653335040e+29 17 -2.3874616622924805e+29\n-0.000006049304008483887 20 -0.00000604930400848389 -6.04930400848388702477e-6 100 -0.000006049304008483887024766063184033626498603553045541048049926757812500000000000000000000000000000000000\n-2269065380096435500 undefined -2269065380096435456 -2.2690653800964355e+18 100 -2269065380096435456.000000000000000000000000000000000000000000000000000000000000000000000000000000000\n7474.83491897583 21 7474.834918975830078125000 7.474834918975830078125e+3 100 7474.834918975830078125000000000000000000000000000000000000000000000000000000000000000000000000000000\n6.609756946563721e+26 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 10 6.609756947e+26\n3.4425854682922365e-16 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 3 3.44e-16\n8.21220874786377e+23 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 1 8e+23\n-6.599795222282409e-22 100 -0.0000000000000000000006599795222282409100265622612395359566334497050211419492569617228916456497245235 -6.5997952222824091002656226123953595663344970502114194925696172289164564972452353686094284057617187500e-22 2 -6.6e-22\n4.6293699741363527e-8 30 0.000000046293699741363526568763 4.629369974136352656876312633601e-8 2 4.6e-8\n9452.018737792969 5 9452.01874 9.45202e+3 21 9452.01873779296875000\n-8.85356843471527e-29 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 6 -8.85357e-29\n0.003665555715560913 2 0.00 3.67e-3 17 0.0036655557155609131\n7.761059403419495e-27 50 0.00000000000000000000000000776105940341949474785045 7.76105940341949474785045343302887840209544598527974e-27 50 7.7610594034194947478504534330288784020954459852797e-27\n2.9668569564819335e+34 15 2.9668569564819335e+34 2.966856956481934e+34 50 29668569564819335120850180743626752.000000000000000\n-2.825891971588135e+31 30 -2.825891971588135e+31 -2.825891971588134846166769808179e+31 50 -28258919715881348461667698081792.000000000000000000\n2.301499843597412e+33 10 2.301499843597412e+33 2.3014998436e+33 100 2301499843597412045736057906397184.000000000000000000000000000000000000000000000000000000000000000000\n47256231307983.4 20 47256231307983.39843750000000000000 4.72562313079833984375e+13 6 4.72562e+13\n1.9009184837341307e-15 2 0.00 1.90e-15 101 RangeError: toPrecision() argument must be between 1 and 100\n8.919882774353029e+23 1.7 8.919882774353029e+23 8.9e+23 undefined 8.919882774353029e+23\n-3610136508.9416504 21 -3610136508.941650390625000000000 -3.610136508941650390625e+9 undefined -3610136508.9416504\n9.39918279647827e+22 undefined 9.39918279647827e+22 9.39918279647827e+22 100 93991827964782706688000.00000000000000000000000000000000000000000000000000000000000000000000000000000\n-7.717993259429932e+24 50 -7.717993259429932e+24 -7.71799325942993191646003200000000000000000000000000e+24 undefined -7.717993259429932e+24\n2.6760339736938476e-12 undefined 0 2.6760339736938476e-12 3 2.68e-12\n-8.630309104919433e+23 20 -8.630309104919433e+23 -8.63030910491943327760e+23 3 -8.63e+23\n-4.319521188735962e-27 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 10 -4.319521189e-27\n-9.836574792861939e-13 3 -0.000 -9.837e-13 50 -9.8365747928619389145933133122711334184878895214865e-13\n-9.170442819595336e-12 1.7 -0.0 -9.2e-12 2 -9.2e-12\n-41713786125183110000 50 -41713786125183107072.00000000000000000000000000000000000000000000000000 -4.17137861251831070720000000000000000000000000000000e+19 10 -4.171378613e+19\n-9.064929485321044e+32 100 -9.064929485321044e+32 -9.0649294853210437867594829109657600000000000000000000000000000000000000000000000000000000000000000000e+32 6 -9.06493e+32\n4.0089607238769534e-9 1 0.0 4.0e-9 17 4.0089607238769534e-9\n6.198883056640625e-23 undefined 0 6.198883056640625e-23 100 6.198883056640624635330754112871870003511496033318605483383571287669955296451007598079741001129150391e-23\n4833393.096923828 30 4833393.096923828125000000000000000000 4.833393096923828125000000000000e+6 3 4.83e+6\n8.993473052978516e+29 undefined 8.993473052978516e+29 8.993473052978516e+29 undefined 8.993473052978516e+29\n0.0002794055938720703 undefined 0 2.794055938720703e-4 21 0.000279405593872070320740\n-396505355834960.94 0 -396505355834961 -4e+14 100 -396505355834960.9375000000000000000000000000000000000000000000000000000000000000000000000000000000000\n5.195637345314026e-37 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 100 5.195637345314025716917939887610301460448097737049291894214001437273226400232716525630026359512409623e-37\n-3.2718491554260254e-9 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 0 RangeError: toPrecision() argument must be between 1 and 100\n1.5599799156188964e-11 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 undefined 1.5599799156188964e-11\n-0.000003568670749664307 1.7 -0.0 -3.6e-6 17 -0.0000035686707496643071\n3499181270599.365 0 3499181270599 3e+12 21 3499181270599.36523438\n6.121690273284912e-13 10 0.0000000000 6.1216902733e-13 3 6.12e-13\n878005.7430267334 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 1 9e+5\n8.875405788421632e+24 2 8.875405788421632e+24 8.88e+24 10 8.875405788e+24\n-4.5993441343307496e-29 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 1 -5e-29\n-1.9072574377059939e-38 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 17 -1.9072574377059939e-38\n57752.72846221924 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 100 57752.72846221923828125000000000000000000000000000000000000000000000000000000000000000000000000000000\n-3.7941932678222654e+37 1.7 -3.7941932678222654e+37 -3.8e+37 101 RangeError: toPrecision() argument must be between 1 and 100\n-2.0188236236572266e+33 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 0 RangeError: toPrecision() argument must be between 1 and 100\n-6.031274199485778e-24 1.7 -0.0 -6.0e-24 undefined -6.031274199485778e-24\n9.88438069820404e-25 10 0.0000000000 9.8843806982e-25 101 RangeError: toPrecision() argument must be between 1 and 100\n3.040213584899902e+30 5 3.040213584899902e+30 3.04021e+30 50 3040213584899902029157567037440.0000000000000000000\n7.755422592163086e-7 0 0 8e-7 6 7.75542e-7\n5.5827093124389646e+23 20 5.5827093124389646e+23 5.58270931243896460542e+23 6 5.58271e+23\n-6.910843253135682e-23 30 -0.000000000000000000000069108433 -6.910843253135681539316075618448e-23 0 RangeError: toPrecision() argument must be between 1 and 100\n4.524258971214294e-24 15 0.000000000000000 4.524258971214294e-24 101 RangeError: toPrecision() argument must be between 1 and 100\n1.898834705352783e+27 50 1.898834705352783e+27 1.89883470535278306926578892800000000000000000000000e+27 10 1.898834705e+27\n-46026873588562010000 1.7 -46026873588562010112.0 -4.6e+19 10 -4.602687359e+19\n9.231637120246886e-35 15 0.000000000000000 9.231637120246886e-35 undefined 9.231637120246886e-35\n7.325754165649414e-7 10 0.0000007326 7.3257541656e-7 101 RangeError: toPrecision() argument must be between 1 and 100\n82841587066.65039 3 82841587066.650 8.284e+10 2 8.3e+10\n-6942739486694335000 2 -6942739486694335488.00 -6.94e+18 2 -6.9e+18\n6.744917631149292e-24 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 10 6.744917631e-24\n-1.6779303550720216e-35 5 -0.00000 -1.67793e-35 21 -1.67793035507202155906e-35\n3235437870.025635 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 3 3.24e+9\n1.2338095903396607e-26 1 0.0 1.2e-26 1 1e-26\n9.951325058937073e-32 100 0.0000000000000000000000000000000995132505893707288266572541978773499556110460091280301386618452116831 9.9513250589370728826657254197877349955611046009128030138661845211683057619458220820207738910312400549e-32 2 1.0e-31\n-7.20684953033924e-40 100 -0.0000000000000000000000000000000000000007206849530339240359429800173036165601756366079738656248381873 -7.2068495303392403594298001730361656017563660797386562483818726354695832863584008886231646832230060822e-40 17 -7.2068495303392404e-40\n5.621233582496643e-22 5 0.00000 5.62123e-22 17 5.6212335824966427e-22\n3.8718110322952273e-26 1 0.0 3.9e-26 10 3.871811032e-26\n0.05635323524475098 5 0.05635 5.63532e-2 undefined 0.05635323524475098\n0.09126343727111817 21 0.091263437271118172389 9.126343727111817238917e-2 undefined 0.09126343727111817\n6.639343500137329e-27 undefined 0 6.639343500137329e-27 undefined 6.639343500137329e-27\n-6.334550380706788e-14 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 undefined -6.334550380706788e-14\n941458702087402400 20 941458702087402368.00000000000000000000 9.41458702087402368000e+17 2 9.4e+17\n52580928802.490234 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 17 52580928802.490234\n3.201208710670471e-30 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 101 RangeError: toPrecision() argument must be between 1 and 100\n5.20865261554718e-23 100 0.0000000000000000000000520865261554718020129051471465863565366709889517701188555476071902206047781192 5.2086526155471802012905147146586356536670988951770118855547607190220604778119195543695241212844848633e-23 undefined 5.20865261554718e-23\n-7.853732109069824e+27 2 -7.853732109069824e+27 -7.85e+27 0 RangeError: toPrecision() argument must be between 1 and 100\n4.925465583801269e+27 0 4.925465583801269e+27 5e+27 2 4.9e+27\n1.2331509590148926e-17 15 0.000000000000000 1.233150959014893e-17 6 1.23315e-17\n9084503650.665283 2 9084503650.67 9.08e+9 50 9084503650.6652832031250000000000000000000000000000\n-2.2949475049972538e-34 30 -0.000000000000000000000000000000 -2.294947504997253758445255269116e-34 21 -2.29494750499725375845e-34\n462102.89001464844 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 50 462102.89001464843750000000000000000000000000000000\n251288414001464.84 30 251288414001464.843750000000000000000000000000 2.512884140014648437500000000000e+14 100 251288414001464.8437500000000000000000000000000000000000000000000000000000000000000000000000000000000\n7.844915390014648e-12 20 0.00000000000784491539 7.84491539001464826960e-12 2 7.8e-12\n-2.2918033599853514e+33 30 -2.2918033599853514e+33 -2.291803359985351393811756142821e+33 101 RangeError: toPrecision() argument must be between 1 and 100\n3.3527284860610964e-27 1 0.0 3.4e-27 50 3.3527284860610963641758554841690865081028104810622e-27\n-5.366146564483643e-15 2 -0.00 -5.37e-15 0 RangeError: toPrecision() argument must be between 1 and 100\n-0.00009925961494445802 2 -0.00 -9.93e-5 101 RangeError: toPrecision() argument must be between 1 and 100\n4.14616584777832e+37 100 4.14616584777832e+37 4.1461658477783200316014087704200347648000000000000000000000000000000000000000000000000000000000000000e+37 undefined 4.14616584777832e+37\n73375.41580200195 5 73375.41580 7.33754e+4 undefined 73375.41580200195\n-7.71130919456482e-24 1.7 -0.0 -7.7e-24 10 -7.711309195e-24\n9.541332721710205e-26 0 0 1e-25 100 9.541332721710205051810959031218294824311033945469950312275673789930673096559843315844773314893245697e-26\n8410.439491271973 0 8410 8e+3 101 RangeError: toPrecision() argument must be between 1 and 100\n6.685395240783692e-18 0 0 7e-18 1 7e-18\n-7.58878231048584e+27 1 -7.58878231048584e+27 -7.6e+27 21 -7.58878231048583961000e+27\n1.1931347846984862e+36 21 1.1931347846984862e+36 1.193134784698486226294e+36 100 1193134784698486226294294297064767488.000000000000000000000000000000000000000000000000000000000000000\n22851490974426268 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 6 2.28515e+16\n1.0189175605773925e+23 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 101 RangeError: toPrecision() argument must be between 1 and 100\n6524.968147277832 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 101 RangeError: toPrecision() argument must be between 1 and 100\n890683412.5518799 3 890683412.552 8.907e+8 3 8.91e+8\n71063923.8357544 0 71063924 7e+7 21 71063923.8357543945313\n9.944775700569153e-33 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 10 9.944775701e-33\n9.480696320533753e-27 20 0.00000000000000000000 9.48069632053375345392e-27 100 9.480696320533753453921340780901945908407459092548168470329264947007790690497452956719826033804565668e-27\n30597496032.714844 5 30597496032.71484 3.05975e+10 1 3e+10\n3.6398130655288694e-29 100 0.0000000000000000000000000000363981306552886939345973321622252491266523204676137964974639787631353262 3.6398130655288693934597332162225249126652320467613796497463978763135326240118518070776332251625717618e-29 undefined 3.6398130655288694e-29\n-8.449804782867432e+26 2 -8.449804782867432e+26 -8.45e+26 2 -8.4e+26\n-5101675987243652000 50 -5101675987243652096.00000000000000000000000000000000000000000000000000 -5.10167598724365209600000000000000000000000000000000e+18 50 -5101675987243652096.0000000000000000000000000000000\n6.272645592689514e-29 15 0.000000000000000 6.272645592689514e-29 17 6.2726455926895138e-29\n5.4121613502502445e-9 5 0.00000 5.41216e-9 1 5e-9\n-7.563741207122802e-8 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 1 -8e-8\n8.230409622192383e+33 0 8.230409622192383e+33 8e+33 6 8.23041e+33\n3.409700989723205e-39 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 0 RangeError: toPrecision() argument must be between 1 and 100\n-580004930496.2158 15 -580004930496.215820312500000 -5.800049304962158e+11 101 RangeError: toPrecision() argument must be between 1 and 100\n5.832611247897148e-41 10 0.0000000000 5.8326112479e-41 21 5.83261124789714765193e-41\n9.173119589686393e-41 100 0.0000000000000000000000000000000000000000917311958968639323961873340587290164593181524984148569021614 9.1731195896863932396187334058729016459318152498414856902161396882463503405921287584525112058779682669e-41 6 9.17312e-41\n9.733079075813293e-36 5 0.00000 9.73308e-36 101 RangeError: toPrecision() argument must be between 1 and 100\n-617.518424987793 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 3 -618\n-0.007564127445220947 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 21 -0.00756412744522094726563\n-7.662196159362793e+35 1 -7.662196159362793e+35 -7.7e+35 undefined -7.662196159362793e+35\n-3.2828974723815915e+30 1 -3.2828974723815915e+30 -3.3e+30 17 -3.2828974723815915e+30\n6.706059575080872e-36 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 3 6.71e-36\n1.2709712982177733e-22 10 0.0000000000 1.2709712982e-22 1 1e-22\n-37278079986572.266 1.7 -37278079986572.3 -3.7e+13 50 -37278079986572.265625000000000000000000000000000000\n-47463417053.22266 2 -47463417053.22 -4.75e+10 10 -4.746341705e+10\n-5.766382217407226e-13 10 -0.0000000000 -5.7663822174e-13 21 -5.76638221740722634275e-13\n-5.528584718704224e-23 100 -0.0000000000000000000000552858471870422366826213712661091355561089281931192288483724057296386789062126 -5.5285847187042236682621371266109135556108928193119228848372405729638678906212589936330914497375488281e-23 50 -5.5285847187042236682621371266109135556108928193119e-23\n-0.009537508487701416 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 10 -0.009537508488\n934028387069702100 1 934028387069702144.0 9.3e+17 6 9.34028e+17\n406636953353881.8 undefined 406636953353882 4.066369533538818e+14 0 RangeError: toPrecision() argument must be between 1 and 100\n9.006536602973939e-33 undefined 0 9.006536602973939e-33 undefined 9.006536602973939e-33\n-5.047593116760254e+23 21 -5.047593116760254e+23 -5.047593116760254091100e+23 21 -5.04759311676025409110e+23\n-71355676651000980000 100 -71355676651000979456.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 -7.1355676651000979456000000000000000000000000000000000000000000000000000000000000000000000000000000000e+19 3 -7.14e+19\n0.0006485223770141602 50 0.00064852237701416024298617379884035472059622406960 6.48522377014160242986173798840354720596224069595337e-4 6 0.000648522\n-3.845650553703308e-30 2 -0.00 -3.85e-30 50 -3.8456505537033077174637191872833984108437366292869e-30\n8.971290588378906e+27 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 10 8.971290588e+27\n-1.5565866231918334e-35 20 -0.00000000000000000000 -1.55658662319183337807e-35 undefined -1.5565866231918334e-35\n5.669003725051879e-38 1.7 0.0 5.7e-38 2 5.7e-38\n4571852684020.996 0 4571852684021 5e+12 21 4571852684020.99609375\n-6.25079870223999e-25 1.7 -0.0 -6.3e-25 3 -6.25e-25\n1876308917999.2676 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 3 1.88e+12\n-7.524535655975341e-20 15 -0.000000000000000 -7.524535655975341e-20 17 -7.5245356559753412e-20\n-3.2633578777313236e-33 21 -0.000000000000000000000 -3.263357877731323646212e-33 6 -3.26336e-33\n7.68044650554657e-33 50 0.00000000000000000000000000000000768044650554657014 7.68044650554657013922136735476771119899549590419921e-33 10 7.680446506e-33\n-4.351620674133301e+28 30 -4.351620674133301e+28 -4.351620674133300640602075955200e+28 10 -4.351620674e+28\n6.45911693572998e+21 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 3 6.46e+21\n9.106097221374511e+30 1.7 9.106097221374511e+30 9.1e+30 3 9.11e+30\n6.589808464050293e+23 15 6.589808464050293e+23 6.589808464050293e+23 undefined 6.589808464050293e+23\n521152.0195007324 100 521152.0195007324218750000000000000000000000000000000000000000000000000000000000000000000000000000000000000 5.2115201950073242187500000000000000000000000000000000000000000000000000000000000000000000000000000000e+5 undefined 521152.0195007324\n7854533195495605000 50 7854533195495605248.00000000000000000000000000000000000000000000000000 7.85453319549560524800000000000000000000000000000000e+18 10 7.854533195e+18\n85154294967651.38 21 85154294967651.375000000000000000000 8.515429496765137500000e+13 50 85154294967651.375000000000000000000000000000000000\n-1.9146776199340818e+28 100 -1.9146776199340818e+28 -1.9146776199340818380963708928000000000000000000000000000000000000000000000000000000000000000000000000e+28 0 RangeError: toPrecision() argument must be between 1 and 100\n-8.618893623352051e-13 30 -0.000000000000861889362335205075 -8.618893623352050751822789146533e-13 10 -8.618893623e-13\n3.1730496883392334e-35 15 0.000000000000000 3.173049688339233e-35 17 3.1730496883392334e-35\n5740308761596680 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 6 5.74031e+15\n9.712781906127929e+36 50 9.712781906127929e+36 9.71278190612792924567903480509261414400000000000000e+36 undefined 9.712781906127929e+36\n-2396287918090820000 15 -2396287918090820096.000000000000000 -2.396287918090820e+18 undefined -2396287918090820000\n-1.8173688650131227e-38 30 -0.000000000000000000000000000000 -1.817368865013122679794410571509e-38 21 -1.81736886501312267979e-38\n-9.144336581230163e-35 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 100 -9.144336581230163225423901658101041172718531551374711951453808801390972159469026814156811895292764625e-35\n-6.671007871627807e-13 1.7 -0.0 -6.7e-13 10 -6.671007872e-13\n-3.0698204040527343e-25 21 -0.000000000000000000000 -3.069820404052734274609e-25 10 -3.069820404e-25\n-713300704956054700 3 -713300704956054656.000 -7.133e+17 3 -7.13e+17\n6.908423900604248e-9 15 0.000000006908424 6.908423900604248e-9 6 6.90842e-9\n6.323612332344055e-29 15 0.000000000000000 6.323612332344055e-29 3 6.32e-29\n5.388913750648499e-33 0 0 5e-33 17 5.3889137506484987e-33\n-1.238226890563965e-11 undefined -0 -1.238226890563965e-11 10 -1.238226891e-11\n8.023169040679931e-22 10 0.0000000000 8.0231690407e-22 6 8.02317e-22\n-7.0407485961914065e+37 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 undefined -7.0407485961914065e+37\n6.292827129364013e+30 20 6.292827129364013e+30 6.29282712936401345278e+30 100 6292827129364013452780655083520.000000000000000000000000000000000000000000000000000000000000000000000\n4.898841977119446e-36 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 10 4.898841977e-36\n-185845851898193340 5 -185845851898193344.00000 -1.85846e+17 17 -1.8584585189819334e+17\n2.4159485101699832e-33 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 undefined 2.4159485101699832e-33\n9.033187031745911e-33 2 0.00 9.03e-33 3 9.03e-33\n-8.457794189453125 21 -8.457794189453125000000 -8.457794189453125000000e+0 3 -8.46\n-3.1024277210235594e-30 undefined -0 -3.1024277210235594e-30 undefined -3.1024277210235594e-30\n-2.4738889932632446e-28 2 -0.00 -2.47e-28 21 -2.47388899326324463978e-28\n-91420.53127288818 0 -91421 -9e+4 1 -9e+4\n6638517379760.742 2 6638517379760.74 6.64e+12 3 6.64e+12\n5.0012499094009396e-30 1 0.0 5.0e-30 101 RangeError: toPrecision() argument must be between 1 and 100\n-0.00005962803363800049 2 -0.00 -5.96e-5 6 -0.0000596280\n-1725816726684570400 21 -1725816726684570368.000000000000000000000 -1.725816726684570368000e+18 1 -2e+18\n-5.308193564414978e-29 2 -0.00 -5.31e-29 21 -5.30819356441497801534e-29\n-1.836196184158325e-8 15 -0.000000018361962 -1.836196184158325e-8 17 -1.8361961841583250e-8\n-2.107468843460083e-10 3 -0.000 -2.107e-10 1 -2e-10\n-14944171905.517578 5 -14944171905.51758 -1.49442e+10 100 -14944171905.51757812500000000000000000000000000000000000000000000000000000000000000000000000000000000\n-1.7693805694580078e+30 30 -1.7693805694580078e+30 -1.769380569458007758362644578304e+30 10 -1.769380569e+30\n9.203214645385741e+28 5 9.203214645385741e+28 9.20321e+28 21 9.20321464538574141840e+28\n4919462203979492 21 4919462203979492.000000000000000000000 4.919462203979492000000e+15 6 4.91946e+15\n-0.0007530794143676758 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 101 RangeError: toPrecision() argument must be between 1 and 100\n4.0323805809021e-20 undefined 0 4.0323805809021e-20 101 RangeError: toPrecision() argument must be between 1 and 100\n-954277276992797800 100 -954277276992797824.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 -9.5427727699279782400000000000000000000000000000000000000000000000000000000000000000000000000000000000e+17 17 -9.5427727699279782e+17\n-8.751528263092041e+34 undefined -8.751528263092041e+34 -8.751528263092041e+34 100 -87515282630920409407990672939548672.00000000000000000000000000000000000000000000000000000000000000000\n57087421417236330 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 1 6e+16\n9.009556770324707e-8 10 0.0000000901 9.0095567703e-8 0 RangeError: toPrecision() argument must be between 1 and 100\n-3.3076810836791994e+25 2 -3.3076810836791994e+25 -3.31e+25 21 -3.30768108367919937959e+25\n3.5971879959106444e-7 1.7 0.0 3.6e-7 6 3.59719e-7\n1.167035698890686e-31 30 0.000000000000000000000000000000 1.167035698890686087948045958551e-31 17 1.1670356988906861e-31\n32586693763732910000 5 32586693763732910080.00000 3.25867e+19 101 RangeError: toPrecision() argument must be between 1 and 100\n4.552023410797119e-21 undefined 0 4.552023410797119e-21 17 4.5520234107971188e-21\n-7.956349849700927e+36 1 -7.956349849700927e+36 -8.0e+36 100 -7956349849700927318798683537160536064.000000000000000000000000000000000000000000000000000000000000000\n8.803377151489259e+24 50 8.803377151489259e+24 8.80337715148925886372249600000000000000000000000000e+24 6 8.80338e+24\n-7.756833434104919e-25 30 -0.000000000000000000000000775683 -7.756833434104918798096298219284e-25 undefined -7.756833434104919e-25\n7.753395438194276e-31 30 0.000000000000000000000000000001 7.753395438194275866907138067018e-31 10 7.753395438e-31\n5.809237957000733e+25 3 5.809237957000733e+25 5.809e+25 100 58092379570007326433738752.00000000000000000000000000000000000000000000000000000000000000000000000000\n4.102447032928466e+28 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 1 4e+28\n3864009380.340576 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 101 RangeError: toPrecision() argument must be between 1 and 100\n6.16204023361206e-9 50 0.00000000616204023361206027539458765789233407605963 6.16204023361206027539458765789233407605962611341965e-9 10 6.162040234e-9\n-7.47636079788208e-9 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 2 -7.5e-9\n541560649.8718262 100 541560649.8718261718750000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 5.4156064987182617187500000000000000000000000000000000000000000000000000000000000000000000000000000000e+8 10 541560649.9\n5.182033181190491e-32 0 0 5e-32 undefined 5.182033181190491e-32\n5.8441472053527835e-19 0 0 6e-19 17 5.8441472053527835e-19\n9.655125141143798e-21 3 0.000 9.655e-21 101 RangeError: toPrecision() argument must be between 1 and 100\n-4.683418273925781e+21 50 -4.683418273925781e+21 -4.68341827392578099609600000000000000000000000000000e+21 2 -4.7e+21\n-8.543205261230468e-17 1 -0.0 -8.5e-17 17 -8.5432052612304682e-17\n5.683315992355347e-29 20 0.00000000000000000000 5.68331599235534662754e-29 100 5.683315992355346627542603868371868548130636429776218213189335220368911911689724369978193863062188029e-29\n0.009682040214538574 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 3 0.00968\n0.00030403852462768555 0 0 3e-4 1 0.0003\n-9.938855171203613e+38 30 -9.938855171203613e+38 -9.938855171203612886160815142218e+38 21 -9.93885517120361288616e+38\n44.72017288208008 5 44.72017 4.47202e+1 21 44.7201728820800781250\n-2.0808690786361696e-28 100 -0.0000000000000000000000000002080869078636169560362578343076294149297836098716819899835185685789450474 -2.0808690786361695603625783430762941492978360987168198998351856857894504743144992175984953064471483231e-28 101 RangeError: toPrecision() argument must be between 1 and 100\n-2.544281482696533e+34 100 -2.544281482696533e+34 -2.5442814826965330584006401533673472000000000000000000000000000000000000000000000000000000000000000000e+34 17 -2.5442814826965331e+34\n5.332515239715577e-16 21 0.000000000000000533252 5.332515239715576585826e-16 17 5.3325152397155766e-16\n-5269.844532012939 30 -5269.844532012939453125000000000000 -5.269844532012939453125000000000e+3 3 -5.27e+3\n-0.07029199600219727 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 50 -0.070291996002197265625000000000000000000000000000000\n-1.5503549575805665e+31 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 0 RangeError: toPrecision() argument must be between 1 and 100\n5.19162654876709e+35 101 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 1 5e+35\n-60214090347.29004 21 -60214090347.290039062500000000000 -6.021409034729003906250e+10 undefined -60214090347.29004\n-3.358646631240845e-26 10 -0.0000000000 -3.3586466312e-26 undefined -3.358646631240845e-26\n-0.036761903762817384 3 -0.037 -3.676e-2 21 -0.0367619037628173842003\n-4.456639289855958e-16 10 -0.0000000000 -4.4566392899e-16 17 -4.4566392898559579e-16\n-98217439.65148926 undefined -98217440 -9.821743965148926e+7 10 -98217439.65\n-6516404.151916504 5 -6516404.15192 -6.51640e+6 6 -6.51640e+6\n9.97936487197876e-18 50 0.00000000000000000997936487197876003091945319466737 9.97936487197876003091945319466736790641358506690102e-18 101 RangeError: toPrecision() argument must be between 1 and 100\n-9.061097502708435e-35 50 -0.00000000000000000000000000000000009061097502708435 -9.06109750270843467192480328010923232121229434021718e-35 1 -9e-35\n-1.000971794128418e+36 -1 RangeError: toFixed() digits argument must be between 0 and 100 RangeError: toExponential() argument must be between 0 and 100 10 -1.000971794e+36\n-949130535125732400000 0 -949130535125732425728 -9e+20 100 -949130535125732425728.0000000000000000000000000000000000000000000000000000000000000000000000000000000\n6.64156436920166e+38 15 6.64156436920166e+38 6.641564369201660e+38 21 6.64156436920165988416e+38\n-5.1793044805526735e-33 0 -0 -5e-33 100 -5.179304480552673532940901049801848625856599657151145094703757316993118439769663751879279212086970574e-33\n6.770401000976562e+27 30 6.770401000976562e+27 6.770401000976561912191909888000e+27 21 6.77040100097656191219e+27\n0.0004478912353515625 0 0 4e-4 2 0.00045\n-577583312988.2812 100 -577583312988.2812500000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 -5.7758331298828125000000000000000000000000000000000000000000000000000000000000000000000000000000000000e+11 undefined -577583312988.2812\n2.1362948417663573e+33 1.7 2.1362948417663573e+33 2.1e+33 undefined 2.1362948417663573e+33\n-3.6659550666809084e+21 21 -3.6659550666809084e+21 -3.665955066680908447744e+21 101 RangeError: toPrecision() argument must be between 1 and 100\n6.697413921356202e-16 1.7 0.0 6.7e-16 21 6.69741392135620176345e-16\n78101420402526850 undefined 78101420402526848 7.810142040252685e+16 10 7.810142040e+16");
    eq_s("let seed = 7; const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; }; const pick = (xs) => xs[Math.floor(rnd() * xs.length)]; const vals = [0, 1, 2, 3, -1, -2, -5, 7, 100, -100, 1.5, -0.5, NaN, Infinity, -Infinity, undefined, '2', null]; const out = []; const show = (f) => { try { return JSON.stringify(f()); } catch (e) { return e.constructor.name + ': ' + e.message; } }; for (let n = 0; n < 400; n++) { const len = Math.floor(rnd() * 6); const base = Array.from({ length: len }, (_, i) => i + 1); const argc = Math.floor(rnd() * 5); const args = Array.from({ length: argc }, () => pick(vals)); const m = pick(['splice', 'copyWithin', 'toSpliced', 'with', 'includes', 'lastIndexOf', 'indexOf', 'fill', 'slice', 'at']); const a = base.slice(); const res = show(() => a[m](...args)); out.push(m + ' ' + JSON.stringify(base) + ' ' + show(() => args) + ' -> ' + res + ' ' + JSON.stringify(a)); } out.join('\\n');",
       "at [1,2,3] [-100] -> undefined [1,2,3]\nsplice [1,2,3,4] [null,\"2\"] -> [1,2] [3,4]\nat [1,2,3,4,5] [3] -> 4 [1,2,3,4,5]\nsplice [] [0] -> [] []\nincludes [1,2,3,4] [null,null,1,-0.5] -> false [1,2,3,4]\nfill [1] [1,null,2,0] -> [1] [1]\nwith [1,2,3,4] [null,null,null,0] -> RangeError: Invalid index : -Infinity [1,2,3,4]\nwith [] [-5,-2,null] -> RangeError: Invalid index : -5 []\nlastIndexOf [1,2,3,4,5] [3,100] -> 2 [1,2,3,4,5]\nindexOf [1,2] [null] -> -1 [1,2]\ntoSpliced [1,2] [1.5,7,1.5] -> [1,1.5] [1,2]\ntoSpliced [] [-100,100] -> [] []\nfill [1,2] [] -> [null,null] [null,null]\nslice [1] [] -> [1] [1]\nwith [1,2,3] [\"2\",-1,100] -> [1,2,-1] [1,2,3]\nsplice [1,2,3] [-1,3,-0.5] -> [3] [1,2,-0.5]\nlastIndexOf [1] [null,7,-5] -> -1 [1]\ntoSpliced [1] [null] -> [] [1]\nslice [1,2,3] [] -> [1,2,3] [1,2,3]\ntoSpliced [1,2,3,4] [3,null] -> [1,2,3,4] [1,2,3,4]\nsplice [1] [null,-5] -> [] [1]\nat [1,2] [] -> 1 [1,2]\nfill [1] [1.5,\"2\"] -> [1] [1]\nincludes [1,2,3,4,5] [-1] -> false [1,2,3,4,5]\nlastIndexOf [1,2] [-100,\"2\",null] -> -1 [1,2]\nslice [1,2,3,4,5] [1.5] -> [2,3,4,5] [1,2,3,4,5]\nslice [1,2] [null] -> [] [1,2]\ntoSpliced [1,2,3,4,5] [] -> [1,2,3,4,5] [1,2,3,4,5]\ncopyWithin [1,2] [] -> [1,2] [1,2]\nindexOf [1,2,3] [-1,-100,-2,3] -> -1 [1,2,3]\nslice [1,2,3] [\"2\",-1,null,2] -> [] [1,2,3]\ncopyWithin [1,2] [null] -> [1,2] [1,2]\nindexOf [1,2,3,4,5] [-1] -> -1 [1,2,3,4,5]\nat [1,2] [\"2\",100] -> undefined [1,2]\nindexOf [] [-0.5,2] -> -1 []\nincludes [1,2,3,4] [0,\"2\"] -> false [1,2,3,4]\nsplice [1,2] [null,2,\"2\",-2] -> [] [1,2,\"2\",-2]\nfill [1] [] -> [null] [null]\nslice [1,2,3,4] [0,7] -> [1,2,3,4] [1,2,3,4]\ntoSpliced [1,2] [] -> [1,2] [1,2]\nindexOf [] [1.5,-2] -> -1 []\nsplice [] [1,null,2,-1] -> [] [2,-1]\nsplice [1,2,3,4] [null,0] -> [] [1,2,3,4]\nincludes [] [] -> false []\nincludes [] [-2,2,3,-2] -> false []\nfill [1,2,3] [null,1.5,2,-2] -> [1,null,3] [1,null,3]\nfill [1,2,3,4] [1,null] -> [1,2,3,4] [1,2,3,4]\nwith [] [] -> RangeError: Invalid index : undefined []\nlastIndexOf [1] [-0.5] -> -1 [1]\ncopyWithin [1,2,3,4,5] [null,-100] -> [1,2,3,4,5] [1,2,3,4,5]\nat [1,2,3] [-100,null] -> undefined [1,2,3]\nincludes [1,2,3] [null,-2,\"2\"] -> false [1,2,3]\nfill [1,2,3,4,5] [\"2\",null,-100,0] -> [1,2,3,4,5] [1,2,3,4,5]\nslice [] [0,null,0] -> [] []\nincludes [1,2,3,4,5] [] -> false [1,2,3,4,5]\ntoSpliced [] [0,-5] -> [] []\nslice [1,2] [null,\"2\",-5] -> [1,2] [1,2]\nlastIndexOf [1,2,3] [2,1.5] -> 1 [1,2,3]\ncopyWithin [] [0] -> [] []\nat [1] [-100] -> undefined [1]\nat [1,2,3] [\"2\",\"2\"] -> 3 [1,2,3]\nat [1,2,3] [1,2,100] -> 2 [1,2,3]\ncopyWithin [1,2,3] [] -> [1,2,3] [1,2,3]\nsplice [1,2,3,4] [null,null,7] -> [] [1,2,3,4,7]\ntoSpliced [1] [null,100,-0.5] -> [-0.5] [1]\nslice [1] [] -> [1] [1]\nwith [1,2,3,4,5] [] -> [null,2,3,4,5] [1,2,3,4,5]\ncopyWithin [1,2] [null,2,3] -> [1,2] [1,2]\nat [1,2,3,4] [-100] -> undefined [1,2,3,4]\nindexOf [1] [null,null,-0.5] -> -1 [1]\ncopyWithin [1,2,3] [-0.5,-100,null,-100] -> [1,2,3] [1,2,3]\nindexOf [1] [null,null,-0.5,2] -> -1 [1]\ncopyWithin [1] [-0.5,3,null,-100] -> [1] [1]\nsplice [1,2,3,4] [] -> [] [1,2,3,4]\nindexOf [1,2] [null,2,1.5] -> -1 [1,2]\nincludes [1,2,3] [] -> false [1,2,3]\nindexOf [1,2] [] -> -1 [1,2]\ncopyWithin [1] [] -> [1] [1]\nat [1,2,3,4,5] [null,null] -> 1 [1,2,3,4,5]\nsplice [1] [3,100,-2,1.5] -> [] [1,-2,1.5]\nindexOf [] [3,null] -> -1 []\nindexOf [1,2,3,4] [] -> -1 [1,2,3,4]\ncopyWithin [1,2] [] -> [1,2] [1,2]\nindexOf [1,2,3] [] -> -1 [1,2,3]\ncopyWithin [1,2,3,4,5] [1.5,2,-1] -> [1,3,4,4,5] [1,3,4,4,5]\nsplice [] [] -> [] []\nincludes [1,2,3] [] -> false [1,2,3]\nfill [] [2,-5,1,null] -> [] []\ncopyWithin [] [null,3] -> [] []\nindexOf [1,2,3,4,5] [100,null,1.5,null] -> -1 [1,2,3,4,5]\ncopyWithin [1,2,3,4] [null] -> [1,2,3,4] [1,2,3,4]\nlastIndexOf [1,2] [\"2\",-2,2,-2] -> -1 [1,2]\nslice [1,2,3,4] [null] -> [1,2,3,4] [1,2,3,4]\nincludes [1,2,3] [null,100,2,\"2\"] -> false [1,2,3]\ntoSpliced [1] [] -> [1] [1]\nslice [1,2,3,4] [1.5,-0.5,null,-1] -> [] [1,2,3,4]\nlastIndexOf [1,2,3] [0,-1] -> -1 [1,2,3]\nwith [1,2] [null,-2,0] -> [-2,2] [1,2]\ncopyWithin [] [1] -> [] []\ncopyWithin [1,2,3,4] [] -> [1,2,3,4] [1,2,3,4]\nindexOf [1,2,3] [] -> -1 [1,2,3]\nindexOf [1,2,3,4,5] [null,-5,-0.5,null] -> -1 [1,2,3,4,5]\nwith [1,2,3,4] [0,0,0] -> [0,2,3,4] [1,2,3,4]\nindexOf [1] [-2,\"2\",\"2\",-5] -> -1 [1]\nfill [1] [\"2\"] -> [\"2\"] [\"2\"]\nwith [1,2,3] [-100,-5,-2] -> RangeError: Invalid index : -100 [1,2,3]\nincludes [1,2,3,4,5] [] -> false [1,2,3,4,5]\nat [1,2,3,4] [] -> 1 [1,2,3,4]\nincludes [1,2,3,4] [7,1,-0.5] -> false [1,2,3,4]\nslice [] [2,-1] -> [] []\ntoSpliced [1,2,3,4,5] [2] -> [1,2] [1,2,3,4,5]\nlastIndexOf [] [1.5,null,-5] -> -1 []\nsplice [] [1.5,-5] -> [] []\nincludes [1,2,3,4] [2,-5] -> true [1,2,3,4]\nwith [1] [] -> [null] [1]\nwith [1,2] [null,-0.5,100] -> [-0.5,2] [1,2]\ncopyWithin [1,2,3,4,5] [] -> [1,2,3,4,5] [1,2,3,4,5]\ntoSpliced [1,2,3,4] [null,0] -> [1,2,3,4] [1,2,3,4]\nindexOf [1,2,3,4] [null] -> -1 [1,2,3,4]\nwith [1,2,3,4] [-100] -> RangeError: Invalid index : -100 [1,2,3,4]\nincludes [1,2,3,4,5] [null,\"2\",-5,-0.5] -> false [1,2,3,4,5]\nsplice [] [] -> [] []\nfill [1,2] [-0.5,-2,\"2\"] -> [-0.5,-0.5] [-0.5,-0.5]\nindexOf [1,2,3,4] [-5] -> -1 [1,2,3,4]\nfill [1,2,3] [] -> [null,null,null] [null,null,null]\ntoSpliced [1,2] [-100] -> [] [1,2]\nwith [] [] -> RangeError: Invalid index : undefined []\ncopyWithin [1,2] [3] -> [1,2] [1,2]\nwith [1,2] [] -> [null,2] [1,2]\nindexOf [1,2] [7] -> -1 [1,2]\nat [1,2] [null] -> undefined [1,2]\nincludes [1,2,3,4] [-100] -> false [1,2,3,4]\nsplice [1,2,3,4,5] [null,0,3] -> [] [3,1,2,3,4,5]\nfill [] [-0.5,null] -> [] []\nsplice [1,2] [100,-0.5] -> [] [1,2]\ncopyWithin [1] [] -> [1] [1]\nlastIndexOf [1,2,3,4] [null,null] -> -1 [1,2,3,4]\nfill [1] [] -> [null] [null]\nincludes [1] [3] -> false [1]\nsplice [1,2] [-5,7,0] -> [1,2] [0]\nlastIndexOf [1,2] [] -> -1 [1,2]\nwith [1,2,3] [null,-0.5] -> [-0.5,2,3] [1,2,3]\ntoSpliced [1,2,3,4] [2,1.5] -> [1,2,4] [1,2,3,4]\nincludes [1,2,3] [-100,-5,-0.5,1.5] -> false [1,2,3]\nslice [1,2,3] [2,null,0] -> [3] [1,2,3]\nat [1,2,3,4,5] [null] -> 1 [1,2,3,4,5]\nsplice [1,2] [0] -> [1,2] []\nlastIndexOf [1,2] [-0.5,\"2\",\"2\",\"2\"] -> -1 [1,2]\ncopyWithin [1,2,3,4,5] [null] -> [1,2,3,4,5] [1,2,3,4,5]\nwith [1,2,3,4] [1.5,null,null,3] -> [1,null,3,4] [1,2,3,4]\ncopyWithin [1,2,3,4] [-5,\"2\",-0.5] -> [1,2,3,4] [1,2,3,4]\nwith [1] [100,3,null] -> RangeError: Invalid index : 100 [1]\nslice [] [null,1,-5,-2] -> [] []\nsplice [1] [1] -> [] [1]\nslice [] [null,-5,-0.5] -> [] []\nwith [1,2] [2,0,1] -> RangeError: Invalid index : 2 [1,2]\nfill [] [\"2\",3,-1,-5] -> [] []\nlastIndexOf [1,2,3,4,5] [1,7,null] -> 0 [1,2,3,4,5]\nslice [1] [\"2\"] -> [] [1]\nwith [1,2,3,4,5] [\"2\",2,-2,null] -> [1,2,2,4,5] [1,2,3,4,5]\nfill [] [null,1.5,-100,null] -> [] []\nfill [1,2,3,4,5] [-1,1,3] -> [1,-1,-1,4,5] [1,-1,-1,4,5]\nslice [1,2,3,4,5] [2] -> [3,4,5] [1,2,3,4,5]\nindexOf [1,2] [] -> -1 [1,2]\ncopyWithin [1,2,3] [-2,7,7] -> [1,2,3] [1,2,3]\nsplice [] [null,1,-2] -> [] [-2]\nsplice [1,2] [] -> [] [1,2]\ncopyWithin [1,2,3,4] [\"2\"] -> [1,2,1,2] [1,2,1,2]\nslice [1,2,3,4] [100] -> [] [1,2,3,4]\nfill [1,2,3] [-5] -> [-5,-5,-5] [-5,-5,-5]\nat [1,2,3,4,5] [] -> 1 [1,2,3,4,5]\nindexOf [] [1.5,null,null,100] -> -1 []\nindexOf [1,2,3,4,5] [] -> -1 [1,2,3,4,5]\ntoSpliced [1] [1.5,null,7] -> [1,7] [1]\ncopyWithin [] [null,-5] -> [] []\nslice [1,2,3,4] [-100,null,\"2\"] -> [] [1,2,3,4]\nwith [1,2] [-1,-100] -> [1,-100] [1,2]\nlastIndexOf [1,2,3,4] [-1,\"2\"] -> -1 [1,2,3,4]\nat [1,2,3] [100,null] -> undefined [1,2,3]\nslice [1] [-1,-0.5,null] -> [] [1]\nat [1,2] [3,100] -> undefined [1,2]\nfill [1] [null] -> [null] [null]\nindexOf [1] [100] -> -1 [1]\nat [1,2,3,4,5] [] -> 1 [1,2,3,4,5]\nwith [1,2] [7] -> RangeError: Invalid index : 7 [1,2]\nfill [1] [7,1.5,0,100] -> [1] [1]\nfill [1,2,3,4,5] [100,2,100] -> [1,2,100,100,100] [1,2,100,100,100]\nindexOf [1,2,3,4,5] [] -> -1 [1,2,3,4,5]\nslice [] [-5] -> [] []\ncopyWithin [1,2] [null,2] -> [1,2] [1,2]\nindexOf [1,2,3] [-2,-2,-5,null] -> -1 [1,2,3]\ncopyWithin [1,2,3,4] [2,2] -> [1,2,3,4] [1,2,3,4]\nsplice [] [1.5] -> [] []\ncopyWithin [] [] -> [] []\nslice [1,2,3,4,5] [] -> [1,2,3,4,5] [1,2,3,4,5]\ncopyWithin [] [0] -> [] []\nindexOf [1] [null,1.5,2] -> -1 [1]\nfill [1,2,3] [null,-5,100] -> [null,null,null] [null,null,null]\nfill [1,2,3,4,5] [2,-0.5,null,null] -> [2,2,2,2,2] [2,2,2,2,2]\nindexOf [1,2,3,4] [\"2\"] -> -1 [1,2,3,4]\nsplice [1,2,3] [-100,null,1,7] -> [] [1,7,1,2,3]\nlastIndexOf [1,2,3] [null,null,null,-2] -> -1 [1,2,3]\ntoSpliced [1,2] [1.5,1.5,-0.5] -> [1,-0.5] [1,2]\nincludes [1,2,3,4] [null] -> false [1,2,3,4]\ntoSpliced [1,2,3] [1,null] -> [1,2,3] [1,2,3]\ntoSpliced [1,2,3,4,5] [1,\"2\"] -> [1,4,5] [1,2,3,4,5]\nslice [] [null,7] -> [] []\nslice [1,2,3] [3] -> [] [1,2,3]\nsplice [1] [] -> [] [1]\ntoSpliced [1,2,3,4] [-100,1,100,-100] -> [100,-100,2,3,4] [1,2,3,4]\nindexOf [1,2] [2,1] -> 1 [1,2]\nat [1,2,3,4,5] [1,-5,0,100] -> 2 [1,2,3,4,5]\nincludes [1,2,3] [0] -> false [1,2,3]\ntoSpliced [1,2,3,4,5] [\"2\",0,1.5,-100] -> [1,2,1.5,-100,3,4,5] [1,2,3,4,5]\nindexOf [1,2] [] -> -1 [1,2]\nfill [] [1.5,7] -> [] []\nfill [1,2,3,4] [\"2\"] -> [\"2\",\"2\",\"2\",\"2\"] [\"2\",\"2\",\"2\",\"2\"]\nat [1,2,3] [-100,3] -> undefined [1,2,3]\nlastIndexOf [1] [1,-1,7,1] -> 0 [1]\ntoSpliced [1,2,3,4] [1] -> [1] [1,2,3,4]\nslice [1,2] [-2,7,-1] -> [1,2] [1,2]\nfill [1,2] [0] -> [0,0] [0,0]\nat [1] [-2,null] -> undefined [1]\nslice [1,2] [null] -> [1,2] [1,2]\ntoSpliced [1,2] [] -> [1,2] [1,2]\nwith [1,2,3] [null,3,null] -> RangeError: Invalid index : Infinity [1,2,3]\ncopyWithin [] [1.5,null,-5] -> [] []\nwith [1,2,3] [7] -> RangeError: Invalid index : 7 [1,2,3]\nfill [1] [100,100,-100,100] -> [1] [1]\nsplice [1,2,3,4,5] [1] -> [2,3,4,5] [1]\nwith [] [-2,2] -> RangeError: Invalid index : -2 []\nfill [] [] -> [] []\ncopyWithin [1,2,3,4] [-1] -> [1,2,3,1] [1,2,3,1]\ntoSpliced [1,2,3] [100,7] -> [1,2,3] [1,2,3]\ntoSpliced [1,2,3,4] [-2,null] -> [1,2,3,4] [1,2,3,4]\nsplice [1,2] [7,null,null] -> [] [1,2,null]\nat [1,2,3,4] [-2,-5,\"2\",2] -> 3 [1,2,3,4]\nlastIndexOf [1,2,3,4] [3,null,100] -> -1 [1,2,3,4]\nslice [1] [null,1,null] -> [1] [1]\nlastIndexOf [] [0] -> -1 []\nat [1,2,3,4] [3,100,7,100] -> 4 [1,2,3,4]\ntoSpliced [] [null,null] -> [] []\nfill [1,2,3,4] [null,-5] -> [null,null,null,null] [null,null,null,null]\ntoSpliced [] [1,null,-0.5] -> [-0.5] []\ntoSpliced [1,2,3,4,5] [null,null,null,2] -> [null,2,1,2,3,4,5] [1,2,3,4,5]\nindexOf [1,2,3,4] [100,-1,null] -> -1 [1,2,3,4]\nindexOf [1,2,3] [0,null] -> -1 [1,2,3]\nfill [1] [] -> [null] [null]\nat [1,2,3,4] [-100,3] -> undefined [1,2,3,4]\nindexOf [1,2,3,4,5] [null] -> -1 [1,2,3,4,5]\nwith [1,2,3] [-2] -> [1,null,3] [1,2,3]\nincludes [1,2,3,4,5] [null,-5,-1,0] -> false [1,2,3,4,5]\nindexOf [1,2,3,4,5] [-100,null] -> -1 [1,2,3,4,5]\nfill [1] [3] -> [3] [3]\nfill [1,2,3] [-1,3] -> [1,2,3] [1,2,3]\nlastIndexOf [] [0,-5,-1] -> -1 []\nslice [1,2,3] [null,-5,2,3] -> [] [1,2,3]\nwith [1,2,3,4] [0,\"2\"] -> [\"2\",2,3,4] [1,2,3,4]\nfill [1,2,3,4,5] [null,null,1.5] -> [null,2,3,4,5] [null,2,3,4,5]\nwith [1,2,3,4] [] -> [null,2,3,4] [1,2,3,4]\nincludes [1,2,3,4] [-5] -> false [1,2,3,4]\ncopyWithin [1,2,3] [-100,null,1,-2] -> [1,2,3] [1,2,3]\nindexOf [1,2,3] [-2,null,-0.5] -> -1 [1,2,3]\nsplice [] [-100] -> [] []\nindexOf [1,2] [null,0,-2,1.5] -> -1 [1,2]\nsplice [1] [null] -> [1] []\nfill [1,2,3,4,5] [1.5,7,2,null] -> [1,2,3,4,5] [1,2,3,4,5]\nfill [1,2,3,4] [3,0,null] -> [1,2,3,4] [1,2,3,4]\nlastIndexOf [1,2,3] [-0.5,\"2\",null] -> -1 [1,2,3]\nwith [1,2,3,4,5] [null,\"2\",-0.5,null] -> [\"2\",2,3,4,5] [1,2,3,4,5]\nindexOf [1,2,3,4] [null] -> -1 [1,2,3,4]\nat [1,2] [1] -> 2 [1,2]\ncopyWithin [1,2] [null] -> [1,2] [1,2]\nindexOf [] [] -> -1 []\nwith [1,2] [null,null,-0.5] -> RangeError: Invalid index : -Infinity [1,2]\nslice [1] [-1,null] -> [] [1]\nfill [1,2,3,4] [null,2,null] -> [1,2,null,null] [1,2,null,null]\nat [1,2,3] [7,1,null] -> undefined [1,2,3]\ncopyWithin [] [0,100,null] -> [] []\ncopyWithin [1] [] -> [1] [1]\nindexOf [1,2,3,4] [-1,\"2\",100,null] -> -1 [1,2,3,4]\nfill [1] [null,1.5] -> [1] [1]\nincludes [] [-1,-0.5] -> false []\nlastIndexOf [1,2,3] [null] -> -1 [1,2,3]\nwith [] [] -> RangeError: Invalid index : undefined []\nwith [1,2,3] [3,null,7,3] -> RangeError: Invalid index : 3 [1,2,3]\nincludes [1,2] [0,2] -> false [1,2]\nat [1,2,3,4] [] -> 1 [1,2,3,4]\nslice [1] [null,-2,-2] -> [] [1]\nsplice [1,2,3] [] -> [] [1,2,3]\nindexOf [1,2,3] [-2,-100,null] -> -1 [1,2,3]\nlastIndexOf [] [null,-100] -> -1 []\nlastIndexOf [1,2,3] [7] -> -1 [1,2,3]\nat [1,2] [] -> 1 [1,2]\nlastIndexOf [] [-0.5,\"2\",3,-5] -> -1 []\nindexOf [1,2,3,4,5] [\"2\",0,null,-0.5] -> -1 [1,2,3,4,5]\nincludes [1,2,3,4] [-0.5,-0.5,1.5,100] -> false [1,2,3,4]\nslice [] [\"2\"] -> [] []\nindexOf [1] [-2] -> -1 [1]\ncopyWithin [1,2,3,4] [] -> [1,2,3,4] [1,2,3,4]\nwith [1,2,3,4,5] [-1,-0.5,\"2\",1.5] -> [1,2,3,4,-0.5] [1,2,3,4,5]\ntoSpliced [1,2,3,4,5] [] -> [1,2,3,4,5] [1,2,3,4,5]\nwith [1,2,3] [] -> [null,2,3] [1,2,3]\nat [1] [100,null,null,null] -> undefined [1]\nlastIndexOf [1,2,3,4] [0,0] -> -1 [1,2,3,4]\ncopyWithin [] [100,null,null,-5] -> [] []\nlastIndexOf [1] [null,null,null] -> -1 [1]\nat [1,2,3] [] -> 1 [1,2,3]\ncopyWithin [1,2,3,4,5] [2,7] -> [1,2,3,4,5] [1,2,3,4,5]\nsplice [] [] -> [] []\nat [] [\"2\"] -> undefined []\nfill [1,2] [null,100] -> [1,2] [1,2]\ncopyWithin [1,2,3] [null,null] -> [1,2,3] [1,2,3]\nwith [1] [1.5,null,-5,1.5] -> RangeError: Invalid index : 1.5 [1]\nslice [1,2,3] [] -> [1,2,3] [1,2,3]\ncopyWithin [1,2,3,4,5] [null,null,1] -> [1,2,3,4,5] [1,2,3,4,5]\nincludes [1,2,3] [null,100] -> false [1,2,3]\ncopyWithin [1] [] -> [1] [1]\nfill [1,2,3,4,5] [0,0,\"2\"] -> [0,0,3,4,5] [0,0,3,4,5]\nslice [1,2] [null,-0.5,7] -> [] [1,2]\nwith [1,2,3,4,5] [-100,7,7,null] -> RangeError: Invalid index : -100 [1,2,3,4,5]\nincludes [1,2] [null,-1,0] -> false [1,2]\ncopyWithin [1] [1] -> [1] [1]\nat [1,2,3,4,5] [7,null,0] -> undefined [1,2,3,4,5]\nfill [1,2,3,4,5] [] -> [null,null,null,null,null] [null,null,null,null,null]\nat [] [2,-1,2,-0.5] -> undefined []\nsplice [1,2] [] -> [] [1,2]\ncopyWithin [] [-100] -> [] []\nwith [1] [] -> [null] [1]\nlastIndexOf [1] [] -> -1 [1]\nindexOf [1,2,3,4] [-1,null] -> -1 [1,2,3,4]\ntoSpliced [1,2] [null,100] -> [1,2] [1,2]\nat [] [3,100] -> undefined []\ntoSpliced [1,2,3,4] [-1,-0.5] -> [1,2,3,4] [1,2,3,4]\nslice [1,2,3,4] [null,3,null] -> [1,2,3] [1,2,3,4]\ntoSpliced [] [-2,2] -> [] []\nwith [1,2,3,4] [null] -> [null,2,3,4] [1,2,3,4]\nincludes [1,2,3] [-5,1.5] -> false [1,2,3]\nfill [] [-2,7] -> [] []\nat [1,2,3,4,5] [null,0] -> undefined [1,2,3,4,5]\nfill [1] [null,-1,100,null] -> [null] [null]\nlastIndexOf [1] [1.5,-5] -> -1 [1]\nwith [1,2,3] [\"2\",-0.5,-100,-1] -> [1,2,-0.5] [1,2,3]\ntoSpliced [1] [null,-2] -> [1] [1]\nslice [1,2,3,4,5] [3,null] -> [] [1,2,3,4,5]\ntoSpliced [1,2,3,4,5] [] -> [1,2,3,4,5] [1,2,3,4,5]\ntoSpliced [1,2,3] [-0.5,2,-0.5] -> [-0.5,3] [1,2,3]\nindexOf [1,2] [null,null] -> -1 [1,2]\nsplice [1,2,3,4,5] [-100] -> [1,2,3,4,5] []\ncopyWithin [1,2,3,4,5] [-2,3,null] -> [1,2,3,4,5] [1,2,3,4,5]\ncopyWithin [1,2,3,4,5] [null,-1,0,100] -> [1,2,3,4,5] [1,2,3,4,5]\nat [1,2] [null,-100,100,-1] -> undefined [1,2]\nincludes [1,2,3] [] -> false [1,2,3]\nfill [1,2] [-1,0,2,-5] -> [-1,-1] [-1,-1]\nincludes [1,2,3,4] [] -> false [1,2,3,4]\nlastIndexOf [] [] -> -1 []\nslice [1,2] [-1,null] -> [] [1,2]\nincludes [] [null] -> false []\nsplice [1,2,3] [] -> [] [1,2,3]\nwith [] [null] -> RangeError: Invalid index : NaN []\ncopyWithin [1,2,3,4,5] [null,null,null,null] -> [1,2,3,4,5] [1,2,3,4,5]\nslice [] [null,100,-1] -> [] []\nincludes [] [2,7,-1,1.5] -> false []\nwith [1] [null,null,1.5,-1] -> RangeError: Invalid index : Infinity [1]\nat [] [-5,-100,-0.5,-5] -> undefined []\nfill [1,2,3,4] [\"2\",100,null,-100] -> [1,2,3,4] [1,2,3,4]\nfill [1,2,3,4] [100,null] -> [100,100,100,100] [100,100,100,100]\nfill [1,2] [-100,-5,null] -> [-100,-100] [-100,-100]\nindexOf [1,2,3,4] [] -> -1 [1,2,3,4]\nincludes [1,2,3] [100,1,-100] -> false [1,2,3]\nincludes [1,2] [-100,-0.5,0,-1] -> false [1,2]\nlastIndexOf [1,2,3,4,5] [null,1,2,1] -> -1 [1,2,3,4,5]\ntoSpliced [1] [] -> [1] [1]\nfill [1,2] [] -> [null,null] [null,null]\nsplice [1,2] [2,1.5,null] -> [] [1,2,null]\nindexOf [1,2,3,4] [null,null,2] -> -1 [1,2,3,4]\nindexOf [1,2] [2,1.5,-5] -> 1 [1,2]\nfill [1,2,3,4,5] [] -> [null,null,null,null,null] [null,null,null,null,null]\nslice [1,2,3] [null,null] -> [] [1,2,3]\nfill [1] [3,1,3,0] -> [1] [1]\nfill [1,2,3] [null,-0.5,-100,null] -> [1,2,3] [1,2,3]\ncopyWithin [1,2] [-0.5,null] -> [1,2] [1,2]\ntoSpliced [1,2] [] -> [1,2] [1,2]\nincludes [1,2,3,4] [null,\"2\",3] -> false [1,2,3,4]\nindexOf [] [3,-0.5,-1] -> -1 []\nindexOf [1,2,3,4,5] [0,1.5,3,1.5] -> -1 [1,2,3,4,5]\nfill [1] [] -> [null] [null]\nindexOf [1,2,3] [-5,1,3,null] -> -1 [1,2,3]\nlastIndexOf [] [] -> -1 []\nfill [1,2] [2,1,null,null] -> [1,2] [1,2]\nsplice [1] [-100,2,0,null] -> [1] [0,null]\nwith [] [] -> RangeError: Invalid index : undefined []\ncopyWithin [1] [] -> [1] [1]\ncopyWithin [1,2,3] [1.5,null,100] -> [1,1,2] [1,1,2]\nwith [1,2,3] [-0.5,null,1.5] -> [null,2,3] [1,2,3]\nincludes [1] [-5,null,null,null] -> false [1]\nincludes [1,2] [-1,null] -> false [1,2]\nsplice [1] [] -> [] [1]\nindexOf [1,2,3,4,5] [null,7] -> -1 [1,2,3,4,5]\ntoSpliced [1,2,3,4,5] [1,-2,7,null] -> [1,7,null,2,3,4,5] [1,2,3,4,5]");
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
    test_sort();
    test_map();
    test_set();
    test_error();
    test_error_types();
    test_uri();
    test_prototype();
    test_to_primitive();
    test_own_property_names();
    test_json_v8();
    test_set_prototype();
    test_function_prototype();
    test_iterators();
    test_iterator_helpers();
    test_v8_methods();
    test_descriptors();
    test_weak();
    test_normalize();
    test_from_async();
    test_regexp_legacy();
    test_lexical();
    test_bigint();
    test_stress();
    test_integration();
    if (checks_failed) {
        fprintf(stderr, "%d/%d builtin checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d builtin checks passed\n", checks_run);
    return 0;
}
