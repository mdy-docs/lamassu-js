/*
 * lamassu reactor — the fleet embedding: precompiled bytecode in, output out.
 *
 * This is a WASI *reactor*, not a command: it has no main, its instance stays
 * alive across calls, and the host drives it through the exports below. It
 * links liblamassu_runtime.a ALONE — note that this file includes <lamassu.h>
 * and not <lamassu_compile.h>, and that there is no js_compile_module call
 * anywhere in it. A process built from this cannot turn source text into code,
 * because the lexer, parser and compiler are not in the binary at all. That is
 * the property the whole frontend/runtime split exists to produce, and
 * `make check-runtime-only` is the same claim checked at link time.
 *
 * THE SHAPE A FLEET WANTS. `lam_load` verifies and keeps one function; `lam_run`
 * executes it and can be called repeatedly against that same loaded function.
 * So the expensive half (parse, compile, verify) happens once, ahead of time,
 * and a request pays only for execution:
 *
 *     build time     lamassu --emit-bytecode page.js page.jsbc
 *     init (once)    lam_configure, lam_load(page.jsbc)      <- Wizer snapshots here
 *     per request    lam_set_input(json), lam_run(), lam_out_ptr/len
 *
 * The init/request boundary is deliberately where a Wizer pre-initialization
 * cut would go: everything before it is pure setup with no host interaction, so
 * a snapshot taken after lam_load leaves each request instantiating a warm heap
 * with the bytecode already verified and resident.
 *
 * FRESH INSTANCE PER REQUEST is still the right deployment: with wasmtime's
 * pooling allocator and copy-on-write memory that costs microseconds, and it
 * removes cross-tenant state leakage as a category rather than managing it.
 * lam_reset exists for hosts that reuse an instance within one tenant.
 *
 * LIMITS. lam_configure sets the interpreter's fuel budget and heap cap. Both
 * are enforced inside the engine; the host should ALSO bound execution from
 * outside (wasmtime epoch interruption or its own fuel) — the two are
 * independent, and only the outer one survives a bug in the inner.
 *
 * ABI. Everything crosses as bytes in linear memory. The host calls lam_alloc
 * to get a region, writes into it, and passes (ptr, len); results come back as
 * a pointer/length pair the host reads before the next call overwrites it.
 * No component model and no WIT: this is a core module, which instantiates
 * leaner than a component and is what the pooling allocator is built around.
 */
#include <stdlib.h>
#include <string.h>

#include "lamassu.h"
#include "utf8.h"

#define EXPORT(name) __attribute__((export_name(#name)))

static JsVm *g_vm;
static JsContext *g_ctx;
static JsValue g_fn;       /* the loaded function; a GC root while g_loaded */
static bool g_loaded;
static uint64_t g_fuel;
static size_t g_heap_limit;

/* Request input, handed to the guest by __input(). */
static uint16_t *g_input;
static size_t g_input_len;

/* Growing UTF-8 output buffer, reused across runs. */
static char *g_buf;
static size_t g_len, g_cap;

static void buf_reset(void) {
    g_len = 0;
    if (g_buf)
        g_buf[0] = 0;
}

static void buf_bytes(const char *b, size_t n) {
    if (g_len + n + 1 > g_cap) {
        size_t ncap = g_cap ? g_cap * 2 : 256;
        while (ncap < g_len + n + 1)
            ncap *= 2;
        char *nb = realloc(g_buf, ncap);
        if (!nb)
            return; /* drop output rather than fail the run */
        g_buf = nb;
        g_cap = ncap;
    }
    memcpy(g_buf + g_len, b, n);
    g_len += n;
    g_buf[g_len] = 0;
}

