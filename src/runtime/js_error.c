#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
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
        if (*found && v.bits == JS_SPECIAL_TDZ) { /* a namespace's uninitialized export */
            *found = false;
            return true;
        }
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
        e->error_cause = JS_CAUSE_GUEST;
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

static bool units_equal_ascii(const JsString *s, const char *ascii) {
    uint32_t n = 0;
    while (ascii[n]) {
        if (n >= s->length || s->units[n] != (uint16_t)(unsigned char)ascii[n])
            return false;
        n++;
    }
    return n == s->length;
}

/* "out of memory", or a site-specific variant ("internal: out of memory",
 * "module load failed (out of memory)"): every one is the allocator saying no. */
static bool is_oom_text(const JsString *s) {
    static const char tail[] = JS_MSG_OOM;
    uint32_t tl = sizeof tail - 1;
    if (s->length < tl)
        return false;
    uint32_t off = s->length - tl;
    for (uint32_t i = 0; i < tl; i++) {
        if (s->units[off + i] != (uint16_t)(unsigned char)tail[i])
            return false;
    }
    if (off == 0)
        return true;
    uint16_t before = s->units[off - 1];
    if (before == ' ' || before == '(')
        return true;
    /* "...(out of memory)" */
    return off >= 1 && s->units[off - 1] == '(' ;
}

/* The engine-imposed stops are recognized by their exact texts (the
 * JS_MSG_* constants the raise sites use); a guest cannot reach this path. */
static JsErrorCause cause_of_text(const JsString *s) {
    if (units_equal_ascii(s, JS_MSG_BUDGET))
        return JS_CAUSE_BUDGET;
    if (units_equal_ascii(s, JS_MSG_INTERRUPT))
        return JS_CAUSE_INTERRUPT;
    if (is_oom_text(s))
        return JS_CAUSE_OOM;
    return JS_CAUSE_GUEST;
}

JsValue js_error_from_cell(JsContext *ctx, JsString *full) {
    JsVm *vm = ctx->vm;
    JsValue fallback = js_value_from_cell(&full->gc);
    js_gc_protect(vm, &fallback);
    uint32_t off;
    JsErrorKind kind = split_prefix(full, &off);
    JsErrorCause cause = cause_of_text(full);
    JsString *msg = off ? js_string_cell_new(vm, full->units + off, full->length - off) : full;
    JsValue ev = msg ? js_error_new(ctx, kind, msg) : js_undefined();
    js_gc_unprotect(vm, &fallback);
    if (!js_is_object(ev)) {
        /* The only way to get here is an allocation failing, so the honest
         * value is the prebuilt OOM string — which js_error_cause recognizes
         * by identity, so no guest-built string can impersonate it. If even
         * that is missing (a VM that never finished construction), keep the
         * text rather than lose it. */
        JsValue oom = js_oom_value(vm);
        return js_is_undefined(oom) ? fallback : oom;
    }
    js_value_object(ev)->error_cause = (uint8_t)cause;
    return ev;
}

JsErrorCause js_error_cause(JsContext *ctx, JsValue error) {
    if (js_is_object(error)) {
        JsObject *o = js_value_object(error);
        return o->obj_kind == JS_OBJ_ERROR ? (JsErrorCause)o->error_cause : JS_CAUSE_GUEST;
    }
    /* The bare-string fallback is exactly one cell, the VM's prebuilt OOM
     * value — identity, not text, so `throw 'out of memory'` from a guest is
     * still JS_CAUSE_GUEST. */
    if (js_is_string(error) && js_value_string(error) == ctx->vm->oom_error)
        return JS_CAUSE_OOM;
    return JS_CAUSE_GUEST;
}

/* ---- formatted engine errors ---- */

#define FMT_MAX_ARGS 6
#define FMT_MAX_PIECE 120

typedef struct {
    uint16_t *u;
    size_t n, cap;
    bool oom;
} FmtBuf;

static void fb_units(FmtBuf *b, const uint16_t *u, size_t n) {
    if (b->oom || n == 0) /* an empty piece: nothing to copy (u or b->u may be NULL) */
        return;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->n + n)
            cap *= 2;
        uint16_t *nu = realloc(b->u, cap * sizeof *nu);
        if (!nu) {
            b->oom = true;
            return;
        }
        b->u = nu;
        b->cap = cap;
    }
    memcpy(b->u + b->n, u, n * sizeof *u);
    b->n += n;
}

static void fb_ascii(FmtBuf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint16_t c = (uint16_t)(unsigned char)s[i];
        fb_units(b, &c, 1);
    }
}

/* A piece of at most FMT_MAX_PIECE units, cut short with "..." */
static void fb_piece(FmtBuf *b, const uint16_t *u, size_t n) {
    if (n > FMT_MAX_PIECE) {
        fb_units(b, u, FMT_MAX_PIECE);
        fb_ascii(b, "...", 3);
    } else {
        fb_units(b, u, n);
    }
}

/* A function's name for a message, or "anonymous" */
static void fb_function_name(JsContext *ctx, FmtBuf *b, JsValue v) {
    JsGcCell *c = js_value_cell(v);
    const JsString *name = NULL;
    if (c->kind == JS_KIND_CLOSURE)
        name = ((JsClosure *)c)->fn->name;
    else if (c->kind == JS_KIND_NATIVE)
        name = ((JsNative *)c)->name;
    (void)ctx;
    if (name && name->length)
        fb_piece(b, name->units, name->length);
    else
        fb_ascii(b, "anonymous", 9);
}

