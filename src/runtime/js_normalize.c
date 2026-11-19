/*
 * String.prototype.normalize: Unicode normalization forms NFC, NFD, NFKC and NFKD (UAX #15), on
 * the data of the Unicode version V8's ICU has (js_normalize_data.h, from
 * tools/gen-normalize.mjs).
 *
 * The text is decoded to code points (a lone surrogate stands for itself: no decomposition,
 * class 0, as ICU treats it), each fully decomposed (canonically, or for NFK* with the
 * compatibility mappings too; Hangul syllables arithmetically), the marks between starters put
 * in canonical order (a stable sort by combining class), and for NFC and NFKC recomposed by the
 * canonical composition algorithm. Text that cannot change is returned as it is.
 */
#include "lamassu_internal.h"
#include "js_normalize_data.h"

enum { S_BASE = 0xAC00, L_BASE = 0x1100, V_BASE = 0x1161, T_BASE = 0x11A7,
       L_COUNT = 19, V_COUNT = 21, T_COUNT = 28, N_COUNT = V_COUNT * T_COUNT, S_COUNT = L_COUNT * N_COUNT };

static uint8_t ccc_of(uint32_t cp) {
    if (cp < 0x300)
        return 0;
    size_t lo = 0, hi = sizeof js_nf_ccc / sizeof js_nf_ccc[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < js_nf_ccc[mid].first)
            hi = mid;
        else if (cp > js_nf_ccc[mid].last)
            lo = mid + 1;
        else
            return js_nf_ccc[mid].ccc;
    }
    return 0;
}

/* CP's row in js_nf_dec, or -1 */
static int dec_row(uint32_t cp) {
    size_t lo = 0, hi = sizeof js_nf_dec / sizeof js_nf_dec[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        uint32_t c = js_nf_dec[mid].key >> 8;
        if (cp < c)
            hi = mid;
        else if (cp > c)
            lo = mid + 1;
        else
            return (int)mid;
    }
    return -1;
}

static uint32_t compose_pair(uint32_t a, uint32_t b) {
    if (a >= L_BASE && a < L_BASE + L_COUNT && b >= V_BASE && b < V_BASE + V_COUNT)
        return S_BASE + ((a - L_BASE) * V_COUNT + (b - V_BASE)) * T_COUNT;
    if (a >= S_BASE && a < S_BASE + S_COUNT && (a - S_BASE) % T_COUNT == 0 && b > T_BASE &&
        b < T_BASE + T_COUNT)
        return a + (b - T_BASE);
    size_t lo = 0, hi = sizeof js_nf_comp / sizeof js_nf_comp[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const uint32_t ma = js_nf_comp[mid].a, mb = js_nf_comp[mid].b;
        if (a < ma || (a == ma && b < mb))
            hi = mid;
        else if (a > ma || b > mb)
            lo = mid + 1;
        else
            return js_nf_comp[mid].c;
    }
    return 0;
}

typedef struct {
    JsVm *vm;
    uint32_t *cp;
    size_t len, cap;
} CpBuf;

static bool cp_push(CpBuf *b, uint32_t c) {
    if (b->len == b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 64;
        uint32_t *n = js_realloc_raw(b->vm, b->cp, b->cap * sizeof *n, ncap * sizeof *n);
        if (!n)
            return false;
        b->cp = n;
        b->cap = ncap;
    }
    b->cp[b->len++] = c;
    return true;
}

/* Appends CP's full decomposition */
static bool decompose(CpBuf *b, uint32_t cp, bool compat) {
    if (cp >= S_BASE && cp < S_BASE + S_COUNT) {
        uint32_t s = cp - S_BASE, t = s % T_COUNT;
        return cp_push(b, L_BASE + s / N_COUNT) && cp_push(b, V_BASE + (s % N_COUNT) / T_COUNT) &&
               (!t || cp_push(b, T_BASE + t));
    }
    int row = cp < 0xA0 ? -1 : dec_row(cp);
    if (row < 0)
        return cp_push(b, cp);
    uint32_t key = js_nf_dec[row].key;
    unsigned clen = (key >> 5) & 7, klen = key & 31;
    unsigned len = compat ? klen : clen;
    if (!len)
        return cp_push(b, cp);
    const uint16_t *u = js_nf_pool + (compat ? js_nf_dec[row].koff : js_nf_dec[row].coff);
    for (unsigned i = 0; i < len; i++) {
        uint32_t c = u[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < len) /* the pool holds only pairs */
            c = 0x10000 + ((c - 0xD800) << 10) + (u[++i] - 0xDC00);
        if (!cp_push(b, c))
            return false;
    }
    return true;
}

