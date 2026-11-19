#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "js_error.h"
#include "js_bytecode.h"
#include "js_iter.h"
#include "js_weak.h"

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

/* Own-or-inherited property by ASCII key, from START (an object, a function or a promise) up
 * its chain — the ordinary walk; an accessor property's marker comes back as it is. */
static bool chain_find_from(JsContext *ctx, JsValue start, const char *k, JsValue *out, bool *found) {
    *out = js_undefined();
    *found = false;
    JsString *ik = key(ctx, k);
    if (!ik)
        return false;
    JsValue p = start, kv = js_value_from_cell(&ik->gc);
    js_gc_protect(ctx->vm, &kv); /* an atom is held weakly, and a function's own property may be
                                  * made as it is read */
    bool ok = true;
    for (int depth = 0; depth < 10000; depth++) {
        if (js_is_object(p)) {
            JsObject *cur = js_value_object(p);
            JsValue v = js_map_get(&cur->props, ik, found);
            if (*found && v.bits == JS_SPECIAL_TDZ) /* a namespace's uninitialized export */
                *found = false;
            else if (*found)
                *out = v;
            if (*found || v.bits == JS_SPECIAL_TDZ)
                break;
            p = cur->proto;
        } else if (js_is_function(p) || js_is_promise(p)) { /* the chain goes on through one */
            uint8_t attrs;
            int st = js_own_property(ctx, p, kv, out, &attrs);
            ok = st >= 0;
            *found = st > 0;
            if (st)
                break;
            p = js_get_proto(ctx, p);
        } else {
            break;
        }
    }
    js_gc_unprotect(ctx->vm, &kv);
    return ok;
}

/* Whether V is an object as the language counts one: an object, a function or a promise */
static bool is_object_type(JsValue v) {
    return js_is_object(v) || js_has_cell_props(v);
}

/* [[Get]] by ASCII key from START (an object, a function or a promise): an accessor's getter
 * runs with START as its receiver. False: failed, with what the getter threw pending (nothing
 * pending: out of memory). */
static bool chain_get_from(JsContext *ctx, JsValue start, const char *k, JsValue *out, bool *found) {
    if (!chain_find_from(ctx, start, k, out, found))
        return false;
    if (*found && js_is_accessor(*out)) {
        JsValue v;
        if (!js_accessor_read(ctx, *out, start, &v)) {
            js_set_error(ctx, v);
            return false;
        }
        *out = v;
    }
    return true;
}

static bool chain_get(JsContext *ctx, JsObject *o, const char *k, JsValue *out, bool *found) {
    return chain_get_from(ctx, js_value_from_cell(&o->gc), k, out, found);
}

/* V8's GetDataProperty, for what must not run code (a message being written, a stack trace):
 * an accessor is passed over as if the property were not there */
static bool chain_data_from(JsContext *ctx, JsValue start, const char *k, JsValue *out, bool *found) {
    if (!chain_find_from(ctx, start, k, out, found))
        return false;
    if (*found && js_is_accessor(*out)) {
        *found = false;
        *out = js_undefined();
    }
    return true;
}

static bool chain_data(JsContext *ctx, JsObject *o, const char *k, JsValue *out, bool *found) {
    return chain_data_from(ctx, js_value_from_cell(&o->gc), k, out, found);
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
    bool oom;      /* stopped: out of memory, or too_long */
    bool too_long; /* stopped because the text would pass JS_MAX_STRING_UNITS */
} FmtBuf;

static void fb_units(FmtBuf *b, const uint16_t *u, size_t n) {
    if (b->oom || n == 0) /* an empty piece: nothing to copy (u or b->u may be NULL) */
        return;
    if (n > JS_MAX_STRING_UNITS - b->n) { /* n stays <= the cap */
        b->oom = b->too_long = true;
        return;
    }
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

static const JsString *type_name_of(JsContext *ctx, JsValue recv, const char **fixed_out, bool frame);
static const JsString *frame_function_name(JsValue fn);
static bool chain_data(JsContext *ctx, JsObject *o, const char *key, JsValue *out, bool *found);

/* An error, or what inherits Error.prototype, as V8 names it in a message: "Name: message" (its
 * name if the message is empty, the message if the name is), from string properties only, with
 * no side effects. False when O is no error. */
static bool errp_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r);

static bool is_error_to_string(JsValue v) {
    if (!js_is_function(v) || js_value_cell(v)->kind != JS_KIND_NATIVE)
        return false;
    const JsNative *nf = (const JsNative *)js_value_cell(v);
    return !nf->is_bound && nf->fn == errp_toString;
}

/* How V8's NoSideEffectsToString shows V, an object or a promise: an error (or what has
 * Error.prototype.toString) as "Name: message"; what has Object.prototype.toString and a
 * constructor with a name as #<Ctor>; anything else by its tag, as [object Tag]. Each is looked
 * up as a data property, so an accessor (Iterator.prototype's constructor) ends the search. */
typedef enum { FORM_FAILED, FORM_ERROR, FORM_CTOR, FORM_TAG } ObjectForm;

static ObjectForm object_form(JsContext *ctx, JsValue v, const JsString **ctor) {
    JsValue ts = js_undefined(), cv = js_undefined();
    bool found;
    *ctor = NULL;
    if (!chain_data_from(ctx, v, "toString", &ts, &found))
        return FORM_FAILED;
    if ((js_is_object(v) && js_value_object(v)->obj_kind == JS_OBJ_ERROR) || is_error_to_string(ts))
        return FORM_ERROR;
    if (js_is_object_to_string(ts) && !chain_data_from(ctx, v, "constructor", &cv, &found))
        return FORM_FAILED;
    *ctor = js_is_function(cv) ? frame_function_name(cv) : NULL; /* the function's, which V keeps */
    return *ctor ? FORM_CTOR : FORM_TAG;
}

