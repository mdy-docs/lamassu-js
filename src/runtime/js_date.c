/*
 * Date: construction, get/set accessors, and string formatting. No time
 * zone database — every value is UTC, so each getX()/getUTCX() pair and
 * each setX()/setUTCX() pair are literally the same function, and
 * getTimezoneOffset() is always 0. String parsing understands a subset of
 * ISO 8601; anything else yields an Invalid Date, same as real engines do
 * for formats they don't recognize.
 *
 * Calendar math is Howard Hinnant's well-known constant-time
 * days_from_civil/civil_from_days (public domain), exact for the proleptic
 * Gregorian calendar in both directions.
 */
#include "js_bytecode.h"
#include "js_date.h"

#define ARG(i) ((i) < argc ? args[i] : js_undefined())

static bool nthrow(JsContext *ctx, JsValue *r, const char *msg) {
    JsString *s = js_ascii_cell(ctx->vm, msg);
    *r = s ? js_value_from_cell(&s->gc) : js_undefined();
    return false;
}

static double dnan(void) { return __builtin_nan(""); }
static bool is_finite_num(double d) {
    return d == d && d != __builtin_inf() && d != -__builtin_inf();
}

/* ---- wall clock ---- */

#include <sys/time.h>
static double host_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

/* ---- integer floor div/mod (well-defined for negative operands) ---- */

static int64_t ifloordiv(int64_t a, int64_t b) {
    int64_t q = a / b, rem = a % b;
    if (rem != 0 && ((rem < 0) != (b < 0)))
        q--;
    return q;
}

static int64_t ifloormod(int64_t a, int64_t b) {
    int64_t rem = a % b;
    if (rem != 0 && ((rem < 0) != (b < 0)))
        rem += b;
    return rem;
}

/* ---- Hinnant civil calendar (days since 1970-01-01, proleptic Gregorian);
 * m is 1-12 in both directions. ---- */

static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, int64_t *m, int64_t *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yy + (*m <= 2);
}

/* ---- time value helpers ---- */

/* Spec's TimeClip: out of range or non-finite -> NaN; else truncated
 * toward zero with -0 normalized to +0. */
static double time_clip(double t) {
    if (!is_finite_num(t))
        return dnan();
    double m = t < 0 ? -t : t;
    if (m > 8.64e15)
        return dnan();
    t = __builtin_trunc(t);
    return t == 0.0 ? 0.0 : t;
}

/*
 * MakeDate/MakeDay/MakeTime collapsed into one call. `legacy_year` applies
 * the spec's "0 <= year <= 99 means 1900+year" rule (the multi-arg
 * constructor and Date.UTC; not single-value construction or parsing).
 * Bounding every component keeps the int64 casts below well-defined even
 * for wild script-supplied doubles (NaN/Infinity/1e300 all just become
 * Invalid Date rather than undefined behavior).
 */
#define JS_DATE_BOUND 1e15

static double make_date(double year, double month, double day, double hours,
                        double minutes, double seconds, double ms, bool legacy_year) {
    double comps[7] = {year, month, day, hours, minutes, seconds, ms};
    for (int i = 0; i < 7; i++) {
        double v = comps[i];
        if (!is_finite_num(v) || v <= -JS_DATE_BOUND || v >= JS_DATE_BOUND)
            return dnan();
    }
    double y = year;
    if (legacy_year && y >= 0 && y <= 99)
        y += 1900;
    int64_t yi = (int64_t)y;
    int64_t mi = (int64_t)month;
    int64_t di = (int64_t)day;
    int64_t ey = yi + ifloordiv(mi, 12);
    int64_t em = ifloormod(mi, 12); /* 0-11 */
    int64_t days = days_from_civil(ey, em + 1, 1) + (di - 1);
    double t = (double)days * 86400000.0 + hours * 3600000.0 + minutes * 60000.0 +
              seconds * 1000.0 + ms;
    return time_clip(t);
}

