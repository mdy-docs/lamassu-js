/*
 * RegExp binding layer over third_party/baru-re. See js_regexp.h for the
 * object model. Matching drives the engine directly (compile_into /
 * vm_execute); the unanchored scan loop mirrors the engine's own WASM shim
 * (third_party/baru-re/src/regex_wasm.c), including one VMContext per call
 * rather than per start position.
 *
 * GC discipline matches js_builtins.c: natives run with the calling fiber
 * rooted, so `args`/`this` survive; every cell created and held across a
 * further allocation (or a js_call into user code) is protected.
 *
 * Known deviations (documented): matchAll returns an array of match results
 * rather than an iterator (for-of over it behaves the same here); split
 * ignores the `limit` argument (as the non-regex split already does); the
 * string methods leave lastIndex at 0 (global) / untouched (non-global)
 * rather than emulating every intermediate spec mutation.
 */
#ifdef LAMASSU_HAS_REGEX

#include "js_bytecode.h"
#include "js_regexp.h"

#define ARG(i) ((i) < argc ? args[i] : js_undefined())

/* engine capture buffers: group 0 is the whole match */
#define CAPS_MAX ((MAX_GROUPS + 1) * 2)

static bool re_throw(JsContext *ctx, JsValue *r, const char *msg) {
    JsString *s = js_ascii_cell(ctx->vm, msg);
    *r = s ? js_value_from_cell(&s->gc) : js_undefined();
    return false;
}

static bool re_oom(JsContext *ctx, JsValue *r) {
    return re_throw(ctx, r, "out of memory");
}

static bool key_is(JsValue key, const char *name) {
    if (!js_is_string(key))
        return false;
    JsString *k = js_value_string(key);
    uint32_t i = 0;
    for (; name[i]; i++) {
        if (i >= k->length || k->units[i] != (uint16_t)(unsigned char)name[i])
            return false;
    }
    return i == k->length;
}

/* ---- flags ---- */

static int flags_mask(const uint16_t *flags, uint32_t n) {
    int mask = 0;
    for (uint32_t i = 0; i < n; i++) {
        int bit;
        switch (flags[i]) {
        case 'd': bit = REGEX_FLAG_INDICES; break;
        case 'g': bit = REGEX_FLAG_GLOBAL; break;
        case 'i': bit = REGEX_FLAG_IGNORECASE; break;
        case 'm': bit = REGEX_FLAG_MULTILINE; break;
        case 's': bit = REGEX_FLAG_DOTALL; break;
        case 'u': bit = REGEX_FLAG_UNICODE; break;
        case 'v': bit = REGEX_FLAG_UNICODE_SETS; break;
        case 'y': bit = REGEX_FLAG_STICKY; break;
        default: return -1;
        }
        if (mask & bit)
            return -1;
        mask |= bit;
    }
    if ((mask & REGEX_FLAG_UNICODE) && (mask & REGEX_FLAG_UNICODE_SETS))
        return -1;
    return mask;
}

/* ---- construction ---- */

const char *js_regexp_validate(JsVm *vm, const uint16_t *pat, uint32_t pat_len,
                               const uint16_t *flags, uint32_t flags_len) {
    int mask = flags_mask(flags, flags_len);
    if (mask < 0)
        return "SyntaxError: invalid regular expression flags";
    Program *prog = js_realloc_raw(vm, NULL, 0, sizeof *prog);
    if (!prog)
        return "out of memory";
    memset(prog, 0, sizeof *prog);
    uint16_t *pbuf = js_realloc_raw(vm, NULL, 0, ((size_t)pat_len + 1) * sizeof(uint16_t));
    if (!pbuf) {
        js_realloc_raw(vm, prog, sizeof *prog, 0);
        return "out of memory";
    }
    if (pat_len)
        memcpy(pbuf, pat, (size_t)pat_len * sizeof(uint16_t));
    pbuf[pat_len] = 0;
    compile_into(prog, pbuf, mask);
    const char *err = prog->error; /* engine errors are static literals */
    for (int i = 0; i < prog->class_count; i++)
        class_strings_free(&prog->classes[i]);
    js_realloc_raw(vm, pbuf, ((size_t)pat_len + 1) * sizeof(uint16_t), 0);
    js_realloc_raw(vm, prog, sizeof *prog, 0);
    return err;
}

