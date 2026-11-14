#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "js_error.h"
#include "js_bytecode.h"

#define ARG(i) ((i) < argc ? args[i] : js_undefined())

static const char *const kind_names[JS_ERROR_KIND_COUNT] = {
    "Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError",
    "EvalError", "URIError", "AggregateError",
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

/* A function in a message, as V8 shows one: its source text (Function.prototype.toString) */
static void fb_function(JsContext *ctx, FmtBuf *b, JsValue v) {
    JsString *s = js_function_source(ctx, v); /* v is rooted by the caller */
    if (s)
        fb_units(b, s->units, s->length);
    else
        b->oom = true;
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
            fb_function(ctx, b, v);
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
            JsString *s = js_primitive_to_string(vm, v);
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

/* Error.prototype.toString's "name: message" (20.5.3.4): each read from O and converted with
 * ToString, which can run their own toString (and nest, through an error as a name, up to the
 * re-entry limit, as V8's stack overflow). NULL: failed, the error pending (js_take_error). */
JsString *js_error_repr(JsContext *ctx, JsObject *o) {
    JsVm *vm = ctx->vm;
    JsValue ov = js_value_from_cell(&o->gc);
    js_gc_protect(vm, &ov);
    JsString *out = NULL;
    JsValue nv, mv, nsv = js_undefined();
    bool found;
    if (!chain_get(ctx, o, "name", &nv, &found))
        goto done;
    JsString *name = js_is_undefined(nv) ? js_ascii_cell(vm, "Error")
                                         : js_op_to_string(ctx, nv);
    if (!name)
        goto done;
    nsv = js_value_from_cell(&name->gc);
    js_gc_protect(vm, &nsv);
    if (!chain_get(ctx, o, "message", &mv, &found)) {
        js_gc_unprotect(vm, &nsv);
        goto done;
    }
    JsString *msg = js_is_undefined(mv) ? js_string_cell_new(vm, NULL, 0)
                                        : js_op_to_string(ctx, mv);
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

/* ---- e.stack ---- */

static bool key_is(JsValue key, const char *a) {
    if (!js_is_string(key))
        return false;
    const JsString *s = js_value_string(key);
    size_t n = strlen(a);
    if (s->length != n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (s->units[i] != (uint16_t)(unsigned char)a[i])
            return false;
    return true;
}

/* Error.stackTraceLimit as V8 reads it: false when it is not a number (no stack at all) */
static bool stack_trace_limit(JsContext *ctx, uint32_t *limit) {
    *limit = 10;
    if (!ctx->error_ctor)
        return true;
    JsObject *own = js_function_props(ctx, js_value_from_cell(&ctx->error_ctor->gc), false);
    JsValue v;
    bool found = false;
    if (own && !chain_get(ctx, own, "stackTraceLimit", &v, &found))
        return false;
    if (!found || !js_is_number(v))
        return false;
    double d = js_get_number(v);
    *limit = !(d > 0) ? 0 : d >= 100000 ? 100000 : (uint32_t)d;
    return true;
}

static void drop_frames(JsContext *ctx, JsErrorObject *e) {
    js_realloc_raw(ctx->vm, e->frames, (size_t)e->frame_count * sizeof *e->frames, 0);
    e->frames = NULL;
    e->frame_count = 0;
}

/* Takes the frames running now into E (rooted by the caller), as V8 does when an error is made:
 * at most Error.stackTraceLimit, leaving out those up to and including SKIP's call. False on
 * OOM. */
bool js_error_capture(JsContext *ctx, JsObject *o, JsValue skip) {
    JsErrorObject *e = (JsErrorObject *)o;
    drop_frames(ctx, e);
    e->stack = js_undefined();
    uint32_t limit;
    if (!stack_trace_limit(ctx, &limit)) {
        e->stack_state = JS_STACK_VALUE; /* V8: a `stack` that reads undefined */
        return true;
    }
    e->stack_state = JS_STACK_FRAMES;
    if (!limit)
        return true;
    uint32_t cap = limit < 256 ? limit : 256;
    for (;;) {
        JsErrorFrame *buf = js_realloc_raw(ctx->vm, NULL, 0, (size_t)cap * sizeof *buf);
        if (!buf)
            return false;
        uint32_t n = js_capture_frames(ctx, buf, cap, skip);
        if (n == cap && cap < limit) { /* a deep stack and a high limit: room for all of it */
            js_realloc_raw(ctx->vm, buf, (size_t)cap * sizeof *buf, 0);
            cap = cap * 4 < limit ? cap * 4 : limit;
            continue;
        }
        if (n < cap) {
            JsErrorFrame *t = n ? js_realloc_raw(ctx->vm, buf, (size_t)cap * sizeof *buf, (size_t)n * sizeof *buf)
                                : (js_realloc_raw(ctx->vm, buf, (size_t)cap * sizeof *buf, 0), NULL);
            buf = t;
            if (n && !buf)
                return false;
        }
        e->frames = buf;
        e->frame_count = n;
        return true;
    }
}

/* An error made by the interpreter for a raise is captured when the raise unwinds, once the
 * instruction's position is known (js_interp.c, on_error) */
bool js_error_capture_pending(JsContext *ctx, JsValue v) {
    if (!js_is_object(v) || js_value_object(v)->obj_kind != JS_OBJ_ERROR ||
        ((JsErrorObject *)js_value_object(v))->stack_state != JS_STACK_PENDING)
        return true;
    return js_error_capture(ctx, js_value_object(v), js_undefined());
}

/* ---- the text, as V8's CallSite serializes a frame ---- */

static void fb_cell_str(FmtBuf *b, const JsString *s) {
    if (s)
        fb_units(b, s->units, s->length);
}

static void fb_uint(FmtBuf *b, uint32_t n) {
    char t[16];
    int k = snprintf(t, sizeof t, "%u", n);
    fb_ascii(b, t, (size_t)k);
}

static bool str_prefix(const JsString *s, const JsString *p) {
    if (p->length > s->length)
        return false;
    return memcmp(s->units, p->units, p->length * sizeof(uint16_t)) == 0;
}

/* V8's StringEndsWithMethodName: S is M, or ends with "." M */
static bool ends_with_method(const JsString *s, const JsString *m) {
    if (s->length == m->length && memcmp(s->units, m->units, s->length * sizeof(uint16_t)) == 0)
        return true;
    if (s->length < m->length + 1)
        return false;
    uint32_t off = s->length - m->length;
    return s->units[off - 1] == '.' &&
           memcmp(s->units + off, m->units, m->length * sizeof(uint16_t)) == 0;
}

/* The name a frame shows for its function: its own (`name` as it was made, computed keys
 * included), else the one inferred from where it was defined (`F.m` for F.prototype.m = ...) */
static const JsString *frame_function_name(JsValue fn) {
    JsGcCell *c = js_value_cell(fn);
    const JsString *n = NULL;
    if (c->kind == JS_KIND_CLOSURE) {
        const JsClosure *cl = (const JsClosure *)c;
        n = cl->name && cl->name->length ? cl->name : cl->fn->name;
        if ((!n || !n->length) && cl->fn->debug_name)
            n = cl->fn->debug_name;
    } else if (c->kind == JS_KIND_NATIVE) {
        n = ((const JsNative *)c)->name;
    }
    return n && n->length ? n : NULL;
}

static JsObject *global_object(JsContext *ctx, const char *name) {
    JsString *k = key(ctx, name);
    if (!k)
        return NULL;
    bool found;
    JsValue v = js_map_get(&ctx->globals->props, k, &found);
    return found && js_is_object(v) ? js_value_object(v) : NULL;
}

/* V8's GetTypeName for a method call's receiver: a function's own name, else the receiver's
 * constructor's (a primitive's wrapper's; JSON and Math by their tags); NULL for an object with
 * no prototype. Allocates (the global lookups). */
static const JsString *type_name(JsContext *ctx, JsValue recv, JsString **tmp) {
    const char *fixed = NULL;
    if (js_is_function(recv)) {
        const JsString *n = frame_function_name(recv);
        if (n)
            return n;
        fixed = "Function";
    } else if (js_is_string(recv)) {
        fixed = "String";
    } else if (js_is_number(recv)) {
        fixed = "Number";
    } else if (js_is_bool(recv)) {
        fixed = "Boolean";
    } else if (js_is_promise(recv)) {
        fixed = "Promise";
    } else if (js_is_object(recv)) {
        JsObject *o = js_value_object(recv);
        if (o->obj_flags & JS_OBJ_NAMESPACE)
            fixed = "Module";
        else if (o->obj_kind == JS_OBJ_ARRAY)
            fixed = "Array";
        else if (o == global_object(ctx, "JSON"))
            fixed = "JSON";
        else if (o == global_object(ctx, "Math"))
            fixed = "Math";
        else {
            JsValue cv;
            bool found;
            if (chain_get(ctx, o, "constructor", &cv, &found) && found && js_is_function(cv)) {
                const JsString *n = frame_function_name(cv);
                if (n)
                    return n;
            }
            if (!js_is_object(o->proto))
                return NULL;
            fixed = o->obj_kind == JS_OBJ_MAP ? "Map" : o->obj_kind == JS_OBJ_SET ? "Set"
                    : o->obj_kind == JS_OBJ_DATE ? "Date" : o->obj_kind == JS_OBJ_REGEXP ? "RegExp"
                    : "Object";
        }
    }
    if (!fixed)
        return NULL;
    *tmp = js_ascii_cell(ctx->vm, fixed);
    return *tmp;
}

/* V8's GetMethodName: the key the receiver (or its prototype chain) holds the function under;
 * the function's own name first, else the one enumerable key with it (none if two) */
static const JsString *method_name(JsContext *ctx, JsValue recv, JsValue fn) {
    JsObject *o = js_is_object(recv) ? js_value_object(recv)
                  : js_is_function(recv) ? js_function_props(ctx, recv, false) : NULL;
    if (!o)
        return NULL;
    JsGcCell *c = js_value_cell(fn);
    const JsString *own = c->kind == JS_KIND_CLOSURE
        ? (((JsClosure *)c)->name && ((JsClosure *)c)->name->length ? ((JsClosure *)c)->name
                                                                    : ((JsClosure *)c)->fn->name)
        : c->kind == JS_KIND_NATIVE ? ((JsNative *)c)->name : NULL;
    if (own && own->length && own->interned) {
        for (JsObject *cur = o; cur;) {
            bool found;
            JsValue v = js_map_get(&cur->props, own, &found);
            if (found) {
                if (js_same_value(v, fn))
                    return own;
                break;
            }
            cur = js_is_object(cur->proto) ? js_value_object(cur->proto) : NULL;
        }
    }
    const JsString *result = NULL;
    for (JsObject *cur = o; cur; cur = js_is_object(cur->proto) ? js_value_object(cur->proto) : NULL) {
        for (uint32_t i = 0; i < cur->props.used; i++) {
            const JsMapEntry *en = &cur->props.entries[i];
            if (!en->key || en->key == JS_MAP_TOMBSTONE || !js_same_value(en->value, fn))
                continue;
            if (result)
                return NULL; /* two keys: V8 names neither */
            result = en->key;
        }
    }
    return result;
}

static void frame_location(FmtBuf *b, JsValue fn, uint32_t pos) {
    JsGcCell *c = js_value_cell(fn);
    if (c->kind != JS_KIND_CLOSURE) {
        fb_ascii(b, "<anonymous>", 11);
        return;
    }
    const JsFunctionCell *f = ((const JsClosure *)c)->fn;
    if (f->module && f->module->specifier && f->module->specifier->length)
        fb_cell_str(b, f->module->specifier);
    else
        fb_ascii(b, "<anonymous>", 11);
    if (f->source) {
        uint32_t line, col;
        js_source_line_col(f->source->units, f->source->length, pos, &line, &col);
        fb_ascii(b, ":", 1);
        fb_uint(b, line);
        fb_ascii(b, ":", 1);
        fb_uint(b, col);
    }
}

static void frame_text(JsContext *ctx, FmtBuf *b, const JsErrorFrame *fr) {
    JsVm *vm = ctx->vm;
    const JsString *name = frame_function_name(fr->fn);
    bool top_level = js_is_undefined(fr->receiver) || js_is_null(fr->receiver);
    bool construct = (fr->flags & JS_EF_CONSTRUCT) != 0;
    if (fr->flags & JS_EF_ASYNC)
        fb_ascii(b, "async ", 6);
    if (!top_level && !construct) {
        JsString *tmp = NULL;
        JsValue tv = js_undefined();
        const JsString *type = type_name(ctx, fr->receiver, &tmp);
        if (tmp) {
            tv = js_value_from_cell(&tmp->gc);
            js_gc_protect(vm, &tv);
        }
        const JsString *method = method_name(ctx, fr->receiver, fr->fn);
        if (name) {
            /* V8 adds the type to a plain name, not to an inferred `x.y` */
            bool dotted = false;
            for (uint32_t i = 0; i < name->length && !dotted; i++)
                dotted = name->units[i] == '.';
            if (type && type->length && !dotted && !str_prefix(name, type)) {
                fb_cell_str(b, type);
                fb_ascii(b, ".", 1);
            }
            fb_cell_str(b, name);
            if (method && method->length && !ends_with_method(name, method)) {
                fb_ascii(b, " [as ", 5);
                fb_cell_str(b, method);
                fb_ascii(b, "]", 1);
            }
        } else {
            if (type && type->length) {
                fb_cell_str(b, type);
                fb_ascii(b, ".", 1);
            }
            if (method && method->length)
                fb_cell_str(b, method);
            else
                fb_ascii(b, "<anonymous>", 11);
        }
        if (tmp)
            js_gc_unprotect(vm, &tv);
    } else if (construct) {
        fb_ascii(b, "new ", 4);
        if (name)
            fb_cell_str(b, name);
        else
            fb_ascii(b, "<anonymous>", 11);
    } else if (name) {
        fb_cell_str(b, name);
    } else {
        frame_location(b, fr->fn, fr->pos);
        return;
    }
    fb_ascii(b, " (", 2);
    frame_location(b, fr->fn, fr->pos);
    fb_ascii(b, ")", 1);
}

/* "Name: message" and a line per frame, as V8's default Error.prepareStackTrace gives it. O and
 * its frames are rooted by the caller. NULL on OOM. */
static JsString *stack_text(JsContext *ctx, JsObject *o, const JsErrorFrame *frames, uint32_t n) {
    JsString *head = js_error_repr(ctx, o);
    if (!head)
        return NULL;
    JsValue hv = js_value_from_cell(&head->gc);
    js_gc_protect(ctx->vm, &hv);
    FmtBuf b = {0};
    fb_cell_str(&b, head);
    for (uint32_t i = 0; i < n; i++) {
        fb_ascii(&b, "\n    at ", 8);
        frame_text(ctx, &b, &frames[i]);
    }
    js_gc_unprotect(ctx->vm, &hv);
    JsString *s = b.oom ? NULL : js_string_cell_new(ctx->vm, b.u, b.n);
    free(b.u);
    return s;
}

/* `stack` on an error: an own property, not enumerable, that a script may assign or delete.
 * Each sets *handled when KEY is `stack` and the error has one; false only on OOM. */
bool js_error_stack_get(JsContext *ctx, JsObject *o, JsValue key, JsValue *out, bool *handled) {
    *handled = false;
    JsErrorObject *e = (JsErrorObject *)o;
    if (e->stack_state == JS_STACK_NONE || !key_is(key, "stack"))
        return true;
    *handled = true;
    if (e->stack_state != JS_STACK_VALUE) {
        JsValue ov = js_value_from_cell(&o->gc);
        js_gc_protect(ctx->vm, &ov);
        JsString *s = stack_text(ctx, o, e->frames, e->stack_state == JS_STACK_FRAMES ? e->frame_count : 0);
        js_gc_unprotect(ctx->vm, &ov);
        if (!s)
            return false;
        drop_frames(ctx, e);
        e->stack = js_value_from_cell(&s->gc);
        e->stack_state = JS_STACK_VALUE;
    }
    *out = e->stack;
    return true;
}

bool js_error_stack_set(JsContext *ctx, JsObject *o, JsValue key, JsValue val) {
    if (!key_is(key, "stack"))
        return false;
    JsErrorObject *e = (JsErrorObject *)o;
    drop_frames(ctx, e);
    e->stack = val;
    e->stack_state = JS_STACK_VALUE;
    return true;
}

bool js_error_has_stack(const JsObject *o) {
    return ((const JsErrorObject *)o)->stack_state != JS_STACK_NONE;
}

bool js_error_stack_has(JsObject *o, JsValue key) {
    return ((JsErrorObject *)o)->stack_state != JS_STACK_NONE && key_is(key, "stack");
}

bool js_error_stack_delete(JsContext *ctx, JsObject *o, JsValue key) {
    if (!key_is(key, "stack"))
        return false;
    JsErrorObject *e = (JsErrorObject *)o;
    drop_frames(ctx, e);
    e->stack = js_undefined();
    e->stack_state = JS_STACK_NONE;
    return true;
}

/* A new error object. SKIP as for js_error_capture; the frames are taken now when a native is
 * running (`new Error()`, a builtin's own error), else when the interpreter's raise unwinds. */
static JsValue error_object_new(JsContext *ctx, JsErrorKind kind, JsString *message, JsValue skip) {
    JsVm *vm = ctx->vm;
    JsValue mv = message ? js_value_from_cell(&message->gc) : js_undefined();
    js_gc_protect(vm, &mv);
    js_gc_protect(vm, &skip);
    JsGcCell *c = js_gc_new_cell(vm, JS_KIND_OBJECT, sizeof(JsErrorObject));
    JsValue ev = js_undefined();
    bool ok = c != NULL;
    if (ok) {
        JsErrorObject *eo = (JsErrorObject *)c;
        JsObject *e = &eo->obj;
        e->obj_kind = JS_OBJ_ERROR;
        e->obj_flags = 0;
        js_map_init(&e->props);
        e->elems = NULL;
        e->elem_count = e->elem_cap = 0;
        e->error_cause = JS_CAUSE_GUEST;
        JsObject *proto = ctx->error_protos[kind];
        e->proto = proto ? js_value_from_cell(&proto->gc) : js_undefined();
        eo->stack = js_undefined();
        eo->frames = NULL;
        eo->frame_count = 0;
        eo->stack_state = JS_STACK_PENDING;
        ev = js_value_from_cell(c);
        js_gc_protect(vm, &ev);
        if (message) {
            ok = js_object_set_hidden(ctx, e, "message", mv);
        }
        if (ok && js_in_native(ctx))
            ok = js_error_capture(ctx, e, skip);
        js_gc_unprotect(vm, &ev);
    }
    js_gc_unprotect(vm, &skip);
    js_gc_unprotect(vm, &mv);
    return ok ? ev : js_undefined();
}

/* An AggregateError's own `errors`: LIST, made non-enumerable as V8 makes it */
static bool set_errors(JsContext *ctx, JsValue ev, JsObject *list) {
    return js_object_set_hidden(ctx, js_value_object(ev), "errors", js_value_from_cell(&list->gc));
}

JsValue js_error_new(JsContext *ctx, JsErrorKind kind, JsString *message) {
    JsValue ev = error_object_new(ctx, kind, message, js_undefined());
    if (kind != JS_ERROR_AGGREGATE || !js_is_object(ev))
        return ev;
    js_gc_protect(ctx->vm, &ev);
    JsObject *list = js_array_new_cell(ctx, 0);
    bool ok = list && set_errors(ctx, ev, list);
    js_gc_unprotect(ctx->vm, &ev);
    return ok ? ev : js_undefined();
}

JsValue js_aggregate_error_new(JsContext *ctx, JsObject *errors, const char *message) {
    JsVm *vm = ctx->vm;
    JsValue lv = js_value_from_cell(&errors->gc);
    js_gc_protect(vm, &lv);
    JsString *m = js_ascii_cell(vm, message);
    JsValue ev = m ? error_object_new(ctx, JS_ERROR_AGGREGATE, m, js_undefined()) : js_undefined();
    js_gc_protect(vm, &ev);
    bool ok = js_is_object(ev) && set_errors(ctx, ev, js_value_object(lv));
    if (ok) { /* V8 makes it with no frames: its stack is the first line alone */
        JsErrorObject *e = (JsErrorObject *)js_value_object(ev);
        drop_frames(ctx, e);
        e->stack_state = JS_STACK_FRAMES;
    }
    js_gc_unprotect(vm, &ev);
    js_gc_unprotect(vm, &lv);
    return ok ? ev : js_undefined();
}

/* ---- constructors ---- */

/* new X(message, options), and new AggregateError(errors, message, options): the message, then
 * the cause, then (an AggregateError's) the errors listed from their iterable, in V8's order */
static bool error_ctor(JsContext *ctx, JsErrorKind kind, const JsValue *args, int argc,
                       JsValue *r) {
    JsVm *vm = ctx->vm;
    JsValue errors = js_undefined(); /* the caller's argument: rooted */
    if (kind == JS_ERROR_AGGREGATE && argc > 0) {
        errors = args[0];
        args++;
        argc--;
    }
    JsValue mv = ARG(0);
    JsString *msg = NULL;
    if (!js_is_undefined(mv)) {
        msg = js_native_string(ctx, mv); /* its toString may run; kept while the error is made */
        if (!msg)
            return (*r = js_take_error(ctx)), false;
    }
    /* the frames below the constructor's own, as V8 leaves it out */
    JsValue self = ctx->running_native ? js_value_from_cell(&ctx->running_native->gc) : js_undefined();
    JsValue ev = error_object_new(ctx, kind, msg, self);
    if (!js_is_object(ev))
        return nthrow(ctx, r, "out of memory");
    JsValue opts = ARG(1);
    if (js_is_object(opts)) {
        js_gc_protect(vm, &ev);
        JsValue cause;
        bool found;
        bool ok = chain_get(ctx, js_value_object(opts), "cause", &cause, &found) &&
                  (!found || js_object_set_hidden(ctx, js_value_object(ev), "cause", cause));
        js_gc_unprotect(vm, &ev);
        if (!ok)
            return nthrow(ctx, r, "out of memory");
    }
    if (kind == JS_ERROR_AGGREGATE) {
        js_gc_protect(vm, &ev);
        JsValue err = js_undefined();
        JsObject *list = js_iterable_to_array(ctx, errors, &err);
        bool ok = list && set_errors(ctx, ev, list);
        js_gc_unprotect(vm, &ev);
        if (!list)
            return (*r = err), false;
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
DEF_CTOR(g_EvalError, JS_ERROR_EVAL)
DEF_CTOR(g_URIError, JS_ERROR_URI)
DEF_CTOR(g_AggregateError, JS_ERROR_AGGREGATE)
#undef DEF_CTOR

static const JsNativeFn kind_ctors[JS_ERROR_KIND_COUNT] = {
    g_Error, g_TypeError, g_RangeError, g_ReferenceError, g_SyntaxError,
    g_EvalError, g_URIError, g_AggregateError,
};

/* Error.prototype.toString: "name: message" for any object receiver. */
static bool errp_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args;
    (void)argc;
    if (!js_is_object(tv))
        return (*r = js_error_receiver(ctx, "Error", tv)), false;
    JsString *s = js_error_repr(ctx, js_value_object(tv));
    if (!s)
        return (*r = js_take_error(ctx)), false;
    *r = js_value_from_cell(&s->gc);
    return true;
}

/* Error.captureStackTrace(object, fn?): gives OBJECT a `stack` of the frames running now, below
 * FN's call when it is given, as V8 does. On an error it is the error's own, unenumerated
 * `stack`; on any other object an ordinary property (there are no property attributes). */
static bool err_captureStackTrace(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue target = ARG(0);
    if (!js_is_object(target))
        return nthrow(ctx, r, "TypeError: invalid_argument"); /* V8's text, as it stands */
    JsValue skip = js_is_function(ARG(1)) ? ARG(1)
                   : ctx->running_native ? js_value_from_cell(&ctx->running_native->gc) : js_undefined();
    JsObject *o = js_value_object(target);
    *r = js_undefined();
    if (o->obj_kind == JS_OBJ_ERROR) {
        if (!js_error_capture(ctx, o, skip))
            return nthrow(ctx, r, "out of memory");
        return true;
    }
    if (o->obj_flags)
        return (*r = js_error_fmt(ctx, "TypeError: Cannot define property stack, object is not extensible")), false;
    uint32_t limit;
    if (!stack_trace_limit(ctx, &limit))
        return true;
    uint32_t cap = limit < 1024 ? limit : 1024;
    JsErrorFrame *buf = cap ? js_realloc_raw(ctx->vm, NULL, 0, (size_t)cap * sizeof *buf) : NULL;
    if (cap && !buf)
        return nthrow(ctx, r, "out of memory");
    uint32_t n = cap ? js_capture_frames(ctx, buf, cap, skip) : 0;
    /* the frames' values stay on the stacks they came from while the text is built */
    JsString *s = stack_text(ctx, o, buf, n);
    js_realloc_raw(ctx->vm, buf, (size_t)cap * sizeof *buf, 0);
    if (!s || !js_object_set_ascii(ctx, o, "stack", js_value_from_cell(&s->gc)))
        return nthrow(ctx, r, "out of memory");
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
        js_native_builtin_ctor(ctor, proto);
        if (k != JS_ERROR_PLAIN) /* TypeError's [[Prototype]] is Error: it inherits its statics */
            ((JsNative *)js_value_cell(ctor))->parent = ctx->error_ctor;
        if (!js_object_set_ascii(ctx, proto, "constructor", ctor) ||
            !js_object_set_ascii(ctx, ctx->globals, kind_names[k], ctor))
            return false;
        if (k == JS_ERROR_PLAIN) {
            /* Error.stackTraceLimit and Error.captureStackTrace, as in V8 */
            ctx->error_ctor = (JsNative *)js_value_cell(ctor);
            JsObject *own = js_function_props(ctx, ctor, true);
            if (!own)
                return false;
            JsValue cst = js_native_new(ctx, "captureStackTrace", err_captureStackTrace, NULL);
            if (!js_is_function(cst) || !js_object_set_hidden(ctx, own, "captureStackTrace", cst) ||
                !js_object_set_ascii(ctx, own, "stackTraceLimit", js_number(10)))
                return false;
        }
    }
    return true;
}
