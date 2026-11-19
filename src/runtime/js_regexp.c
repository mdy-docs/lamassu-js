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
 * The methods a script can redirect - through exec, flags, constructor, lastIndex - are written
 * twice: as the spec's steps over properties, and as a fast path that gives the same result on
 * a regexp none of those is redefined for ("the prototype chain", below).
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
    re->last_index = js_number(0);
    re->last_index_ro = false;
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
    js_gc_mark_value(vm, re->last_index);
}

size_t js_regexp_release(JsVm *vm, JsObject *o) {
    JsRegExp *re = (JsRegExp *)o;
    vm_context_free(re->vctx);
    program_release(&re->prog);
    vm->regexp_live--;
    return sizeof *re;
}

/* ---- properties ---- */

/* lastIndex, a regexp's one own property (a data property kept in the cell, not in its map) */
bool js_regexp_prop_get(JsContext *ctx, JsObject *o, JsValue key, JsValue *out,
                        bool *handled) {
    (void)ctx;
    JsRegExp *re = (JsRegExp *)o;
    *handled = key_is(key, "lastIndex");
    if (*handled)
        *out = re->last_index;
    return true;
}

/* Any value: a match converts it (ToLength), not the write */
bool js_regexp_prop_set(JsObject *o, JsValue key, JsValue val) {
    if (!key_is(key, "lastIndex"))
        return false;
    ((JsRegExp *)o)->last_index = val;
    return true;
}

void js_regexp_define_last_index(JsObject *o, JsValue val, bool readonly) {
    JsRegExp *re = (JsRegExp *)o;
    re->last_index = val;
    if (readonly && !re->last_index_ro) {
        re->last_index_ro = true;
        o->obj_flags |= JS_OBJ_ATTRS; /* so that a write to it, or through an inheritor, looks */
    }
}

static bool re_legacy_get(JsContext *ctx, int idx, JsValue *out);
static bool re_legacy_set(JsContext *ctx, int idx, JsValue v);

/* RegExp.prototype's accessors (dotAll, flags, global, hasIndices, ignoreCase, multiline, source,
 * sticky, unicode, unicodeSets: getters only) and the RegExp constructor's (the legacy statics:
 * a getter and a setter each), for js_accessor_call. A flag's getter answers for a regexp and,
 * as the spec has it, for RegExp.prototype itself (undefined; "(?:)" for source), and throws
 * for anything else; `flags` reads the others from its receiver, whatever object that is, so a
 * regexp whose `global` a script redefined shows it in `flags` too. */