JsValue js_error_fmt(JsContext *ctx, const char *fmt, ...) {
    JsVm *vm = ctx->vm;
    /* First the arguments that need no allocation are copied out (text, strings), so the
     * conversions of %v values below, which allocate and may collect, cannot free them. */
    FmtBuf pieces[FMT_MAX_ARGS] = {{0}};
    JsValue values[FMT_MAX_ARGS];
    char kinds[FMT_MAX_ARGS];
    int nargs = 0;
    va_list ap;
    va_start(ap, fmt);
    for (const char *f = fmt; *f; f++) {
        if (*f != '%' || !f[1] || f[1] == '%') {
            if (*f == '%' && f[1])
                f++;
            continue;
        }
        f++;
        if (nargs == FMT_MAX_ARGS)
            break;
        int i = nargs++;
        kinds[i] = *f;
        values[i] = js_undefined();
        if (*f == 's') {
            const char *s = va_arg(ap, const char *);
            fb_ascii(&pieces[i], s, strlen(s));
        } else if (*f == 'S') {
            const JsString *s = va_arg(ap, const JsString *);
            if (s)
                fb_piece(&pieces[i], s->units, s->length);
        } else if (*f == 'v' || *f == 't') {
            values[i] = va_arg(ap, JsValue);
        }
    }
    va_end(ap);
    for (int i = 0; i < nargs; i++)
        js_gc_protect(vm, &values[i]);
    for (int i = 0; i < nargs; i++) {
        if (kinds[i] != 'v' && kinds[i] != 't')
            continue;
        JsValue v = values[i];
        FmtBuf *b = &pieces[i];
        if (kinds[i] == 't') {
            /* V8's typed form for built-ins' messages: number 5, string "x", boolean true,
             * object null, undefined, object (any object), function */
            if (js_is_undefined(v)) {
                fb_ascii(b, "undefined", 9);
                continue;
            }
            if (js_is_null(v)) {
                fb_ascii(b, "object null", 11);
                continue;
            }
            if (js_is_function(v)) {
                fb_ascii(b, "function", 8);
                continue;
            }
            if (js_is_object(v) || js_is_promise(v)) {
                fb_ascii(b, "object", 6);
                continue;
            }
            if (js_is_string(v)) {
                JsString *s = js_value_string(v);
                fb_ascii(b, "string \"", 8);
                fb_piece(b, s->units, s->length);
                fb_ascii(b, "\"", 1);
                continue;
            }
            if (js_is_number(v))
                fb_ascii(b, "number ", 7);
            else if (js_is_bool(v))
                fb_ascii(b, "boolean ", 8);
        }
        if (js_is_string(v)) {
            JsString *s = js_value_string(v);
            fb_piece(b, s->units, s->length);
        } else if (js_is_function(v)) {
            fb_function_name(ctx, b, v);
        } else if (js_is_object(v)) {
            if (js_value_object(v)->obj_kind == JS_OBJ_ARRAY)
                fb_ascii(b, "[object Array]", 14);
            else if (js_value_object(v)->obj_flags & JS_OBJ_NAMESPACE)
                fb_ascii(b, "[object Module]", 15);
            else
                fb_ascii(b, "#<Object>", 9);
        } else if (js_is_promise(v)) {
            fb_ascii(b, "#<Promise>", 10);
        } else {
            JsString *s = js_to_string_cell(ctx, v, 0); /* a primitive: no guest code runs */
            if (s)
                fb_piece(b, s->units, s->length);
            else
                b->oom = true;
        }
    }
    for (int i = 0; i < nargs; i++)
        js_gc_unprotect(vm, &values[i]);

    FmtBuf out = {0};
    int arg = 0;
    for (const char *f = fmt; *f; f++) {
        if (*f == '%' && f[1] == '%') {
            fb_ascii(&out, "%", 1);
            f++;
        } else if (*f == '%' && f[1]) {
            f++;
            if (arg < nargs) {
                if (pieces[arg].oom)
                    out.oom = true;
                fb_units(&out, pieces[arg].u, pieces[arg].n);
                arg++;
            }
        } else {
            fb_ascii(&out, f, 1);
        }
    }
    for (int i = 0; i < nargs; i++)
        free(pieces[i].u);
    JsValue r;
    if (out.oom) {
        r = js_oom_value(vm);
    } else {
        JsString *s = js_string_cell_new(vm, out.u, (uint32_t)out.n);
        r = s ? js_error_from_cell(ctx, s) : js_oom_value(vm);
    }
    free(out.u);
    return r;
}

JsValue js_error_receiver(JsContext *ctx, const char *cls, JsValue receiver) {
    const JsString *name = ctx->running_native ? ctx->running_native->name : NULL;
    if (name && name->length)
        return js_error_fmt(ctx, "TypeError: Method %s.prototype.%S called on incompatible receiver %v",
                            cls, name, receiver);
    return js_error_fmt(ctx, "TypeError: Method %s.prototype method called on incompatible receiver %v",
                        cls, receiver);
}

/* Public constructors (lamassu.h): a message of any length, no "Name: " prefix parsing. */
JsValue js_error_new_utf16(JsContext *ctx, JsErrorKind kind, const uint16_t *message, size_t len) {
    if ((unsigned)kind >= JS_ERROR_KIND_COUNT)
        kind = JS_ERROR_PLAIN;
    JsString *s = js_string_cell_new(ctx->vm, message, (uint32_t)len);
    return s ? js_error_new(ctx, kind, s) : js_undefined();
}

JsValue js_error_new_ascii(JsContext *ctx, JsErrorKind kind, const char *message) {
    size_t n = strlen(message);
    uint16_t *u = malloc((n ? n : 1) * sizeof *u);
    if (!u)
        return js_undefined();
    for (size_t i = 0; i < n; i++)
        u[i] = (uint16_t)(unsigned char)message[i];
    JsValue v = js_error_new_utf16(ctx, kind, u, n);
    free(u);
    return v;
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
        return (*r = js_error_receiver(ctx, "Error", tv)), false;
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
