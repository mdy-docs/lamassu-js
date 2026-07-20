/*
 * Date binding layer. No time zone database: every value is treated as UTC, so
 * getFullYear()/getUTCFullYear() etc. are identical and
 * getTimezoneOffset() is always 0. Date parsing understands a subset of
 * ISO 8601 ("YYYY-MM-DD" / "YYYY-MM-DDTHH:mm:ss.sssZ"); other formats
 * yield an Invalid Date, same as unparsable input does in real engines.
 *
 * A date is a JS_KIND_OBJECT cell with obj_kind == JS_OBJ_DATE whose cell
 * embeds the time value directly (mirrors JsRegExp's embedding pattern).
 */
#ifndef JS_DATE_H
#define JS_DATE_H

#include "lamassu_internal.h"

typedef struct JsDateObject {
    JsObject obj;  /* obj_kind == JS_OBJ_DATE; props hold expandos only */
    double time;   /* ms since Unix epoch, UTC; NaN = Invalid Date */
} JsDateObject;

static inline bool js_date_is(JsValue v) {
    return js_is_object(v) && js_value_object(v)->obj_kind == JS_OBJ_DATE;
}

/* "Day Mon DD YYYY HH:mm:ss GMT+0000 (UTC)"-style ToString; NULL on OOM. */
JsString *js_date_repr(JsContext *ctx, JsObject *o);

/* Installs the Date global and a real, script-visible Date.prototype
 * (getFullYear, toISOString, ...) — every instance's [[Prototype]] points
 * there. Called from js_builtins_init. */
bool js_date_builtins_init(JsContext *ctx);

#endif /* JS_DATE_H */