bool js_regexp_accessor(JsContext *ctx, unsigned id, JsValue recv, JsValue arg, JsValue *out) {
    static const char *const names[] = {"dotAll", "flags", "global", "hasIndices", "ignoreCase",
                                        "multiline", "source", "sticky", "unicode", "unicodeSets"};
    *out = js_undefined();
    if (id >= JS_ACC_RE_LEGACY_SET) {
        if (re_legacy_set(ctx, (int)(id - JS_ACC_RE_LEGACY_SET), arg))
            return true;
        return re_oom(ctx, out);
    }
    if (id >= JS_ACC_RE_LEGACY_GET)
        return re_legacy_get(ctx, (int)(id - JS_ACC_RE_LEGACY_GET), out) || re_oom(ctx, out);
    if (id == JS_ACC_RE_FLAGS) {
        if (!js_is_object(recv) && !js_is_function(recv) && !js_is_promise(recv)) {
            *out = js_error_fmt(ctx, "TypeError: RegExp.prototype.flags getter called on non-object %v", recv);
            return false;
        }
        /* the order the spec reads them in, which is the order of the letters */
        static const char *const order[] = {"hasIndices", "global", "ignoreCase", "multiline",
                                            "dotAll", "unicode", "unicodeSets", "sticky"};
        static const char letters[] = "dgimsuvy";
        uint16_t buf[8];
        uint32_t n = 0;
        bool ok = true;
        js_gc_protect(ctx->vm, &recv);
        for (int i = 0; ok && i < 8; i++) {
            JsString *k = js_ascii_cell(ctx->vm, order[i]);
            JsValue v;
            ok = k && js_get_property(ctx, recv, js_value_from_cell(&k->gc), &v);
            if (ok && js_to_boolean(v))
                buf[n++] = (uint16_t)letters[i];
        }
        js_gc_unprotect(ctx->vm, &recv);
        JsValue sv = ok ? js_string_new(ctx->vm, buf, n) : js_undefined();
        if (!js_is_string(sv))
            return re_oom(ctx, out);
        *out = sv;
        return true;
    }
    if (!js_regexp_is(recv)) {
        if (js_is_object(recv) && js_value_object(recv) == ctx->regexp_proto) {
            if (id != JS_ACC_RE_SOURCE)
                return true;
            JsString *e = js_ascii_cell(ctx->vm, "(?:)");
            if (!e)
                return re_oom(ctx, out);
            *out = js_value_from_cell(&e->gc);
            return true;
        }
        *out = js_error_fmt(ctx, "TypeError: RegExp.prototype.%s getter called on non-RegExp object",
                            names[id - JS_ACC_RE_DOTALL]);
        return false;
    }
    JsRegExp *re = (JsRegExp *)js_value_object(recv);
    switch (id) {
    case JS_ACC_RE_DOTALL: *out = js_bool(re->prog.dot_all); break;
    case JS_ACC_RE_GLOBAL: *out = js_bool(re->global); break;
    case JS_ACC_RE_HASINDICES: *out = js_bool(re->prog.has_indices); break;
    case JS_ACC_RE_IGNORECASE: *out = js_bool(re->prog.ignore_case); break;
    case JS_ACC_RE_MULTILINE: *out = js_bool(re->prog.multiline); break;
    case JS_ACC_RE_SOURCE: *out = js_value_from_cell(&re->source->gc); break;
    case JS_ACC_RE_STICKY: *out = js_bool(re->prog.sticky); break;
    case JS_ACC_RE_UNICODE: *out = js_bool(re->prog.unicode && !re->prog.unicode_sets); break;
    default: *out = js_bool(re->prog.unicode_sets); break;
    }
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
#define RE_RUN_READONLY (-2) /* exec_protocol: lastIndex cannot be written (a frozen regexp) */

static const char *const re_budget_msg =
    "RangeError: regular expression step budget exhausted";

/* What a RE_RUN_BUDGET throws: the fuel budget's or the interrupt's error when one is pending (the
 * match was charged for, or stopped), else the step budget, or the out-of-memory it stood for. */
static bool re_budget_throw(JsContext *ctx, JsValue *r) {
    bool oom = re_budget_was_oom;
    re_budget_was_oom = false;
    if (!js_is_undefined(ctx->pending_error))
        return (*r = js_take_error(ctx)), false;
    return oom ? re_oom(ctx, r) : re_throw(ctx, r, re_budget_msg);
}

/*
 * Runs the compiled pattern against s. anchored=true tries only at `start`
 * (split's forced-sticky semantics); otherwise sticky patterns anchor
 * themselves and the scan loop advances by code point under u/v. On a match
 * caps[] holds (group_count+1)*2 unit offsets, -1 for an unmatched group.
 * Every call gets a fresh engine context with a step budget scaled to the
 * subject, so catastrophic backtracking returns RE_RUN_BUDGET instead of
 * hanging the VM. The steps are paid for in fuel (JS_REGEXP_STEPS_PER_FUEL):
 * the step budget is no more than the fuel left buys, the engine polls the
 * interrupt flag, and what the match spent is charged; a budget spent or an
 * interrupt is RE_RUN_BUDGET with that error pending.
 */
static int re_run(JsContext *ctx, JsRegExp *re, const JsString *s, uint32_t start, bool anchored,
                  int32_t *caps) {
    Program *prog = &re->prog;
    const uint16_t *text = s->units;
    const uint16_t *text_end = text + s->length;
    const uint16_t *captures[CAPS_MAX] = {0};
    bool matched = false, charge_failed = false;

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
    uint64_t cap = JS_REGEXP_STEP_BASE + (uint64_t)s->length * JS_REGEXP_STEPS_PER_UNIT;
    uint64_t fuel = js_fuel_remaining(ctx);
    uint64_t afford = fuel > UINT64_MAX / JS_REGEXP_STEPS_PER_FUEL ? UINT64_MAX
                                                                   : fuel * JS_REGEXP_STEPS_PER_FUEL;
    bool fuel_bound = afford < cap; /* the budget, not the subject, sets how far it may go */
    vm_context_set_step_budget(vctx, fuel_bound ? afford : cap);
    _Static_assert(sizeof(JsInterruptFlag) == sizeof(int), "the engine polls an int flag");
    vm_context_set_interrupt(vctx, (const volatile int *)&ctx->vm->interrupt);
    if (anchored || prog->sticky) {
        matched = vm_execute(prog, vctx, 0, 1, text, text_end, text + start, captures);
    } else {
        bool uni = re_is_unicode(re);
        uint32_t tried = 0; /* start positions not yet charged */
        for (uint32_t i = start; i <= s->length;) {
            matched = vm_execute(prog, vctx, 0, 1, text, text_end, text + i, captures);
            if (matched || vm_context_budget_exhausted(vctx))
                break;
            i = advance_index(s, i, uni);
            if (++tried == 4096) { /* a batch at a time, which polls the interrupt too */
                tried = 0;
                if (!js_native_charge_n(ctx, 4096 * JS_REGEXP_POSITION_FUEL)) {
                    charge_failed = true;
                    break;
                }
            }
        }
        if (!charge_failed && !js_native_charge_n(ctx, (uint64_t)tried * JS_REGEXP_POSITION_FUEL))
            charge_failed = true;
    }
    bool exhausted = vm_context_budget_exhausted(vctx) || charge_failed;
    bool stopped = vm_context_interrupted(vctx) || (exhausted && fuel_bound && !re_alloc_failed);
    /* what it spent, or (stopped) all there was: the error is pending when that is too much */
    if (charge_failed ||
        !js_native_charge_n(ctx, stopped ? UINT64_MAX
                                         : JS_REGEXP_CALL_FUEL + vm_context_steps_used(vctx) / JS_REGEXP_STEPS_PER_FUEL)) {
        matched = false;
        exhausted = true;
    }
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
    int rc = re_run(ctx, re, s, start, anchored, caps);
    if (rc == RE_RUN_MATCH)
        re_record(ctx, re, s, caps);
    return rc;
}

/* Where a match from lastIndex I starts. A unicode pattern's input is code points, and an index
 * into the middle of one is that code point's (RegExpBuiltinExec's inputIndex): /./uy with
 * lastIndex 1 matches all of a pair at 0. */
static uint32_t unicode_start(const JsString *s, uint32_t i, bool uni) {
    if (uni && i > 0 && i < s->length && is_trail(s->units[i]) && is_lead(s->units[i - 1]))
        return i - 1;
    return i;
}

/* lastIndex when it is a number, which is all a fast path takes (re_pristine): anything else is
 * converted by exec, where a script's code may run */
static double li_number(const JsRegExp *re) {
    return js_get_number(re->last_index);
}

/*
 * exec's lastIndex protocol (RegExpBuiltinExec; also test, and match and replace on a regexp
 * that is not global), from LI, the number lastIndex is (li_number, or the caller's conversion
 * of it): it is consulted and updated only when the pattern is global or sticky. Returns an RE_RUN_* result with caps filled on a match; a budget result leaves
 * lastIndex untouched (the caller throws), and RE_RUN_READONLY is for the write to it a frozen
 * regexp refuses, at the point exec would make it.
 */
static int exec_protocol(JsContext *ctx, JsRegExp *re, JsString *s, double li, int32_t *caps) {
    bool track = re->global || re->prog.sticky;
    bool readonly = track && js_regexp_last_index_readonly(&re->obj);
    uint32_t start = 0;
    if (track) {
        li = __builtin_trunc(li);
        if (li != li || li < 0)
            li = 0;
        if (li > (double)s->length) {
            if (readonly)
                return RE_RUN_READONLY;
            re->last_index = js_number(0);
            return RE_RUN_NOMATCH;
        }
        start = unicode_start(s, (uint32_t)li, re_is_unicode(re));
    }
    int rc = re_match(ctx, re, s, start, false, caps);
    if (rc != RE_RUN_MATCH) {
        if (track && rc == RE_RUN_NOMATCH) {
            if (readonly)
                return RE_RUN_READONLY;
            re->last_index = js_number(0);
        }
        return rc;
    }
    if (readonly)
        return RE_RUN_READONLY;
    if (track)
        re->last_index = js_number(caps[1]);
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

/* ---- the prototype chain, as the methods below consult it ----
 *
 * There are no symbols here, so RegExp.prototype's @@match, @@matchAll, @@replace, @@search and
 * @@split are not properties a script could reach or replace: String.prototype's methods find
 * them as V8 would, on an argument that is RegExp.prototype or inherits from it
 * (js_regexp_like). What those methods read from the regexp they are given - exec, flags (and
 * through it the flag getters), constructor, lastIndex - they read as properties, up the
 * prototype chain, so a script's own are the ones used. Each has a fast path for a regexp
 * nothing has redefined them for (re_pristine), which does the same without the lookups.
 *
 * A property read cannot run a script's code here (there are no script getters); a conversion
 * and a call can. So a fast path is chosen after the conversions the method starts with, and
 * runs no code of the script's while it holds a JsRegExp pointer's contents to be the same.
 */

static bool obj_type(JsValue v) {
    return js_is_object(v) || js_is_function(v) || js_is_promise(v);
}

bool js_regexp_like(JsContext *ctx, JsValue v) {
    const JsGcCell *rp = &ctx->regexp_proto->gc;
    for (int depth = 0; depth < 10000 && obj_type(v); depth++) {
        if (js_value_cell(v) == rp)
            return true;
        v = js_get_proto(ctx, v);
    }
    return false;
}

bool js_regexp_isregexp(JsContext *ctx, JsValue v) {
    return js_regexp_is(v) || js_regexp_like(ctx, v);
}

#define RE_NEED_EXEC (1u << JS_INTACT_RE_EXEC)
#define RE_NEED_FLAGS                                                                             \
    ((1u << JS_INTACT_RE_FLAGS) | (1u << JS_INTACT_RE_HASINDICES) | (1u << JS_INTACT_RE_GLOBAL) | \
     (1u << JS_INTACT_RE_IGNORECASE) | (1u << JS_INTACT_RE_MULTILINE) |                           \
     (1u << JS_INTACT_RE_DOTALL) | (1u << JS_INTACT_RE_UNICODE) |                                 \
     (1u << JS_INTACT_RE_UNICODESETS) | (1u << JS_INTACT_RE_STICKY))
#define RE_NEED_CTOR (1u << JS_INTACT_RE_CONSTRUCTOR)
#define RE_NEED_SOURCE (1u << JS_INTACT_RE_SOURCE)

/* V is a regexp whose properties named in NEED (RE_NEED_*) are the builtin ones: its prototype
 * is RegExp.prototype, which still has them as it was made, and it has none of its own by
 * those names. And its lastIndex is a number, so that nothing a fast path does with it is a
 * conversion. */
static bool re_pristine(JsContext *ctx, JsValue v, unsigned need) {
    if (!js_regexp_is(v))
        return false;
    const JsRegExp *re = (const JsRegExp *)js_value_object(v);
    if (re->obj.proto.bits != js_value_from_cell(&ctx->regexp_proto->gc).bits ||
        !js_is_number(re->last_index))
        return false;
    bool own = re->obj.props.count != 0;
    for (unsigned id = 0; need; id++, need >>= 1) {
        if (!(need & 1))
            continue;
        if (!js_intact(ctx, id))
            return false;
        if (own && js_map_position(&re->obj.props, ctx->intact[id].key) != JS_MAP_EMPTY)
            return false;
    }
    return true;
}

/* O[name]. False thrown, *err the error. */
static bool re_get(JsContext *ctx, JsValue o, const char *name, JsValue *out, JsValue *err) {
    JsString *k = js_ascii_cell(ctx->vm, name);
    if (k && js_get_property(ctx, o, js_value_from_cell(&k->gc), out))
        return true;
    *err = js_take_error(ctx);
    return false;
}

/* O[i] */
static bool re_get_index(JsContext *ctx, JsValue o, double i, JsValue *out, JsValue *err) {
    if (js_get_property(ctx, o, js_number(i), out))
        return true;
    *err = js_take_error(ctx);
    return false;
}

/* O[name] = v, a refused write thrown (a module is strict code) */
static bool re_put(JsContext *ctx, JsValue o, const char *name, JsValue v, JsValue *err) {
    JsString *k = js_ascii_cell(ctx->vm, name);
    if (k && js_set_property(ctx, o, js_value_from_cell(&k->gc), v))
        return true;
    *err = js_take_error(ctx);
    return false;
}

/* ToString(*slot), in place: SLOT is one the caller has rooted */
static bool to_str(JsContext *ctx, JsValue *slot, JsValue *err) {
    JsString *s = js_op_to_string(ctx, *slot);
    if (!s)
        return (*err = js_take_error(ctx)), false;
    *slot = js_value_from_cell(&s->gc);
    return true;
}

/* ToIntegerOrInfinity(O[name]), or ToLength of it (0 .. 2^53 - 1) */
static bool re_get_integer(JsContext *ctx, JsValue o, const char *name, bool length, double *n,
                           JsValue *err) {
    JsValue v = js_undefined();
    double d = 0;
    js_gc_protect(ctx->vm, &v);
    bool ok = re_get(ctx, o, name, &v, err);
    if (ok && !js_op_to_number(ctx, v, &d)) {
        *err = js_take_error(ctx);
        ok = false;
    }
    js_gc_unprotect(ctx->vm, &v);
    d = d != d ? 0 : __builtin_trunc(d);
    if (length)
        d = d <= 0 ? 0 : d > 9007199254740991.0 ? 9007199254740991.0 : d;
    *n = d;
    return ok;
}

static bool str_has(JsValue sv, uint16_t c) {
    const JsString *s = js_value_string(sv);
    for (uint32_t i = 0; i < s->length; i++) {
        if (s->units[i] == c)
            return true;
    }
    return false;
}

static bool same_value(JsValue x, JsValue y) {
    if (js_is_number(x) && js_is_number(y)) {
        double dx = js_get_number(x), dy = js_get_number(y);
        return dx != dx ? dy != dy : (dx == dy && __builtin_signbit(dx) == __builtin_signbit(dy));
    }
    return js_strict_equals(x, y);
}

/* rx.lastIndex past an empty match: AdvanceStringIndex(S, ToLength(rx.lastIndex), unicode) */
static bool bump_last_index(JsContext *ctx, JsValue rxv, const JsString *s, bool uni,
                            JsValue *err) {
    double li;
    if (!re_get_integer(ctx, rxv, "lastIndex", true, &li, err))
        return false;
    double next = li + 1;
    if (uni && li + 1 < (double)s->length && is_lead(s->units[(uint32_t)li]) &&
        is_trail(s->units[(uint32_t)li + 1]))
        next = li + 2;
    return re_put(ctx, rxv, "lastIndex", js_number(next), err);
}

/* ---- RegExpExec ---- */

/* What a negative RE_RUN_* throws */
static bool re_run_throw(JsContext *ctx, int rc, JsValue *r) {
    if (rc == RE_RUN_READONLY)
        return re_throw(ctx, r, "TypeError: Cannot assign to read only property 'lastIndex' of "
                                "object '[object RegExp]'");
    return re_budget_throw(ctx, r);
}

/* RegExpBuiltinExec: exec's result for the regexp RV on the string SV (both rooted by the
 * caller), null for no match */
static bool builtin_exec(JsContext *ctx, JsValue rv, JsValue sv, JsValue *r) {
    JsRegExp *re = (JsRegExp *)js_value_object(rv);
    int32_t caps[CAPS_MAX];
    double li;
    if (js_is_number(re->last_index)) {
        li = js_get_number(re->last_index);
    } else {
        /* ToLength(lastIndex), whatever the flags: the one place a script's code runs here, and
         * the pattern is what it is after it (a valueOf may compile() the regexp) */
        JsValue v = re->last_index;
        js_gc_protect(ctx->vm, &v);
        bool ok = js_op_to_number(ctx, v, &li);
        js_gc_unprotect(ctx->vm, &v);
        if (!ok)
            return (*r = js_take_error(ctx)), false;
    }
    int rc = exec_protocol(ctx, re, js_value_string(sv), li, caps);
    if (rc < 0)
        return re_run_throw(ctx, rc, r);
    if (rc == RE_RUN_NOMATCH) {
        *r = js_null();
        return true;
    }
    return build_exec_result(ctx, re, sv, caps, r) || re_oom(ctx, r);
}

/* RegExpExec(R, S): R's `exec` called when it has one that is a function (its result an object
 * or null), else the builtin on a regexp. False thrown, *out the error. RV and SV are rooted by
 * the caller. */
static bool regexp_exec(JsContext *ctx, JsValue rv, JsValue sv, JsValue *out) {
    JsValue exec;
    if (!re_get(ctx, rv, "exec", &exec, out))
        return false;
    if (js_is_function(exec)) {
        js_gc_protect(ctx->vm, &exec);
        bool ok = js_call(ctx, exec, rv, &sv, 1, out);
        js_gc_unprotect(ctx->vm, &exec);
        if (ok && !obj_type(*out) && !js_is_null(*out))
            return re_throw(ctx, out, "TypeError: RegExp exec method returned something other "
                                      "than an Object or null");
        return ok;
    }
    if (!js_regexp_is(rv)) {
        *out = js_error_fmt(ctx, "TypeError: Method RegExp.prototype.exec called on incompatible receiver %v", rv);
        return false;
    }
    return builtin_exec(ctx, rv, sv, out);
}

/* ---- RegExp methods ---- */

static bool rexp_exec(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                      JsValue *r) {
    if (!js_regexp_is(tv))
        return (*r = js_error_receiver(ctx, "RegExp", tv)), false;
    JsString *s = js_native_string(ctx, ARG(0));
    if (!s)
        return re_oom(ctx, r);
    return builtin_exec(ctx, tv, js_value_from_cell(&s->gc), r);
}

/* test(S): RegExpExec(this, S) is not null, on any object */
static bool rexp_test(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                      JsValue *r) {
    if (!obj_type(tv))
        return (*r = js_error_receiver(ctx, "RegExp", tv)), false;
    /* Rooted across the match like every other subject here: matching allocates through the
     * VM, so it is a GC safe point, and a coerced subject (test(123)) is a fresh cell nothing
     * else references. */
    JsString *s = js_native_string(ctx, ARG(0));
    if (!s)
        return re_oom(ctx, r);
    if (re_pristine(ctx, tv, RE_NEED_EXEC)) {
        int32_t caps[CAPS_MAX];
        JsRegExp *re = (JsRegExp *)js_value_object(tv);
        int rc = exec_protocol(ctx, re, s, li_number(re), caps);
        if (rc < 0)
            return re_run_throw(ctx, rc, r);
        *r = js_bool(rc == RE_RUN_MATCH);
        return true;
    }
    if (!regexp_exec(ctx, tv, js_value_from_cell(&s->gc), r))
        return false;
    *r = js_bool(!js_is_null(*r));
    return true;
}

/* RegExp.prototype.toString (22.2.6.17): "/" + ToString(source) + "/" + ToString(flags), read
 * from the receiver, whatever object it is */
static bool regexp_generic_string(JsContext *ctx, JsValue tv, JsValue *r) {
    JsVm *vm = ctx->vm;
    JsValue parts[2] = {js_undefined(), js_undefined()};
    js_gc_protect(vm, &parts[0]);
    js_gc_protect(vm, &parts[1]);
    bool ok = re_get(ctx, tv, "source", &parts[0], r) && to_str(ctx, &parts[0], r) &&
              re_get(ctx, tv, "flags", &parts[1], r) && to_str(ctx, &parts[1], r);
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
        if (!out)
            ok = n > JS_MAX_STRING_UNITS ? re_throw(ctx, r, JS_STRING_LENGTH_ERROR) : re_oom(ctx, r);
    }
    js_gc_unprotect(vm, &parts[1]);
    js_gc_unprotect(vm, &parts[0]);
    if (ok)
        *r = js_value_from_cell(&out->gc);
    return ok;
}

static bool rexp_toString(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                          JsValue *r) {
    (void)args;
    (void)argc;
    if (!obj_type(tv))
        return (*r = js_error_receiver(ctx, "RegExp", tv)), false;
    if (!re_pristine(ctx, tv, RE_NEED_SOURCE | RE_NEED_FLAGS))
        return regexp_generic_string(ctx, tv, r);
    JsString *s = js_regexp_repr(ctx, js_value_object(tv));
    if (!s) {
        const JsRegExp *re = (const JsRegExp *)js_value_object(tv);
        return (uint64_t)re->source->length + re->flags->length + 2 > JS_MAX_STRING_UNITS
                   ? re_throw(ctx, r, JS_STRING_LENGTH_ERROR) : re_oom(ctx, r);
    }
    *r = js_value_from_cell(&s->gc);
    return true;
}

/* ---- RegExp global ---- */

/* The regexp js_regexp_new makes of the strings (or undefined, the empty one) PV and FV, which
 * the caller has rooted; a failure thrown as V8 words it */
static bool regexp_new_of(JsContext *ctx, JsValue pv, JsValue fv, JsValue *r) {
    const JsString *ps = js_is_undefined(pv) ? NULL : js_value_string(pv);
    const JsString *fs = js_is_undefined(fv) ? NULL : js_value_string(fv);
    const uint16_t *pu = ps ? ps->units : NULL, *fu = fs ? fs->units : NULL;
    uint32_t pn = ps ? ps->length : 0, fn = fs ? fs->length : 0;
    const char *err = NULL;
    JsValue rev = js_regexp_new(ctx, pu, pn, fu, fn, &err);
    if (!js_is_object(rev)) {
        *r = js_regexp_error(ctx, err ? err : "out of memory", pu, pn, fu, fn);
        return false;
    }
    *r = rev;
    return true;
}

/* A new regexp for PATTERN and FLAGS, as the constructor makes one (22.2.4.1 from step 4): a
 * regexp gives its own source, and its flags where FLAGS is undefined; with LIKE, an object
 * that inherits from RegExp.prototype gives its `source` and `flags` properties; anything else
 * is the pattern, converted. False thrown, *r the error. */
static bool regexp_create(JsContext *ctx, JsValue pv, JsValue fv, bool like, JsValue *r) {
    JsVm *vm = ctx->vm;
    JsValue p = pv, f = fv;
    js_gc_protect(vm, &p);
    js_gc_protect(vm, &f);
    bool ok = true;
    if (js_regexp_is(pv)) {
        const JsRegExp *src = (const JsRegExp *)js_value_object(pv);
        p = js_value_from_cell(&src->source->gc);
        if (js_is_undefined(fv))
            f = js_value_from_cell(&src->flags->gc);
    } else if (like && js_regexp_like(ctx, pv)) {
        ok = re_get(ctx, pv, "source", &p, r) &&
             (!js_is_undefined(fv) || re_get(ctx, pv, "flags", &f, r));
    }
    ok = ok && (js_is_undefined(p) || to_str(ctx, &p, r)) &&
         (js_is_undefined(f) || to_str(ctx, &f, r)) && regexp_new_of(ctx, p, f, r);
    js_gc_unprotect(vm, &f);
    js_gc_unprotect(vm, &p);
    return ok;
}

/* RegExp(pattern, flags): called without `new` and without flags on something that is a regexp
 * (IsRegExp) whose `constructor` is RegExp, it gives that back; else a new one */
static bool g_RegExp(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                     JsValue *r) {
    (void)tv;
    JsValue pv = ARG(0), fv = ARG(1);
    if (!js_native_is_new(ctx) && js_is_undefined(fv) && js_regexp_isregexp(ctx, pv)) {
        JsValue c;
        if (!re_get(ctx, pv, "constructor", &c, r))
            return false;
        if (js_is_function(c) && js_value_cell(c) == &ctx->regexp_ctor->gc) {
            *r = pv;
            return true;
        }
    }
    return regexp_create(ctx, pv, fv, true, r);
}

/* ---- String methods ---- */

/* RegExpCreate(P, flags) for match, matchAll and search given something that does not inherit
 * from RegExp.prototype: undefined is the empty pattern, anything else its ToString (a regexp
 * with another prototype too). *out is kept for the running native. False thrown. */
static bool regexp_of_pattern(JsContext *ctx, JsValue pv, const char *flags, JsValue *out,
                              JsValue *r) {
    JsVm *vm = ctx->vm;
    JsValue p = pv, f = js_undefined();
    js_gc_protect(vm, &p);
    js_gc_protect(vm, &f);
    bool ok = js_is_undefined(p) || to_str(ctx, &p, r);
    if (ok && flags[0]) {
        JsString *fs = js_ascii_cell(vm, flags);
        if (fs)
            f = js_value_from_cell(&fs->gc);
        else
            ok = re_oom(ctx, r);
    }
    ok = ok && regexp_new_of(ctx, p, f, r);
    if (ok) {
        *out = *r;
        if (!js_native_keep(ctx, *out))
            ok = re_oom(ctx, r);
    }
    js_gc_unprotect(vm, &f);
    js_gc_unprotect(vm, &p);
    return ok;
}

/* RegExp.prototype[@@match] on RXV (22.2.6.8), for the string SV. Both are rooted by the
 * caller, here and in the methods after it. */
static bool re_do_match(JsContext *ctx, JsValue rxv, JsValue sv, JsValue *r) {
    JsVm *vm = ctx->vm;
    JsString *s = js_value_string(sv);
    if (re_pristine(ctx, rxv, RE_NEED_EXEC | RE_NEED_FLAGS)) {
        JsRegExp *re = (JsRegExp *)js_value_object(rxv);
        if (!re->global)
            return builtin_exec(ctx, rxv, sv, r);
        if (js_regexp_last_index_readonly(&re->obj))
            return re_run_throw(ctx, RE_RUN_READONLY, r);
        re->last_index = js_number(0); /* where the last, failed search leaves it too */
        JsObject *arr = js_array_new_cell(ctx, 0);
        if (!arr)
            return re_oom(ctx, r);
        JsValue arrv = js_value_from_cell(&arr->gc);
        js_gc_protect(vm, &arrv);
        int32_t caps[CAPS_MAX];
        bool ok = true, uni = re_is_unicode(re), oom = false;
        uint32_t pos = 0;
        int rc = RE_RUN_NOMATCH;
        while (ok && pos <= s->length &&
               (rc = re_match(ctx, re, s, pos, false, caps)) == RE_RUN_MATCH) {
            JsValue el = substr_or_undef(vm, s, caps[0], caps[1], &oom);
            ok = !oom && js_array_append(vm, js_value_object(arrv), el);
            pos = caps[1] == caps[0] ? advance_index(s, (uint32_t)caps[1], uni)
                                     : (uint32_t)caps[1];
        }
        js_gc_unprotect(vm, &arrv);
        if (rc == RE_RUN_BUDGET)
            return re_budget_throw(ctx, r);
        if (!ok)
            return re_oom(ctx, r);
        *r = js_value_object(arrv)->elem_count ? arrv : js_null();
        return true;
    }

    JsValue fl = js_undefined(), res = js_undefined(), ms = js_undefined(), arrv = js_undefined();
    js_gc_protect(vm, &fl);
    js_gc_protect(vm, &res);
    js_gc_protect(vm, &ms);
    js_gc_protect(vm, &arrv);
    bool ok = re_get(ctx, rxv, "flags", &fl, r) && to_str(ctx, &fl, r);
    if (ok && !str_has(fl, 'g')) {
        ok = regexp_exec(ctx, rxv, sv, r);
    } else if (ok) {
        bool uni = str_has(fl, 'u') || str_has(fl, 'v');
        ok = re_put(ctx, rxv, "lastIndex", js_number(0), r);
        if (ok) {
            JsObject *arr = js_array_new_cell(ctx, 0);
            if (arr)
                arrv = js_value_from_cell(&arr->gc);
            else
                ok = re_oom(ctx, r);
        }
        while (ok) {
            if (!js_native_charge(ctx)) {
                *r = js_take_error(ctx);
                ok = false;
            } else if (!regexp_exec(ctx, rxv, sv, &res)) {
                *r = res;
                ok = false;
            } else if (js_is_null(res)) {
                break;
            } else {
                ok = re_get_index(ctx, res, 0, &ms, r) && to_str(ctx, &ms, r);
                if (ok && !js_array_append(vm, js_value_object(arrv), ms))
                    ok = re_oom(ctx, r);
                if (ok && js_value_string(ms)->length == 0)
                    ok = bump_last_index(ctx, rxv, s, uni, r);
            }
        }
        if (ok)
            *r = js_value_object(arrv)->elem_count ? arrv : js_null();
    }
    js_gc_unprotect(vm, &arrv);
    js_gc_unprotect(vm, &ms);
    js_gc_unprotect(vm, &res);
    js_gc_unprotect(vm, &fl);
    return ok;
}

bool js_re_str_match(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                     JsValue *r) {
    if (!js_str_this_check(ctx, tv, r))
        return false;
    JsValue rxv = ARG(0);
    bool like = js_regexp_like(ctx, rxv);
    JsString *s = js_native_string(ctx, tv);
    if (!s)
        return re_oom(ctx, r);
    if (!like && !regexp_of_pattern(ctx, rxv, "", &rxv, r))
        return false;
    return re_do_match(ctx, rxv, js_value_from_cell(&s->gc), r);
}

/* A RegExp String Iterator (js_iter.h, JS_ITER_REGEXP), in one of two shapes. FAST: the regexp
 * matchAll was given stands for the copy of it the method makes, while nothing can tell them
 * apart - `target` is that regexp, `inner` its source as it was, `op` its flags (a bit for each
 * of "dgimsuvy"), `index` the copy's lastIndex. A step that finds RegExp.prototype.exec
 * replaced, or the regexp recompiled, makes the copy and goes on as GENERIC: `target` is the
 * matcher, whatever object it is, stepped by RegExpExec and its lastIndex property, and `op`
 * has bit 0 for global and bit 1 for unicode. DONE: a matcher that is not global has given its
 * one match. */
enum { RE_ITER_FAST, RE_ITER_GENERIC, RE_ITER_DONE };
static const char re_flag_letters[] = "dgimsuvy";
enum { RE_BIT_G = 1u << 1, RE_BIT_U = 1u << 5, RE_BIT_V = 1u << 6, RE_BIT_Y = 1u << 7 };

/* RegExp.prototype[@@matchAll] on RXV (22.2.6.9) */
static bool re_do_match_all(JsContext *ctx, JsValue rxv, JsValue sv, JsValue *r) {
    JsVm *vm = ctx->vm;
    JsString *s = js_value_string(sv);
    if (re_pristine(ctx, rxv, RE_NEED_EXEC | RE_NEED_FLAGS | RE_NEED_CTOR)) {
        JsRegExp *re = (JsRegExp *)js_value_object(rxv);
        double li = li_number(re);
        uint32_t start = li != li || li <= 0 ? 0
                         : li > (double)s->length ? s->length + 1 : (uint32_t)li;
        *r = js_iterobj_new(ctx, JS_ITER_REGEXP, rxv, sv, start);
        if (js_is_undefined(*r))
            return re_oom(ctx, r);
        JsIterObj *it = (JsIterObj *)js_value_object(*r);
        it->inner = js_value_from_cell(&re->source->gc);
        it->op = 0;
        for (int i = 0; i < 8; i++) {
            if (str_has(js_value_from_cell(&re->flags->gc), (uint16_t)re_flag_letters[i]))
                it->op |= (uint8_t)(1u << i);
        }
        it->state = RE_ITER_FAST;
        return true;
    }

    JsValue ctor = js_undefined(), fl = js_undefined(), m = js_undefined();
    js_gc_protect(vm, &ctor);
    js_gc_protect(vm, &fl);
    js_gc_protect(vm, &m);
    int sp = js_species(ctx, rxv, ctx->regexp_ctor, &ctor);
    bool ok = sp >= 0;
    if (!ok)
        *r = ctor;
    ok = ok && re_get(ctx, rxv, "flags", &fl, r) && to_str(ctx, &fl, r);
    if (ok) {
        JsValue cargs[2] = {rxv, fl};
        ok = sp ? js_construct(ctx, ctor, cargs, 2, &m) : regexp_create(ctx, rxv, fl, true, &m);
        if (!ok)
            *r = m;
    }
    double li = 0;
    ok = ok && re_get_integer(ctx, rxv, "lastIndex", true, &li, r) &&
         re_put(ctx, m, "lastIndex", js_number(li), r);
    if (ok) {
        *r = js_iterobj_new(ctx, JS_ITER_REGEXP, m, sv, 0);
        if (js_is_undefined(*r)) {
            ok = re_oom(ctx, r);
        } else {
            JsIterObj *it = (JsIterObj *)js_value_object(*r);
            it->op = (uint8_t)((str_has(fl, 'g') ? 1 : 0) |
                               (str_has(fl, 'u') || str_has(fl, 'v') ? 2 : 0));
            it->state = RE_ITER_GENERIC;
        }
    }
    js_gc_unprotect(vm, &m);
    js_gc_unprotect(vm, &fl);
    js_gc_unprotect(vm, &ctor);
    return ok;
}

/* matchAll's and replaceAll's check of an argument that is a regexp (IsRegExp): its `flags`
 * must have "g" */
bool js_re_global_check(JsContext *ctx, JsValue v, bool match_all, JsValue *r) {
    if (!js_regexp_isregexp(ctx, v))
        return true;
    bool global;
    if (re_pristine(ctx, v, RE_NEED_FLAGS)) {
        global = ((const JsRegExp *)js_value_object(v))->global;
    } else {
        JsValue f = js_undefined();
        js_gc_protect(ctx->vm, &f);
        bool ok = re_get(ctx, v, "flags", &f, r);
        if (ok && (js_is_undefined(f) || js_is_null(f)))
            ok = re_throw(ctx, r, match_all ? "TypeError: The .flags property of the argument to "
                                              "String.prototype.matchAll cannot be null or undefined"
                                            : "TypeError: String.prototype.replaceAll called on null "
                                              "or undefined");
        ok = ok && to_str(ctx, &f, r);
        global = ok && str_has(f, 'g');
        js_gc_unprotect(ctx->vm, &f);
        if (!ok)
            return false;
    }
    if (!global)
        return re_throw(ctx, r, match_all ? "TypeError: String.prototype.matchAll called with a "
                                            "non-global RegExp argument"
                                          : "TypeError: String.prototype.replaceAll called with a "
                                            "non-global RegExp argument");
    return true;
}

bool js_re_str_matchAll(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                        JsValue *r) {
    if (!js_str_this_check(ctx, tv, r))
        return false;
    JsValue rxv = ARG(0);
    if (!js_re_global_check(ctx, rxv, true, r))
        return false;
    bool like = js_regexp_like(ctx, rxv);
    JsString *s = js_native_string(ctx, tv);
    if (!s)
        return re_oom(ctx, r);
    if (!like && !regexp_of_pattern(ctx, rxv, "g", &rxv, r))
        return false;
    return re_do_match_all(ctx, rxv, js_value_from_cell(&s->gc), r);
}

int js_re_iter_step(JsContext *ctx, JsIterObj *it, JsValue *out) {
    JsVm *vm = ctx->vm;
    if (it->state == RE_ITER_DONE)
        return 0;
    if (it->state == RE_ITER_FAST) {
        JsRegExp *re = (JsRegExp *)js_value_object(it->target);
        JsString *s = js_value_string(it->subject);
        if (js_intact(ctx, JS_INTACT_RE_EXEC) &&
            js_value_from_cell(&re->source->gc).bits == it->inner.bits) {
            bool global = (it->op & RE_BIT_G) != 0;
            uint32_t start = global || (it->op & RE_BIT_Y) ? it->index : 0;
            if (start > s->length)
                return 0;
            start = unicode_start(s, start, (it->op & (RE_BIT_U | RE_BIT_V)) != 0);
            int32_t caps[CAPS_MAX];
            int rc = re_match(ctx, re, s, start, false, caps);
            if (rc == RE_RUN_BUDGET)
                return re_budget_throw(ctx, out), -1;
            if (rc == RE_RUN_NOMATCH)
                return 0;
            if (!build_exec_result(ctx, re, it->subject, caps, out))
                return re_oom(ctx, out), -1;
            if (!global)
                it->state = RE_ITER_DONE;
            else
                it->index = caps[1] != caps[0] ? (uint32_t)caps[1]
                            : advance_index(s, (uint32_t)caps[1], (it->op & (RE_BIT_U | RE_BIT_V)) != 0);
            return 1;
        }
        /* the copy, as it would be by now */
        uint16_t fbuf[8];
        uint32_t fn = 0;
        for (int i = 0; i < 8; i++) {
            if (it->op & (1u << i))
                fbuf[fn++] = (uint16_t)re_flag_letters[i];
        }
        const JsString *src = js_value_string(it->inner);
        const char *err = NULL;
        JsValue copy = js_regexp_new(ctx, src->units, src->length, fbuf, fn, &err);
        if (!js_is_object(copy)) {
            *out = js_regexp_error(ctx, err ? err : "out of memory", src->units, src->length, fbuf, fn);
            return -1;
        }
        ((JsRegExp *)js_value_object(copy))->last_index = js_number(it->index);
        uint8_t op = (uint8_t)((it->op & RE_BIT_G ? 1 : 0) | (it->op & (RE_BIT_U | RE_BIT_V) ? 2 : 0));
        it->target = copy;
        it->inner = js_undefined();
        it->op = op;
        it->state = RE_ITER_GENERIC;
    }

    /* a script's exec may step this iterator to its end, which lets go of what it holds */
    JsValue rxv = it->target, sv = it->subject, m = js_undefined(), ms = js_undefined();
    js_gc_protect(vm, &rxv);
    js_gc_protect(vm, &sv);
    js_gc_protect(vm, &m);
    js_gc_protect(vm, &ms);
    int ret = 1;
    if (!regexp_exec(ctx, rxv, sv, &m)) {
        *out = m;
        ret = -1;
    } else if (js_is_null(m)) {
        ret = 0;
    } else if (!(it->op & 1)) {
        it->state = RE_ITER_DONE;
    } else if (!re_get_index(ctx, m, 0, &ms, out) || !to_str(ctx, &ms, out) ||
               (js_value_string(ms)->length == 0 &&
                !bump_last_index(ctx, rxv, js_value_string(sv), (it->op & 2) != 0, out))) {
        ret = -1;
    }
    if (ret == 1)
        *out = m;
    js_gc_unprotect(vm, &ms);
    js_gc_unprotect(vm, &m);
    js_gc_unprotect(vm, &sv);
    js_gc_unprotect(vm, &rxv);
    return ret;
}

/* RegExp.prototype[@@search] on RXV (22.2.6.12) */
static bool re_do_search(JsContext *ctx, JsValue rxv, JsValue sv, JsValue *r) {
    JsVm *vm = ctx->vm;
    if (re_pristine(ctx, rxv, RE_NEED_EXEC)) {
        /* lastIndex is 0 for the search and what it was after it; a regexp whose lastIndex
         * cannot be written fails where one of those writes would */
        JsRegExp *re = (JsRegExp *)js_value_object(rxv);
        bool zero = li_number(re) == 0 && !__builtin_signbit(li_number(re));
        if (js_regexp_last_index_readonly(&re->obj) && (re->global || re->prog.sticky || !zero))
            return re_run_throw(ctx, RE_RUN_READONLY, r);
        int32_t caps[CAPS_MAX];
        int rc = re_match(ctx, re, js_value_string(sv), 0, false, caps);
        if (rc < 0)
            return re_run_throw(ctx, rc, r);
        *r = js_number(rc == RE_RUN_MATCH ? (double)caps[0] : -1.0);
        return true;
    }
    JsValue prev = js_undefined(), res = js_undefined(), cur;
    js_gc_protect(vm, &prev);
    js_gc_protect(vm, &res);
    bool ok = re_get(ctx, rxv, "lastIndex", &prev, r) &&
              (same_value(prev, js_number(0)) || re_put(ctx, rxv, "lastIndex", js_number(0), r));
    if (ok && !regexp_exec(ctx, rxv, sv, &res)) {
        *r = res;
        ok = false;
    }
    ok = ok && re_get(ctx, rxv, "lastIndex", &cur, r) &&
         (same_value(cur, prev) || re_put(ctx, rxv, "lastIndex", prev, r));
    if (ok && js_is_null(res))
        *r = js_number(-1);
    else if (ok)
        ok = re_get(ctx, res, "index", r, r);
    js_gc_unprotect(vm, &res);
    js_gc_unprotect(vm, &prev);
    return ok;
}

bool js_re_str_search(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                      JsValue *r) {
    if (!js_str_this_check(ctx, tv, r))
        return false;
    JsValue rxv = ARG(0);
    bool like = js_regexp_like(ctx, rxv);
    JsString *s = js_native_string(ctx, tv);
    if (!s)
        return re_oom(ctx, r);
    if (!like && !regexp_of_pattern(ctx, rxv, "", &rxv, r))
        return false;
    return re_do_search(ctx, rxv, js_value_from_cell(&s->gc), r);
}

/* ---- replace ---- */

/* C-side unit buffer (mirror of js_builtins.c's StrBuf, which is static there) */
typedef struct {
    JsVm *vm;
    uint16_t *u;
    uint32_t len, cap;
    bool oom;      /* stopped: out of memory, or too_long */
    bool too_long; /* stopped because the text would pass JS_MAX_STRING_UNITS */
} UBuf;

static void ub_init(UBuf *b, JsVm *vm) {
    b->vm = vm;
    b->u = NULL;
    b->len = b->cap = 0;
    b->oom = b->too_long = false;
}

static bool ub_reserve(UBuf *b, uint32_t extra) {
    if (b->oom)
        return false;
    if (extra > JS_MAX_STRING_UNITS - b->len) { /* len stays <= the cap, so this cannot wrap */
        b->oom = b->too_long = true;
        return false;
    }
    uint32_t need = b->len + extra;
    if (need <= b->cap)
        return true;
    uint32_t ncap = b->cap ? b->cap * 2 : 32;
    while (ncap < need)
        ncap *= 2;
    if (ncap > JS_MAX_STRING_UNITS) /* need fits, so the cap is enough */
        ncap = JS_MAX_STRING_UNITS;
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

/* Why a UBuf's text never became a string: the length limit (V8's RangeError), else re_oom */
static bool ub_fail(JsContext *ctx, const UBuf *b, JsValue *r) {
    return b->too_long ? re_throw(ctx, r, JS_STRING_LENGTH_ERROR) : re_oom(ctx, r);
}

/* undefined when the buffer stopped (ub_fail says why: too_long survives the free) */
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

/* The names of RE's groups, for a replacer that runs after RE may have changed: an array with
 * group g's name (an atom) at g and undefined for a group without one; *out is undefined when
 * no group has a name. False out of memory. */
static bool group_names(JsContext *ctx, const JsRegExp *re, JsValue *out) {
    JsVm *vm = ctx->vm;
    *out = js_undefined();
    if (!has_named_groups(re))
        return true;
    JsObject *arr = js_array_new_cell(ctx, (uint32_t)re->prog.group_count + 1);
    if (!arr)
        return false;
    JsValue arrv = js_value_from_cell(&arr->gc);
    js_gc_protect(vm, &arrv);
    bool ok = true;
    for (int g = 0; ok && g <= re->prog.group_count; g++) {
        JsValue name = js_undefined();
        if (g && re->prog.group_names[g][0]) {
            uint16_t nbuf[64];
            name = js_atom(vm, nbuf, name_units(re->prog.group_names[g], nbuf, 64));
            ok = js_is_string(name);
        }
        ok = ok && js_array_append(vm, js_value_object(arrv), name);
    }
    js_gc_unprotect(vm, &arrv);
    *out = arrv;
    return ok;
}

/* The `groups` object of a match, from group_names' array */
static bool groups_from_names(JsContext *ctx, JsValue names, const JsString *s,
                              const int32_t *caps, JsValue *out) {
    JsVm *vm = ctx->vm;
    JsValue obj = js_object_new(ctx);
    if (!js_is_object(obj))
        return false;
    js_value_object(obj)->proto = js_undefined(); /* no prototype, as named_groups_obj's */
    js_gc_protect(vm, &obj);
    JsValue vv = js_undefined();
    js_gc_protect(vm, &vv);
    bool ok = true, oom = false;
    for (uint32_t g = 1; ok && g < js_value_object(names)->elem_count; g++) {
        JsValue name = js_value_object(names)->elems[g];
        if (js_is_undefined(name))
            continue;
        vv = substr_or_undef(vm, s, caps[g * 2], caps[g * 2 + 1], &oom);
        ok = !oom && js_map_set(vm, &js_value_object(obj)->props, js_value_string(name), vv);
    }
    js_gc_unprotect(vm, &vv);
    js_gc_unprotect(vm, &obj);
    *out = obj;
    return ok;
}

/*
 * Rooted argument vector for a function replacer: (match, g1..gn, index, input[, groups]), for
 * a match of a regexp with NG groups named by NAMES (group_names). Returned as an array object
 * whose elems back js_call's args; undefined out of memory. The caller protects it.
 */
static JsValue replacer_args(JsContext *ctx, int ng, JsValue names, JsValue sv,
                             const int32_t *caps) {
    JsVm *vm = ctx->vm;
    JsString *s = js_value_string(sv);
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
    if (ok && !js_is_undefined(names)) {
        JsValue groups;
        ok = groups_from_names(ctx, names, s, caps, &groups) &&
             js_array_append(vm, js_value_object(arrv), groups);
    }
    js_gc_unprotect(vm, &arrv);
    return ok ? arrv : js_undefined();
}

/* replace with a string, on a regexp nothing is redefined for: no code of the script's runs */
static bool replace_fast_str(JsContext *ctx, JsValue rxv, JsValue sv, JsValue repv, JsValue *r) {
    JsRegExp *re = (JsRegExp *)js_value_object(rxv);
    JsString *s = js_value_string(sv);
    const JsString *rep = js_value_string(repv);
    UBuf sb;
    ub_init(&sb, ctx->vm);
    int32_t caps[CAPS_MAX];
    uint32_t cursor = 0;
    bool any = false;
    int rc;
    if (!re->global) {
        rc = exec_protocol(ctx, re, s, li_number(re), caps);
        if (rc == RE_RUN_MATCH) {
            ub_units(&sb, s->units, (uint32_t)caps[0]);
            expand_replacement(&sb, re, s, caps, rep);
            cursor = (uint32_t)caps[1];
            any = true;
        }
    } else {
        if (js_regexp_last_index_readonly(&re->obj))
            return re_run_throw(ctx, RE_RUN_READONLY, r);
        re->last_index = js_number(0); /* where the last, failed search leaves it too */
        bool uni = re_is_unicode(re);
        uint32_t pos = 0;
        rc = RE_RUN_NOMATCH;
        while (pos <= s->length && (rc = re_match(ctx, re, s, pos, false, caps)) == RE_RUN_MATCH) {
            uint32_t m0 = (uint32_t)caps[0], m1 = (uint32_t)caps[1];
            ub_units(&sb, s->units + cursor, m0 - cursor);
            expand_replacement(&sb, re, s, caps, rep);
            cursor = m1;
            any = true;
            pos = m1 == m0 ? advance_index(s, m1, uni) : m1;
        }
    }
    if (rc < 0 || !any) {
        ub_free(&sb);
        if (rc < 0)
            return re_run_throw(ctx, rc, r);
        *r = sv;
        return true;
    }
    ub_units(&sb, s->units + cursor, s->length - cursor);
    *r = ub_finish(&sb);
    return !js_is_undefined(*r) || ub_fail(ctx, &sb, r);
}

/* replace with a function, on a regexp nothing is redefined for. The matches are all found
 * before the function is first called, as the spec has it, and the regexp is not looked at
 * again: the function may recompile it, or move its lastIndex. */
static bool replace_fast_fn(JsContext *ctx, JsValue rxv, JsValue sv, JsValue fn, JsValue *r) {
    JsVm *vm = ctx->vm;
    JsRegExp *re = (JsRegExp *)js_value_object(rxv);
    JsString *s = js_value_string(sv);
    int ng = re->prog.group_count;
    size_t stride = 2 * ((size_t)ng + 1);
    int32_t one[CAPS_MAX];
    int32_t *list = one; /* COUNT matches' captures; the VM's when CAP is not 0 */
    size_t count = 0, cap = 0;
    if (!re->global) {
        int rc = exec_protocol(ctx, re, s, li_number(re), one);
        if (rc < 0)
            return re_run_throw(ctx, rc, r);
        count = rc == RE_RUN_MATCH;
    } else {
        if (js_regexp_last_index_readonly(&re->obj))
            return re_run_throw(ctx, RE_RUN_READONLY, r);
        re->last_index = js_number(0);
        bool uni = re_is_unicode(re), oom = false;
        uint32_t pos = 0;
        int32_t caps[CAPS_MAX];
        int rc = RE_RUN_NOMATCH;
        while (pos <= s->length && (rc = re_match(ctx, re, s, pos, false, caps)) == RE_RUN_MATCH) {
            if (count == cap) {
                size_t ncap = cap ? cap * 2 : 8;
                int32_t *nl = js_realloc_raw(vm, cap ? list : NULL, cap * stride * sizeof(int32_t),
                                             ncap * stride * sizeof(int32_t));
                if (!nl) {
                    oom = true;
                    break;
                }
                list = nl;
                cap = ncap;
            }
            memcpy(list + count * stride, caps, stride * sizeof(int32_t));
            count++;
            pos = caps[1] == caps[0] ? advance_index(s, (uint32_t)caps[1], uni) : (uint32_t)caps[1];
        }
        if (oom || rc == RE_RUN_BUDGET) {
            if (cap)
                js_realloc_raw(vm, list, cap * stride * sizeof(int32_t), 0);
            return oom ? re_oom(ctx, r) : re_budget_throw(ctx, r);
        }
    }
    if (count == 0) {
        *r = sv;
        return true;
    }

    JsValue names = js_undefined(), argv = js_undefined(), res = js_undefined();
    js_gc_protect(vm, &names);
    js_gc_protect(vm, &argv);
    js_gc_protect(vm, &res);
    UBuf sb;
    ub_init(&sb, vm);
    bool ok = group_names(ctx, re, &names) || re_oom(ctx, r);
    uint32_t cursor = 0;
    /* past the length limit the replacer is still called for every match, and the RangeError
     * comes at the end, as in V8 (the buffer only stops growing) */
    for (size_t i = 0; ok && i < count; i++) {
        const int32_t *c = list + i * stride;
        argv = replacer_args(ctx, ng, names, sv, c);
        if (!js_is_object(argv)) {
            ok = re_oom(ctx, r);
            break;
        }
        JsObject *aobj = js_value_object(argv);
        if (!js_call(ctx, fn, js_undefined(), aobj->elems, (int)aobj->elem_count, &res)) {
            *r = res; /* what it threw */
            ok = false;
            break;
        }
        if (!to_str(ctx, &res, r)) {
            ok = false;
            break;
        }
        ub_units(&sb, s->units + cursor, (uint32_t)c[0] - cursor);
        ub_units(&sb, js_value_string(res)->units, js_value_string(res)->length);
        cursor = (uint32_t)c[1];
    }
    if (cap)
        js_realloc_raw(vm, list, cap * stride * sizeof(int32_t), 0);
    if (ok) {
        ub_units(&sb, s->units + cursor, s->length - cursor);
        *r = ub_finish(&sb);
        ok = !js_is_undefined(*r) || ub_fail(ctx, &sb, r);
    } else {
        ub_free(&sb);
    }
    js_gc_unprotect(vm, &res);
    js_gc_unprotect(vm, &argv);
    js_gc_unprotect(vm, &names);
    return ok;
}

/* GetSubstitution (22.1.3.19.1) for a match that is any object's: MATCHED at POSITION in S,
 * CAPSV an array of the matched text and then its captures (strings, or undefined), GROUPS the
 * result's `groups` (undefined for none). A named reference reads a property of GROUPS and
 * converts it, which may run the script's code. False thrown. All are rooted by the caller. */
static bool substitute(JsContext *ctx, UBuf *sb, JsValue repv, JsValue sv, uint32_t position,
                       JsValue capsv, JsValue groups, JsValue *err) {
    JsVm *vm = ctx->vm;
    const JsString *rep = js_value_string(repv), *s = js_value_string(sv);
    const JsObject *capso = js_value_object(capsv);
    const JsString *matched = js_value_string(capso->elems[0]);
    uint32_t m = capso->elem_count - 1;
    uint32_t tail = position + matched->length; /* no more than twice a string's length */
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
            ub_units(sb, matched->units, matched->length);
            i += 2;
        } else if (n == '`') {
            ub_units(sb, s->units, position);
            i += 2;
        } else if (n == '\'') {
            if (tail < s->length)
                ub_units(sb, s->units + tail, s->length - tail);
            i += 2;
        } else if (n >= '0' && n <= '9') {
            uint32_t g = (uint32_t)(n - '0');
            uint32_t adv = 2;
            if (i + 2 < rep->length && rep->units[i + 2] >= '0' && rep->units[i + 2] <= '9') {
                uint32_t g2 = g * 10 + (uint32_t)(rep->units[i + 2] - '0');
                if (g2 >= 1 && g2 <= m) {
                    g = g2;
                    adv = 3;
                }
            }
            if (g >= 1 && g <= m) {
                JsValue cv = capso->elems[g];
                if (!js_is_undefined(cv))
                    ub_units(sb, js_value_string(cv)->units, js_value_string(cv)->length);
                i += adv;
            } else {
                ub_unit(sb, '$');
                i++;
            }
        } else if (n == '<' && !js_is_undefined(groups)) {
            uint32_t close = i + 2;
            while (close < rep->length && rep->units[close] != '>')
                close++;
            if (close >= rep->length) {
                ub_unit(sb, '$');
                i++;
                continue;
            }
            JsString *name = js_string_cell_new(vm, rep->units + i + 2, close - (i + 2));
            if (!name)
                return re_oom(ctx, err);
            JsValue v = js_value_from_cell(&name->gc), got;
            js_gc_protect(vm, &v);
            bool ok = js_get_property(ctx, groups, v, &got) || ((*err = js_take_error(ctx)), false);
            v = ok ? got : js_undefined();
            ok = ok && (js_is_undefined(v) || to_str(ctx, &v, err));
            if (ok && !js_is_undefined(v))
                ub_units(sb, js_value_string(v)->units, js_value_string(v)->length);
            js_gc_unprotect(vm, &v);
            if (!ok)
                return false;
            i = close + 1;
        } else {
            ub_unit(sb, '$');
            i++;
        }
    }
    return true;
}

