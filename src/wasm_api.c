/*
 * Browser/Node embedding surface (built with emscripten, which supplies libc
 * malloc/realloc/free — so this is NOT a freestanding build). Keeps one
 * persistent VM+context so state carries across evals; a `print()` native
 * captures output into a growing buffer that jsvm_eval returns as a
 * NUL-terminated UTF-8 string.
 *
 * Exposed to JS (see the Makefile `pkg` target):
 *   const char *jsvm_eval(const char *src_utf8);  // output + result/error
 *   void        jsvm_reset(void);                 // fresh VM
 *
 * HOST CALLS: guest code can call `__hostcall(name, argsJson)` — a native
 * that suspends the entire wasm execution (emscripten Asyncify) while the
 * embedder's JS handler (`Module.lamassuHostCall`, installed by the npm
 * shim) runs. The handler may be async — that is the point: the guest gets
 * a synchronous-looking call whose answer may take time (a database query,
 * a fetch, …). The handler receives (name, argsJson) and returns a string;
 * a handler error is re-thrown inside the guest. NOTE: while an eval is
 * suspended in a host call, this wasm instance must not be re-entered
 * (Asyncify is not reentrant) — embedders that need nested evals use one
 * instance per nesting level.
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

/* UTF-16 -> malloc'd NUL-terminated UTF-8 (caller frees). */
static char *utf16_to_utf8_dup(const uint16_t *u, size_t n) {
    char *out = malloc(n * 4 + 1); /* worst case */
    if (!out)
        return NULL;
    size_t len = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned cp = u[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && u[i + 1] >= 0xDC00 &&
            u[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) {
            out[len++] = (char)cp;
        } else if (cp < 0x800) {
            out[len++] = (char)(0xC0 | (cp >> 6));
            out[len++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[len++] = (char)(0xE0 | (cp >> 12));
            out[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[len++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[len++] = (char)(0xF0 | (cp >> 18));
            out[len++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[len++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[len] = 0;
    return out;
}

static size_t utf8_to_utf16(const char *in, size_t len, uint16_t *out);

/* JsValue (any) -> malloc'd UTF-8 via ToString (caller frees). */
static char *value_to_utf8_dup(JsContext *ctx, JsValue v) {
    JsValue s = js_to_string(ctx, v);
    size_t n;
    const uint16_t *u = js_string_units(s, &n);
    if (!u)
        return NULL;
    return utf16_to_utf8_dup(u, n);
}

/* UTF-8 -> engine string value. */
static JsValue string_value_from_utf8(const char *s) {
    size_t blen = strlen(s);
    uint16_t *u = malloc((blen + 1) * sizeof(uint16_t));
    if (!u)
        return js_undefined();
    size_t ulen = utf8_to_utf16(s, blen, u);
    JsValue v = js_string_new(g_vm, u, ulen);
    free(u);
    return v;
}

#ifdef __EMSCRIPTEN__
/*
 * Suspend wasm execution (Asyncify) and run the embedder's host-call
 * handler. Returns a wasm-malloc'd reply the caller must free:
 *   "V<value>"   the handler's string result
 *   "E<message>" the handler threw (or none is installed)
 */
EM_ASYNC_JS(char *, lamassu_host_call_js, (const char *name, const char *args_json), {
    let reply;
    try {
        const handler = Module["lamassuHostCall"];
        if (!handler)
            throw new Error("no host-call handler installed (createLamassu natives option)");
        const value = await handler(UTF8ToString(name), UTF8ToString(args_json));
        reply = "V" + (value === undefined ? "" : String(value));
    } catch (e) {
        reply = "E" + String(e && e.message !== undefined ? e.message : e);
    }
    const n = lengthBytesUTF8(reply) + 1;
    const p = _malloc(n);
    stringToUTF8(reply, p, n);
    return p;
});

/* __hostcall(name, argsJson): call out to the host; may take time. */
static bool native_hostcall(JsContext *ctx, JsValue this_val, const JsValue *args,
                            int argc, JsValue *result) {
    (void)this_val;
    char *name = argc > 0 ? value_to_utf8_dup(ctx, args[0]) : NULL;
    char *json = argc > 1 ? value_to_utf8_dup(ctx, args[1]) : NULL;
    char *reply = lamassu_host_call_js(name ? name : "", json ? json : "");
    free(name);
    free(json);
    if (!reply) {
        *result = string_value_from_utf8("host call failed (out of memory)");
        return false;
    }
    bool ok = reply[0] == 'V';
    *result = string_value_from_utf8(reply + 1);
    free(reply);
    return ok; /* on false the engine throws *result */
}
#endif

static void ensure_vm(void) {
    if (g_vm)
        return;
    g_vm = js_vm_new(NULL); /* libc realloc via emscripten */
    g_ctx = js_context_new(g_vm);
    static const uint16_t print_name[] = {'p', 'r', 'i', 'n', 't'};
    js_register_native(g_ctx, print_name, 5, native_print, NULL);
#ifdef __EMSCRIPTEN__
    static const uint16_t hostcall_name[] = {'_', '_', 'h', 'o', 's', 't',
                                             'c', 'a', 'l', 'l'};
    js_register_native(g_ctx, hostcall_name, 10, native_hostcall, NULL);
#endif
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
    /* REPL mode: top-level declarations persist as globals across evals */
    JsValue fn = js_compile_module_repl(g_ctx, u, ulen, &err_msg, &err_pos);
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