JsValue js_regexp_new(JsContext *ctx, const uint16_t *pat, uint32_t pat_len,
                      const uint16_t *flags, uint32_t flags_len, const char **err) {
    JsVm *vm = ctx->vm;
    *err = NULL;
    int mask = flags_mask(flags, flags_len);
    if (mask < 0) {
        *err = "SyntaxError: invalid regular expression flags";
        return js_undefined();
    }
    if (vm->regexp_live >= JS_REGEXP_MAX_LIVE) {
        *err = "RangeError: too many live regular expressions";
        return js_undefined();
    }
    uint16_t *pbuf = js_realloc_raw(vm, NULL, 0, ((size_t)pat_len + 1) * sizeof(uint16_t));
    if (!pbuf) {
        *err = "out of memory";
        return js_undefined();
    }
    if (pat_len)
        memcpy(pbuf, pat, (size_t)pat_len * sizeof(uint16_t));
    pbuf[pat_len] = 0;

    JsGcCell *c = js_gc_new_cell(vm, JS_KIND_OBJECT, sizeof(JsRegExp));
    if (!c) {
        js_realloc_raw(vm, pbuf, ((size_t)pat_len + 1) * sizeof(uint16_t), 0);
        *err = "out of memory";
        return js_undefined();
    }
    JsRegExp *re = (JsRegExp *)c;
    re->obj.obj_kind = JS_OBJ_REGEXP;
    js_map_init(&re->obj.props);
    re->obj.elems = NULL;
    re->obj.elem_count = re->obj.elem_cap = 0;
    re->obj.proto = ctx->regexp_proto ? js_value_from_cell(&ctx->regexp_proto->gc) : js_undefined();
    re->source = NULL;
    re->flags = NULL;
    re->last_index = 0;
    re->global = (mask & REGEX_FLAG_GLOBAL) != 0;
    memset(&re->prog, 0, sizeof re->prog); /* compile_into requires zero init */
    vm->regexp_live++; /* balanced by js_regexp_release at sweep */

    JsValue rev = js_value_from_cell(c);
    js_gc_protect(vm, &rev);
    compile_into(&re->prog, pbuf, mask);
    js_realloc_raw(vm, pbuf, ((size_t)pat_len + 1) * sizeof(uint16_t), 0);
    if (re->prog.error) {
        /* the cell stays on the GC list and is swept as a normal regexp;
         * release() frees whatever class strings the failed compile left */
        *err = re->prog.error;
        js_gc_unprotect(vm, &rev);
        return js_undefined();
    }

    JsString *src = pat_len ? js_string_cell_new(vm, pat, pat_len)
                            : js_ascii_cell(vm, "(?:)");
    if (!src)
        goto oom;
    re->source = src;

    char fl[9];
    int n = 0;
    if (mask & REGEX_FLAG_INDICES) fl[n++] = 'd';
    if (mask & REGEX_FLAG_GLOBAL) fl[n++] = 'g';
    if (mask & REGEX_FLAG_IGNORECASE) fl[n++] = 'i';
    if (mask & REGEX_FLAG_MULTILINE) fl[n++] = 'm';
    if (mask & REGEX_FLAG_DOTALL) fl[n++] = 's';
    if (mask & REGEX_FLAG_UNICODE) fl[n++] = 'u';
    if (mask & REGEX_FLAG_UNICODE_SETS) fl[n++] = 'v';
    if (mask & REGEX_FLAG_STICKY) fl[n++] = 'y';
    fl[n] = 0;
    JsString *fs = js_ascii_cell(vm, fl);
    if (!fs)
        goto oom;
    re->flags = fs;

    js_gc_unprotect(vm, &rev);
    return rev;

oom:
    *err = "out of memory";
    js_gc_unprotect(vm, &rev);
    return js_undefined();
}

/* ---- GC hooks ---- */

void js_regexp_mark(JsVm *vm, JsObject *o) {
    JsRegExp *re = (JsRegExp *)o;
    if (re->source)
        js_gc_mark_cell(vm, &re->source->gc);
    if (re->flags)
        js_gc_mark_cell(vm, &re->flags->gc);
}

size_t js_regexp_release(JsVm *vm, JsObject *o) {
    JsRegExp *re = (JsRegExp *)o;
    for (int i = 0; i < re->prog.class_count; i++)
        class_strings_free(&re->prog.classes[i]);
    vm->regexp_live--;
    return sizeof *re;
}

/* ---- synthesized properties ---- */

bool js_regexp_prop_get(JsContext *ctx, JsObject *o, JsValue key, JsValue *out,
                        bool *handled) {
    (void)ctx;
    JsRegExp *re = (JsRegExp *)o;
    *handled = true;
    if (key_is(key, "source")) { *out = js_value_from_cell(&re->source->gc); return true; }
    if (key_is(key, "flags")) { *out = js_value_from_cell(&re->flags->gc); return true; }
    if (key_is(key, "lastIndex")) { *out = js_number(re->last_index); return true; }
    if (key_is(key, "global")) { *out = js_bool(re->global); return true; }
    if (key_is(key, "ignoreCase")) { *out = js_bool(re->prog.ignore_case); return true; }
    if (key_is(key, "multiline")) { *out = js_bool(re->prog.multiline); return true; }
    if (key_is(key, "dotAll")) { *out = js_bool(re->prog.dot_all); return true; }
    if (key_is(key, "sticky")) { *out = js_bool(re->prog.sticky); return true; }
    if (key_is(key, "unicode")) { *out = js_bool(re->prog.unicode); return true; }
    if (key_is(key, "unicodeSets")) { *out = js_bool(re->prog.unicode_sets); return true; }
    if (key_is(key, "hasIndices")) { *out = js_bool(re->prog.has_indices); return true; }
    *handled = false;
    return true;
}

bool js_regexp_prop_set(JsContext *ctx, JsObject *o, JsValue key, JsValue val,
                        bool *handled, const char **errmsg) {
    JsRegExp *re = (JsRegExp *)o;
    if (key_is(key, "lastIndex")) {
        re->last_index = js_to_number_value(ctx, val);
        *handled = true;
        return true;
    }
    if (key_is(key, "source") || key_is(key, "flags") || key_is(key, "global") ||
        key_is(key, "ignoreCase") || key_is(key, "multiline") || key_is(key, "dotAll") ||
        key_is(key, "sticky") || key_is(key, "unicode") || key_is(key, "unicodeSets") ||
        key_is(key, "hasIndices")) {
        *errmsg = "TypeError: cannot assign to read-only RegExp property";
        return false;
    }
    *handled = false;
    return true;
}

JsString *js_regexp_repr(JsContext *ctx, JsObject *o) {
    JsVm *vm = ctx->vm;
    JsRegExp *re = (JsRegExp *)o;
    uint32_t n = 1 + re->source->length + 1 + re->flags->length;
    uint16_t *buf = js_realloc_raw(vm, NULL, 0, (size_t)n * sizeof(uint16_t));
    if (!buf)
        return NULL;
    uint32_t at = 0;
    buf[at++] = '/';
    memcpy(buf + at, re->source->units, (size_t)re->source->length * sizeof(uint16_t));
    at += re->source->length;
    buf[at++] = '/';
    memcpy(buf + at, re->flags->units, (size_t)re->flags->length * sizeof(uint16_t));
    at += re->flags->length;
    JsString *s = js_string_cell_new(vm, buf, n);
    js_realloc_raw(vm, buf, (size_t)n * sizeof(uint16_t), 0);
    return s;
}

/* ---- matching core ---- */

static bool is_lead(uint16_t u) { return u >= 0xD800 && u <= 0xDBFF; }
static bool is_trail(uint16_t u) { return u >= 0xDC00 && u <= 0xDFFF; }