typedef struct {
    int64_t year;
    int month, day, weekday, hour, minute, second, ms;
} DateParts;

/* `t` must already be a finite, integer-valued, in-range time (i.e. the
 * result of time_clip); callers check `t == t` (not NaN) first. */
static void decompose(double t, DateParts *p) {
    int64_t total_ms = (int64_t)t; /* safe: |t| <= 8.64e15 << INT64_MAX */
    int64_t days = ifloordiv(total_ms, 86400000);
    int64_t tod = total_ms - days * 86400000; /* [0, 86400000) */
    int64_t y, m, d;
    civil_from_days(days, &y, &m, &d);
    p->year = y;
    p->month = (int)(m - 1);
    p->day = (int)d;
    p->weekday = (int)ifloormod(days + 4, 7); /* days=0 (1970-01-01) was Thursday */
    p->hour = (int)(tod / 3600000);
    tod %= 3600000;
    p->minute = (int)(tod / 60000);
    tod %= 60000;
    p->second = (int)(tod / 1000);
    p->ms = (int)(tod % 1000);
}

/* ---- minimal ISO 8601 parsing: YYYY-MM-DD[( |T)HH:mm[:ss[.sss]]][Z|±HH:MM] ---- */

static bool read_digits(const uint16_t *u, size_t len, size_t *i, int n, int *out) {
    if (*i + (size_t)n > len)
        return false;
    int v = 0;
    for (int k = 0; k < n; k++) {
        uint16_t c = u[*i + (size_t)k];
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + (int)(c - '0');
    }
    *i += (size_t)n;
    *out = v;
    return true;
}

static double parse_iso(const uint16_t *u, size_t len) {
    size_t i = 0;
    int y = 0, mo = 1, d = 1, h = 0, mi = 0, s = 0, ms = 0;
    if (!read_digits(u, len, &i, 4, &y))
        return dnan();
    if (i < len && u[i] == '-') {
        i++;
        if (!read_digits(u, len, &i, 2, &mo))
            return dnan();
        if (i < len && u[i] == '-') {
            i++;
            if (!read_digits(u, len, &i, 2, &d))
                return dnan();
        }
    }
    if (i < len && (u[i] == 'T' || u[i] == ' ')) {
        i++;
        if (!read_digits(u, len, &i, 2, &h) || i >= len || u[i] != ':')
            return dnan();
        i++;
        if (!read_digits(u, len, &i, 2, &mi))
            return dnan();
        if (i < len && u[i] == ':') {
            i++;
            if (!read_digits(u, len, &i, 2, &s))
                return dnan();
            if (i < len && u[i] == '.') {
                i++;
                int digs = 0, frac = 0;
                while (i < len && u[i] >= '0' && u[i] <= '9' && digs < 9) {
                    frac = frac * 10 + (int)(u[i] - '0');
                    i++;
                    digs++;
                }
                if (digs == 0)
                    return dnan();
                for (int k = digs; k < 3; k++)
                    frac *= 10;
                for (int k = 3; k < digs; k++)
                    frac /= 10;
                ms = frac;
            }
        }
    }
    double offset_ms = 0;
    if (i < len && u[i] == 'Z') {
        i++;
    } else if (i < len && (u[i] == '+' || u[i] == '-')) {
        int osign = u[i] == '-' ? -1 : 1;
        i++;
        int oh = 0, om = 0;
        if (!read_digits(u, len, &i, 2, &oh))
            return dnan();
        if (i < len && u[i] == ':')
            i++;
        if (i < len && u[i] >= '0' && u[i] <= '9') {
            if (!read_digits(u, len, &i, 2, &om))
                return dnan();
        }
        if (oh > 23 || om > 59) /* timezone offset ±HH:mm, HH 00-23, mm 00-59 */
            return dnan();
        offset_ms = (double)osign * ((double)oh * 3600000.0 + (double)om * 60000.0);
    }
    /* Range-validate every field per the ES Date Time String grammar: an
     * out-of-range component makes the whole string invalid (NaN), it does not
     * silently overflow into an adjacent unit. Hour 24 is allowed only as the
     * end-of-day midnight (minutes/seconds/ms all zero). */
    if (i != len || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 24 ||
        (h == 24 && (mi != 0 || s != 0 || ms != 0)) || mi > 59 || s > 59)
        return dnan();
    double t = make_date(y, mo - 1, d, h, mi, s, ms, false);
    if (t != t)
        return t;
    return time_clip(t - offset_ms);
}

