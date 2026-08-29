#include "js_error.h"
#include "js_bytecode.h"

#define ARG(i) ((i) < argc ? args[i] : js_undefined())
#define REPR_MAX_DEPTH 8

static const char *const kind_names[JS_ERROR_KIND_COUNT] = {
    "Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError",
};

static bool nthrow(JsContext *ctx, JsValue *r, const char *msg) {
    *r = js_error_from_ascii(ctx, msg);
    return false;
}

/* Interned ASCII key. Allocates (may collect): callers root what they hold. */
static JsString *key(JsContext *ctx, const char *k) {
    JsString *s = js_ascii_cell(ctx->vm, k);
    return s ? js_intern_cell(ctx->vm, s) : NULL;
}

/* Own-or-inherited property by ASCII key — the ordinary chain walk. */
static bool chain_get(JsContext *ctx, JsObject *o, const char *k, JsValue *out, bool *found) {
    *out = js_undefined();
    *found = false;
    JsString *ik = key(ctx, k);
    if (!ik)
        return false;
    for (JsObject *cur = o; cur;) {
        JsValue v = js_map_get(&cur->props, ik, found);
        if (*found) {
            *out = v;
            return true;
        }
        cur = js_is_object(cur->proto) ? js_value_object(cur->proto) : NULL;
    }
    return true;
}

JsValue js_error_new(JsContext *ctx, JsErrorKind kind, JsString *message) {
    JsVm *vm = ctx->vm;
    JsValue mv = message ? js_value_from_cell(&message->gc) : js_undefined();
    js_gc_protect(vm, &mv);
    JsValue ev = js_object_new(ctx);
    bool ok = js_is_object(ev);
    if (ok) {
        JsObject *e = js_value_object(ev);
        e->obj_kind = JS_OBJ_ERROR;
        JsObject *proto = ctx->error_protos[kind];
        if (proto)
            e->proto = js_value_from_cell(&proto->gc);
        if (message) {
            js_gc_protect(vm, &ev);
            ok = js_object_set_ascii(ctx, e, "message", mv);
            js_gc_unprotect(vm, &ev);
        }
    }
    js_gc_unprotect(vm, &mv);
    return ok ? ev : js_undefined();
}

/* "TypeError: x" -> (JS_ERROR_TYPE, "x"); anything else -> (PLAIN, whole). */
static JsErrorKind split_prefix(const JsString *s, uint32_t *msg_off) {
    *msg_off = 0;
    for (int k = 0; k < JS_ERROR_KIND_COUNT; k++) {
        const char *name = kind_names[k];
        uint32_t n = 0;
        while (name[n] && n < s->length && s->units[n] == (uint16_t)(unsigned char)name[n])
            n++;
        if (name[n] != '\0')
            continue;
        if (n + 2 <= s->length && s->units[n] == ':' && s->units[n + 1] == ' ') {
            *msg_off = n + 2;
            return (JsErrorKind)k;
        }
    }
    return JS_ERROR_PLAIN;
}

JsValue js_error_from_cell(JsContext *ctx, JsString *full) {
    JsVm *vm = ctx->vm;
    JsValue fallback = js_value_from_cell(&full->gc);
    js_gc_protect(vm, &fallback);
    uint32_t off;
    JsErrorKind kind = split_prefix(full, &off);
    JsString *msg = off ? js_string_cell_new(vm, full->units + off, full->length - off) : full;
    JsValue ev = msg ? js_error_new(ctx, kind, msg) : js_undefined();
    js_gc_unprotect(vm, &fallback);
    return js_is_object(ev) ? ev : fallback;
}

JsValue js_error_from_ascii(JsContext *ctx, const char *msg) {
    JsString *s = js_ascii_cell(ctx->vm, msg);
    return s ? js_error_from_cell(ctx, s) : js_oom_value(ctx->vm);
}

JsString *js_error_repr(JsContext *ctx, JsObject *o, int depth) {
    JsVm *vm = ctx->vm;
    if (depth >= REPR_MAX_DEPTH)
        return js_string_cell_new(vm, NULL, 0);
    JsValue ov = js_value_from_cell(&o->gc);
    js_gc_protect(vm, &ov);
    JsString *out = NULL;
    JsValue nv, mv, nsv = js_undefined();
    bool found;
    if (!chain_get(ctx, o, "name", &nv, &found))
        goto done;
    JsString *name = js_is_undefined(nv) ? js_ascii_cell(vm, "Error")
                                         : js_to_string_cell(ctx, nv, depth + 1);
    if (!name)
        goto done;
    nsv = js_value_from_cell(&name->gc);
    js_gc_protect(vm, &nsv);
    if (!chain_get(ctx, o, "message", &mv, &found)) {
        js_gc_unprotect(vm, &nsv);
        goto done;
    }
    JsString *msg = js_is_undefined(mv) ? js_string_cell_new(vm, NULL, 0)
                                        : js_to_string_cell(ctx, mv, depth + 1);
    js_gc_unprotect(vm, &nsv);
    if (!msg)
        goto done;
    if (msg->length == 0) {
        out = name;
    } else if (name->length == 0) {
        out = msg;
    } else {
        size_t total = (size_t)name->length + 2 + msg->length;
        uint16_t *buf = js_realloc_raw(vm, NULL, 0, total * sizeof(uint16_t));
        if (!buf)
            goto done;
        size_t n = 0;
        for (uint32_t i = 0; i < name->length; i++)
            buf[n++] = name->units[i];
        buf[n++] = ':';
        buf[n++] = ' ';
        for (uint32_t i = 0; i < msg->length; i++)
            buf[n++] = msg->units[i];
        out = js_string_cell_new(vm, buf, total);
        js_realloc_raw(vm, buf, total * sizeof(uint16_t), 0);
    }
done:
    js_gc_unprotect(vm, &ov);
    return out;
}