static bool re_is_unicode(const JsRegExp *re) {
    return re->prog.unicode || re->prog.unicode_sets;
}

/* AdvanceStringIndex: one code point under u/v, else one code unit */
static uint32_t advance_index(const JsString *s, uint32_t i, bool uni) {
    if (uni && i + 1 < s->length && is_lead(s->units[i]) && is_trail(s->units[i + 1]))
        return i + 2;
    return i + 1;
}

/* re_run results: a tri-state so budget exhaustion is distinguishable from
 * an ordinary no-match and can be thrown as a catchable RangeError. */
#define RE_RUN_MATCH 1
#define RE_RUN_NOMATCH 0
#define RE_RUN_BUDGET (-1)

static const char *const re_budget_msg =
    "RangeError: regular expression step budget exhausted";

/*
 * Runs the compiled pattern against s. anchored=true tries only at `start`
 * (split's forced-sticky semantics); otherwise sticky patterns anchor
 * themselves and the scan loop advances by code point under u/v. On a match
 * caps[] holds (group_count+1)*2 unit offsets, -1 for an unmatched group.
 * Every call gets a fresh engine context with a step budget scaled to the
 * subject, so catastrophic backtracking returns RE_RUN_BUDGET instead of
 * hanging the VM.
 */
static int re_run(JsRegExp *re, const JsString *s, uint32_t start, bool anchored,
                  int32_t *caps) {
    Program *prog = &re->prog;
    const uint16_t *text = s->units;
    const uint16_t *text_end = text + s->length;
    const uint16_t *captures[CAPS_MAX] = {0};
    bool matched = false;

    VMContext *vctx = vm_context_new(prog);
    if (!vctx)
        return RE_RUN_NOMATCH; /* OOM: report as no-match rather than crash */
    vm_context_set_step_budget(vctx, JS_REGEXP_STEP_BASE +
                                         (uint64_t)s->length * JS_REGEXP_STEPS_PER_UNIT);
    if (anchored || prog->sticky) {
        matched = vm_execute(prog, vctx, 0, 1, text, text_end, text + start, captures);
    } else {
        bool uni = re_is_unicode(re);
        for (uint32_t i = start; i <= s->length;) {
            matched = vm_execute(prog, vctx, 0, 1, text, text_end, text + i, captures);
            if (matched || vm_context_budget_exhausted(vctx))
                break;
            i = advance_index(s, i, uni);
        }
    }
    bool exhausted = vm_context_budget_exhausted(vctx);
    vm_context_free(vctx);
    if (!matched)
        return exhausted ? RE_RUN_BUDGET : RE_RUN_NOMATCH;
    for (int g = 0; g <= prog->group_count; g++) {
        const uint16_t *cs = captures[g * 2], *ce = captures[g * 2 + 1];
        if (cs && ce) {
            caps[g * 2] = (int32_t)(cs - text);
            caps[g * 2 + 1] = (int32_t)(ce - text);
        } else {
            caps[g * 2] = caps[g * 2 + 1] = -1;
        }
    }
    return RE_RUN_MATCH;
}

/*
 * exec's lastIndex protocol (also used by test and non-global match):
 * lastIndex is consulted and updated only when the pattern is global or
 * sticky. Returns an RE_RUN_* result with caps filled on a match; a budget
 * result leaves lastIndex untouched (the caller throws).
 */
static int exec_protocol(JsRegExp *re, JsString *s, int32_t *caps) {
    bool track = re->global || re->prog.sticky;
    uint32_t start = 0;
    if (track) {
        double li = __builtin_trunc(re->last_index);
        if (li != li || li < 0)
            li = 0;
        if (li > (double)s->length) {
            re->last_index = 0;
            return RE_RUN_NOMATCH;
        }
        start = (uint32_t)li;
    }
    int rc = re_run(re, s, start, false, caps);
    if (rc != RE_RUN_MATCH) {
        if (track && rc == RE_RUN_NOMATCH)
            re->last_index = 0;
        return rc;
    }
    if (track)
        re->last_index = caps[1];
    return RE_RUN_MATCH;
}

/* ---- match result construction ---- */

static JsValue substr_or_undef(JsVm *vm, const JsString *s, int32_t a, int32_t b,
                               bool *oom) {
    if (a < 0)
        return js_undefined();
    JsString *sub = js_string_cell_new(vm, s->units + a, (uint32_t)(b - a));
    if (!sub) {
        *oom = true;
        return js_undefined();
    }
    return js_value_from_cell(&sub->gc);
}

/* [start, end] pair for /d results */
static JsValue pair_or_undef(JsContext *ctx, int32_t a, int32_t b, bool *oom) {
    if (a < 0)
        return js_undefined();
    JsObject *arr = js_array_new_cell(ctx, 2);
    if (!arr || !js_array_append(ctx->vm, arr, js_number(a)) ||
        !js_array_append(ctx->vm, arr, js_number(b))) {
        *oom = true;
        return js_undefined();
    }
    return js_value_from_cell(&arr->gc);
}

/* decodes an engine group name (UTF-8, engine-validated) to UTF-16 units */
static uint32_t name_units(const char *u8, uint16_t *out, uint32_t cap) {
    const unsigned char *p = (const unsigned char *)u8;
    uint32_t n = 0;
    while (*p && n + 2 <= cap) {
        uint32_t cp;
        if (p[0] < 0x80) {
            cp = p[0];
            p += 1;
        } else if ((p[0] & 0xE0) == 0xC0) {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((p[0] & 0xF0) == 0xE0) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
                 (p[2] & 0x3F);
            p += 3;
        } else {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                 ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            p += 4;
        }
        if (cp >= 0x10000) {
            out[n++] = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10));
            out[n++] = (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else {
            out[n++] = (uint16_t)cp;
        }
    }
    return n;
}

static bool has_named_groups(const JsRegExp *re) {
    for (int g = 1; g <= re->prog.group_count; g++) {
        if (re->prog.group_names[g][0])
            return true;
    }
    return false;
}