/* ---- object plumbing ---- */

static bool alloc_date(JsContext *ctx, double time, JsValue *out) {
    JsGcCell *c = js_gc_new_cell(ctx->vm, JS_KIND_OBJECT, sizeof(JsDateObject));
    if (!c)
        return false;
    JsDateObject *d = (JsDateObject *)c;
    d->obj.obj_kind = JS_OBJ_DATE;
    js_map_init(&d->obj.props);
    d->obj.elems = NULL;
    d->obj.elem_count = d->obj.elem_cap = 0;
    d->obj.proto = ctx->date_proto ? js_value_from_cell(&ctx->date_proto->gc) : js_undefined();
    d->time = time;
    *out = js_value_from_cell(c);
    return true;
}

static bool this_date(JsContext *ctx, JsValue tv, JsDateObject **out, JsValue *r) {
    if (!js_date_is(tv))
        return nthrow(ctx, r, "TypeError: Date.prototype method called on a non-Date value");
    *out = (JsDateObject *)js_value_object(tv);
    return true;
}

/* ---- constructor + statics ---- */

static bool g_Date(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    double time;
    if (argc == 0) {
        time = time_clip(host_now_ms());
    } else if (argc == 1) {
        JsValue a = ARG(0);
        if (js_date_is(a)) {
            time = ((JsDateObject *)js_value_object(a))->time;
        } else if (js_is_string(a)) {
            size_t slen;
            const uint16_t *su = js_string_units(a, &slen);
            time = parse_iso(su, slen);
        } else {
            time = time_clip(js_to_number_value(ctx, a));
        }
    } else {
        double year = js_to_number_value(ctx, ARG(0));
        double month = js_to_number_value(ctx, ARG(1));
        double day = argc > 2 ? js_to_number_value(ctx, ARG(2)) : 1;
        double hours = argc > 3 ? js_to_number_value(ctx, ARG(3)) : 0;
        double minutes = argc > 4 ? js_to_number_value(ctx, ARG(4)) : 0;
        double seconds = argc > 5 ? js_to_number_value(ctx, ARG(5)) : 0;
        double msec = argc > 6 ? js_to_number_value(ctx, ARG(6)) : 0;
        time = make_date(year, month, day, hours, minutes, seconds, msec, true);
    }
    JsValue dv;
    if (!alloc_date(ctx, time, &dv))
        return nthrow(ctx, r, "out of memory");
    *r = dv;
    return true;
}

static bool date_now(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)ctx; (void)tv; (void)args; (void)argc;
    *r = js_number(time_clip(host_now_ms()));
    return true;
}

static bool date_UTC(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    double year = argc > 0 ? js_to_number_value(ctx, ARG(0)) : dnan();
    double month = argc > 1 ? js_to_number_value(ctx, ARG(1)) : 0;
    double day = argc > 2 ? js_to_number_value(ctx, ARG(2)) : 1;
    double hours = argc > 3 ? js_to_number_value(ctx, ARG(3)) : 0;
    double minutes = argc > 4 ? js_to_number_value(ctx, ARG(4)) : 0;
    double seconds = argc > 5 ? js_to_number_value(ctx, ARG(5)) : 0;
    double msec = argc > 6 ? js_to_number_value(ctx, ARG(6)) : 0;
    *r = js_number(make_date(year, month, day, hours, minutes, seconds, msec, true));
    return true;
}

