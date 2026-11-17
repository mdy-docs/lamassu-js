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
 * Known deviation (documented): the string methods leave lastIndex at 0 (global) / untouched
 * (non-global) rather than emulating every intermediate spec mutation.
 */
#ifdef LAMASSU_HAS_REGEX

#include "js_bytecode.h"
#include "js_error.h"
#include "js_iter.h"
#include "js_regexp.h"

#define ARG(i) ((i) < argc ? args[i] : js_undefined())

/* engine capture buffers: group 0 is the whole match */
#define CAPS_MAX ((MAX_GROUPS + 1) * 2)

static bool re_throw(JsContext *ctx, JsValue *r, const char *msg) {
    *r = js_error_from_ascii(ctx, msg); /* Error object; string on OOM */
    return false;
}

/* A failure's error: what a conversion threw (a toString can run guest code), else out of memory */
static bool re_oom(JsContext *ctx, JsValue *r) {
    if (!js_is_undefined(ctx->pending_error)) {
        *r = js_take_error(ctx);
        return false;
    }
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

/* ---- engine allocator ----
 *
 * Everything the regex engine allocates — compiled bytecode, parser AST
 * nodes, class range/string buffers, the per-match context and its thread
 * arenas — is routed here, so it lands in bytes_live and is refused by
 * heap_limit like every other allocation. Before this existed the engine
 * called libc directly and a guest could hold regex memory outside its
 * configured budget entirely.
 *
 * The handle is installed per-Program (and inherited by every VMContext
 * created for it), never process-wide: two JsVms have two heaps, and each
 * pattern must be charged to the one that owns it.
 *
 * js_realloc_raw needs the OLD size on every call to keep bytes_live by
 * delta, and the engine's hook is malloc-shaped — it does not track one.
 * Each block therefore carries its own charged size in a header written and
 * read in this one function, so the size at free time is by construction
 * the size charged at allocation time and the accounting cannot drift. The
 * header is a max_align_t union so the payload keeps malloc-grade
 * alignment.
 *
 * A NULL return needs no special handling here: the engine already reports
 * allocation failure as a compile error (Program.error) or an abandoned
 * match (vm_context_budget_exhausted), both of which reach the guest as a
 * catchable error.
 */
typedef union {
    size_t charged; /* header + payload: exactly what the VM was told */
    max_align_t align;
} ReBlock;

/*
 * baru-re reports an allocation it could not make the same way it reports
 * an exhausted step budget — through vm_context_budget_exhausted — and the
 * two must not throw alike: a budget is the pattern's fault and a guest may
 * catch it; no memory is the host's, and a guest that caught it as a
 * pattern's would carry on with a wrong answer. The allocator below is
 * ours, so it remembers that it refused, and re_run turns that into the
 * message the throw sites use. Single-threaded, like the VM.
 */
static bool re_alloc_failed;
static bool re_budget_was_oom;

static void *re_vm_realloc(void *ud, void *ptr, size_t size) {
    JsVm *vm = ud;
    ReBlock *h = ptr ? (ReBlock *)ptr - 1 : NULL;
    size_t old = h ? h->charged : 0;
    if (size == 0) {
        js_realloc_raw(vm, h, old, 0);
        return NULL;
    }
    if (size > SIZE_MAX - sizeof(ReBlock)) {
        re_alloc_failed = true;
        return NULL;
    }
    size_t charged = size + sizeof(ReBlock);
    ReBlock *nh = js_realloc_raw(vm, h, old, charged);
    if (!nh) {
        re_alloc_failed = true;
        return NULL; /* the old block, and its header, are untouched */
    }
    nh->charged = charged;
    return nh + 1;
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
        return JS_REGEXP_FLAGS_ERROR;
    Program *prog = js_realloc_raw(vm, NULL, 0, sizeof *prog);
    if (!prog)
        return "out of memory";
    memset(prog, 0, sizeof *prog);
    program_set_allocator(prog, re_vm_realloc, vm);
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
    program_release(prog);
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
        *err = JS_REGEXP_FLAGS_ERROR;
        return js_undefined();
    }
    if (vm->regexp_live >= JS_REGEXP_MAX_LIVE) {
        /*
         * The count falls at sweep, so a script that evaluates regex literals
         * in a loop — every evaluation is a fresh object, per the spec — can
         * be holding the whole cap in garbage. Refusing on that would fail
         * code that is not, in fact, keeping 64 patterns alive: collect first
         * and only refuse if they really are all reachable. Same shape as
         * js_gc_new_cell's collect-and-retry for an allocation that did not
         * fit, and safe for the same reason — this function is about to
         * allocate anyway, so every caller already tolerates a collection
         * here.
         */
        js_gc_collect(vm);
        if (vm->regexp_live >= JS_REGEXP_MAX_LIVE) {
            *err = "RangeError: too many live regular expressions";
            return js_undefined();
        }
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
    re->obj.obj_flags = 0;
    js_map_init(&re->obj.props);
    re->obj.elems = NULL;
    re->obj.elem_count = re->obj.elem_cap = 0;
    re->obj.proto = ctx->regexp_proto ? js_value_from_cell(&ctx->regexp_proto->gc) : js_undefined();
    re->source = NULL;
    re->flags = NULL;
    re->last_index = 0;
    re->global = (mask & REGEX_FLAG_GLOBAL) != 0;
    re->vctx = NULL; /* built on first match, reused after — see js_regexp.h */
    memset(&re->prog, 0, sizeof re->prog); /* compile_into requires zero init */
    program_set_allocator(&re->prog, re_vm_realloc, vm);
    vm->regexp_live++; /* balanced by js_regexp_release at sweep */

    JsValue rev = js_value_from_cell(c);
    js_gc_protect(vm, &rev);
    compile_into(&re->prog, pbuf, mask);
    js_realloc_raw(vm, pbuf, ((size_t)pat_len + 1) * sizeof(uint16_t), 0);
    if (re->prog.error) {
        /* the cell stays on the GC list and is swept as a normal regexp;
         * release() frees whatever buffers the failed compile left */
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
    vm_context_free(re->vctx);
    program_release(&re->prog);
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
        /* converted as it is set (V8 keeps the value and converts it at the next match) */
        *handled = true;
        if (js_op_to_number(ctx, val, &re->last_index))
            return true;
        *errmsg = NULL; /* not a refused write: the conversion threw (pending) */
        return false;
    }
    if (key_is(key, "source") || key_is(key, "flags") || key_is(key, "global") ||
        key_is(key, "ignoreCase") || key_is(key, "multiline") || key_is(key, "dotAll") ||
        key_is(key, "sticky") || key_is(key, "unicode") || key_is(key, "unicodeSets") ||
        key_is(key, "hasIndices")) {
        *errmsg = JS_REGEXP_GETTER_ONLY;
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

/* re_run results: a tri-state so a match the engine ABANDONED is
 * distinguishable from an ordinary no-match and can be thrown as a
 * catchable RangeError. Abandonment is almost always the step budget; an
 * engine-side allocation failure during matching lands here too, and is
 * reported with the same message rather than earning a fourth state, since
 * guest JS can do nothing different with the distinction. */
#define RE_RUN_MATCH 1
#define RE_RUN_NOMATCH 0
#define RE_RUN_BUDGET (-1)

static const char *const re_budget_msg =
    "RangeError: regular expression step budget exhausted";

/* What a RE_RUN_BUDGET throws: the budget, or the out-of-memory it stood for. */
static bool re_budget_throw(JsContext *ctx, JsValue *r) {
    bool oom = re_budget_was_oom;
    re_budget_was_oom = false;
    return oom ? re_oom(ctx, r) : re_throw(ctx, r, re_budget_msg);
}

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

    VMContext *vctx = re->vctx;
    if (!vctx) {
        vctx = vm_context_new(prog);
        if (!vctx) {
            re_alloc_failed = false;
            re_budget_was_oom = true;
            return RE_RUN_BUDGET; /* OOM: the match was never evaluated, so it
                                   * must throw, not report a bogus non-match */
        }
        re->vctx = vctx;
    }
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
    if (exhausted)
        re_budget_was_oom = re_alloc_failed;
    re_alloc_failed = false;
    if (exhausted) {
        /* Exhaustion also reports a match-time OOM, which can leave a depth
         * level half-initialised; baru-re's contract is that such a context
         * must never enter the VM again. Cheaper to drop it than to tell the
         * two apart, and both are rare. */
        vm_context_free(vctx);
        re->vctx = NULL;
    }
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

/* The last successful match, for RegExp's legacy statics (as V8's RegExpBuiltinExec records it) */
static void re_record(JsContext *ctx, const JsRegExp *re, JsString *s, const int32_t *caps) {
    ctx->re_subject = s;
    ctx->re_input = js_value_from_cell(&s->gc);
    int groups = re->prog.group_count;
    for (int g = 0; g < 10; g++) {
        ctx->re_caps[2 * g] = g <= groups ? caps[2 * g] : -1;
        ctx->re_caps[2 * g + 1] = g <= groups ? caps[2 * g + 1] : -1;
    }
    ctx->re_last_paren[0] = groups > 0 ? caps[2 * groups] : -1;
    ctx->re_last_paren[1] = groups > 0 ? caps[2 * groups + 1] : -1;
}

/* re_run, recording a match for the legacy statics */
static int re_match(JsContext *ctx, JsRegExp *re, JsString *s, uint32_t start, bool anchored,
                    int32_t *caps) {
    int rc = re_run(re, s, start, anchored, caps);
    if (rc == RE_RUN_MATCH)
        re_record(ctx, re, s, caps);
    return rc;
}

/*
 * exec's lastIndex protocol (also used by test and non-global match):
 * lastIndex is consulted and updated only when the pattern is global or
 * sticky. Returns an RE_RUN_* result with caps filled on a match; a budget
 * result leaves lastIndex untouched (the caller throws).
 */
static int exec_protocol(JsContext *ctx, JsRegExp *re, JsString *s, int32_t *caps) {
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
    int rc = re_match(ctx, re, s, start, false, caps);
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
    js_value_object(obj)->proto = js_undefined(); /* no prototype, as V8: a group may be named anything */
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

JsValue js_regexp_error(JsContext *ctx, const char *err, const uint16_t *pat, uint32_t pat_len,
                        const uint16_t *flags, uint32_t flags_len) {
    JsVm *vm = ctx->vm;
    bool is_flags = strcmp(err, JS_REGEXP_FLAGS_ERROR) == 0;
    if (!is_flags && strncmp(err, "SyntaxError: ", 13) != 0)
        return js_error_from_ascii(ctx, err);
    /* the texts become strings for %v; the caller holds what they point into */
    JsString *ps = js_string_cell_new(vm, pat, pat_len);
    JsValue pv = ps ? js_value_from_cell(&ps->gc) : js_undefined();
    js_gc_protect(vm, &pv);
    JsString *fs = js_string_cell_new(vm, flags, flags_len);
    JsValue fv = fs ? js_value_from_cell(&fs->gc) : js_undefined();
    js_gc_protect(vm, &fv);
    JsValue e;
    if (!ps || !fs)
        e = js_oom_value(vm);
    else if (is_flags)
        e = js_error_fmt(ctx, "SyntaxError: Invalid flags supplied to RegExp constructor '%v'", fv);
    else
        e = js_error_fmt(ctx, "SyntaxError: Invalid regular expression: /%v/%v: %s", pv, fv, err + 13);
    js_gc_unprotect(vm, &fv);
    js_gc_unprotect(vm, &pv);
    return e;
}

/* ---- RegExp methods ---- */

static bool rexp_exec(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                      JsValue *r) {
    if (!js_regexp_is(tv))
        return (*r = js_error_receiver(ctx, "RegExp", tv)), false;
    JsRegExp *re = (JsRegExp *)js_value_object(tv);
    JsString *s = js_op_to_string(ctx, ARG(0));
    if (!s)
        return re_oom(ctx, r);
    JsValue sv = js_value_from_cell(&s->gc);
    js_gc_protect(ctx->vm, &sv);
    int32_t caps[CAPS_MAX];
    bool ok = true;
    int rc = exec_protocol(ctx, re, s, caps);
    if (rc == RE_RUN_BUDGET) {
        js_gc_unprotect(ctx->vm, &sv);
        return re_budget_throw(ctx, r);
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
        return (*r = js_error_receiver(ctx, "RegExp", tv)), false;
    JsRegExp *re = (JsRegExp *)js_value_object(tv);
    JsString *s = js_op_to_string(ctx, ARG(0));
    if (!s)
        return re_oom(ctx, r);
    /* Rooted across the match like every other subject here: matching now
     * allocates through the VM, so it is a GC safe point, and a coerced
     * subject (test(123)) is a fresh cell nothing else references. */
    JsValue sv = js_value_from_cell(&s->gc);
    js_gc_protect(ctx->vm, &sv);
    int32_t caps[CAPS_MAX];
    int rc = exec_protocol(ctx, re, s, caps);
    js_gc_unprotect(ctx->vm, &sv);
    if (rc == RE_RUN_BUDGET)
        return re_budget_throw(ctx, r);
    *r = js_bool(rc == RE_RUN_MATCH);
    return true;
}

/* RegExp.prototype.toString on any other object (22.2.6.17), as V8: "/" + ToString(source) +
 * "/" + ToString(flags), read from it */
static bool regexp_generic_string(JsContext *ctx, JsValue tv, JsValue *r) {
    JsVm *vm = ctx->vm;
    static const uint16_t ks[] = {'s', 'o', 'u', 'r', 'c', 'e'}, kf[] = {'f', 'l', 'a', 'g', 's'};
    JsValue parts[2] = {js_undefined(), js_undefined()};
    js_gc_protect(vm, &parts[0]);
    js_gc_protect(vm, &parts[1]);
    bool ok = true;
    for (int i = 0; i < 2 && ok; i++) {
        JsValue key = i == 0 ? js_atom(vm, ks, 6) : js_atom(vm, kf, 5), v;
        JsString *str = NULL;
        ok = js_is_string(key) && js_get_property(ctx, tv, key, &v) && (str = js_op_to_string(ctx, v));
        if (ok)
            parts[i] = js_value_from_cell(&str->gc);
    }
    JsString *out = NULL;
    if (ok) {
        const JsString *a = js_value_string(parts[0]), *b = js_value_string(parts[1]);
        size_t n = (size_t)a->length + b->length + 2;
        uint16_t *buf = n <= JS_MAX_STRING_UNITS ? js_realloc_raw(vm, NULL, 0, n * sizeof(uint16_t)) : NULL;
        if (buf) {
            buf[0] = '/';
            memcpy(buf + 1, a->units, a->length * sizeof(uint16_t));
            buf[1 + a->length] = '/';
            memcpy(buf + 2 + a->length, b->units, b->length * sizeof(uint16_t));
            out = js_string_cell_new(vm, buf, n);
            js_realloc_raw(vm, buf, n * sizeof(uint16_t), 0);
        }
    }
    js_gc_unprotect(vm, &parts[1]);
    js_gc_unprotect(vm, &parts[0]);
    if (!out)
        return re_oom(ctx, r);
    *r = js_value_from_cell(&out->gc);
    return true;
}

static bool rexp_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                          JsValue *r) {
    (void)args;
    (void)argc;
    if (!js_regexp_is(tv) && (js_is_object(tv) || js_is_function(tv) || js_is_promise(tv)))
        return regexp_generic_string(ctx, tv, r);
    if (!js_regexp_is(tv))
        return (*r = js_error_receiver(ctx, "RegExp", tv)), false;
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
        JsString *ps = js_op_to_string(ctx, pv);
        if (!ps) {
            ok = false;
        } else {
            pstr = js_value_from_cell(&ps->gc);
            pu = ps->units;
            pn = ps->length;
        }
    }
    if (ok && !js_is_undefined(fv)) {
        JsString *fs = js_op_to_string(ctx, fv);
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
    if (!js_is_object(rev)) {
        *r = js_regexp_error(ctx, err ? err : "out of memory", pu, pn, fu, fn);
        return false;
    }
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
        JsString *ps = js_op_to_string(ctx, pv);
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
    if (!js_is_object(*rev)) {
        *r = js_regexp_error(ctx, err ? err : "out of memory", pu, pn, fbuf, fn);
        js_gc_unprotect(vm, &pstr);
        return false;
    }
    js_gc_unprotect(vm, &pstr);
    *out = (JsRegExp *)js_value_object(*rev);
    return true;
}

bool js_re_str_match(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                     JsValue *r) {
    if (!js_str_this_check(ctx, tv, r))
        return false;
    JsVm *vm = ctx->vm;
    JsString *s = js_op_to_string(ctx, tv);
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
        int rc = exec_protocol(ctx, re, s, caps);
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
                   (rc = re_match(ctx, re, s, pos, false, caps)) == RE_RUN_MATCH) {
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
        return re_budget_throw(ctx, r);
    return ok ? true : re_oom(ctx, r);
}

/* matchAll: a RegExp String Iterator (js_iter.h) that runs one search per step, from the
 * regexp's lastIndex (ToLength) as V8's clone of it starts; the regexp's own lastIndex is left
 * alone. Its matches are exec's results. */
bool js_re_str_matchAll(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                        JsValue *r) {
    if (!js_str_this_check(ctx, tv, r))
        return false;
    JsVm *vm = ctx->vm;
    if (js_regexp_is(ARG(0)) && !((JsRegExp *)js_value_object(args[0]))->global)
        return re_throw(ctx, r, "TypeError: String.prototype.matchAll called with a non-global RegExp argument");
    JsString *s = js_op_to_string(ctx, tv);
    if (!s)
        return re_oom(ctx, r);
    JsValue sv = js_value_from_cell(&s->gc);
    js_gc_protect(vm, &sv);
    JsValue rev = js_undefined();
    js_gc_protect(vm, &rev);
    JsRegExp *re;
    bool ok = coerce_regexp(ctx, ARG(0), "g", &rev, &re, r);
    if (ok) {
        double li = js_regexp_is(ARG(0)) ? re->last_index : 0;
        uint32_t start = li != li || li <= 0 ? 0
                         : li > (double)s->length ? s->length + 1 : (uint32_t)li;
        *r = js_iterobj_new(ctx, JS_ITER_REGEXP, js_value_from_cell(&re->obj.gc), sv, start);
        if (js_is_undefined(*r))
            ok = re_oom(ctx, r);
    }
    js_gc_unprotect(vm, &rev);
    js_gc_unprotect(vm, &sv);
    return ok;
}

int js_re_iter_step(JsContext *ctx, JsValue rev, JsValue sv, uint32_t *pos, JsValue *out) {
    JsRegExp *re = (JsRegExp *)js_value_object(rev);
    JsString *s = js_value_string(sv);
    if (*pos > s->length)
        return 0;
    int32_t caps[CAPS_MAX];
    int rc = re_match(ctx, re, s, *pos, false, caps);
    if (rc == RE_RUN_BUDGET)
        return re_budget_throw(ctx, out), -1;
    if (rc == RE_RUN_NOMATCH)
        return 0;
    uint32_t next = caps[1] == caps[0] ? advance_index(s, (uint32_t)caps[1], re_is_unicode(re))
                                       : (uint32_t)caps[1];
    if (!build_exec_result(ctx, re, sv, caps, out))
        return re_oom(ctx, out), -1;
    *pos = next;
    return 1;
}

bool js_re_str_search(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                      JsValue *r) {
    if (!js_str_this_check(ctx, tv, r))
        return false;
    JsVm *vm = ctx->vm;
    JsString *s = js_op_to_string(ctx, tv);
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
    int rc = re_match(ctx, re, s, 0, false, caps);
    js_gc_unprotect(vm, &rev);
    js_gc_unprotect(vm, &sv);
    if (rc == RE_RUN_BUDGET)
        return re_budget_throw(ctx, r);
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
        return re_throw(ctx, r, "TypeError: String.prototype.replaceAll called with a non-global RegExp argument");
    JsString *s = js_op_to_string(ctx, tv);
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
        reps = js_op_to_string(ctx, repfn);
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

    while (pos <= s->length && (rc = re_match(ctx, re, s, pos, false, caps)) == RE_RUN_MATCH) {
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
            JsString *rs = js_op_to_string(ctx, res);
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
        return re_budget_throw(ctx, r);
    return ok ? true : re_oom(ctx, r);
}

/* ---- split ---- */

bool js_re_str_split(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                     JsValue *r) {
    JsVm *vm = ctx->vm;
    JsRegExp *re = (JsRegExp *)js_value_object(args[0]); /* caller-checked */
    JsString *s = js_op_to_string(ctx, tv);
    if (!s)
        return re_oom(ctx, r);
    JsValue sv = js_value_from_cell(&s->gc);
    js_gc_protect(vm, &sv);
    uint32_t lim; /* after the string, as RegExp.prototype[@@split] converts them */
    if (!js_split_limit(ctx, ARG(1), &lim)) {
        js_gc_unprotect(vm, &sv);
        return re_oom(ctx, r);
    }

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

    if (lim == 0) {
        /* [] */
    } else if (s->length == 0) {
        /* whole-string match -> []; otherwise [S] */
        int rc = re_match(ctx, re, s, 0, true, caps);
        budget = rc == RE_RUN_BUDGET;
        if (rc == RE_RUN_NOMATCH)
            ok = js_array_append(vm, js_value_object(arrv), sv);
    } else {
        uint32_t p = 0, q = 0;
        while (ok && q < s->length) {
            /* anchored at q: the spec's splitter is forced-sticky */
            int rc = re_match(ctx, re, s, q, true, caps);
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
            if (ok && js_value_object(arrv)->elem_count == lim)
                break;
            for (int g = 1; ok && g <= ng; g++) {
                JsValue cap = substr_or_undef(vm, s, caps[g * 2], caps[g * 2 + 1], &oom);
                ok = !oom && js_array_append(vm, js_value_object(arrv), cap);
                if (ok && js_value_object(arrv)->elem_count == lim)
                    break;
            }
            if (js_value_object(arrv)->elem_count == lim)
                break;
            p = e;
            q = e;
        }
        if (ok && !budget && js_value_object(arrv)->elem_count < lim) {
            JsValue tail = substr_or_undef(vm, s, (int32_t)p, (int32_t)s->length, &oom);
            ok = !oom && js_array_append(vm, js_value_object(arrv), tail);
        }
    }

    js_gc_unprotect(vm, &arrv);
    js_gc_unprotect(vm, &sv);
    if (budget)
        return re_budget_throw(ctx, r);
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

/* RegExp.escape(s) (ES2025 22.2.5.1): S with every character a pattern could read as syntax
 * escaped, and a leading digit or ASCII letter as \xHH */
static bool re_escape_ws(uint32_t c) {
    return c == 0x09 || c == 0x0B || c == 0x0C || c == 0x20 || c == 0xA0 || c == 0xFEFF ||
           c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000 ||
           c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029;
}

static bool re_escape(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue v = argc > 0 ? args[0] : js_undefined();
    if (!js_is_string(v)) {
        *r = js_error_fmt(ctx, "TypeError: input argument must be a string");
        return false;
    }
    const JsString *s = js_value_string(v);
    uint16_t *buf = js_realloc_raw(ctx->vm, NULL, 0, (size_t)s->length * 12 + 1);
    if (!buf && s->length)
        return re_oom(ctx, r);
    size_t n = 0;
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < s->length; i++) {
        uint32_t c = s->units[i];
        bool lone = false;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s->length && s->units[i + 1] >= 0xDC00 &&
            s->units[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s->units[i + 1] - 0xDC00);
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            lone = true;
        }
        bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (n == 0 && alnum) {
            buf[n++] = '\\';
            buf[n++] = 'x';
            buf[n++] = (uint16_t)hex[c >> 4];
            buf[n++] = (uint16_t)hex[c & 15];
        } else if (c < 0x80 && strchr("^$\\.*+?()[]{}|/", (int)c) && c) {
            buf[n++] = '\\';
            buf[n++] = (uint16_t)c;
        } else if (c >= 0x09 && c <= 0x0D) {
            buf[n++] = '\\';
            buf[n++] = (uint16_t)"tnvfr"[c - 0x09];
        } else if ((c < 0x80 && c && strchr(",-=<>#&!%:;@~'`\"", (int)c)) || re_escape_ws(c) || lone) {
            if (c <= 0xFF) {
                buf[n++] = '\\';
                buf[n++] = 'x';
                buf[n++] = (uint16_t)hex[c >> 4];
                buf[n++] = (uint16_t)hex[c & 15];
            } else {
                buf[n++] = '\\';
                buf[n++] = 'u';
                for (int sh = 12; sh >= 0; sh -= 4)
                    buf[n++] = (uint16_t)hex[(c >> sh) & 15];
            }
        } else if (c > 0xFFFF) {
            buf[n++] = s->units[i];
            buf[n++] = s->units[++i];
        } else {
            buf[n++] = (uint16_t)c;
        }
    }
    JsValue out = js_string_new(ctx->vm, buf, n);
    js_realloc_raw(ctx->vm, buf, (size_t)s->length * 12 + 1, 0);
    if (!js_is_string(out))
        return re_oom(ctx, r);
    *r = out;
    return true;
}

/* ---- RegExp's legacy statics (Annex B, as V8 has them) ----
 *
 * RegExp.input ($_), lastMatch ($&), lastParen ($+), leftContext ($`), rightContext ($'), $1-$9:
 * V8's accessors on the constructor, read from the last successful match (re_record). There are
 * no accessors here, so reads, writes and deletes of these names on the RegExp constructor are
 * answered by these functions: input takes a string, the others ignore a write, as V8's setters
 * do; each is configurable. */

static const char *const legacy_names[JS_REGEXP_LEGACY_COUNT] = {
    "input", "$_", "lastMatch", "$&", "lastParen", "$+", "leftContext", "$`", "rightContext", "$'",
    "$1", "$2", "$3", "$4", "$5", "$6", "$7", "$8", "$9"};

int js_regexp_legacy_index(JsContext *ctx, JsValue key) {
    if (!js_is_string(key))
        return -1;
    const JsString *k = js_value_string(key);
    if (k->length < 2 || k->length > 12 || (k->units[0] != '$' && k->units[0] != 'i' && k->units[0] != 'l' &&
                                            k->units[0] != 'r'))
        return -1;
    for (int i = 0; i < JS_REGEXP_LEGACY_COUNT; i++)
        if (key_is(key, legacy_names[i]))
            return (ctx->re_legacy_deleted & (1u << i)) ? -1 : i;
    return -1;
}

bool js_regexp_legacy_get(JsContext *ctx, int idx, JsValue *out) {
    if (idx <= 1) {
        if (!js_is_undefined(ctx->re_input)) {
            *out = ctx->re_input;
            return true;
        }
    }
    const JsString *s = ctx->re_subject;
    int32_t a = -1, b = -1;
    if (s && idx > 1) {
        if (idx <= 3) {
            a = ctx->re_caps[0];
            b = ctx->re_caps[1];
        } else if (idx <= 5) {
            a = ctx->re_last_paren[0];
            b = ctx->re_last_paren[1];
        } else if (idx <= 7) {
            a = 0;
            b = ctx->re_caps[0];
        } else if (idx <= 9) {
            a = ctx->re_caps[1];
            b = (int32_t)s->length;
        } else {
            a = ctx->re_caps[2 * (idx - 9)];
            b = ctx->re_caps[2 * (idx - 9) + 1];
        }
    }
    JsString *r = a >= 0 && b >= a ? js_string_cell_new(ctx->vm, s->units + a, (uint32_t)(b - a))
                                   : js_ascii_cell(ctx->vm, "");
    if (!r)
        return false;
    *out = js_value_from_cell(&r->gc);
    return true;
}

bool js_regexp_legacy_set(JsContext *ctx, int idx, JsValue v) {
    if (idx > 1)
        return true; /* V8's setter does nothing */
    JsString *str = js_op_to_string(ctx, v);
    if (!str)
        return false;
    ctx->re_input = js_value_from_cell(&str->gc);
    return true;
}

void js_regexp_legacy_delete(JsContext *ctx, int idx) {
    ctx->re_legacy_deleted |= 1u << idx;
}

bool js_regexp_legacy_names(JsContext *ctx, JsObject *out) {
    for (int i = 0; i < JS_REGEXP_LEGACY_COUNT; i++) {
        if (ctx->re_legacy_deleted & (1u << i))
            continue;
        JsString *k = js_ascii_cell(ctx->vm, legacy_names[i]);
        if (!k || !js_array_append(ctx->vm, out, js_value_from_cell(&k->gc)))
            return false;
    }
    return true;
}

/* re.compile(pattern, flags): this regexp recompiled in place (Annex B), lastIndex 0 */
static bool rexp_compile(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (!js_regexp_is(tv))
        return (*r = js_error_receiver(ctx, "RegExp", tv)), false;
    JsValue pv = ARG(0), fv = ARG(1);
    if (js_regexp_is(pv) && !js_is_undefined(fv))
        return re_throw(ctx, r, "TypeError: Cannot supply flags when constructing one RegExp from another");
    JsValue rev, cargs[2] = {pv, fv};
    js_gc_protect(ctx->vm, &tv);
    bool ok = g_RegExp(ctx, js_undefined(), cargs, js_regexp_is(pv) ? 1 : 2, &rev);
    if (ok) {
        /* swap the compiled pattern in: the new cell, holding the old one, goes to the collector */
        JsRegExp *re = (JsRegExp *)js_value_object(tv), *nre = (JsRegExp *)js_value_object(rev);
        JsRegExp tmp = *re;
        re->source = nre->source;
        re->flags = nre->flags;
        re->global = nre->global;
        re->vctx = nre->vctx;
        re->prog = nre->prog;
        nre->source = tmp.source;
        nre->flags = tmp.flags;
        nre->global = tmp.global;
        nre->vctx = tmp.vctx;
        nre->prog = tmp.prog;
        static const uint16_t kli[] = {'l', 'a', 's', 't', 'I', 'n', 'd', 'e', 'x'};
        JsValue key = js_atom(ctx->vm, kli, 9);
        ok = js_is_string(key) && js_set_property(ctx, tv, key, js_number(0));
        if (!ok)
            rev = js_is_undefined(ctx->pending_error) ? js_error_from_ascii(ctx, "out of memory") : js_take_error(ctx);
    }
    js_gc_unprotect(ctx->vm, &tv);
    *r = ok ? tv : rev;
    return ok;
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
        !def_method(ctx, ctx->regexp_proto, "toString", rexp_toString) ||
        !def_method(ctx, ctx->regexp_proto, "compile", rexp_compile))
        return false;
    JsValue ctor = js_native_new(ctx, "RegExp", g_RegExp, NULL);
    if (!js_is_function(ctor) ||
        !js_object_set_ascii(ctx, ctx->regexp_proto, "constructor", ctor) ||
        !js_object_set_ascii(ctx, ctx->globals, "RegExp", ctor))
        return false;
    js_native_builtin_ctor(ctor, ctx->regexp_proto);
    ctx->regexp_ctor = (JsNative *)js_value_cell(ctor); /* reachable from the globals */
    JsValue st = js_object_new(ctx);
    if (!js_is_object(st))
        return false;
    ((JsNative *)js_value_cell(ctor))->statics = js_value_object(st);
    return def_method(ctx, js_value_object(st), "escape", re_escape);
}

#else /* !LAMASSU_HAS_REGEX: keep the translation unit non-empty */
typedef int js_regexp_disabled;
#endif