/* 1-based group index for a name, or 0 if absent */
static int named_group_index(const JsRegExp *re, const uint16_t *name, uint32_t nlen) {
    uint16_t buf[64];
    for (int g = 1; g <= re->prog.group_count; g++) {
        if (!re->prog.group_names[g][0])
            continue;
        uint32_t n = name_units(re->prog.group_names[g], buf, 64);
        if (n != nlen)
            continue;
        bool eq = true;
        for (uint32_t i = 0; i < n && eq; i++)
            eq = buf[i] == name[i];
        if (eq)
            return g;
    }
    return 0;
}

/*
 * The { name: value } object for named groups; `indices` selects [start,end]
 * pairs (for result.indices.groups) over matched substrings. *out is left
 * unprotected — the caller stores it into a protected container.
 */
static bool named_groups_obj(JsContext *ctx, JsRegExp *re, const JsString *s,
                             const int32_t *caps, bool indices, JsValue *out) {
    JsVm *vm = ctx->vm;
    JsValue obj = js_object_new(ctx);
    if (!js_is_object(obj))
        return false;
    js_gc_protect(vm, &obj);
    JsValue kv = js_undefined(), vv = js_undefined();
    js_gc_protect(vm, &kv);
    js_gc_protect(vm, &vv);
    bool ok = true, oom = false;
    for (int g = 1; ok && g <= re->prog.group_count; g++) {
        if (!re->prog.group_names[g][0])
            continue;
        vv = indices ? pair_or_undef(ctx, caps[g * 2], caps[g * 2 + 1], &oom)
                     : substr_or_undef(vm, s, caps[g * 2], caps[g * 2 + 1], &oom);
        if (oom) {
            ok = false;
            break;
        }
        uint16_t nbuf[64];
        uint32_t nlen = name_units(re->prog.group_names[g], nbuf, 64);
        kv = js_atom(vm, nbuf, nlen);
        if (!js_is_string(kv)) {
            ok = false;
            break;
        }
        ok = js_map_set(vm, &js_value_object(obj)->props, js_value_string(kv), vv);
    }
    js_gc_unprotect(vm, &vv);
    js_gc_unprotect(vm, &kv);
    js_gc_unprotect(vm, &obj);
    if (ok)
        *out = obj;
    return ok;
}

/*
 * Builds the exec-style result array: [match, g1..gn] plus index, input,
 * groups, and (with /d) indices (+ indices.groups). sv must be rooted by
 * the caller.
 */
static bool build_exec_result(JsContext *ctx, JsRegExp *re, JsValue sv,
                              const int32_t *caps, JsValue *out) {
    JsVm *vm = ctx->vm;
    JsString *s = js_value_string(sv);
    int ng = re->prog.group_count;

    JsObject *arr = js_array_new_cell(ctx, (uint32_t)ng + 1);
    if (!arr)
        return false;
    JsValue arrv = js_value_from_cell(&arr->gc);
    js_gc_protect(vm, &arrv);
    bool ok = true, oom = false;

    for (int g = 0; ok && g <= ng; g++) {
        JsValue el = substr_or_undef(vm, s, caps[g * 2], caps[g * 2 + 1], &oom);
        ok = !oom && js_array_append(vm, js_value_object(arrv), el);
    }
    ok = ok && js_object_set_ascii(ctx, js_value_object(arrv), "index", js_number(caps[0]));
    ok = ok && js_object_set_ascii(ctx, js_value_object(arrv), "input", sv);

    if (ok) {
        JsValue groups = js_undefined();
        if (has_named_groups(re))
            ok = named_groups_obj(ctx, re, s, caps, false, &groups);
        if (ok) {
            js_gc_protect(vm, &groups);
            ok = js_object_set_ascii(ctx, js_value_object(arrv), "groups", groups);
            js_gc_unprotect(vm, &groups);
        }
    }

    if (ok && re->prog.has_indices) {
        JsObject *ind = js_array_new_cell(ctx, (uint32_t)ng + 1);
        JsValue indv = ind ? js_value_from_cell(&ind->gc) : js_undefined();
        ok = ind != NULL;
        if (ok)
            js_gc_protect(vm, &indv);
        for (int g = 0; ok && g <= ng; g++) {
            JsValue el = pair_or_undef(ctx, caps[g * 2], caps[g * 2 + 1], &oom);
            ok = !oom && js_array_append(vm, js_value_object(indv), el);
        }
        if (ok) {
            JsValue groups = js_undefined();
            if (has_named_groups(re))
                ok = named_groups_obj(ctx, re, s, caps, true, &groups);
            if (ok) {
                js_gc_protect(vm, &groups);
                ok = js_object_set_ascii(ctx, js_value_object(indv), "groups", groups);
                js_gc_unprotect(vm, &groups);
            }
        }
        if (ok)
            ok = js_object_set_ascii(ctx, js_value_object(arrv), "indices", indv);
        if (!js_is_undefined(indv))
            js_gc_unprotect(vm, &indv);
    }

    js_gc_unprotect(vm, &arrv);
    if (!ok)
        return false;
    *out = arrv;
    return true;
}

/* ---- RegExp methods ---- */

static bool rexp_exec(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                      JsValue *r) {
    if (!js_regexp_is(tv))
        return re_throw(ctx, r, "TypeError: exec called on a non-RegExp");
    JsRegExp *re = (JsRegExp *)js_value_object(tv);
    JsString *s = js_to_string_cell(ctx, ARG(0), 0);
    if (!s)
        return re_oom(ctx, r);
    JsValue sv = js_value_from_cell(&s->gc);
    js_gc_protect(ctx->vm, &sv);
    int32_t caps[CAPS_MAX];
    bool ok = true;
    int rc = exec_protocol(re, s, caps);
    if (rc == RE_RUN_BUDGET) {
        js_gc_unprotect(ctx->vm, &sv);
        return re_throw(ctx, r, re_budget_msg);
    }
    if (rc == RE_RUN_NOMATCH)
        *r = js_null();
    else
        ok = build_exec_result(ctx, re, sv, caps, r);
    js_gc_unprotect(ctx->vm, &sv);
    return ok ? true : re_oom(ctx, r);
}