/* ---- constructors ---- */

static bool error_ctor(JsContext *ctx, JsErrorKind kind, const JsValue *args, int argc,
                       JsValue *r) {
    JsVm *vm = ctx->vm;
    JsValue mv = ARG(0);
    JsString *msg = NULL;
    if (!js_is_undefined(mv)) {
        msg = js_to_string_cell(ctx, mv, 0);
        if (!msg)
            return nthrow(ctx, r, "out of memory");
    }
    JsValue ev = js_error_new(ctx, kind, msg);
    if (!js_is_object(ev))
        return nthrow(ctx, r, "out of memory");
    JsValue opts = ARG(1);
    if (js_is_object(opts)) {
        js_gc_protect(vm, &ev);
        JsValue cause;
        bool found;
        bool ok = chain_get(ctx, js_value_object(opts), "cause", &cause, &found) &&
                  (!found || js_object_set_ascii(ctx, js_value_object(ev), "cause", cause));
        js_gc_unprotect(vm, &ev);
        if (!ok)
            return nthrow(ctx, r, "out of memory");
    }
    *r = ev;
    return true;
}

#define DEF_CTOR(fn, kind)                                                          \
    static bool fn(JsContext *ctx, JsValue tv, const JsValue *args, int argc,       \
                   JsValue *r) {                                                    \
        (void)tv;                                                                   \
        return error_ctor(ctx, kind, args, argc, r);                                \
    }
DEF_CTOR(g_Error, JS_ERROR_PLAIN)
DEF_CTOR(g_TypeError, JS_ERROR_TYPE)
DEF_CTOR(g_RangeError, JS_ERROR_RANGE)
DEF_CTOR(g_ReferenceError, JS_ERROR_REFERENCE)
DEF_CTOR(g_SyntaxError, JS_ERROR_SYNTAX)
#undef DEF_CTOR

static const JsNativeFn kind_ctors[JS_ERROR_KIND_COUNT] = {
    g_Error, g_TypeError, g_RangeError, g_ReferenceError, g_SyntaxError,
};

/* Error.prototype.toString: "name: message" for any object receiver. */
static bool errp_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    if (!js_is_object(tv))
        return nthrow(ctx, r, "TypeError: Error.prototype.toString called on a non-object");
    JsString *s = js_error_repr(ctx, js_value_object(tv), 0);
    if (!s)
        return nthrow(ctx, r, "out of memory");
    *r = js_value_from_cell(&s->gc);
    return true;
}

/* ---- registration ---- */

bool js_error_builtins_init(JsContext *ctx) {
    JsVm *vm = ctx->vm;
    for (int k = 0; k < JS_ERROR_KIND_COUNT; k++) {
        /* The prototype is assigned into ctx before the next allocation so it
         * is a GC root immediately; instances point their [[Prototype]] here
         * and Ctor.prototype is this same object (see js_mapobj.c). */
        JsValue pv = js_object_new(ctx);
        if (!js_is_object(pv))
            return false;
        JsObject *proto = js_value_object(pv);
        ctx->error_protos[k] = proto;
        if (k != JS_ERROR_PLAIN)
            proto->proto = js_value_from_cell(&ctx->error_protos[JS_ERROR_PLAIN]->gc);

        JsString *name = js_ascii_cell(vm, kind_names[k]);
        if (!name || !js_object_set_ascii(ctx, proto, "name", js_value_from_cell(&name->gc)))
            return false;
        if (k == JS_ERROR_PLAIN) {
            JsString *empty = js_string_cell_new(vm, NULL, 0);
            if (!empty || !js_object_set_ascii(ctx, proto, "message", js_value_from_cell(&empty->gc)))
                return false;
            JsValue ts = js_native_new(ctx, "toString", errp_toString, NULL);
            if (!js_is_function(ts) || !js_object_set_ascii(ctx, proto, "toString", ts))
                return false;
        }

        JsValue ctor = js_native_new(ctx, kind_names[k], kind_ctors[k], NULL);
        if (!js_is_function(ctor))
            return false;
        ((JsNative *)js_value_cell(ctor))->prototype = proto;
        if (!js_object_set_ascii(ctx, proto, "constructor", ctor) ||
            !js_object_set_ascii(ctx, ctx->globals, kind_names[k], ctor))
            return false;
    }
    return true;
}