/* RegExp.prototype[@@replace] (22.2.6.11) as the spec writes it, on any object: its `flags`,
 * `exec` and `lastIndex` are the ones a script gave it, and a match is whatever object exec
 * returned */
static bool replace_generic(JsContext *ctx, JsValue rxv, JsValue sv, JsValue repv, bool is_fn,
                            JsValue *r) {
    JsVm *vm = ctx->vm;
    JsString *s = js_value_string(sv);
    enum { FL, LIST, RES, STR, ARGS, GROUPS, REPL, NSLOT };
    JsValue v[NSLOT];
    for (int i = 0; i < NSLOT; i++) {
        v[i] = js_undefined();
        js_gc_protect(vm, &v[i]);
    }
    UBuf sb;
    ub_init(&sb, vm);
    bool ok = re_get(ctx, rxv, "flags", &v[FL], r) && to_str(ctx, &v[FL], r);
    bool global = ok && str_has(v[FL], 'g');
    bool uni = ok && (str_has(v[FL], 'u') || str_has(v[FL], 'v'));
    if (global)
        ok = re_put(ctx, rxv, "lastIndex", js_number(0), r);
    if (ok) {
        JsObject *arr = js_array_new_cell(ctx, 0);
        if (arr)
            v[LIST] = js_value_from_cell(&arr->gc);
        else
            ok = re_oom(ctx, r);
    }
    while (ok) {
        if (!js_native_charge(ctx)) {
            *r = js_take_error(ctx);
            ok = false;
        } else if (!regexp_exec(ctx, rxv, sv, &v[RES])) {
            *r = v[RES];
            v[RES] = js_undefined();
            ok = false;
        } else if (js_is_null(v[RES])) {
            break;
        } else if (!js_array_append(vm, js_value_object(v[LIST]), v[RES])) {
            ok = re_oom(ctx, r);
        } else if (!global) {
            break;
        } else {
            ok = re_get_index(ctx, v[RES], 0, &v[STR], r) && to_str(ctx, &v[STR], r);
            if (ok && js_value_string(v[STR])->length == 0)
                ok = bump_last_index(ctx, rxv, s, uni, r);
        }
    }

    double next = 0;
    for (uint32_t i = 0; ok && i < js_value_object(v[LIST])->elem_count; i++) {
        v[RES] = js_value_object(v[LIST])->elems[i];
        double ncap = 0, position = 0;
        ok = re_get_integer(ctx, v[RES], "length", true, &ncap, r) &&
             re_get_index(ctx, v[RES], 0, &v[STR], r) && to_str(ctx, &v[STR], r) &&
             re_get_integer(ctx, v[RES], "index", false, &position, r);
        if (!ok)
            break;
        ncap = ncap > 0 ? ncap - 1 : 0;
        position = position < 0 ? 0 : position > (double)s->length ? (double)s->length : position;
        uint32_t mlen = js_value_string(v[STR])->length;
        JsObject *arr = js_array_new_cell(ctx, 0);
        if (!arr || ((v[ARGS] = js_value_from_cell(&arr->gc)), !js_array_append(vm, arr, v[STR]))) {
            ok = re_oom(ctx, r);
            break;
        }
        for (double n = 1; ok && n <= ncap; n++) {
            if (!js_native_charge(ctx)) {
                *r = js_take_error(ctx);
                ok = false;
                break;
            }
            ok = re_get_index(ctx, v[RES], n, &v[REPL], r) &&
                 (js_is_undefined(v[REPL]) || to_str(ctx, &v[REPL], r));
            if (ok && !js_array_append(vm, js_value_object(v[ARGS]), v[REPL]))
                ok = re_oom(ctx, r);
        }
        ok = ok && re_get(ctx, v[RES], "groups", &v[GROUPS], r);
        if (!ok)
            break;
        uint32_t mark = sb.len;
        bool stopped = sb.oom;
        bool keep = position >= next;
        if (keep)
            ub_units(&sb, s->units + (uint32_t)next, (uint32_t)position - (uint32_t)next);
        if (is_fn) {
            JsObject *aobj = js_value_object(v[ARGS]);
            if (!js_array_append(vm, aobj, js_number(position)) || !js_array_append(vm, aobj, sv) ||
                (!js_is_undefined(v[GROUPS]) && !js_array_append(vm, aobj, v[GROUPS]))) {
                ok = re_oom(ctx, r);
                break;
            }
            if (!js_call(ctx, repv, js_undefined(), aobj->elems, (int)aobj->elem_count, &v[REPL])) {
                *r = v[REPL];
                v[REPL] = js_undefined();
                ok = false;
                break;
            }
            ok = to_str(ctx, &v[REPL], r);
            if (ok)
                ub_units(&sb, js_value_string(v[REPL])->units, js_value_string(v[REPL])->length);
        } else if (js_is_null(v[GROUPS])) {
            ok = re_throw(ctx, r, "TypeError: Cannot convert undefined or null to object");
        } else {
            ok = substitute(ctx, &sb, repv, sv, (uint32_t)position, v[ARGS], v[GROUPS], r);
        }
        if (!keep) {
            sb.len = mark; /* a match before the last one's end is not in the result */
            if (!stopped && sb.too_long) /* nor does its text count against the limit */
                sb.oom = sb.too_long = false;
        } else
            next = position + mlen;
    } /* past the length limit, as in V8, every match is still replaced and the RangeError comes
       * at the end */
    if (ok) {
        if (next < (double)s->length)
            ub_units(&sb, s->units + (uint32_t)next, s->length - (uint32_t)next);
        *r = ub_finish(&sb);
        ok = !js_is_undefined(*r) || ub_fail(ctx, &sb, r);
    } else {
        ub_free(&sb);
    }
    for (int i = NSLOT - 1; i >= 0; i--)
        js_gc_unprotect(vm, &v[i]);
    return ok;
}