static bool date_parse(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsString *s = js_to_string_cell(ctx, ARG(0), 0);
    if (!s)
        return nthrow(ctx, r, "out of memory");
    *r = js_number(parse_iso(s->units, s->length));
    return true;
}

/* ---- getters ---- */

static bool date_getTime(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args; (void)argc;
    JsDateObject *d;
    if (!this_date(ctx, tv, &d, r))
        return false;
    *r = js_number(d->time);
    return true;
}

static bool date_valueOf(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    return date_getTime(ctx, tv, args, argc, r);
}

static bool date_setTime(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsDateObject *d;
    if (!this_date(ctx, tv, &d, r))
        return false;
    d->time = time_clip(js_to_number_value(ctx, ARG(0)));
    *r = js_number(d->time);
    return true;
}

static bool date_getTimezoneOffset(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                                   JsValue *r) {
    (void)args; (void)argc;
    JsDateObject *d;
    if (!this_date(ctx, tv, &d, r))
        return false;
    *r = js_number(d->time != d->time ? dnan() : 0.0);
    return true;
}

#define DATE_GETTER(fnname, field)                                                    \
    static bool fnname(JsContext *ctx, JsValue tv, const JsValue *args, int argc,      \
                       JsValue *r) {                                                  \
        (void)args; (void)argc;                                                        \
        JsDateObject *d;                                                               \
        if (!this_date(ctx, tv, &d, r))                                               \
            return false;                                                             \
        if (d->time != d->time) {                                                     \
            *r = js_number(dnan());                                                   \
            return true;                                                              \
        }                                                                              \
        DateParts p;                                                                   \
        decompose(d->time, &p);                                                        \
        *r = js_number((double)(p.field));                                            \
        return true;                                                                   \
    }

DATE_GETTER(date_getFullYear, year)
DATE_GETTER(date_getMonth, month)
DATE_GETTER(date_getDate, day)
DATE_GETTER(date_getDay, weekday)
DATE_GETTER(date_getHours, hour)
DATE_GETTER(date_getMinutes, minute)
DATE_GETTER(date_getSeconds, second)
DATE_GETTER(date_getMilliseconds, ms)

#undef DATE_GETTER

/* ---- setters ----
 * `start`/`maxn` select which of the 7 [year,month,day,hours,minutes,
 * seconds,ms] fields this setter may overwrite, left to right; missing
 * trailing args are simply not touched (setMonth(3) keeps the day), but at
 * least one field is always processed (a no-arg call reads ARG(0) as
 * undefined -> NaN, correctly invalidating the date, matching spec). An
 * Invalid Date's fields default to the epoch before applying overrides, so
 * e.g. `new Date(NaN); .setFullYear(2024)` yields a valid date.
 */
static bool set_field(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r,
                      int start, int maxn) {
    JsDateObject *d;
    if (!this_date(ctx, tv, &d, r))
        return false;
    DateParts p;
    if (d->time == d->time) {
        decompose(d->time, &p);
    } else {
        DateParts z = {1970, 0, 1, 4, 0, 0, 0, 0};
        p = z;
    }
    double f[7] = {(double)p.year, (double)p.month, (double)p.day,
                  (double)p.hour, (double)p.minute, (double)p.second, (double)p.ms};
    int n = argc < maxn ? argc : maxn;
    if (n < 1)
        n = 1;
    for (int k = 0; k < n; k++)
        f[start + k] = js_to_number_value(ctx, ARG(k));
    d->time = make_date(f[0], f[1], f[2], f[3], f[4], f[5], f[6], false);
    *r = js_number(d->time);
    return true;
}