static bool rexp_test(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                      JsValue *r) {
    if (!js_regexp_is(tv))
        return re_throw(ctx, r, "TypeError: test called on a non-RegExp");
    JsRegExp *re = (JsRegExp *)js_value_object(tv);
    JsString *s = js_to_string_cell(ctx, ARG(0), 0);
    if (!s)
        return re_oom(ctx, r);
    int32_t caps[CAPS_MAX];
    int rc = exec_protocol(re, s, caps);
    if (rc == RE_RUN_BUDGET)
        return re_throw(ctx, r, re_budget_msg);
    *r = js_bool(rc == RE_RUN_MATCH);
    return true;
}

static bool rexp_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                          JsValue *r) {
    (void)args;
    (void)argc;
    if (!js_regexp_is(tv))
        return re_throw(ctx, r, "TypeError: toString called on a non-RegExp");
    JsString *s = js_regexp_repr(ctx, js_value_object(tv));
    if (!s)
        return re_oom(ctx, r);
    *r = js_value_from_cell(&s->gc);
    return true;
}

/* ---- RegExp global (plain callable; the engine has no `new`) ---- */

static bool g_RegExp(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                     JsValue *r) {
    (void)tv;
    JsVm *vm = ctx->vm;
    JsValue pv = ARG(0), fv = ARG(1);
    const uint16_t *pu = NULL, *fu = NULL;
    uint32_t pn = 0, fn = 0;
    JsValue pstr = js_undefined(), fstr = js_undefined();
    js_gc_protect(vm, &pstr);
    js_gc_protect(vm, &fstr);
    bool ok = true;

    if (js_regexp_is(pv)) {
        JsRegExp *src = (JsRegExp *)js_value_object(pv);
        pu = src->source->units;
        pn = src->source->length;
        if (js_is_undefined(fv)) {
            fu = src->flags->units;
            fn = src->flags->length;
        }
    } else if (!js_is_undefined(pv)) {
        JsString *ps = js_to_string_cell(ctx, pv, 0);
        if (!ps) {
            ok = false;
        } else {
            pstr = js_value_from_cell(&ps->gc);
            pu = ps->units;
            pn = ps->length;
        }
    }
    if (ok && !js_is_undefined(fv)) {
        JsString *fs = js_to_string_cell(ctx, fv, 0);
        if (!fs) {
            ok = false;
        } else {
            fstr = js_value_from_cell(&fs->gc);
            fu = fs->units;
            fn = fs->length;
        }
    }
    const char *err = NULL;
    JsValue rev = js_undefined();
    if (ok)
        rev = js_regexp_new(ctx, pu, pn, fu, fn, &err);
    js_gc_unprotect(vm, &fstr);
    js_gc_unprotect(vm, &pstr);
    if (!ok)
        return re_oom(ctx, r);
    if (!js_is_object(rev))
        return re_throw(ctx, r, err ? err : "out of memory");
    *r = rev;
    return true;
}

/* ---- String methods ---- */

/*
 * The regexp for match/matchAll/search: the argument itself, or a fresh one
 * compiled from ToString(argument) (undefined -> empty pattern) with
 * `extra_flags`. *rev must be a protected slot owned by the caller — it
 * keeps a freshly compiled regexp alive. False = thrown, *r set.
 */
static bool coerce_regexp(JsContext *ctx, JsValue pv, const char *extra_flags,
                          JsValue *rev, JsRegExp **out, JsValue *r) {
    JsVm *vm = ctx->vm;
    if (js_regexp_is(pv)) {
        *out = (JsRegExp *)js_value_object(pv);
        return true;
    }
    const uint16_t *pu = NULL;
    uint32_t pn = 0;
    JsValue pstr = js_undefined();
    js_gc_protect(vm, &pstr);
    if (!js_is_undefined(pv)) {
        JsString *ps = js_to_string_cell(ctx, pv, 0);
        if (!ps) {
            js_gc_unprotect(vm, &pstr);
            return re_oom(ctx, r);
        }
        pstr = js_value_from_cell(&ps->gc);
        pu = ps->units;
        pn = ps->length;
    }
    uint16_t fbuf[4];
    uint32_t fn = 0;
    while (extra_flags[fn]) {
        fbuf[fn] = (uint16_t)extra_flags[fn];
        fn++;
    }
    const char *err = NULL;
    *rev = js_regexp_new(ctx, pu, pn, fbuf, fn, &err);
    js_gc_unprotect(vm, &pstr);
    if (!js_is_object(*rev))
        return re_throw(ctx, r, err ? err : "out of memory");
    *out = (JsRegExp *)js_value_object(*rev);
    return true;
}

bool js_re_str_match(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                     JsValue *r) {
    JsVm *vm = ctx->vm;
    JsString *s = js_to_string_cell(ctx, tv, 0);
    if (!s)
        return re_oom(ctx, r);
    JsValue sv = js_value_from_cell(&s->gc);
    js_gc_protect(vm, &sv);
    JsValue rev = js_undefined();
    js_gc_protect(vm, &rev);
    JsRegExp *re;
    if (!coerce_regexp(ctx, ARG(0), "", &rev, &re, r)) {
        js_gc_unprotect(vm, &rev);
        js_gc_unprotect(vm, &sv);
        return false;
    }
    bool ok = true, budget = false;
    int32_t caps[CAPS_MAX];
    if (!re->global) {
        int rc = exec_protocol(re, s, caps);
        if (rc == RE_RUN_BUDGET)
            budget = true;
        else if (rc == RE_RUN_NOMATCH)
            *r = js_null();
        else
            ok = build_exec_result(ctx, re, sv, caps, r);
    } else {
        JsObject *arr = js_array_new_cell(ctx, 0);
        JsValue arrv = arr ? js_value_from_cell(&arr->gc) : js_undefined();
        ok = arr != NULL;
        if (ok) {
            js_gc_protect(vm, &arrv);
            bool uni = re_is_unicode(re), oom = false;
            uint32_t pos = 0;
            int rc = RE_RUN_NOMATCH;
            while (ok && pos <= s->length &&
                   (rc = re_run(re, s, pos, false, caps)) == RE_RUN_MATCH) {
                JsValue el = substr_or_undef(vm, s, caps[0], caps[1], &oom);
                ok = !oom && js_array_append(vm, js_value_object(arrv), el);
                pos = caps[1] == caps[0] ? advance_index(s, (uint32_t)caps[1], uni)
                                         : (uint32_t)caps[1];
            }
            budget = rc == RE_RUN_BUDGET;
            re->last_index = 0;
            if (ok && !budget)
                *r = js_value_object(arrv)->elem_count ? arrv : js_null();
            js_gc_unprotect(vm, &arrv);
        }
    }
    js_gc_unprotect(vm, &rev);
    js_gc_unprotect(vm, &sv);
    if (budget)
        return re_throw(ctx, r, re_budget_msg);
    return ok ? true : re_oom(ctx, r);
}