bool js_re_str_replace(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                       JsValue *r) {
    JsValue rxv = args[0]; /* caller-checked: js_regexp_like */
    JsString *s = js_native_string(ctx, tv);
    if (!s)
        return re_oom(ctx, r);
    JsValue sv = js_value_from_cell(&s->gc), repv = ARG(1);
    bool is_fn = js_is_function(repv);
    if (!is_fn) {
        JsString *rep = js_native_string(ctx, repv);
        if (!rep)
            return re_oom(ctx, r);
        repv = js_value_from_cell(&rep->gc);
    }
    if (!re_pristine(ctx, rxv, RE_NEED_EXEC | RE_NEED_FLAGS))
        return replace_generic(ctx, rxv, sv, repv, is_fn, r);
    return is_fn ? replace_fast_fn(ctx, rxv, sv, repv, r) : replace_fast_str(ctx, rxv, sv, repv, r);
}

/* ---- split ---- */

/* split on a regexp nothing is redefined for: the splitter the spec makes of it (a copy with
 * "y") is this regexp matched at each position, and no code of the script's runs */
static bool split_fast(JsContext *ctx, JsValue rxv, JsValue sv, uint32_t lim, JsValue *r) {
    JsVm *vm = ctx->vm;
    JsRegExp *re = (JsRegExp *)js_value_object(rxv);
    JsString *s = js_value_string(sv);
    JsObject *arr = js_array_new_cell(ctx, 0);
    if (!arr)
        return re_oom(ctx, r);
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
    if (budget)
        return re_budget_throw(ctx, r);
    if (!ok)
        return re_oom(ctx, r);
    *r = arrv;
    return true;
}