static bool date_setFullYear(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                             JsValue *r) {
    return set_field(ctx, tv, args, argc, r, 0, 3);
}
static bool date_setMonth(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                          JsValue *r) {
    return set_field(ctx, tv, args, argc, r, 1, 2);
}
static bool date_setDate(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    return set_field(ctx, tv, args, argc, r, 2, 1);
}
static bool date_setHours(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                          JsValue *r) {
    return set_field(ctx, tv, args, argc, r, 3, 4);
}
static bool date_setMinutes(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                            JsValue *r) {
    return set_field(ctx, tv, args, argc, r, 4, 3);
}
static bool date_setSeconds(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                            JsValue *r) {
    return set_field(ctx, tv, args, argc, r, 5, 2);
}
static bool date_setMilliseconds(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                                 JsValue *r) {
    return set_field(ctx, tv, args, argc, r, 6, 1);
}

/* ---- string formatting ---- */

static const char *const WEEKDAY_NAME[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const MONTH_NAME[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static void wr_str(uint16_t *buf, size_t *n, const char *s) {
    while (*s)
        buf[(*n)++] = (uint16_t)(unsigned char)(*s++);
}

static void wr_uint(uint16_t *buf, size_t *n, int64_t val, int width) {
    uint16_t tmp[16];
    for (int i = width - 1; i >= 0; i--) {
        tmp[i] = (uint16_t)('0' + (val % 10));
        val /= 10;
    }
    for (int i = 0; i < width; i++)
        buf[(*n)++] = tmp[i];
}

/* 0-9999 -> 4 digits; outside that (rare: only reachable via explicit huge
 * numeric-timestamp or component construction) -> signed 6-digit extended
 * form, matching toISOString's spec'd extended-year format. */
static void wr_year(uint16_t *buf, size_t *n, int64_t year) {
    if (year >= 0 && year <= 9999) {
        wr_uint(buf, n, year, 4);
    } else {
        buf[(*n)++] = year < 0 ? '-' : '+';
        wr_uint(buf, n, year < 0 ? -year : year, 6);
    }
}

static size_t fmt_date_part(uint16_t *buf, const DateParts *p) {
    size_t n = 0;
    wr_str(buf, &n, WEEKDAY_NAME[p->weekday]);
    buf[n++] = ' ';
    wr_str(buf, &n, MONTH_NAME[p->month]);
    buf[n++] = ' ';
    wr_uint(buf, &n, p->day, 2);
    buf[n++] = ' ';
    wr_year(buf, &n, p->year);
    return n;
}

static size_t fmt_time_part(uint16_t *buf, const DateParts *p) {
    size_t n = 0;
    wr_uint(buf, &n, p->hour, 2);
    buf[n++] = ':';
    wr_uint(buf, &n, p->minute, 2);
    buf[n++] = ':';
    wr_uint(buf, &n, p->second, 2);
    buf[n++] = ' ';
    wr_str(buf, &n, "GMT+0000 (UTC)");
    return n;
}

static size_t fmt_utc_string(uint16_t *buf, const DateParts *p) {
    size_t n = 0;
    wr_str(buf, &n, WEEKDAY_NAME[p->weekday]);
    wr_str(buf, &n, ", ");
    wr_uint(buf, &n, p->day, 2);
    buf[n++] = ' ';
    wr_str(buf, &n, MONTH_NAME[p->month]);
    buf[n++] = ' ';
    wr_year(buf, &n, p->year);
    buf[n++] = ' ';
    wr_uint(buf, &n, p->hour, 2);
    buf[n++] = ':';
    wr_uint(buf, &n, p->minute, 2);
    buf[n++] = ':';
    wr_uint(buf, &n, p->second, 2);
    buf[n++] = ' ';
    wr_str(buf, &n, "GMT");
    return n;
}

static bool build_and_return(JsContext *ctx, double t, size_t (*fmt)(uint16_t *, const DateParts *),
                             JsValue *r) {
    if (t != t) {
        JsString *s = js_ascii_cell(ctx->vm, "Invalid Date");
        if (!s)
            return nthrow(ctx, r, "out of memory");
        *r = js_value_from_cell(&s->gc);
        return true;
    }
    DateParts p;
    decompose(t, &p);
    uint16_t buf[48];
    size_t n = fmt(buf, &p);
    JsString *s = js_string_cell_new(ctx->vm, buf, n);
    if (!s)
        return nthrow(ctx, r, "out of memory");
    *r = js_value_from_cell(&s->gc);
    return true;
}

static bool date_toDateString(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                              JsValue *r) {
    (void)args; (void)argc;
    JsDateObject *d;
    if (!this_date(ctx, tv, &d, r))
        return false;
    return build_and_return(ctx, d->time, fmt_date_part, r);
}

static bool date_toTimeString(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                              JsValue *r) {
    (void)args; (void)argc;
    JsDateObject *d;
    if (!this_date(ctx, tv, &d, r))
        return false;
    return build_and_return(ctx, d->time, fmt_time_part, r);
}

static bool date_toUTCString(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                             JsValue *r) {
    (void)args; (void)argc;
    JsDateObject *d;
    if (!this_date(ctx, tv, &d, r))
        return false;
    return build_and_return(ctx, d->time, fmt_utc_string, r);
}

static bool date_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)args; (void)argc;
    JsDateObject *d;
    if (!this_date(ctx, tv, &d, r))
        return false;
    if (d->time != d->time) {
        JsString *s = js_ascii_cell(ctx->vm, "Invalid Date");
        if (!s)
            return nthrow(ctx, r, "out of memory");
        *r = js_value_from_cell(&s->gc);
        return true;
    }
    DateParts p;
    decompose(d->time, &p);
    uint16_t buf[64];
    size_t n = fmt_date_part(buf, &p);
    buf[n++] = ' ';
    n += fmt_time_part(buf + n, &p);
    JsString *s = js_string_cell_new(ctx->vm, buf, n);
    if (!s)
        return nthrow(ctx, r, "out of memory");
    *r = js_value_from_cell(&s->gc);
    return true;
}