JsString *js_normalize(JsVm *vm, JsString *s, int form, bool *too_long) {
    *too_long = false;
    bool compose = form == JS_NF_NFC || form == JS_NF_NFKC;
    bool compat = form == JS_NF_NFKC || form == JS_NF_NFKD;
    /* quick check: nothing below U+00A0 changes in any form, nothing below U+0300 in NFC */
    uint16_t limit = form == JS_NF_NFC ? 0x300 : 0xA0;
    uint32_t i = 0;
    while (i < s->length && s->units[i] < limit)
        i++;
    if (i == s->length)
        return s;
    CpBuf b = {vm, NULL, 0, 0};
    bool ok = true;
    for (i = 0; ok && i < s->length; i++) {
        uint32_t c = s->units[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s->length && s->units[i + 1] >= 0xDC00 &&
            s->units[i + 1] <= 0xDFFF)
            c = 0x10000 + ((c - 0xD800) << 10) + (s->units[++i] - 0xDC00);
        ok = decompose(&b, c, compat);
        /* every code point is a unit at least, and without composition none goes away */
        if (ok && !compose && b.len > JS_MAX_STRING_UNITS)
            ok = !(*too_long = true);
    }
    /* canonical ordering: a stable insertion sort of each run of marks by class */
    for (size_t k = 1; ok && k < b.len; k++) {
        uint32_t c = b.cp[k];
        uint8_t cc = ccc_of(c);
        if (!cc)
            continue;
        size_t j = k;
        while (j > 0 && ccc_of(b.cp[j - 1]) > cc) {
            b.cp[j] = b.cp[j - 1];
            j--;
        }
        b.cp[j] = c;
    }
    /* canonical composition (UAX #15's reference algorithm) */
    if (ok && compose && b.len) {
        size_t starter = 0, out = 1;
        uint32_t starter_cp = b.cp[0];
        int last = ccc_of(starter_cp) ? 256 : 0; /* text that starts with a mark has no starter */
        for (size_t k = 1; k < b.len; k++) {
            uint32_t c = b.cp[k];
            int cc = ccc_of(c);
            uint32_t comp = compose_pair(starter_cp, c); /* no primary composite starts with a mark */
            if (comp && (last < cc || last == 0)) {
                b.cp[starter] = starter_cp = comp;
                continue;
            }
            if (cc == 0) {
                starter = out;
                starter_cp = c;
            }
            last = cc;
            b.cp[out++] = c;
        }
        b.len = out;
    }
    JsString *r = NULL;
    if (ok) {
        size_t units = 0;
        for (size_t k = 0; k < b.len; k++)
            units += b.cp[k] > 0xFFFF ? 2 : 1;
        *too_long = units > JS_MAX_STRING_UNITS;
        uint16_t *u = units && !*too_long ? js_realloc_raw(vm, NULL, 0, units * sizeof *u) : NULL;
        if (u || (!units && !*too_long)) {
            size_t n = 0;
            for (size_t k = 0; k < b.len; k++) {
                uint32_t c = b.cp[k];
                if (c > 0xFFFF) {
                    u[n++] = (uint16_t)(0xD800 + ((c - 0x10000) >> 10));
                    u[n++] = (uint16_t)(0xDC00 + ((c - 0x10000) & 0x3FF));
                } else {
                    u[n++] = (uint16_t)c;
                }
            }
            js_realloc_raw(vm, b.cp, b.cap * sizeof *b.cp, 0);
            b.cp = NULL;
            b.cap = 0;
            r = js_string_cell_new(vm, u, units);
            js_realloc_raw(vm, u, units * sizeof *u, 0);
        }
    }
    js_realloc_raw(vm, b.cp, b.cap * sizeof *b.cp, 0);
    return r;
}