bool js_re_str_matchAll(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                        JsValue *r) {
    JsVm *vm = ctx->vm;
    if (js_regexp_is(ARG(0)) && !((JsRegExp *)js_value_object(args[0]))->global)
        return re_throw(ctx, r, "TypeError: matchAll must be called with a global RegExp");
    JsString *s = js_to_string_cell(ctx, tv, 0);
    if (!s)
        return re_oom(ctx, r);
    JsValue sv = js_value_from_cell(&s->gc);
    js_gc_protect(vm, &sv);
    JsValue rev = js_undefined();
    js_gc_protect(vm, &rev);
    JsRegExp *re;
    if (!coerce_regexp(ctx, ARG(0), "g", &rev, &re, r)) {
        js_gc_unprotect(vm, &rev);
        js_gc_unprotect(vm, &sv);
        return false;
    }
    JsObject *arr = js_array_new_cell(ctx, 0);
    JsValue arrv = arr ? js_value_from_cell(&arr->gc) : js_undefined();
    bool ok = arr != NULL, budget = false;
    if (ok) {
        js_gc_protect(vm, &arrv);
        bool uni = re_is_unicode(re);
        int32_t caps[CAPS_MAX];
        uint32_t pos = 0;
        int rc = RE_RUN_NOMATCH;
        while (ok && pos <= s->length &&
               (rc = re_run(re, s, pos, false, caps)) == RE_RUN_MATCH) {
            JsValue m;
            ok = build_exec_result(ctx, re, sv, caps, &m) &&
                 js_array_append(vm, js_value_object(arrv), m);
            pos = caps[1] == caps[0] ? advance_index(s, (uint32_t)caps[1], uni)
                                     : (uint32_t)caps[1];
        }
        budget = rc == RE_RUN_BUDGET;
        if (ok && !budget)
            *r = arrv;
        js_gc_unprotect(vm, &arrv);
    }
    js_gc_unprotect(vm, &rev);
    js_gc_unprotect(vm, &sv);
    if (budget)
        return re_throw(ctx, r, re_budget_msg);
    return ok ? true : re_oom(ctx, r);
}

bool js_re_str_search(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                      JsValue *r) {
    JsVm *vm = ctx->vm;
    JsString *s = js_to_string_cell(ctx, tv, 0);
    if (!s)
        return re_oom(ctx, r);
    JsValue sv = js_value_from_cell(&s->gc);
    js_gc_protect(vm, &sv);
    JsValue rev = js_undefined();
    js_gc_protect(vm, &rev);
    JsRegExp *re;
    if (!coerce_regexp(ctx, ARG(0), "", &rev, &re, r)) {
        js_gc_unprotect(vm, &rev);
        js_gc_unprotect(vm, &sv);
        return false;
    }
    int32_t caps[CAPS_MAX];
    int rc = re_run(re, s, 0, false, caps);
    js_gc_unprotect(vm, &rev);
    js_gc_unprotect(vm, &sv);
    if (rc == RE_RUN_BUDGET)
        return re_throw(ctx, r, re_budget_msg);
    *r = js_number(rc == RE_RUN_MATCH ? (double)caps[0] : -1.0);
    return true;
}

/* ---- replace ---- */

/* C-side unit buffer (mirror of js_builtins.c's StrBuf, which is static there) */
typedef struct {
    JsVm *vm;
    uint16_t *u;
    uint32_t len, cap;
    bool oom;
} UBuf;

static void ub_init(UBuf *b, JsVm *vm) {
    b->vm = vm;
    b->u = NULL;
    b->len = b->cap = 0;
    b->oom = false;
}

static bool ub_reserve(UBuf *b, uint32_t extra) {
    if (b->oom)
        return false;
    if (extra > UINT32_MAX - b->len) { /* len + extra would wrap uint32 */
        b->oom = true;
        return false;
    }
    uint32_t need = b->len + extra;
    if (need <= b->cap)
        return true;
    uint32_t ncap = b->cap ? b->cap * 2 : 32;
    while (ncap < need) {
        if (ncap > UINT32_MAX / 2) { /* next doubling would overflow */
            ncap = need;
            break;
        }
        ncap *= 2;
    }
    uint16_t *nu = js_realloc_raw(b->vm, b->u, (size_t)b->cap * sizeof(uint16_t),
                                  (size_t)ncap * sizeof(uint16_t));
    if (!nu) {
        b->oom = true;
        return false;
    }
    b->u = nu;
    b->cap = ncap;
    return true;
}

static void ub_unit(UBuf *b, uint16_t c) {
    if (ub_reserve(b, 1))
        b->u[b->len++] = c;
}

static void ub_units(UBuf *b, const uint16_t *u, uint32_t n) {
    if (n && ub_reserve(b, n)) {
        memcpy(b->u + b->len, u, (size_t)n * sizeof(uint16_t));
        b->len += n;
    }
}