static bool date_toISOString(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                             JsValue *r) {
    (void)args; (void)argc;
    JsDateObject *d;
    if (!this_date(ctx, tv, &d, r))
        return false;
    if (d->time != d->time)
        return nthrow(ctx, r, "RangeError: invalid date");
    DateParts p;
    decompose(d->time, &p);
    uint16_t buf[40];
    size_t n = 0;
    wr_year(buf, &n, p.year);
    buf[n++] = '-';
    wr_uint(buf, &n, p.month + 1, 2);
    buf[n++] = '-';
    wr_uint(buf, &n, p.day, 2);
    buf[n++] = 'T';
    wr_uint(buf, &n, p.hour, 2);
    buf[n++] = ':';
    wr_uint(buf, &n, p.minute, 2);
    buf[n++] = ':';
    wr_uint(buf, &n, p.second, 2);
    buf[n++] = '.';
    wr_uint(buf, &n, p.ms, 3);
    buf[n++] = 'Z';
    JsString *s = js_string_cell_new(ctx->vm, buf, n);
    if (!s)
        return nthrow(ctx, r, "out of memory");
    *r = js_value_from_cell(&s->gc);
    return true;
}

static bool date_toJSON(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsDateObject *d;
    if (!this_date(ctx, tv, &d, r))
        return false;
    if (d->time != d->time) {
        *r = js_null();
        return true;
    }
    return date_toISOString(ctx, tv, args, argc, r);
}

/* used by js_to_string_cell for implicit ToString (`'' + date`, template
 * literals, String(date)) — this engine's coercion doesn't do a general
 * user-property toString/valueOf lookup, so built-in object kinds that need
 * a real string form (Date, RegExp) are special-cased there directly. */
JsString *js_date_repr(JsContext *ctx, JsObject *o) {
    JsDateObject *d = (JsDateObject *)o;
    if (d->time != d->time)
        return js_ascii_cell(ctx->vm, "Invalid Date");
    DateParts p;
    decompose(d->time, &p);
    uint16_t buf[64];
    size_t n = fmt_date_part(buf, &p);
    buf[n++] = ' ';
    n += fmt_time_part(buf + n, &p);
    return js_string_cell_new(ctx->vm, buf, n);
}