/* S[a, b) appended to the array ARRV. *full: the array has LIM elements now. */
static bool split_push(JsContext *ctx, JsValue arrv, const JsString *s, double a, double b,
                       uint32_t lim, bool *full, JsValue *err) {
    bool oom = false;
    JsValue piece = substr_or_undef(ctx->vm, s, (int32_t)a, (int32_t)b, &oom);
    if (oom || !js_array_append(ctx->vm, js_value_object(arrv), piece))
        return re_oom(ctx, err);
    *full = js_value_object(arrv)->elem_count >= lim;
    return true;
}

/* RegExp.prototype[@@split] (22.2.6.14) as the spec writes it: the splitter is made by the
 * regexp's species constructor from the regexp and its `flags` with "y", and matched through
 * its own exec and lastIndex */
static bool split_generic(JsContext *ctx, JsValue rxv, JsValue sv, JsValue limit, JsValue *r) {
    JsVm *vm = ctx->vm;
    JsString *s = js_value_string(sv);
    enum { CTOR, FL, SPL, ARR, Z, EL, NSLOT };
    JsValue v[NSLOT];
    for (int i = 0; i < NSLOT; i++) {
        v[i] = js_undefined();
        js_gc_protect(vm, &v[i]);
    }
    int sp = js_species(ctx, rxv, ctx->regexp_ctor, &v[CTOR]);
    bool ok = sp >= 0;
    if (!ok)
        *r = v[CTOR];
    ok = ok && re_get(ctx, rxv, "flags", &v[FL], r) && to_str(ctx, &v[FL], r);
    bool uni = ok && (str_has(v[FL], 'u') || str_has(v[FL], 'v'));
    if (ok && !str_has(v[FL], 'y')) {
        UBuf fb;
        ub_init(&fb, vm);
        ub_units(&fb, js_value_string(v[FL])->units, js_value_string(v[FL])->length);
        ub_unit(&fb, 'y');
        v[FL] = ub_finish(&fb);
        if (js_is_undefined(v[FL]))
            ok = ub_fail(ctx, &fb, r);
    }
    if (ok) {
        JsValue cargs[2] = {rxv, v[FL]};
        ok = sp ? js_construct(ctx, v[CTOR], cargs, 2, &v[SPL])
                : regexp_create(ctx, rxv, v[FL], true, &v[SPL]);
        if (!ok) {
            *r = v[SPL];
            v[SPL] = js_undefined();
        }
    }
    if (ok) {
        JsObject *arr = js_array_new_cell(ctx, 0);
        if (arr)
            v[ARR] = js_value_from_cell(&arr->gc);
        else
            ok = re_oom(ctx, r);
    }
    uint32_t lim = 0;
    if (ok && !js_split_limit(ctx, limit, &lim)) {
        *r = js_take_error(ctx);
        ok = false;
    }
    double size = s->length, p = 0, q = 0;
    bool full = lim == 0;
    if (ok && !full && s->length == 0) {
        if (!regexp_exec(ctx, v[SPL], sv, &v[Z])) {
            *r = v[Z];
            v[Z] = js_undefined();
            ok = false;
        } else if (js_is_null(v[Z]) && !js_array_append(vm, js_value_object(v[ARR]), sv)) {
            ok = re_oom(ctx, r);
        }
        full = true; /* and no tail */
    }
    while (ok && !full && q < size) {
        double e = 0, ncap = 0;
        if (!js_native_charge(ctx)) {
            *r = js_take_error(ctx);
            ok = false;
            break;
        }
        if (!re_put(ctx, v[SPL], "lastIndex", js_number(q), r)) {
            ok = false;
            break;
        }
        if (!regexp_exec(ctx, v[SPL], sv, &v[Z])) {
            *r = v[Z];
            v[Z] = js_undefined();
            ok = false;
            break;
        }
        if (!js_is_null(v[Z]) && !re_get_integer(ctx, v[SPL], "lastIndex", true, &e, r)) {
            ok = false;
            break;
        }
        e = e > size ? size : e;
        if (js_is_null(v[Z]) || e == p) {
            q = advance_index(s, (uint32_t)q, uni);
            continue;
        }
        ok = split_push(ctx, v[ARR], s, p, q, lim, &full, r);
        if (!ok || full)
            break;
        p = e;
        ok = re_get_integer(ctx, v[Z], "length", true, &ncap, r);
        ncap = ncap > 0 ? ncap - 1 : 0;
        for (double i = 1; ok && !full && i <= ncap; i++) {
            if (!js_native_charge(ctx)) {
                *r = js_take_error(ctx);
                ok = false;
            } else if (re_get_index(ctx, v[Z], i, &v[EL], r)) {
                if (!js_array_append(vm, js_value_object(v[ARR]), v[EL]))
                    ok = re_oom(ctx, r);
                full = js_value_object(v[ARR])->elem_count >= lim;
            } else {
                ok = false;
            }
        }
        q = p;
    }
    if (ok && !full)
        ok = split_push(ctx, v[ARR], s, p, size, lim, &full, r);
    if (ok)
        *r = v[ARR];
    for (int i = NSLOT - 1; i >= 0; i--)
        js_gc_unprotect(vm, &v[i]);
    return ok;
}

