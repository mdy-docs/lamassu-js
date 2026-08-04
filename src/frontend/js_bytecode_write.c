/*
 * Bytecode serialization (phase 8) — the build-time half of the format.
 *
 * "Compile once, run many": js_bytecode_serialize turns a compiled top-level
 * function into a portable byte buffer, js_bc_serialize_module does the same
 * for one module (body + link metadata). Both are build-time operations — they
 * consume what the compiler just produced — which is why they live in the
 * FRONTEND half and a runtime-only process links neither.
 *
 * The reader is src/runtime/js_bytecode_read.c; the format they share is
 * src/js_bcformat.h. Nothing here validates: the writer's input is a function
 * cell this process just compiled, so it is trusted by construction. Every
 * check that matters happens on the read side, against untrusted bytes.
 *
 * Only non-module functions are portable through js_bytecode_serialize: module
 * bodies carry GET_EXPORT/GET_IMPORT opcodes bound to a live JsModule that has
 * no standalone meaning, so it refuses a module fn (they go through
 * js_bc_serialize_module instead, which records the link metadata too).
 */
#include "js_bcformat.h"
#include "js_compile.h"
#include "lamassu_internal.h"

/* =====================================================================
 * Serialization
 * ===================================================================== */

typedef struct {
    JsVm *vm;
    uint8_t *data;
    size_t len, cap;
    bool oom;
} OutBuf;

static bool out_reserve(OutBuf *b, size_t extra) {
    if (b->oom)
        return false;
    if (b->len + extra <= b->cap)
        return true;
    size_t ncap = b->cap ? b->cap * 2 : 256;
    while (ncap < b->len + extra)
        ncap *= 2;
    uint8_t *nd = js_realloc_raw(b->vm, b->data, b->cap, ncap);
    if (!nd) {
        b->oom = true;
        return false;
    }
    b->data = nd;
    b->cap = ncap;
    return true;
}

static void out_u8(OutBuf *b, uint8_t v) {
    if (out_reserve(b, 1))
        b->data[b->len++] = v;
}
static void out_u16(OutBuf *b, uint16_t v) {
    out_u8(b, (uint8_t)(v & 0xFF));
    out_u8(b, (uint8_t)(v >> 8));
}
static void out_u32(OutBuf *b, uint32_t v) {
    out_u8(b, (uint8_t)(v & 0xFF));
    out_u8(b, (uint8_t)((v >> 8) & 0xFF));
    out_u8(b, (uint8_t)((v >> 16) & 0xFF));
    out_u8(b, (uint8_t)(v >> 24));
}
static void out_bytes(OutBuf *b, const void *p, size_t n) {
    if (out_reserve(b, n)) {
        memcpy(b->data + b->len, p, n);
        b->len += n;
    }
}
static void out_units(OutBuf *b, const uint16_t *u, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        out_u16(b, u[i]);
}

static bool serialize_fn(OutBuf *b, JsFunctionCell *fn, uint32_t depth);

static bool serialize_const(OutBuf *b, JsValue v, uint32_t depth) {
    if (v.bits == JS_SPECIAL_UNDEFINED) {
        out_u8(b, CTAG_UNDEFINED);
    } else if (v.bits == JS_SPECIAL_NULL) {
        out_u8(b, CTAG_NULL);
    } else if (v.bits == JS_SPECIAL_FALSE) {
        out_u8(b, CTAG_FALSE);
    } else if (v.bits == JS_SPECIAL_TRUE) {
        out_u8(b, CTAG_TRUE);
    } else if (js_is_number(v)) {
        out_u8(b, CTAG_NUMBER);
        out_u32(b, (uint32_t)(v.bits & 0xFFFFFFFF));
        out_u32(b, (uint32_t)(v.bits >> 32));
    } else if (js_is_string(v)) {
        JsString *s = js_value_string(v);
        out_u8(b, CTAG_STRING);
        out_u32(b, s->length);
        out_units(b, s->units, s->length);
    } else if (js_value_is_cell(v) && js_value_cell(v)->kind == JS_KIND_FUNCTION) {
        out_u8(b, CTAG_FUNCTION);
        return serialize_fn(b, js_value_function(v), depth + 1);
    } else {
        /* objects/promises/closures/natives never appear as compiled consts */
        b->oom = true; /* reuse the error channel: aborts with a clean failure */
        return false;
    }
    return true;
}