static void buf_utf16(const uint16_t *u, size_t n) {
    for (size_t i = 0; i < n;) {
        char t[4];
        int len = js_utf8_encode_cp(js_utf16_next_cp(u, n, &i), t);
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

/* ---- guest-visible natives ---- */

/* print(...args): appends to the output buffer, space-separated. */
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

/* __input(): the request payload the host set, as a string ("" if none). */
static bool native_input(JsContext *ctx, JsValue this_val, const JsValue *args,
                         int argc, JsValue *result) {
    (void)ctx;
    (void)this_val;
    (void)args;
    (void)argc;
    static const uint16_t empty[1] = {0};
    *result = js_atom(g_vm, g_input ? g_input : empty, g_input ? g_input_len : 0);
    return true;
}

/* ---- lifecycle ---- */

static void teardown(void) {
    if (g_loaded) {
        js_gc_unprotect(g_vm, &g_fn);
        g_loaded = false;
    }
    if (g_vm) {
        js_vm_free(g_vm);
        g_vm = NULL;
        g_ctx = NULL;
    }
}

/* Builds a fresh VM + context with the configured limits and natives. */
static bool bring_up(void) {
    JsVmConfig cfg = {0};
    cfg.heap_limit = g_heap_limit;
    g_vm = js_vm_new(g_heap_limit ? &cfg : NULL);
    g_ctx = g_vm ? js_context_new(g_vm) : NULL;
    if (!g_ctx) {
        teardown();
        return false;
    }
    if (g_fuel)
        js_context_set_fuel(g_ctx, g_fuel);
    static const uint16_t print_name[] = {'p', 'r', 'i', 'n', 't'};
    static const uint16_t input_name[] = {'_', '_', 'i', 'n', 'p', 'u', 't'};
    js_register_native(g_ctx, print_name, 5, native_print, NULL);
    js_register_native(g_ctx, input_name, 7, native_input, NULL);
    return true;
}

static bool ensure_vm(void) { return g_ctx ? true : bring_up(); }

/* ---- exports ---- */

/*
 * Host-owned scratch in linear memory. lam_alloc/lam_free are the guest's
 * allocator, exposed so the host can hand bytes in without a second copy.
 */
EXPORT(lam_alloc) void *lam_alloc(uint32_t n) { return malloc(n ? n : 1); }
EXPORT(lam_free) void lam_free(void *p, uint32_t n) {
    (void)n;
    free(p);
}

/*
 * Fuel budget (0 = unlimited) and heap cap in bytes (0 = unlimited). Takes
 * effect on the next VM bring-up, so call it before lam_load. The host should
 * bound execution from outside as well — see the note at the top.
 */
EXPORT(lam_configure) void lam_configure(uint64_t fuel, uint32_t heap_limit) {
    g_fuel = fuel;
    g_heap_limit = heap_limit;
    teardown(); /* limits apply to a fresh VM */
}

/*
 * Loads and fully validates a bytecode buffer (js_bytecode_load treats it as
 * hostile: bad magic, truncation, out-of-range indices, jumps off an
 * instruction boundary, and stack underflow/overflow are all rejected).
 * Returns 0 on success, -1 on failure with the reason in the output buffer.
 * The loaded function is kept and rooted, so lam_run can be called repeatedly.
 */
EXPORT(lam_load) int32_t lam_load(const uint8_t *bc, uint32_t len) {
    buf_reset();
    if (!ensure_vm()) {
        buf_bytes("out of memory", 13);
        return -1;
    }
    if (g_loaded) {
        js_gc_unprotect(g_vm, &g_fn);
        g_loaded = false;
    }
    const char *err = NULL;
    JsValue fn = js_bytecode_load(g_ctx, bc, len, &err);
    if (!js_is_function(fn)) {
        buf_bytes(err ? err : "invalid bytecode", strlen(err ? err : "invalid bytecode"));
        return -1;
    }
    g_fn = fn;
    js_gc_protect(g_vm, &g_fn);
    g_loaded = true;
    return 0;
}

/*
 * Sets the payload __input() returns, as UTF-8. Cleared by lam_reset. Passing
 * len 0 clears it.
 */
EXPORT(lam_set_input) int32_t lam_set_input(const uint8_t *utf8, uint32_t len) {
    free(g_input);
    g_input = NULL;
    g_input_len = 0;
    if (!len)
        return 0;
    uint16_t *u = malloc((size_t)len * sizeof(uint16_t));
    if (!u)
        return -1;
    g_input_len = js_utf8_to_utf16(utf8, len, u);
    g_input = u;
    return 0;
}

/*
 * Runs the loaded function. Returns 0 if it completed, 1 if it threw or is
 * still pending (nothing here settles host promises), -1 if nothing is loaded.
 * Output — everything print() emitted, then the completion value or the error —
 * is at lam_out_ptr()/lam_out_len(), valid until the next call.
 */
EXPORT(lam_run) int32_t lam_run(void) {
    buf_reset();
    if (!g_loaded)
        return -1;
    JsValue p = js_run_module(g_ctx, g_fn);
    js_gc_protect(g_vm, &p);
    int st = js_promise_state(p);
    JsValue result = js_promise_result(p);
    js_gc_protect(g_vm, &result);
    if (st == 2)
        buf_bytes("uncaught ", 9);
    buf_value(g_ctx, result);
    js_gc_unprotect(g_vm, &result);
    js_gc_unprotect(g_vm, &p);
    return st == 2 || st == 0 ? 1 : 0;
}

EXPORT(lam_out_ptr) const char *lam_out_ptr(void) { return g_buf ? g_buf : ""; }
EXPORT(lam_out_len) uint32_t lam_out_len(void) { return (uint32_t)g_len; }

/*
 * Drops the VM, the loaded function and the input. The next lam_load starts
 * from a clean realm. A host reusing one instance across tenants should prefer
 * a fresh instance instead — with pooling + CoW that is cheaper than it sounds
 * and forecloses state leakage entirely.
 */
EXPORT(lam_reset) void lam_reset(void) {
    teardown();
    free(g_input);
    g_input = NULL;
    g_input_len = 0;
    buf_reset();
}
