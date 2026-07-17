/*
 * Browser/Node REPL surface for the demo playground (built with emscripten,
 * which supplies libc malloc/realloc/free — so this is NOT a freestanding
 * build). Keeps one persistent VM+context so state carries across evals; a
 * `print()` native captures output into a growing buffer that jsvm_eval
 * returns as a NUL-terminated UTF-8 string.
 *
 * Exposed to JS (see the Makefile `web` target):
 *   const char *jsvm_eval(const char *src_utf8);  // output + result/error
 *   void        jsvm_reset(void);                 // fresh VM
 */
#include <stdlib.h>
#include <string.h>

#include "jsvm.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

static JsVm *g_vm;
static JsContext *g_ctx;

/* growing UTF-8 output buffer, reused across evals */
static char *g_buf;
static size_t g_len, g_cap;

static void buf_reset(void) { g_len = 0; }

static void buf_bytes(const char *b, size_t n) {
    if (g_len + n + 1 > g_cap) {
        size_t ncap = g_cap ? g_cap * 2 : 256;
        while (ncap < g_len + n + 1)
            ncap *= 2;
        char *nb = realloc(g_buf, ncap);
        if (!nb)
            return;
        g_buf = nb;
        g_cap = ncap;
    }
    memcpy(g_buf + g_len, b, n);
    g_len += n;
    g_buf[g_len] = 0;
}

static void buf_cstr(const char *s) { buf_bytes(s, strlen(s)); }

/* append UTF-16 code units as UTF-8 */
static void buf_utf16(const uint16_t *u, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned cp = u[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && u[i + 1] >= 0xDC00 &&
            u[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        }
        char t[4];
        if (cp < 0x80) {
            t[0] = (char)cp;
            buf_bytes(t, 1);
        } else if (cp < 0x800) {
            t[0] = (char)(0xC0 | (cp >> 6));
            t[1] = (char)(0x80 | (cp & 0x3F));
            buf_bytes(t, 2);
        } else if (cp < 0x10000) {
            t[0] = (char)(0xE0 | (cp >> 12));
            t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            t[2] = (char)(0x80 | (cp & 0x3F));
            buf_bytes(t, 3);
        } else {
            t[0] = (char)(0xF0 | (cp >> 18));
            t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            t[3] = (char)(0x80 | (cp & 0x3F));
            buf_bytes(t, 4);
        }
    }
}

static void buf_value(JsContext *ctx, JsValue v) {
    JsValue s = js_to_string(ctx, v);
    size_t n;
    const uint16_t *u = js_string_units(s, &n);
    if (u)
        buf_utf16(u, n);
}

/* print(...args): space-separated, trailing newline */
static bool native_print(JsContext *ctx, JsValue this_val, const JsValue *args,
                         int argc, JsValue *result) {
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        if (i)
            buf_bytes(" ", 1);
        buf_value(ctx, args[i]);
    }
    buf_bytes("\n", 1);
    *result = js_undefined();
    return true;
}

static void ensure_vm(void) {
    if (g_vm)
        return;
    g_vm = js_vm_new(NULL); /* libc realloc via emscripten */
    g_ctx = js_context_new(g_vm);
    static const uint16_t print_name[] = {'p', 'r', 'i', 'n', 't'};
    js_register_native(g_ctx, print_name, 5, native_print, NULL);
}

/* UTF-8 -> UTF-16 (invalid sequences -> U+FFFD); returns unit count. */
static size_t utf8_to_utf16(const char *in, size_t len, uint16_t *out) {
    size_t n = 0, i = 0;
    const unsigned char *b = (const unsigned char *)in;
    while (i < len) {
        unsigned cp;
        unsigned char c = b[i];
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < len) {
            cp = ((unsigned)(c & 0x1F) << 6) | (b[i + 1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len) {
            cp = ((unsigned)(c & 0x0F) << 12) | ((unsigned)(b[i + 1] & 0x3F) << 6) |
                 (b[i + 2] & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < len) {
            cp = ((unsigned)(c & 0x07) << 18) | ((unsigned)(b[i + 1] & 0x3F) << 12) |
                 ((unsigned)(b[i + 2] & 0x3F) << 6) | (b[i + 3] & 0x3F);
            i += 4;
        } else {
            cp = 0xFFFD;
            i += 1;
        }
        if (cp > 0x10FFFF)
            cp = 0xFFFD;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out[n++] = (uint16_t)cp;
        }
    }
    return n;
}

EXPORT void jsvm_reset(void) {
    if (g_vm) {
        js_vm_free(g_vm);
        g_vm = NULL;
        g_ctx = NULL;
    }
    ensure_vm();
}

/*
 * Evaluate `src_utf8` in the persistent context. Returns a pointer to a
 * NUL-terminated UTF-8 buffer (valid until the next call) holding any
 * print() output followed by the completion value or an error line.
 */
EXPORT const char *jsvm_eval(const char *src_utf8) {
    ensure_vm();
    buf_reset();
    if (!g_buf)
        buf_bytes("", 0); /* allocate at least the NUL */

    size_t blen = src_utf8 ? strlen(src_utf8) : 0;
    uint16_t *u = malloc((blen + 1) * sizeof(uint16_t));
    if (!u) {
        buf_cstr("internal: out of memory");
        return g_buf ? g_buf : "";
    }
    size_t ulen = utf8_to_utf16(src_utf8 ? src_utf8 : "", blen, u);

    const char *err_msg;
    uint32_t err_pos;
    JsValue fn = js_compile_module(g_ctx, u, ulen, &err_msg, &err_pos);
    if (!js_is_function(fn)) {
        buf_cstr("SyntaxError: ");
        buf_cstr(err_msg ? err_msg : "compile error");
    } else {
        js_gc_protect(g_vm, &fn);
        JsValue result;
        bool ok = js_run_module(g_ctx, fn, &result);
        js_gc_protect(g_vm, &result);
        if (ok) {
            buf_cstr("\xE2\x87\x92 "); /* U+21D2 rightwards double arrow */
            buf_value(g_ctx, result);
        } else {
            buf_cstr("Uncaught ");
            buf_value(g_ctx, result);
        }
        js_gc_unprotect(g_vm, &result);
        js_gc_unprotect(g_vm, &fn);
    }
    free(u);
    return g_buf ? g_buf : "";
}
