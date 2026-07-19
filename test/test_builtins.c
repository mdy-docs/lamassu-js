/* Builtin library tests — driven through compile+run, checking ToString of
 * the completion value. Shares the harness shape with test_exec.c. */
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

/* Runs src, returns malloc'd ASCII of ToString(result); ok=false on error. */
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
    eq("'foo'.concat('bar', 'baz');", "foobarbaz");
    eq("String.fromCharCode(72, 105);", "Hi");
    eq("String(42);", "42");
    eq("String(true);", "true");
    /* `new String(...)`: no boxed-primitive type here, so `new` on a native
     * conversion function just calls it and returns the primitive. */
    eq("new String(42);", "42");
    eq("typeof new String('x');", "string");
    /* the test harness maps input bytes to code units (no UTF-8 decode), so
     * the two UTF-8 bytes of 'é' count as two units here. */
    eq("'caf\xc3\xa9'.length;", "5");
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
}

static void test_number(void) {
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
    eq("let o = Object.freeze({a:1}); o.a;", "1");
    /* build object from entries after transform */
    eq("Object.fromEntries(Object.entries({a:1,b:2}).map(([k,v]) => [k, v*10])).a;", "10");
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
    err("new Map().forEach(5);", "TypeError: callback is not a function");
    err("new Map([1, 2]);", "TypeError: iterable for new Map() must yield");
    err("let s = new Map().set; s();", "TypeError: Map.prototype method called on a non-Map value");
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
    err("new Set().forEach(5);", "TypeError: callback is not a function");
    err("new Set(5);", "TypeError: value is not iterable");
    err("let a = new Set().add; a();", "TypeError: Set.prototype method called on a non-Set value");
}

/* GC-sensitive cases: allocation-heavy methods and callbacks that call back
 * into user code (js_call) while building results — exercises rooting. */
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

int main(void) {
    test_string();
    test_array();
    test_number();
    test_math();
    test_json();
    test_object();
    test_date();
    test_map();
    test_set();
    test_stress();
    test_integration();
    if (checks_failed) {
        fprintf(stderr, "%d/%d builtin checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d builtin checks passed\n", checks_run);
    return 0;
}