bool js_re_str_split(JsContext *ctx, JsValue tv, const JsValue *args, int argc,
                     JsValue *r) {
    JsValue rxv = args[0], limit = ARG(1); /* caller-checked: js_regexp_like */
    JsString *s = js_native_string(ctx, tv);
    if (!s)
        return re_oom(ctx, r);
    JsValue sv = js_value_from_cell(&s->gc);
    /* the spec makes the splitter and then converts the limit: an object's conversion is the
     * script's code, which may change the regexp the fast path would still be reading */
    if (obj_type(limit) || !re_pristine(ctx, rxv, RE_NEED_EXEC | RE_NEED_FLAGS | RE_NEED_CTOR))
        return split_generic(ctx, rxv, sv, limit, r);
    uint32_t lim;
    if (!js_split_limit(ctx, limit, &lim))
        return re_oom(ctx, r);
    return split_fast(ctx, rxv, sv, lim, r);
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
 * accessors on the constructor as V8 has them (js_regexp_accessor), read from the last
 * successful match (re_record) whatever their receiver. input takes a string, the others ignore
 * a write, as V8's setters do; each is configurable. */

static const char *const legacy_names[JS_REGEXP_LEGACY_COUNT] = {
    "input", "$_", "lastMatch", "$&", "lastParen", "$+", "leftContext", "$`", "rightContext", "$'",
    "$1", "$2", "$3", "$4", "$5", "$6", "$7", "$8", "$9"};

const char *js_regexp_legacy_name(int idx) {
    return legacy_names[idx];
}

static bool re_legacy_get(JsContext *ctx, int idx, JsValue *out) {
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

static bool re_legacy_set(JsContext *ctx, int idx, JsValue v) {
    if (idx > 1)
        return true; /* V8's setter does nothing */
    JsString *str = js_op_to_string(ctx, v);
    if (!str)
        return false;
    ctx->re_input = js_value_from_cell(&str->gc);
    return true;
}

/* re.compile(pattern, flags): this regexp recompiled in place (Annex B), lastIndex 0 */
static bool rexp_compile(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (!js_regexp_is(tv))
        return (*r = js_error_receiver(ctx, "RegExp", tv)), false;
    JsValue pv = ARG(0), fv = ARG(1);
    if (js_regexp_is(pv) && !js_is_undefined(fv))
        return re_throw(ctx, r, "TypeError: Cannot supply flags when constructing one RegExp from another");
    JsValue rev;
    js_gc_protect(ctx->vm, &tv);
    bool ok = regexp_create(ctx, pv, fv, false, &rev);
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
    /* its properties in V8's order: constructor, exec, the accessors, compile, toString, test */
    JsValue ctor = js_native_new(ctx, "RegExp", g_RegExp, NULL);
    if (!js_is_function(ctor) ||
        !js_object_set_ascii(ctx, ctx->regexp_proto, "constructor", ctor) ||
        !js_object_set_ascii(ctx, ctx->globals, "RegExp", ctor))
        return false;
    js_native_builtin_ctor(ctor, ctx->regexp_proto);
    ctx->regexp_ctor = (JsNative *)js_value_cell(ctor); /* reachable from the globals */
    static const char *const getters[] = {"dotAll", "flags", "global", "hasIndices", "ignoreCase",
                                          "multiline", "source", "sticky", "unicode", "unicodeSets"};
    if (!def_method(ctx, ctx->regexp_proto, "exec", rexp_exec))
        return false;
    for (unsigned i = 0; i < 10; i++)
        if (!js_object_set_accessor(ctx, ctx->regexp_proto, getters[i], JS_ACC_RE_DOTALL + i, 0))
            return false;
    if (!def_method(ctx, ctx->regexp_proto, "compile", rexp_compile) ||
        !def_method(ctx, ctx->regexp_proto, "toString", rexp_toString) ||
        !def_method(ctx, ctx->regexp_proto, "test", rexp_test))
        return false;
    JsValue st = js_object_new(ctx);
    if (!js_is_object(st))
        return false;
    JsObject *statics = js_value_object(st);
    ((JsNative *)js_value_cell(ctor))->statics = statics;
    for (unsigned i = 0; i < JS_REGEXP_LEGACY_COUNT; i++)
        if (!js_object_set_accessor(ctx, statics, legacy_names[i], JS_ACC_RE_LEGACY_GET + i,
                                    JS_ACC_RE_LEGACY_SET + i))
            return false;
    if (!def_method(ctx, statics, "escape", re_escape))
        return false;
    /* what the fast paths take to be as it was made (re_pristine) */
    static const char *const watched[] = {
        [JS_INTACT_RE_EXEC] = "exec",           [JS_INTACT_RE_FLAGS] = "flags",
        [JS_INTACT_RE_HASINDICES] = "hasIndices", [JS_INTACT_RE_GLOBAL] = "global",
        [JS_INTACT_RE_IGNORECASE] = "ignoreCase", [JS_INTACT_RE_MULTILINE] = "multiline",
        [JS_INTACT_RE_DOTALL] = "dotAll",       [JS_INTACT_RE_UNICODE] = "unicode",
        [JS_INTACT_RE_UNICODESETS] = "unicodeSets", [JS_INTACT_RE_STICKY] = "sticky",
        [JS_INTACT_RE_CONSTRUCTOR] = "constructor", [JS_INTACT_RE_SOURCE] = "source",
    };
    for (unsigned id = 0; id < sizeof watched / sizeof watched[0]; id++)
        if (watched[id] && !js_intact_watch(ctx, id, ctx->regexp_proto, watched[id]))
            return false;
    return true;
}

#else /* !LAMASSU_HAS_REGEX: keep the translation unit non-empty */
typedef int js_regexp_disabled;
#endif