static void ub_free(UBuf *b) {
    js_realloc_raw(b->vm, b->u, (size_t)b->cap * sizeof(uint16_t), 0);
    b->u = NULL;
    b->cap = b->len = 0;
}

static JsValue ub_finish(UBuf *b) {
    if (b->oom) {
        ub_free(b);
        return js_undefined();
    }
    JsString *s = js_string_cell_new(b->vm, b->u, b->len);
    ub_free(b);
    return s ? js_value_from_cell(&s->gc) : js_undefined();
}

/* GetSubstitution: $$ $& $` $' $n $nn $<name>; anything else is literal */
static void expand_replacement(UBuf *sb, JsRegExp *re, const JsString *s,
                               const int32_t *caps, const JsString *rep) {
    int ng = re->prog.group_count;
    bool named = has_named_groups(re);
    uint32_t i = 0;
    while (i < rep->length) {
        uint16_t c = rep->units[i];
        if (c != '$' || i + 1 >= rep->length) {
            ub_unit(sb, c);
            i++;
            continue;
        }
        uint16_t n = rep->units[i + 1];
        if (n == '$') {
            ub_unit(sb, '$');
            i += 2;
        } else if (n == '&') {
            ub_units(sb, s->units + caps[0], (uint32_t)(caps[1] - caps[0]));
            i += 2;
        } else if (n == '`') {
            ub_units(sb, s->units, (uint32_t)caps[0]);
            i += 2;
        } else if (n == '\'') {
            ub_units(sb, s->units + caps[1], s->length - (uint32_t)caps[1]);
            i += 2;
        } else if (n >= '0' && n <= '9') {
            uint32_t g = (uint32_t)(n - '0');
            uint32_t adv = 2;
            if (i + 2 < rep->length && rep->units[i + 2] >= '0' &&
                rep->units[i + 2] <= '9') {
                uint32_t g2 = g * 10 + (uint32_t)(rep->units[i + 2] - '0');
                if (g2 >= 1 && (int)g2 <= ng) {
                    g = g2;
                    adv = 3;
                }
            }
            if (g >= 1 && (int)g <= ng) {
                if (caps[g * 2] >= 0)
                    ub_units(sb, s->units + caps[g * 2],
                             (uint32_t)(caps[g * 2 + 1] - caps[g * 2]));
                i += adv;
            } else {
                ub_unit(sb, '$');
                i++;
            }
        } else if (n == '<' && named) {
            uint32_t close = i + 2;
            while (close < rep->length && rep->units[close] != '>')
                close++;
            if (close >= rep->length) {
                ub_unit(sb, '$');
                i++;
            } else {
                int g = named_group_index(re, rep->units + i + 2, close - (i + 2));
                if (g && caps[g * 2] >= 0)
                    ub_units(sb, s->units + caps[g * 2],
                             (uint32_t)(caps[g * 2 + 1] - caps[g * 2]));
                i = close + 1; /* unknown/unmatched name -> empty string */
            }
        } else {
            ub_unit(sb, '$');
            i++;
        }
    }
}

/*
 * Rooted argument vector for a function replacer:
 * (match, g1..gn, index, input[, groups]). Returned as an array object whose
 * elems back js_call's args; the caller protects the returned value.
 */
static JsValue replacer_args(JsContext *ctx, JsRegExp *re, JsValue sv,
                             const int32_t *caps) {
    JsVm *vm = ctx->vm;
    JsString *s = js_value_string(sv);
    int ng = re->prog.group_count;
    JsObject *arr = js_array_new_cell(ctx, (uint32_t)ng + 3);
    if (!arr)
        return js_undefined();
    JsValue arrv = js_value_from_cell(&arr->gc);
    js_gc_protect(vm, &arrv);
    bool ok = true, oom = false;
    for (int g = 0; ok && g <= ng; g++) {
        JsValue el = substr_or_undef(vm, s, caps[g * 2], caps[g * 2 + 1], &oom);
        ok = !oom && js_array_append(vm, js_value_object(arrv), el);
    }
    ok = ok && js_array_append(vm, js_value_object(arrv), js_number(caps[0]));
    ok = ok && js_array_append(vm, js_value_object(arrv), sv);
    if (ok && has_named_groups(re)) {
        JsValue groups;
        ok = named_groups_obj(ctx, re, s, caps, false, &groups) &&
             js_array_append(vm, js_value_object(arrv), groups);
    }
    js_gc_unprotect(vm, &arrv);
    return ok ? arrv : js_undefined();
}

