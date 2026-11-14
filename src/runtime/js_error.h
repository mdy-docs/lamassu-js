#ifndef JS_ERROR_H
#define JS_ERROR_H

/*
 * Error objects: the `Error` global and its standard subclasses
 * (TypeError, RangeError, ReferenceError, SyntaxError, EvalError, URIError,
 * AggregateError).
 *
 * An error is an ordinary JsObject with obj_kind JS_OBJ_ERROR, an own
 * `message` string, and [[Prototype]] = the kind's prototype object (which
 * carries `name` and, for Error.prototype, `message = ""` and `toString`).
 * ToString of an error is "name: message", so string concatenation and
 * template literals render `new TypeError('x')` as "TypeError: x" — the
 * same text the engine used to throw as a bare string.
 *
 * Every engine-raised error goes through js_error_from_ascii /
 * js_error_from_cell, so guest code handles engine and script errors
 * uniformly: `catch (e) { e.name; e.message; }`. Both fall back to the
 * plain string when the Error object cannot be allocated (out of memory),
 * so a throw never silently loses its text.
 */

#include "lamassu_internal.h"

/*
 * The messages of the three engine-imposed stops. Raise sites use these
 * macros, and js_error_from_cell recognizes exactly these texts to stamp the
 * structured cause (js_error_cause) on the Error object — one definition,
 * so the two can never drift apart.
 */
#define JS_MSG_BUDGET "RangeError: execution budget exhausted"
#define JS_MSG_INTERRUPT "Error: execution interrupted"
#define JS_MSG_OOM "out of memory"

/* e.stack (js_error.c over js_interp.c's frames) */
bool js_in_native(const JsContext *ctx);
uint32_t js_capture_frames(JsContext *ctx, JsErrorFrame *out, uint32_t limit, JsValue skip);
bool js_error_capture(JsContext *ctx, JsObject *o, JsValue skip);
bool js_error_capture_pending(JsContext *ctx, JsValue v);
bool js_error_stack_get(JsContext *ctx, JsObject *o, JsValue key, JsValue *out, bool *handled);
bool js_error_stack_set(JsContext *ctx, JsObject *o, JsValue key, JsValue val);
bool js_error_stack_has(JsObject *o, JsValue key);
bool js_error_stack_delete(JsContext *ctx, JsObject *o, JsValue key);

/* New error object of `kind`; `message` may be NULL (inherits ""). Roots
 * `message` itself. undefined on OOM. */
JsValue js_error_new(JsContext *ctx, JsErrorKind kind, JsString *message);
/* An AggregateError with MESSAGE and ERRORS (an array) as its `errors`, as Promise.any rejects
 * with. Roots ERRORS itself. undefined on OOM. */
JsValue js_aggregate_error_new(JsContext *ctx, JsObject *errors, const char *message);

/* Engine-raised error from "Name: message" text: a leading standard error
 * name selects the kind (else JS_ERROR_PLAIN) and the remainder becomes the
 * message. Returns the error object, or the bare string on OOM, or the
 * VM's prebuilt "out of memory" value when not even that could be allocated. */
JsValue js_error_from_ascii(JsContext *ctx, const char *msg); /* <=128 chars */
JsValue js_error_from_cell(JsContext *ctx, JsString *msg);

/* An engine-raised error from a format, worded as V8 words it: "TypeError: Cannot read
 * properties of %v (reading '%S')". The leading "Name: " selects the kind, as for
 * js_error_from_ascii. Directives: %s ASCII text, %S a JsString, %v a value as V8 shows it in
 * a message (a primitive by ToString, an array as [object Array], another object as
 * #<Object>, a function as its name), %t a value in V8's typed form for built-ins (number 5,
 * string "x", boolean true, object null, undefined, object, function), %% a percent sign. At
 * most 6 arguments; a %S or %v
 * longer than 120 units is cut short with "...". Allocates; the arguments need no rooting. */
JsValue js_error_fmt(JsContext *ctx, const char *fmt, ...);

/* V8's error for a built-in method called on the wrong kind of value: "Method Map.prototype.get
 * called on incompatible receiver 5". The method's name is the running native's. */
JsValue js_error_receiver(JsContext *ctx, const char *cls, JsValue receiver);

/* "name: message" (either half omitted when empty). `depth` guards against
 * an error whose message/name refers back to itself. NULL on OOM. */
JsString *js_error_repr(JsContext *ctx, JsObject *o); /* NULL: failed, error pending */

/* Installs Error/TypeError/RangeError/ReferenceError/SyntaxError and their
 * prototypes. Must run after ctx->object_proto exists. */
bool js_error_builtins_init(JsContext *ctx);

#endif