/* ---- registration ---- */

static bool def_method(JsContext *ctx, JsObject *table, const char *name, JsNativeFn fn) {
    JsValue nf = js_native_new(ctx, name, fn, NULL);
    return js_is_function(nf) && js_object_set_ascii(ctx, table, name, nf);
}

bool js_date_builtins_init(JsContext *ctx) {
    /* Date.prototype: a real object, set before any instance exists (so
     * alloc_date can point new instances at it) and also assigned as the
     * constructor's `.prototype` below. Chains to Object.prototype like any
     * other real prototype object (js_object_new). */
    JsValue t = js_object_new(ctx);
    if (!js_is_object(t))
        return false;
    ctx->date_proto = js_value_object(t); /* rooted via the context now */

    static const struct {
        const char *name;
        JsNativeFn fn;
    } methods[] = {
        {"getTime", date_getTime},
        {"valueOf", date_valueOf},
        {"setTime", date_setTime},
        {"getFullYear", date_getFullYear}, {"getUTCFullYear", date_getFullYear},
        {"getMonth", date_getMonth}, {"getUTCMonth", date_getMonth},
        {"getDate", date_getDate}, {"getUTCDate", date_getDate},
        {"getDay", date_getDay}, {"getUTCDay", date_getDay},
        {"getHours", date_getHours}, {"getUTCHours", date_getHours},
        {"getMinutes", date_getMinutes}, {"getUTCMinutes", date_getMinutes},
        {"getSeconds", date_getSeconds}, {"getUTCSeconds", date_getSeconds},
        {"getMilliseconds", date_getMilliseconds}, {"getUTCMilliseconds", date_getMilliseconds},
        {"getTimezoneOffset", date_getTimezoneOffset},
        {"setFullYear", date_setFullYear}, {"setUTCFullYear", date_setFullYear},
        {"setMonth", date_setMonth}, {"setUTCMonth", date_setMonth},
        {"setDate", date_setDate}, {"setUTCDate", date_setDate},
        {"setHours", date_setHours}, {"setUTCHours", date_setHours},
        {"setMinutes", date_setMinutes}, {"setUTCMinutes", date_setMinutes},
        {"setSeconds", date_setSeconds}, {"setUTCSeconds", date_setSeconds},
        {"setMilliseconds", date_setMilliseconds}, {"setUTCMilliseconds", date_setMilliseconds},
        {"toISOString", date_toISOString},
        {"toJSON", date_toJSON},
        {"toString", date_toString},
        {"toDateString", date_toDateString},
        {"toTimeString", date_toTimeString},
        {"toUTCString", date_toUTCString},
        {"toGMTString", date_toUTCString},
    };
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        if (!def_method(ctx, ctx->date_proto, methods[i].name, methods[i].fn))
            return false;
    }

    JsValue ctor = js_native_new(ctx, "Date", g_Date, NULL);
    if (!js_is_function(ctor))
        return false;
    js_gc_protect(ctx->vm, &ctor); /* keep rooted through statics + global set */
    ((JsNative *)js_value_cell(ctor))->prototype = ctx->date_proto;
    JsValue staticsv = js_object_new(ctx);
    bool ok = js_is_object(staticsv);
    JsObject *statics = ok ? js_value_object(staticsv) : NULL;
    if (ok) {
        ((JsNative *)js_value_cell(ctor))->statics = statics;
        ok = def_method(ctx, statics, "now", date_now) &&
            def_method(ctx, statics, "UTC", date_UTC) &&
            def_method(ctx, statics, "parse", date_parse) &&
            js_object_set_ascii(ctx, ctx->date_proto, "constructor", ctor) &&
            js_object_set_ascii(ctx, ctx->globals, "Date", ctor);
    }
    js_gc_unprotect(ctx->vm, &ctor);
    return ok;
}