bool js_re_str_replace(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                       JsValue *r, bool all) {
    JsVm *vm = ctx->vm;
    JsRegExp *re = (JsRegExp *)js_value_object(args[0]); /* caller-checked */
    if (all && !re->global)
        return re_throw(ctx, r, "TypeError: replaceAll must be called with a global RegExp");
    JsString *s = js_to_string_cell(ctx, tv, 0);
    if (!s)
        return re_oom(ctx, r);
    JsValue sv = js_value_from_cell(&s->gc);
    js_gc_protect(vm, &sv);

    JsValue repfn = ARG(1);
    bool is_fn = js_is_function(repfn);
    JsString *reps = NULL;
    JsValue repsv = js_undefined();
    js_gc_protect(vm, &repsv);
    if (!is_fn) {
        reps = js_to_string_cell(ctx, repfn, 0);
        if (!reps) {
            js_gc_unprotect(vm, &repsv);
            js_gc_unprotect(vm, &sv);
            return re_oom(ctx, r);
        }
        repsv = js_value_from_cell(&reps->gc);
    }

    UBuf sb;
    ub_init(&sb, vm);
    bool many = re->global;
    bool uni = re_is_unicode(re);
    int32_t caps[CAPS_MAX];
    uint32_t pos = 0, cursor = 0;
    bool ok = true, thrown = false, budget = false;
    int rc = RE_RUN_NOMATCH;

    while (pos <= s->length && (rc = re_run(re, s, pos, false, caps)) == RE_RUN_MATCH) {
        uint32_t m0 = (uint32_t)caps[0], m1 = (uint32_t)caps[1];
        ub_units(&sb, s->units + cursor, m0 - cursor);
        if (is_fn) {
            JsValue argv = replacer_args(ctx, re, sv, caps);
            if (!js_is_object(argv)) {
                ok = false;
                break;
            }
            js_gc_protect(vm, &argv);
            JsObject *aobj = js_value_object(argv);
            JsValue res;
            if (!js_call(ctx, repfn, js_undefined(), aobj->elems,
                         (int)aobj->elem_count, &res)) {
                *r = res; /* propagate the thrown value */
                js_gc_unprotect(vm, &argv);
                ok = false;
                thrown = true;
                break;
            }
            js_gc_unprotect(vm, &argv);
            js_gc_protect(vm, &res);
            JsString *rs = js_to_string_cell(ctx, res, 0);
            js_gc_unprotect(vm, &res);
            if (!rs) {
                ok = false;
                break;
            }
            ub_units(&sb, rs->units, rs->length);
        } else {
            expand_replacement(&sb, re, s, caps, reps);
        }
        cursor = m1;
        if (!many)
            break;
        pos = m1 == m0 ? advance_index(s, m1, uni) : m1;
    }
    budget = rc == RE_RUN_BUDGET;

    if (ok && !budget) {
        ub_units(&sb, s->units + cursor, s->length - cursor);
        if (re->global)
            re->last_index = 0;
        *r = ub_finish(&sb);
        ok = !js_is_undefined(*r);
    } else {
        ub_free(&sb);
    }
    js_gc_unprotect(vm, &repsv);
    js_gc_unprotect(vm, &sv);
    if (thrown)
        return false;
    if (budget)
        return re_throw(ctx, r, re_budget_msg);
    return ok ? true : re_oom(ctx, r);
}

/* ---- split ---- */

bool js_re_str_split(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                     JsValue *r) {
    (void)argc;
    JsVm *vm = ctx->vm;
    JsRegExp *re = (JsRegExp *)js_value_object(args[0]); /* caller-checked */
    JsString *s = js_to_string_cell(ctx, tv, 0);
    if (!s)
        return re_oom(ctx, r);
    JsValue sv = js_value_from_cell(&s->gc);
    js_gc_protect(vm, &sv);

    JsObject *arr = js_array_new_cell(ctx, 0);
    if (!arr) {
        js_gc_unprotect(vm, &sv);
        return re_oom(ctx, r);
    }
    JsValue arrv = js_value_from_cell(&arr->gc);
    js_gc_protect(vm, &arrv);

    int ng = re->prog.group_count;
    int32_t caps[CAPS_MAX];
    bool ok = true, oom = false, budget = false;
    bool uni = re_is_unicode(re);

    if (s->length == 0) {
        /* whole-string match -> []; otherwise [S] */
        int rc = re_run(re, s, 0, true, caps);
        budget = rc == RE_RUN_BUDGET;
        if (rc == RE_RUN_NOMATCH)
            ok = js_array_append(vm, js_value_object(arrv), sv);
    } else {
        uint32_t p = 0, q = 0;
        while (ok && q < s->length) {
            /* anchored at q: the spec's splitter is forced-sticky */
            int rc = re_run(re, s, q, true, caps);
            if (rc == RE_RUN_BUDGET) {
                budget = true;
                break;
            }
            if (rc == RE_RUN_NOMATCH || (uint32_t)caps[1] == p) {
                q = advance_index(s, q, uni);
                continue;
            }
            uint32_t e = (uint32_t)caps[1];
            JsValue piece = substr_or_undef(vm, s, (int32_t)p, (int32_t)q, &oom);
            ok = !oom && js_array_append(vm, js_value_object(arrv), piece);
            for (int g = 1; ok && g <= ng; g++) {
                JsValue cap = substr_or_undef(vm, s, caps[g * 2], caps[g * 2 + 1], &oom);
                ok = !oom && js_array_append(vm, js_value_object(arrv), cap);
            }
            p = e;
            q = e;
        }
        if (ok && !budget) {
            JsValue tail = substr_or_undef(vm, s, (int32_t)p, (int32_t)s->length, &oom);
            ok = !oom && js_array_append(vm, js_value_object(arrv), tail);
        }
    }

    js_gc_unprotect(vm, &arrv);
    js_gc_unprotect(vm, &sv);
    if (budget)
        return re_throw(ctx, r, re_budget_msg);
    if (!ok)
        return re_oom(ctx, r);
    *r = arrv;
    return true;
}

/* ---- registration ---- */

static bool def_method(JsContext *ctx, JsObject *table, const char *name,
                       JsNativeFn fn) {
    JsValue nf = js_native_new(ctx, name, fn, NULL);
    return js_is_function(nf) && js_object_set_ascii(ctx, table, name, nf);
}

bool js_regexp_builtins_init(JsContext *ctx) {
    /* RegExp.prototype: a real object, set before any regexp (literal or
     * constructed) exists — js_regexp_new reads ctx->regexp_proto — and
     * also assigned as the constructor's `.prototype` below. */
    JsValue t = js_object_new(ctx);
    if (!js_is_object(t))
        return false;
    ctx->regexp_proto = js_value_object(t); /* rooted via the context now */
    if (!def_method(ctx, ctx->regexp_proto, "exec", rexp_exec) ||
        !def_method(ctx, ctx->regexp_proto, "test", rexp_test) ||
        !def_method(ctx, ctx->regexp_proto, "toString", rexp_toString))
        return false;
    JsValue ctor = js_native_new(ctx, "RegExp", g_RegExp, NULL);
    if (!js_is_function(ctor) ||
        !js_object_set_ascii(ctx, ctx->regexp_proto, "constructor", ctor) ||
        !js_object_set_ascii(ctx, ctx->globals, "RegExp", ctor))
        return false;
    ((JsNative *)js_value_cell(ctor))->prototype = ctx->regexp_proto;
    return true;
}

#else /* !LAMASSU_HAS_REGEX: keep the translation unit non-empty */
typedef int js_regexp_disabled;
#endif