/* An error's "Name: message" (its name if the message is empty, the message if the name is),
 * from string data properties only: what is no string counts as empty */
static void fb_error_like(JsContext *ctx, FmtBuf *b, JsValue v) {
    JsValue nv = js_undefined(), mv = js_undefined();
    bool found;
    if (!chain_data_from(ctx, v, "name", &nv, &found) ||
        !chain_data_from(ctx, v, "message", &mv, &found)) {
        b->oom = true;
        return;
    }
    const JsString *name = js_is_string(nv) ? js_value_string(nv) : NULL;
    const JsString *msg = js_is_string(mv) ? js_value_string(mv) : NULL;
    bool has_name = name && name->length > 0;
    bool has_msg = msg && msg->length > 0;
    if (has_name)
        fb_piece(b, name->units, name->length);
    if (has_name && has_msg)
        fb_ascii(b, ": ", 2);
    if (has_msg)
        fb_piece(b, msg->units, msg->length);
}

bool js_message_names(JsContext *ctx, JsValue v) {
    const JsString *ctor;
    return (!js_is_object(v) && !js_is_promise(v)) || object_form(ctx, v, &ctor) != FORM_TAG;
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
            if (js_is_bigint(v)) { /* V8 names the type alone */
                fb_ascii(b, "bigint", 6);
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
        } else if (js_is_object(v) || js_is_promise(v)) {
            const JsString *n;
            ObjectForm form = object_form(ctx, v, &n);
            if (form == FORM_FAILED) {
                b->oom = true;
            } else if (form == FORM_ERROR) {
                fb_error_like(ctx, b, v);
            } else if (form == FORM_CTOR) {
                fb_ascii(b, "#<", 2);
                fb_piece(b, n->units, n->length);
                fb_ascii(b, ">", 1);
            } else {
                const char *tag = js_message_tag(ctx, v);
                fb_ascii(b, "[object ", 8);
                fb_ascii(b, tag, strlen(tag));
                fb_ascii(b, "]", 1);
            }
        } else {
            JsString *s = js_primitive_to_string(ctx, v);
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
                if (pieces[arg].oom) {
                    out.oom = true;
                    out.too_long |= pieces[arg].too_long;
                }
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
    if (out.too_long) { /* a message past the string length limit: V8's RangeError in its place */
        JsString *s = js_ascii_cell(vm, JS_STRING_LENGTH_ERROR);
        r = s ? js_error_from_cell(ctx, s) : js_oom_value(vm);
    } else if (out.oom) { /* what converting a value threw (a BigInt's budget), else out of memory */
        r = js_is_undefined(ctx->pending_error) ? js_oom_value(vm) : js_take_error(ctx);
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

/* Error.prototype.toString's "name: message" (20.5.3.4): each read from OV (an object, a
 * function or a promise) and converted with ToString, which can run their own toString (and
 * nest, through an error as a name, up to the re-entry limit, as V8's stack overflow). NULL:
 * failed, the error pending (js_take_error). */
JsString *js_error_repr(JsContext *ctx, JsValue ov) {
    JsVm *vm = ctx->vm;
    js_gc_protect(vm, &ov);
    JsString *out = NULL;
    JsValue nv, mv, nsv = js_undefined();
    bool found;
    if (!chain_get_from(ctx, ov, "name", &nv, &found))
        goto done;
    JsString *name = js_is_undefined(nv) ? js_ascii_cell(vm, "Error")
                                         : js_op_to_string(ctx, nv);
    if (!name)
        goto done;
    nsv = js_value_from_cell(&name->gc);
    js_gc_protect(vm, &nsv);
    if (!chain_get_from(ctx, ov, "message", &mv, &found)) {
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
        if (total > JS_MAX_STRING_UNITS) { /* V8 throws it too */
            js_set_error(ctx, js_error_from_ascii(ctx, JS_STRING_LENGTH_ERROR));
            goto done;
        }
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

/* Error.stackTraceLimit as V8 reads it: false when it is not a number (no stack at all) */
static bool stack_trace_limit(JsContext *ctx, uint32_t *limit) {
    *limit = 10;
    if (!ctx->error_ctor)
        return true;
    JsObject *own = js_cell_props(ctx, js_value_from_cell(&ctx->error_ctor->gc), false);
    JsValue v;
    bool found = false;
    if (own && !chain_data(ctx, own, "stackTraceLimit", &v, &found))
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

void js_error_set_origin(JsContext *ctx, JsValue v, JsString *specifier, uint32_t pos) {
    if (!js_is_object(v) || js_value_object(v)->obj_kind != JS_OBJ_ERROR)
        return;
    JsErrorObject *e = (JsErrorObject *)js_value_object(v);
    e->has_origin = true;
    e->origin_spec = specifier;
    e->origin_pos = pos;
    if (e->stack_state == JS_STACK_PENDING) { /* nothing was running: no raise is to fill it in */
        drop_frames(ctx, e);
        e->stack_state = JS_STACK_FRAMES;
    }
}

size_t js_error_stack(JsContext *ctx, JsValue error, JsStackFrame *out, size_t max) {
    (void)ctx;
    if (!out || !js_is_object(error) || js_value_cell(error)->kind != JS_KIND_OBJECT ||
        js_value_object(error)->obj_kind != JS_OBJ_ERROR)
        return 0;
    const JsErrorObject *e = (const JsErrorObject *)js_value_object(error);
    size_t n = 0;
    if (e->has_origin) {
        if (max) {
            out[0].specifier = e->origin_spec ? js_value_from_cell(&e->origin_spec->gc) : js_undefined();
            out[0].function = js_undefined();
            out[0].pos = e->origin_pos;
            n = 1;
        }
        return n;
    }
    if (e->stack_state != JS_STACK_FRAMES)
        return 0;
    for (uint32_t i = 0; i < e->frame_count && n < max; i++) {
        const JsGcCell *c = js_value_cell(e->frames[i].fn);
        if (c->kind != JS_KIND_CLOSURE)
            continue; /* a builtin has no place in a source */
        const JsClosure *cl = (const JsClosure *)c;
        const JsFunctionCell *fn = cl->fn;
        JsString *name = cl->name ? cl->name : fn->name;
        out[n].specifier = fn->module && fn->module->specifier
                               ? js_value_from_cell(&fn->module->specifier->gc)
                               : js_undefined();
        out[n].function = name ? js_value_from_cell(&name->gc) : js_undefined();
        out[n].pos = e->frames[i].pos;
        n++;
    }
    return n;
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
/* The name V8 gives RECV's type (its constructor's name, as GetConstructorName): a name string the
 * engine holds, or NULL with *FIXED a C string, or both NULL for none. Allocates nothing. */
static const JsString *type_name_of(JsContext *ctx, JsValue recv, const char **fixed_out, bool frame) {
    const char *fixed = NULL;
    *fixed_out = NULL;
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
        else if (frame && o == global_object(ctx, "JSON")) /* a stack frame's "JSON.parse" */
            fixed = "JSON";
        else if (frame && o == global_object(ctx, "Math"))
            fixed = "Math";
        else {
            JsValue cv;
            bool found;
            if (chain_data(ctx, o, "constructor", &cv, &found) && found && js_is_function(cv)) {
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
    *fixed_out = fixed;
    return NULL;
}

static const JsString *type_name(JsContext *ctx, JsValue recv, JsString **tmp) {
    const char *fixed;
    const JsString *n = type_name_of(ctx, recv, &fixed, true);
    if (n || !fixed)
        return n;
    *tmp = js_ascii_cell(ctx->vm, fixed);
    return *tmp;
}

/* V8's GetMethodName: the key the receiver (or its prototype chain) holds the function under;
 * the function's own name first, else the one enumerable key with it (none if two) */
static const JsString *method_name(JsContext *ctx, JsValue recv, JsValue fn) {
    JsObject *o = js_is_object(recv) ? js_value_object(recv)
                  : js_is_function(recv) ? js_cell_props(ctx, recv, false) : NULL;
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

/* O's "Name: message" (Error.prototype.toString's, whatever kind of object O is) and a line
 * per frame, as Node's formatter gives it and, but for two things, V8's own: V8's heads the
 * text with the message an error was made with where this has the one it has now, and writes
 * "<error: ...>" where converting the name or message throws, which here, as in Node, is what
 * reading `stack` throws. O and the frames' values are rooted by the caller. NULL: failed, the
 * error pending (js_take_error). */
static JsString *stack_text(JsContext *ctx, JsValue o, const JsErrorFrame *frames, uint32_t n) {
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
    JsString *s2 = b.oom ? NULL : js_string_cell_new(ctx->vm, b.u, b.n);
    free(b.u);
    if (b.too_long) /* V8 throws it from the stack read too */
        js_set_error(ctx, js_error_from_ascii(ctx, JS_STRING_LENGTH_ERROR));
    return s2;
}

/* ---- Error.prepareStackTrace ---- */

/* What a CallSite answers, taken from its frame when it is made: values only (strings, numbers,
 * booleans, null), never the frame's function or receiver, which V8 does not give a strict-mode
 * frame's CallSite either (getThis() and getFunction() are undefined), and every frame here is
 * strict. The record is an array kept in JsContext.callsite_records, which no script reaches. */
enum {
    CS_FUNCTION_NAME, CS_METHOD_NAME, CS_TYPE_NAME, CS_FILE_NAME, CS_LINE, CS_COLUMN,
    CS_ENCLOSING_LINE, CS_ENCLOSING_COLUMN, CS_POSITION, CS_TOPLEVEL, CS_CONSTRUCTOR, CS_ASYNC,
    CS_TEXT, CS_SOURCE, CS_COUNT
};

static JsValue str_or_null(const JsString *s) {
    return s && s->length ? js_value_from_cell((JsGcCell *)&s->gc) : js_null();
}

/* The function of FR as a closure compiled from source, or NULL (a native) */
static const JsFunctionCell *frame_code(const JsErrorFrame *fr) {
    JsGcCell *c = js_value_cell(fr->fn);
    return c->kind == JS_KIND_CLOSURE ? ((const JsClosure *)c)->fn : NULL;
}

/* FR's record (CS_*), or undefined on OOM. FR's values are rooted by the caller. */
static JsValue callsite_record(JsContext *ctx, const JsErrorFrame *fr) {
    JsVm *vm = ctx->vm;
    JsObject *rec = js_array_new_cell(ctx, 0);
    if (!rec)
        return js_undefined();
    JsValue rv = js_value_from_cell(&rec->gc);
    js_gc_protect(vm, &rv);
    bool top_level = js_is_undefined(fr->receiver) || js_is_null(fr->receiver);
    bool construct = (fr->flags & JS_EF_CONSTRUCT) != 0, called = !top_level && !construct;
    const JsFunctionCell *f = frame_code(fr);
    bool ok = js_array_append(vm, rec, str_or_null(frame_function_name(fr->fn)));
    /* a method's name and the receiver's type, for a frame called on an object (V8: null for a
     * plain call and for `new`) */
    ok = ok && js_array_append(vm, rec, called ? str_or_null(method_name(ctx, fr->receiver, fr->fn)) : js_null());
    JsString *tmp = NULL;
    const JsString *type = called ? type_name(ctx, fr->receiver, &tmp) : NULL;
    JsValue tv = tmp ? js_value_from_cell(&tmp->gc) : js_undefined();
    js_gc_protect(vm, &tv);
    ok = ok && js_array_append(vm, rec, str_or_null(type));
    js_gc_unprotect(vm, &tv);
    bool named = f && f->module && f->module->specifier && f->module->specifier->length;
    ok = ok && js_array_append(vm, rec, named ? js_value_from_cell(&f->module->specifier->gc) : js_undefined());
    uint32_t line = 0, col = 0, eline = 0, ecol = 0;
    if (f && f->source) {
        js_source_line_col(f->source->units, f->source->length, fr->pos, &line, &col);
        js_source_line_col(f->source->units, f->source->length, f->src_end ? f->src_start : 0, &eline, &ecol);
    }
    ok = ok && js_array_append(vm, rec, line ? js_number(line) : js_null());
    ok = ok && js_array_append(vm, rec, col ? js_number(col) : js_null());
    ok = ok && js_array_append(vm, rec, eline ? js_number(eline) : js_null());
    ok = ok && js_array_append(vm, rec, ecol ? js_number(ecol) : js_null());
    ok = ok && js_array_append(vm, rec, js_number(fr->pos));
    ok = ok && js_array_append(vm, rec, js_bool(top_level && !construct));
    ok = ok && js_array_append(vm, rec, js_bool(construct));
    ok = ok && js_array_append(vm, rec, js_bool((fr->flags & JS_EF_ASYNC) != 0));
    if (ok) {
        FmtBuf b = {0};
        frame_text(ctx, &b, fr);
        JsString *t = b.oom ? NULL : js_string_cell_new(vm, b.u, b.n);
        free(b.u);
        if (b.too_long)
            js_set_error(ctx, js_error_from_ascii(ctx, JS_STRING_LENGTH_ERROR));
        JsValue t_v = t ? js_value_from_cell(&t->gc) : js_undefined();
        js_gc_protect(vm, &t_v);
        ok = t && js_array_append(vm, rec, t_v);
        js_gc_unprotect(vm, &t_v);
    }
    ok = ok && js_array_append(vm, rec, f && f->source ? js_value_from_cell(&f->source->gc) : js_undefined());
    js_gc_unprotect(vm, &rv);
    return ok ? rv : js_undefined();
}

/* SHA-256 (FIPS 180-4) of LEN bytes */
static void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
    uint64_t bits = (uint64_t)len * 8;
    size_t total = ((len + 9 + 63) / 64) * 64;
    for (size_t off = 0; off < total; off += 64) {
        uint8_t blk[64];
        for (int i = 0; i < 64; i++) {
            size_t p = off + (size_t)i;
            blk[i] = p < len ? data[p] : p == len ? 0x80 : 0;
        }
        if (off + 64 == total)
            for (int i = 0; i < 8; i++)
                blk[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)blk[4 * i] << 24 | (uint32_t)blk[4 * i + 1] << 16 |
                   (uint32_t)blk[4 * i + 2] << 8 | blk[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
#undef ROR
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++)
            out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

/* getScriptHash(): the SHA-256 of the source's UTF-8 (a lone surrogate as U+FFFD), in hex, as V8
 * gives it; "" when there is no source */
static bool script_hash(JsContext *ctx, JsValue src, JsValue *r) {
    if (!js_is_string(src)) {
        JsString *e = js_string_cell_new(ctx->vm, NULL, 0);
        *r = e ? js_value_from_cell(&e->gc) : js_undefined();
        return e ? true : nthrow(ctx, r, "out of memory");
    }
    const JsString *s = js_value_string(src);
    size_t cap = (size_t)s->length * 3 + 1, n = 0;
    uint8_t *u8 = js_realloc_raw(ctx->vm, NULL, 0, cap);
    if (!u8)
        return nthrow(ctx, r, "out of memory");
    for (uint32_t i = 0; i < s->length; i++) {
        uint32_t c = s->units[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s->length && s->units[i + 1] >= 0xDC00 && s->units[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s->units[++i] - 0xDC00);
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            u8[n++] = (uint8_t)c;
        } else if (c < 0x800) {
            u8[n++] = (uint8_t)(0xC0 | c >> 6);
            u8[n++] = (uint8_t)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            u8[n++] = (uint8_t)(0xE0 | c >> 12);
            u8[n++] = (uint8_t)(0x80 | (c >> 6 & 0x3F));
            u8[n++] = (uint8_t)(0x80 | (c & 0x3F));
        } else {
            u8[n++] = (uint8_t)(0xF0 | c >> 18);
            u8[n++] = (uint8_t)(0x80 | (c >> 12 & 0x3F));
            u8[n++] = (uint8_t)(0x80 | (c >> 6 & 0x3F));
            u8[n++] = (uint8_t)(0x80 | (c & 0x3F));
        }
    }
    uint8_t d[32];
    sha256(u8, n, d);
    js_realloc_raw(ctx->vm, u8, cap, 0);
    char hex[65];
    for (int i = 0; i < 32; i++) {
        hex[2 * i] = "0123456789abcdef"[d[i] >> 4];
        hex[2 * i + 1] = "0123456789abcdef"[d[i] & 15];
    }
    hex[64] = 0;
    JsString *hs = js_ascii_cell(ctx->vm, hex);
    if (!hs)
        return nthrow(ctx, r, "out of memory");
    *r = js_value_from_cell(&hs->gc);
    return true;
}

/* The record of the CallSite TV, or false with V8's TypeError naming METHOD */
static bool callsite_of(JsContext *ctx, JsValue tv, const char *method, JsValue *rec, JsValue *r) {
    if (is_object_type(tv) && js_weak_side_get(ctx->callsite_records, tv, rec) && js_is_object(*rec) &&
        js_value_object(*rec)->obj_kind == JS_OBJ_ARRAY && js_value_object(*rec)->elem_count == CS_COUNT)
        return true;
    *r = js_error_fmt(ctx, "TypeError: CallSite method %s expects CallSite as receiver", method);
    return false;
}

static bool cs_field(JsContext *ctx, JsValue tv, const char *method, int field, JsValue *r) {
    JsValue rec;
    if (!callsite_of(ctx, tv, method, &rec, r))
        return false;
    *r = js_value_object(rec)->elems[field];
    return true;
}

static bool cs_const(JsContext *ctx, JsValue tv, const char *method, JsValue v, JsValue *r) {
    JsValue rec;
    if (!callsite_of(ctx, tv, method, &rec, r))
        return false;
    *r = v;
    return true;
}

#define CS_FIELD(fn, method, field)                                                                \
    static bool fn(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {        \
        (void)args; (void)argc;                                                                    \
        return cs_field(ctx, tv, method, field, r);                                                \
    }
#define CS_CONST(fn, method, value)                                                                \
    static bool fn(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {        \
        (void)args; (void)argc;                                                                    \
        return cs_const(ctx, tv, method, value, r);                                                \
    }
CS_FIELD(cs_getColumnNumber, "getColumnNumber", CS_COLUMN)
CS_FIELD(cs_getEnclosingColumnNumber, "getEnclosingColumnNumber", CS_ENCLOSING_COLUMN)
CS_FIELD(cs_getEnclosingLineNumber, "getEnclosingLineNumber", CS_ENCLOSING_LINE)
CS_CONST(cs_getEvalOrigin, "getEvalOrigin", js_undefined())
CS_FIELD(cs_getFileName, "getFileName", CS_FILE_NAME)
CS_CONST(cs_getFunction, "getFunction", js_undefined())
CS_FIELD(cs_getFunctionName, "getFunctionName", CS_FUNCTION_NAME)
CS_FIELD(cs_getLineNumber, "getLineNumber", CS_LINE)
CS_FIELD(cs_getMethodName, "getMethodName", CS_METHOD_NAME)
CS_FIELD(cs_getPosition, "getPosition", CS_POSITION)
CS_CONST(cs_getPromiseIndex, "getPromiseIndex", js_null())
CS_FIELD(cs_getScriptNameOrSourceURL, "getScriptNameOrSourceURL", CS_FILE_NAME)
CS_CONST(cs_getThis, "getThis", js_undefined())
CS_FIELD(cs_getTypeName, "getTypeName", CS_TYPE_NAME)
CS_FIELD(cs_isAsync, "isAsync", CS_ASYNC)
CS_FIELD(cs_isConstructor, "isConstructor", CS_CONSTRUCTOR)
CS_CONST(cs_isEval, "isEval", js_bool(false))
CS_CONST(cs_isNative, "isNative", js_bool(false))
CS_CONST(cs_isPromiseAll, "isPromiseAll", js_bool(false))
CS_FIELD(cs_isToplevel, "isToplevel", CS_TOPLEVEL)
CS_FIELD(cs_toString, "toString", CS_TEXT)
#undef CS_FIELD
#undef CS_CONST

static bool cs_getScriptHash(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args; (void)argc;
    JsValue rec;
    if (!callsite_of(ctx, tv, "getScriptHash", &rec, r))
        return false;
    return script_hash(ctx, js_value_object(rec)->elems[CS_SOURCE], r);
}

/* CallSite, the constructor of CallSite.prototype (not a global): V8's refuses to make one */
static bool cs_ctor(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv; (void)args; (void)argc;
    return nthrow(ctx, r, "Error: Not supported");
}

/* KEY on O, not enumerable, with ATTRS besides. V is rooted while the key is made. */
static bool cs_define(JsContext *ctx, JsObject *o, const char *k, JsValue v, uint8_t attrs) {
    js_gc_protect(ctx->vm, &v);
    JsString *ks = js_ascii_cell(ctx->vm, k);
    JsString *ik = ks ? js_intern_cell(ctx->vm, ks) : NULL;
    bool ok = ik && js_map_set(ctx->vm, &o->props, ik, v);
    js_gc_unprotect(ctx->vm, &v);
    if (!ok)
        return false;
    js_map_add_attrs(&o->props, ik, (uint8_t)(JS_ATTR_HIDDEN | attrs));
    if (attrs)
        o->obj_flags |= JS_OBJ_ATTRS;
    return true;
}

/* CallSite.prototype, made when first needed: its methods in V8's order, read-only and not
 * configurable as V8 has them; `constructor` writable and configurable */
static JsObject *callsite_proto(JsContext *ctx) {
    if (ctx->callsite_proto)
        return ctx->callsite_proto;
    static const struct { const char *name; JsNativeFn fn; } methods[] = {
        {"getColumnNumber", cs_getColumnNumber}, {"getEnclosingColumnNumber", cs_getEnclosingColumnNumber},
        {"getEnclosingLineNumber", cs_getEnclosingLineNumber}, {"getEvalOrigin", cs_getEvalOrigin},
        {"getFileName", cs_getFileName}, {"getFunction", cs_getFunction},
        {"getFunctionName", cs_getFunctionName}, {"getLineNumber", cs_getLineNumber},
        {"getMethodName", cs_getMethodName}, {"getPosition", cs_getPosition},
        {"getPromiseIndex", cs_getPromiseIndex}, {"getScriptNameOrSourceURL", cs_getScriptNameOrSourceURL},
        {"getScriptHash", cs_getScriptHash}, {"getThis", cs_getThis}, {"getTypeName", cs_getTypeName},
        {"isAsync", cs_isAsync}, {"isConstructor", cs_isConstructor}, {"isEval", cs_isEval},
        {"isNative", cs_isNative}, {"isPromiseAll", cs_isPromiseAll}, {"isToplevel", cs_isToplevel},
        {"toString", cs_toString}};
    JsValue pv = js_object_new(ctx);
    if (!js_is_object(pv))
        return NULL;
    js_gc_protect(ctx->vm, &pv);
    JsObject *proto = js_value_object(pv);
    JsValue ctor = js_native_new(ctx, "CallSite", cs_ctor, NULL);
    bool ok = js_is_function(ctor);
    if (ok) {
        js_native_builtin_ctor(ctor, proto);
        ok = cs_define(ctx, proto, "constructor", ctor, 0);
    }
    for (size_t i = 0; ok && i < sizeof methods / sizeof methods[0]; i++) {
        JsValue m = js_native_new(ctx, methods[i].name, methods[i].fn, NULL);
        ok = js_is_function(m) && cs_define(ctx, proto, methods[i].name, m, JS_ATTR_READONLY | JS_ATTR_FIXED);
    }
    if (ok)
        ctx->callsite_proto = proto;
    js_gc_unprotect(ctx->vm, &pv);
    return ok ? proto : NULL;
}

/* The function Error.prepareStackTrace names, and the `this` it is called with, as Node finds
 * them: globalThis.Error's, else the Error this context made. False when neither is a function. */
static bool find_prepare(JsContext *ctx, JsValue *fn, JsValue *self) {
    for (int i = 0; i < 2; i++) {
        JsValue err = js_value_from_cell(&ctx->error_ctor->gc), f = js_undefined();
        bool found = false;
        if (i == 0 && !(chain_get(ctx, ctx->globals, "Error", &err, &found) && found && is_object_type(err))) {
            js_take_error(ctx); /* nothing there, or a read that failed: the next place */
            continue;
        }
        found = false;
        if (chain_get_from(ctx, err, "prepareStackTrace", &f, &found) && found && js_is_function(f)) {
            *fn = f;
            *self = err;
            return true;
        }
        js_take_error(ctx);
    }
    return false;
}

/* `stack` through Error.prepareStackTrace: FN called as V8 calls it, with SELF as `this`, the
 * holder H and an array of CallSites for the N FRAMES (rooted by the caller). 1 with what it
 * returned in *OUT, -1 with what it threw (or out of memory) in *OUT. */
static int prepared_stack(JsContext *ctx, JsValue fn, JsValue self, JsValue h,
                          const JsErrorFrame *frames, uint32_t n, JsValue *out) {
    JsVm *vm = ctx->vm;
    JsValue args[2] = {h, js_undefined()};
    js_gc_protect(vm, &fn);
    js_gc_protect(vm, &self);
    js_gc_protect(vm, &args[1]);
    JsObject *proto = callsite_proto(ctx);
    JsObject *arr = proto ? js_array_new_cell(ctx, 0) : NULL;
    bool ok = arr != NULL;
    if (arr)
        args[1] = js_value_from_cell(&arr->gc);
    for (uint32_t i = 0; ok && i < n; i++) {
        JsValue cs = js_object_new(ctx);
        ok = js_is_object(cs);
        if (!ok)
            break;
        js_value_object(cs)->proto = js_value_from_cell(&proto->gc);
        js_gc_protect(vm, &cs);
        JsValue rec = callsite_record(ctx, &frames[i]);
        js_gc_protect(vm, &rec);
        ok = js_is_object(rec) && js_weak_side_set(ctx, &ctx->callsite_records, cs, rec) &&
             js_array_append(vm, arr, cs);
        js_gc_unprotect(vm, &rec);
        js_gc_unprotect(vm, &cs);
    }
    int rc = -1;
    if (!ok) {
        *out = js_take_error(ctx); /* the length limit, else out of memory */
    } else {
        bool saved = ctx->formatting_stack;
        ctx->formatting_stack = true; /* a `stack` read inside formats as if there were no hook */
        rc = js_call(ctx, fn, self, args, 2, out) ? 1 : -1;
        ctx->formatting_stack = saved;
    }
    js_gc_unprotect(vm, &args[1]);
    js_gc_unprotect(vm, &self);
    js_gc_unprotect(vm, &fn);
    return rc;
}

/* What holds the stack RECV reads and writes: the first object on its prototype chain that is
 * an error or was given one by Error.captureStackTrace (V8 looks for the private symbol both
 * carry), in *HOLDER, and the JsErrorObject keeping the frames and the text: the error itself,
 * or the other object's record. NULL when there is none. V8 calls its accessors as it calls
 * any function of the embedder's, the receiver made an object first: a primitive's chain is
 * its prototype's, and undefined and null stand for the global object. */
static JsErrorObject *stack_holder(JsContext *ctx, JsValue recv, JsValue *holder) {
    JsValue h = is_object_type(recv)                         ? recv
                : js_is_undefined(recv) || js_is_null(recv) ? js_value_from_cell(&ctx->globals->gc)
                                                             : js_get_proto(ctx, recv);
    for (int depth = 0; depth < 1000 && is_object_type(h); depth++) {
        JsValue rec = h;
        if ((js_is_object(h) && js_value_object(h)->obj_kind == JS_OBJ_ERROR) ||
            js_weak_side_get(ctx->stack_records, h, &rec)) {
            *holder = h;
            return (JsErrorObject *)js_value_object(rec);
        }
        h = js_get_proto(ctx, h);
    }
    return NULL;
}

/* `stack`: an own accessor property, not enumerable, as V8 has it, of an error (the entry
 * error_object_new makes) and of what Error.captureStackTrace was given; its getter and setter
 * are these (js_accessor_call), the same two functions everywhere. What they read and write is
 * the holder's (stack_holder): the frames it captured, or what a script assigned. So they
 * answer for what inherits from a holder, and still for one whose `stack` property was deleted
 * or redefined, and give undefined where there is no holder.
 *
 * The text is made the first time it is read, as V8 makes it: the first line uses the name and
 * message the holder has by then, over the frames it has by then (a copy, rooted in
 * JsContext.frame_roots: converting the name can run a toString that captures again or assigns
 * `stack`, and as in V8 the text then made replaces either). False with the error in *OUT. */
bool js_error_stack_accessor(JsContext *ctx, bool set, JsValue recv, JsValue arg, JsValue *out) {
    *out = js_undefined();
    JsVm *vm = ctx->vm;
    JsValue h = js_undefined();
    JsErrorObject *e = stack_holder(ctx, recv, &h);
    if (!e)
        return true;
    if (set) {
        drop_frames(ctx, e);
        e->stack = arg;
        e->stack_state = JS_STACK_VALUE;
        return true;
    }
    if (e->stack_state == JS_STACK_VALUE) {
        *out = e->stack;
        return true;
    }
    uint32_t n = e->stack_state == JS_STACK_FRAMES ? e->frame_count : 0;
    JsErrorFrame *frames = NULL;
    if (n) {
        frames = js_realloc_raw(vm, NULL, 0, (size_t)n * sizeof *frames);
        if (!frames)
            return nthrow(ctx, out, "out of memory");
        memcpy(frames, e->frames, (size_t)n * sizeof *frames);
    }
    JsFrameRoot root = {frames, n, ctx->frame_roots};
    ctx->frame_roots = &root;
    JsValue rec = js_value_from_cell(&e->obj.gc);
    js_gc_protect(vm, &h);
    js_gc_protect(vm, &rec);
    /* Error.prepareStackTrace, when it is a function, makes the value instead, as in V8 and
     * Node: what it returns is `stack`; what it throws is what this read throws, and the next
     * read tries again. A read from inside it formats as if there were none. */
    JsValue prep = js_undefined(), self = js_undefined();
    JsString *s = NULL;
    int prepared = 0;
    if (!ctx->formatting_stack && find_prepare(ctx, &prep, &self))
        prepared = prepared_stack(ctx, prep, self, h, frames, n, out);
    else
        s = stack_text(ctx, h, frames, n);
    js_gc_unprotect(vm, &rec);
    js_gc_unprotect(vm, &h);
    ctx->frame_roots = root.next;
    js_realloc_raw(vm, frames, (size_t)n * sizeof *frames, 0);
    if (prepared < 0)
        return false;
    if (!prepared && !s)
        return (*out = js_take_error(ctx)), false;
    if (!prepared)
        *out = js_value_from_cell(&s->gc);
    drop_frames(ctx, e);
    e->stack = *out;
    e->stack_state = JS_STACK_VALUE;
    return true;
}

/* A JsErrorObject cell with no stack yet and no properties, its prototype PROTO (NULL: none).
 * Undefined on OOM. */
static JsValue error_cell_new(JsContext *ctx, JsObject *proto) {
    JsGcCell *c = js_gc_new_cell(ctx->vm, JS_KIND_OBJECT, sizeof(JsErrorObject));
    if (!c)
        return js_undefined();
    JsErrorObject *eo = (JsErrorObject *)c;
    JsObject *e = &eo->obj;
    e->obj_kind = JS_OBJ_ERROR;
    e->obj_flags = 0; /* js_object_set_accessor marks it as holding an accessor */
    js_map_init(&e->props);
    e->elems = NULL;
    e->elem_count = e->elem_cap = 0;
    e->error_cause = JS_CAUSE_GUEST;
    e->proto = proto ? js_value_from_cell(&proto->gc) : js_undefined();
    eo->stack = js_undefined();
    eo->frames = NULL;
    eo->frame_count = 0;
    eo->stack_state = JS_STACK_PENDING;
    eo->has_origin = false;
    eo->origin_pos = 0;
    eo->origin_spec = NULL;
    return js_value_from_cell(c);
}

/* A new error object. SKIP as for js_error_capture; the frames are taken now when a native is
 * running (`new Error()`, a builtin's own error), else when the interpreter's raise unwinds. */
static JsValue error_object_new(JsContext *ctx, JsErrorKind kind, JsString *message, JsValue skip) {
    JsVm *vm = ctx->vm;
    JsValue mv = message ? js_value_from_cell(&message->gc) : js_undefined();
    js_gc_protect(vm, &mv);
    js_gc_protect(vm, &skip);
    JsValue ev = error_cell_new(ctx, ctx->error_protos[kind]);
    bool ok = !js_is_undefined(ev);
    if (ok) {
        JsObject *e = js_value_object(ev);
        js_gc_protect(vm, &ev);
        /* `stack` first, then `message`: the order V8 lists them in */
        ok = js_object_set_accessor(ctx, e, "stack", JS_ACC_STACK_GET, JS_ACC_STACK_SET);
        if (ok && message)
            ok = js_object_set_hidden(ctx, e, "message", mv);
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
        if (!ok && !js_is_undefined(ctx->pending_error)) { /* what a getter of `cause` threw */
            *r = js_take_error(ctx);
            return false;
        }
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
    if (!is_object_type(tv))
        return (*r = js_error_receiver(ctx, "Error", tv)), false;
    JsString *s = js_error_repr(ctx, tv);
    if (!s)
        return (*r = js_take_error(ctx)), false;
    *r = js_value_from_cell(&s->gc);
    return true;
}

/* Error.captureStackTrace(object, fn?): gives OBJECT, anything that is an object, a `stack` of
 * the frames running now, below FN's call when it is given, as V8 does: the `stack` accessor
 * every error has, defined on it (again, for an error: one a script deleted or redefined is
 * back), over frames the object now holds: an error in itself, anything else in a record the
 * context keeps for it (JsContext.stack_records), for as long as the object lives. V8 refuses
 * an object that is not extensible even when it has a `stack`, and so does this; an own
 * `stack` that cannot be redefined is the definition's error. */
static bool err_captureStackTrace(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsVm *vm = ctx->vm;
    JsValue target = ARG(0);
    if (!is_object_type(target))
        return nthrow(ctx, r, "TypeError: invalid_argument"); /* V8's text, as it stands */
    JsValue skip = js_is_function(ARG(1)) ? ARG(1)
                   : ctx->running_native ? js_value_from_cell(&ctx->running_native->gc) : js_undefined();
    *r = js_undefined();
    if (!js_is_extensible(ctx, target))
        return (*r = js_error_fmt(ctx, "TypeError: Cannot define property stack, object is not extensible")), false;
    JsString *k = key(ctx, "stack"); /* the target and SKIP are the caller's arguments: rooted */
    if (!k)
        return nthrow(ctx, r, "out of memory");
    JsPropDesc d = {JS_DESC_GET | JS_DESC_SET | JS_DESC_ENUMERABLE | JS_DESC_CONFIGURABLE, false, false, true,
                    js_accessor(JS_ACC_STACK_GET, JS_ACC_STACK_SET)};
    if (!js_define_own_property(ctx, target, js_value_from_cell(&k->gc), &d))
        return (*r = js_take_error(ctx)), false;
    JsValue rec = target;
    if (!(js_is_object(target) && js_value_object(target)->obj_kind == JS_OBJ_ERROR) &&
        !js_weak_side_get(ctx->stack_records, target, &rec)) {
        rec = error_cell_new(ctx, NULL);
        if (js_is_undefined(rec))
            return nthrow(ctx, r, "out of memory");
        js_gc_protect(vm, &rec);
        bool ok = js_weak_side_set(ctx, &ctx->stack_records, target, rec);
        js_gc_unprotect(vm, &rec);
        if (!ok)
            return nthrow(ctx, r, "out of memory");
    }
    /* the record is its object's from here: alive while the target, an argument, is */
    if (!js_error_capture(ctx, js_value_object(rec), skip))
        return nthrow(ctx, r, "out of memory");
    return true;
}

/* ---- registration ---- */

/* Error.isError(v): whether V is an error object (made by an error constructor), whatever its
 * prototype */
static bool err_isError(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)ctx;
    (void)tv;
    JsValue v = argc > 0 ? args[0] : js_undefined();
    *r = js_bool(js_is_object(v) && js_value_object(v)->obj_kind == JS_OBJ_ERROR);
    return true;
}

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
        /* every error prototype has its own message, "", as in V8 */
        JsString *empty = js_string_cell_new(vm, NULL, 0);
        if (!empty || !js_object_set_ascii(ctx, proto, "message", js_value_from_cell(&empty->gc)))
            return false;
        if (k == JS_ERROR_PLAIN) {
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
            JsObject *own = js_cell_props(ctx, ctor, true);
            if (!own)
                return false;
            JsValue cst = js_native_new(ctx, "captureStackTrace", err_captureStackTrace, NULL);
            if (!js_is_function(cst) || !js_object_set_hidden(ctx, own, "captureStackTrace", cst))
                return false;
            JsValue ie = js_native_new(ctx, "isError", err_isError, NULL);
            if (!js_is_function(ie) || !js_object_set_hidden(ctx, own, "isError", ie) ||
                !js_object_set_ascii(ctx, own, "stackTraceLimit", js_number(10)))
                return false;
        }
    }
    return true;
}
