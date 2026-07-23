/*
 * Browser/Node embedding surface (built with emscripten, which supplies libc
 * malloc/realloc/free). Keeps one persistent VM+context so state carries
 * across evals; a `print()` native captures output into a growing buffer
 * that lamassu_eval returns as a NUL-terminated UTF-8 string.
 *
 * Exposed to JS (see the Makefile `pkg` target):
 *   const char *lamassu_eval(const char *src_utf8);  // output + result/error
 *   void        lamassu_reset(void);                 // fresh VM
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

#include "lamassu.h"

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

/*
 * Shared UTF-16 <-> UTF-8 primitives (used by every conversion below, so the
 * encoding tables live in exactly one place).
 *
 * Decodes the code unit at u[*i] into a scalar value and advances *i: a valid
 * surrogate pair combines and consumes two units; an unpaired surrogate yields
 * U+FFFD (so the emitted UTF-8 is always well-formed, never CESU-8).
 */
static unsigned utf16_next_cp(const uint16_t *u, size_t n, size_t *i) {
    unsigned cp = u[*i];
    if (cp >= 0xD800 && cp <= 0xDBFF) {
        if (*i + 1 < n && u[*i + 1] >= 0xDC00 && u[*i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[*i + 1] - 0xDC00);
            *i += 2;
            return cp;
        }
        cp = 0xFFFD; /* unpaired high surrogate */
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
        cp = 0xFFFD; /* unpaired low surrogate */
    }
    *i += 1;
    return cp;
}

/* Encodes scalar cp into t[0..3]; returns the byte count (1..4). */
static int utf8_encode_cp(unsigned cp, char t[4]) {
    if (cp < 0x80) {
        t[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        t[0] = (char)(0xC0 | (cp >> 6));
        t[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        t[0] = (char)(0xE0 | (cp >> 12));
        t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    t[0] = (char)(0xF0 | (cp >> 18));
    t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    t[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* append UTF-16 code units as UTF-8 */
static void buf_utf16(const uint16_t *u, size_t n) {
    for (size_t i = 0; i < n;) {
        char t[4];
        int len = utf8_encode_cp(utf16_next_cp(u, n, &i), t);
        buf_bytes(t, (size_t)len);
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
    char *out = malloc(n * 4 + 1); /* worst case: 4 UTF-8 bytes per unit */
    if (!out)
        return NULL;
    size_t len = 0;
    for (size_t i = 0; i < n;)
        len += (size_t)utf8_encode_cp(utf16_next_cp(u, n, &i), out + len);
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

/* Nonzero while a __hostcall is suspended in Asyncify awaiting the host: the
 * fiber and VM are live on the paused C stack, so lamassu_reset must not free
 * them. Declared unconditionally so lamassu_reset compiles in any build; only
 * the Asyncify hostcall path (below) ever raises it. */
static int g_hostcall_active;

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
    g_hostcall_active++;
    char *reply = lamassu_host_call_js(name ? name : "", json ? json : "");
    g_hostcall_active--;
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

/*
 * __nativeDefer(id): the OTHER host-async mechanism (see js_promise.c) —
 * returns a pending promise immediately (no Asyncify involved) that guest
 * code `await`s; the fiber suspends at the interpreter level and lamassu_eval
 * returns with the module still pending. The host settles it later, from a
 * real JS callback (timer, fetch, …), via lamassu_settle_deferred(id, value)
 * below. Exists to exercise this path as compiled WASM in Node, which
 * __hostcall's Asyncify-based test does not cover.
 */
#define MAX_DEFERRED 32
static JsValue g_deferred[MAX_DEFERRED];
static bool g_deferred_live[MAX_DEFERRED];

static bool native_native_defer(JsContext *ctx, JsValue this_val, const JsValue *args,
                                int argc, JsValue *result) {
    (void)this_val;
    int id = argc > 0 && js_is_number(args[0]) ? (int)js_get_number(args[0]) : -1;
    if (id < 0 || id >= MAX_DEFERRED || g_deferred_live[id]) {
        *result = string_value_from_utf8("__nativeDefer: bad or reused id");
        return false;
    }
    JsValue p = js_promise_new(ctx);
    if (!js_is_promise(p)) {
        *result = string_value_from_utf8("__nativeDefer: out of memory");
        return false;
    }
    g_deferred[id] = p;
    js_gc_protect(g_vm, &g_deferred[id]);
    g_deferred_live[id] = true;
    *result = p;
    return true;
}

/* Settles a pending __nativeDefer(id) promise with a string value and drains
 * the microtask queue, resuming any guest fiber awaiting it. Call
 * lamassu_eval again afterward to observe the result (e.g. a persisted global
 * the guest assigned the awaited value to). */
EXPORT void lamassu_settle_deferred(int id, const char *value_utf8) {
    if (!g_ctx || id < 0 || id >= MAX_DEFERRED || !g_deferred_live[id])
        return;
    JsValue v = string_value_from_utf8(value_utf8 ? value_utf8 : "");
    js_resolve(g_ctx, g_deferred[id], v);
    js_gc_unprotect(g_vm, &g_deferred[id]);
    g_deferred_live[id] = false;
    js_run_jobs(g_ctx);
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
    static const uint16_t defer_name[] = {'_', '_', 'n', 'a', 't', 'i', 'v',
                                          'e', 'D', 'e', 'f', 'e', 'r'};
    js_register_native(g_ctx, defer_name, 13, native_native_defer, NULL);
    memset(g_deferred_live, 0, sizeof g_deferred_live);
#endif
}

/*
 * UTF-8 -> UTF-16. Well-formed only: overlong encodings, surrogate codepoints,
 * values above U+10FFFF, truncated sequences, and bad continuation bytes each
 * become one U+FFFD and decoding resyncs at the next byte. Returns unit count.
 */
static size_t utf8_to_utf16(const char *in, size_t len, uint16_t *out) {
    size_t n = 0, i = 0;
    const unsigned char *b = (const unsigned char *)in;
    while (i < len) {
        unsigned char c = b[i];
        unsigned cp;
        int seqlen;
        unsigned min; /* smallest value not overlong for this length */
        if (c < 0x80) {
            out[n++] = c; /* ASCII */
            i++;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F; seqlen = 2; min = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F; seqlen = 3; min = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07; seqlen = 4; min = 0x10000;
        } else {
            out[n++] = 0xFFFD; i++; continue;
        }
        bool ok = i + (size_t)seqlen <= len;
        for (int k = 1; ok && k < seqlen; k++) {
            unsigned char cc = b[i + (size_t)k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok || cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out[n++] = 0xFFFD; i++; continue;
        }
        i += (size_t)seqlen;
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

EXPORT void lamassu_reset(void) {
    /* Refuse to reset while a __hostcall is suspended in Asyncify: the current
     * fiber (and the VM) are still live on the paused C stack and will resume
     * when the host promise settles. Freeing them here would leave that resume
     * dereferencing freed memory. The host should settle/finish the hostcall
     * before resetting. */
    if (g_hostcall_active)
        return;
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
EXPORT const char *lamassu_eval(const char *src_utf8) {
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
        JsValue p = js_run_module(g_ctx, fn);
        int st = js_promise_state(p);
        bool ok = st == 0 || st == 1;
        JsValue result = js_promise_result(p);
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
