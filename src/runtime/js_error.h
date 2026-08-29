#ifndef JS_ERROR_H
#define JS_ERROR_H

/*
 * Error objects: the `Error` global and its standard subclasses
 * (TypeError, RangeError, ReferenceError, SyntaxError).
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

/* New error object of `kind`; `message` may be NULL (inherits ""). Roots
 * `message` itself. undefined on OOM. */
JsValue js_error_new(JsContext *ctx, JsErrorKind kind, JsString *message);

/* Engine-raised error from "Name: message" text: a leading standard error
 * name selects the kind (else JS_ERROR_PLAIN) and the remainder becomes the
 * message. Returns the error object, or the bare string on OOM, or the
 * VM's prebuilt "out of memory" value when not even that could be allocated. */
JsValue js_error_from_ascii(JsContext *ctx, const char *msg); /* <=128 chars */
JsValue js_error_from_cell(JsContext *ctx, JsString *msg);

/* "name: message" (either half omitted when empty). `depth` guards against
 * an error whose message/name refers back to itself. NULL on OOM. */
JsString *js_error_repr(JsContext *ctx, JsObject *o, int depth);

/* Installs Error/TypeError/RangeError/ReferenceError/SyntaxError and their
 * prototypes. Must run after ctx->object_proto exists. */
bool js_error_builtins_init(JsContext *ctx);

#endif