static bool serialize_fn(OutBuf *b, JsFunctionCell *fn, uint32_t depth) {
    if (depth > JSBC_MAX_FN_DEPTH)
        return false;
    out_u16(b, fn->n_params);
    out_u8(b, fn->fn_flags);
    out_u32(b, fn->n_locals);
    out_u16(b, fn->n_upvals);
    for (uint16_t i = 0; i < fn->n_upvals; i++) {
        out_u16(b, fn->upvals[i].idx);
        out_u8(b, fn->upvals[i].from_local);
    }
    if (fn->name) {
        out_u8(b, 1);
        out_u32(b, fn->name->length);
        out_units(b, fn->name->units, fn->name->length);
    } else {
        out_u8(b, 0);
    }
    out_u32(b, fn->code_len);
    out_bytes(b, fn->code, fn->code_len);
    out_u32(b, fn->const_count);
    for (uint32_t i = 0; i < fn->const_count; i++) {
        if (!serialize_const(b, fn->consts[i], depth))
            return false;
    }
    out_u32(b, fn->line_count);
    for (uint32_t i = 0; i < fn->line_count; i++) {
        out_u32(b, fn->lines[i].off);
        out_u32(b, fn->lines[i].pos);
    }
    return !b->oom;
}

/* 16-byte header: magic, version, feature flags, buffer kind (script/module). */
static void write_header(OutBuf *b, uint32_t kind) {
    out_u8(b, 'J');
    out_u8(b, 'S');
    out_u8(b, 'B');
    out_u8(b, 'C');
    out_u32(b, JSBC_VERSION);
    uint32_t flags = 0;
#ifdef LAMASSU_HAS_REGEX
    flags |= JSBC_FLAG_HAS_REGEX;
#endif
    out_u32(b, flags);
    out_u32(b, kind);
}

bool js_bytecode_serialize(JsContext *ctx, JsValue fn, uint8_t **out, size_t *out_len) {
    if (!js_is_function(fn) || !js_value_is_cell(fn) ||
        js_value_cell(fn)->kind != JS_KIND_FUNCTION)
        return false;
    if (js_value_function(fn)->module) /* a module body — use the module API */
        return false;
    OutBuf b = {ctx->vm, NULL, 0, 0, false};
    write_header(&b, JS_BC_SCRIPT);
    if (!serialize_fn(&b, js_value_function(fn), 0) || b.oom) {
        js_realloc_raw(ctx->vm, b.data, b.cap, 0);
        return false;
    }
    /* trim to exact length so js_bytecode_free's size is unambiguous */
    if (b.cap != b.len) {
        uint8_t *trimmed = js_realloc_raw(ctx->vm, b.data, b.cap, b.len);
        if (trimmed)
            b.data = trimmed;
    }
    *out = b.data;
    *out_len = b.len;
    return true;
}

void js_bytecode_free(JsContext *ctx, uint8_t *buf, size_t len) {
    js_realloc_raw(ctx->vm, buf, len, 0);
}

/* =====================================================================
 * Module records — write side (specifier + import/star/dep metadata + body).
 * Runtime state (resolved deps, live exports, status) is never written; the
 * loader rebuilds it.
 * ===================================================================== */

static void out_str(OutBuf *b, const JsString *s) {
    out_u32(b, s->length);
    out_units(b, s->units, s->length);
}
static void out_opt_str(OutBuf *b, const JsString *s) {
    if (s) {
        out_u8(b, 1);
        out_str(b, s);
    } else {
        out_u8(b, 0);
    }
}

bool js_bc_serialize_module(JsContext *ctx, JsModule *m, uint8_t **out, size_t *out_len) {
    if (!m || !m->body)
        return false;
    OutBuf b = {ctx->vm, NULL, 0, 0, false};
    write_header(&b, JS_BC_MODULE);
    out_str(&b, m->specifier);

    out_u32(&b, m->import_count);
    for (uint32_t i = 0; i < m->import_count; i++) {
        out_u8(&b, m->imports[i].kind);
        out_str(&b, m->imports[i].specifier);
        out_opt_str(&b, m->imports[i].imported_name);
    }
    out_u32(&b, m->star_count);
    for (uint32_t i = 0; i < m->star_count; i++) {
        out_u8(&b, m->stars[i].kind);
        out_str(&b, m->stars[i].specifier);
        out_opt_str(&b, m->stars[i].imported);
        out_opt_str(&b, m->stars[i].exported);
    }
    out_u32(&b, m->dep_spec_count);
    for (uint32_t i = 0; i < m->dep_spec_count; i++)
        out_str(&b, m->dep_specs[i]);

    if (!serialize_fn(&b, m->body, 0) || b.oom) {
        js_realloc_raw(ctx->vm, b.data, b.cap, 0);
        return false;
    }
    if (b.cap != b.len) {
        uint8_t *trimmed = js_realloc_raw(ctx->vm, b.data, b.cap, b.len);
        if (trimmed)
            b.data = trimmed;
    }
    *out = b.data;
    *out_len = b.len;
    return true;
}
