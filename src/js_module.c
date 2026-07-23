/*
 * ES modules: an async fetch graph feeding the synchronous
 * instantiate -> link -> evaluate pipeline.
 *
 * Every module — the root included — arrives through the host's async
 * loader (JsModuleLoader): the loader returns a promise that fulfills with
 * source text (compiled here), precompiled bytecode (js_bytecode_value), or
 * a plain object adopted directly as the module's exports (a synthetic leaf,
 * e.g. an imported stylesheet). Specifiers pass through the optional
 * synchronous canonicalizer first; the canonical specifier is the registry
 * identity, so cyclic and diamond graphs resolve to shared records and each
 * module is fetched exactly once.
 *
 * The pipeline mirrors the spec's split of async HostLoadImportedModule
 * from synchronous Link: js_eval_module walks the static import graph
 * asynchronously (a shared-counter graph load — cycle-proof because each
 * module is counted once, not awaited per-subtree), and only when every
 * transitive dependency has arrived does it run the unchanged synchronous
 * instantiate/link. Evaluation is promise-chained (deps strictly
 * left-to-right, as the spec requires) so a top-level-await body that
 * suspends on a host promise keeps the whole graph's completion pending
 * until it genuinely finishes.
 *
 * Orchestration reuses the promise machinery itself: state objects + bound
 * natives via js_promise_then, the same idiom as Promise.all in
 * js_promise.c. JsModule cells ride through captures disguised as JsValues
 * (they tag as objects via js_value_from_cell) — never hand one to script.
 *
 * Live bindings: an importer reads `source.exports[name]` at each use
 * (GET_IMPORT), so a mutation in the exporting module is observed. Star
 * re-exports are snapshot-copied once the source has evaluated.
 */
#include "js_bytecode.h"
#include "lamassu_internal.h"

/* ---- GC helpers ---- */

void js_gc_mark_module(JsVm *vm, JsModule *m) {
    if (m->specifier)
        js_gc_mark_cell(vm, &m->specifier->gc);
    if (m->body)
        js_gc_mark_cell(vm, &m->body->gc);
    if (m->exports)
        js_gc_mark_cell(vm, &m->exports->gc);
    if (m->fetch_promise)
        js_gc_mark_cell(vm, &m->fetch_promise->gc);
    js_gc_mark_value(vm, m->eval_error);
    for (uint32_t i = 0; i < m->import_count; i++) {
        if (m->imports[i].specifier)
            js_gc_mark_cell(vm, &m->imports[i].specifier->gc);
        if (m->imports[i].imported_name)
            js_gc_mark_cell(vm, &m->imports[i].imported_name->gc);
        if (m->imports[i].source)
            js_gc_mark_cell(vm, &m->imports[i].source->gc);
    }
    for (uint32_t i = 0; i < m->star_count; i++) {
        if (m->stars[i].specifier)
            js_gc_mark_cell(vm, &m->stars[i].specifier->gc);
        if (m->stars[i].source)
            js_gc_mark_cell(vm, &m->stars[i].source->gc);
        if (m->stars[i].imported)
            js_gc_mark_cell(vm, &m->stars[i].imported->gc);
        if (m->stars[i].exported)
            js_gc_mark_cell(vm, &m->stars[i].exported->gc);
    }
    for (uint32_t i = 0; i < m->dep_spec_count; i++)
        if (m->dep_specs[i])
            js_gc_mark_cell(vm, &m->dep_specs[i]->gc);
    for (uint32_t i = 0; i < m->dep_count; i++)
        if (m->deps[i])
            js_gc_mark_cell(vm, &m->deps[i]->gc);
}

void js_module_free_cell(JsVm *vm, JsModule *m) {
    js_realloc_raw(vm, m->imports, (size_t)m->import_count * sizeof(JsModuleImport), 0);
    js_realloc_raw(vm, m->stars, (size_t)m->star_count * sizeof(JsStarExport), 0);
    js_realloc_raw(vm, m->dep_specs, (size_t)m->dep_spec_count * sizeof(JsString *), 0);
    js_realloc_raw(vm, m->deps, (size_t)m->dep_count * sizeof(JsModule *), 0);
}

void js_gc_mark_module_registry(JsContext *ctx) {
    for (uint32_t i = 0; i < ctx->module_count; i++)
        if (ctx->modules[i])
            js_gc_mark_cell(ctx->vm, &ctx->modules[i]->gc);
}

static bool str_is_default(const JsString *s) {
    static const char d[] = "default";
    if (s->length != 7)
        return false;
    for (uint32_t i = 0; i < 7; i++)
        if (s->units[i] != (uint16_t)(unsigned char)d[i])
            return false;
    return true;
}

void js_modules_free_registry(JsContext *ctx) {
    js_realloc_raw(ctx->vm, ctx->modules, (size_t)ctx->module_cap * sizeof(JsModule *), 0);
    ctx->modules = NULL;
    ctx->module_count = ctx->module_cap = 0;
}

/* ---- registry ---- */

static JsModule *registry_find(JsContext *ctx, JsString *specifier) {
    for (uint32_t i = 0; i < ctx->module_count; i++)
        if (ctx->modules[i]->specifier == specifier)
            return ctx->modules[i];
    return NULL;
}

static bool registry_add(JsContext *ctx, JsModule *m) {
    if (ctx->module_count == ctx->module_cap) {
        uint32_t ncap = ctx->module_cap ? ctx->module_cap * 2 : 8;
        JsModule **na = js_realloc_raw(ctx->vm, ctx->modules,
                                       (size_t)ctx->module_cap * sizeof(JsModule *),
                                       (size_t)ncap * sizeof(JsModule *));
        if (!na)
            return false;
        ctx->modules = na;
        ctx->module_cap = ncap;
    }
    ctx->modules[ctx->module_count++] = m;
    return true;
}

/* ---- small helpers ---- */

typedef struct {
    const char *msg;
    uint32_t pos;
    bool oom;
} ModError;

/* JsModule cells tag as objects in js_value_from_cell — these make the
 * existing disguise convention explicit for promise captures/fulfillments.
 * Only for js_gc_protect / bound-native plumbing within this file. */
static JsValue module_value(JsModule *m) {
    return js_value_from_cell(&m->gc);
}
static JsModule *value_module(JsValue v) {
    return (JsModule *)js_value_cell(v);
}

static JsValue ascii_value(JsContext *ctx, const char *msg) {
    JsString *s = js_ascii_cell(ctx->vm, msg);
    return s ? js_value_from_cell(&s->gc) : js_undefined();
}

static JsValue mod_error_value(JsContext *ctx, const ModError *err) {
    const char *msg = err->oom ? "out of memory" : (err->msg ? err->msg : "module error");
    return ascii_value(ctx, msg);
}

/* Fresh promise already settled with v; undefined on OOM. */
static JsValue settled_promise(JsContext *ctx, bool fulfilled, JsValue v) {
    js_gc_protect(ctx->vm, &v);
    JsPromise *p = js_promise_alloc(ctx);
    js_gc_unprotect(ctx->vm, &v);
    if (!p)
        return js_undefined();
    if (fulfilled)
        js_promise_fulfill(ctx, p, v);
    else
        js_promise_reject(ctx, p, v);
    return js_value_from_cell(&p->gc);
}

static JsValue rejected_ascii(JsContext *ctx, const char *msg) {
    return settled_promise(ctx, false, ascii_value(ctx, msg));
}

/* read/write orchestration-state fields by ascii key (the Promise.all idiom) */
static JsValue mstate_get(JsContext *ctx, JsObject *st, const char *k) {
    JsString *ks = js_ascii_cell(ctx->vm, k);
    if (!ks)
        return js_undefined();
    JsString *ik = js_intern_cell(ctx->vm, ks);
    if (!ik)
        return js_undefined();
    bool found;
    return js_map_get(&st->props, ik, &found);
}

static void mstate_set(JsContext *ctx, JsObject *st, const char *k, JsValue v) {
    js_object_set_ascii(ctx, st, k, v);
}

/* Attaches C continuations to a promise; false on OOM (nothing attached). */
static bool then_bound(JsContext *ctx, JsValue promise, JsBoundFn on_f, JsBoundFn on_r,
                       JsValue bound) {
    JsVm *vm = ctx->vm;
    js_gc_protect(vm, &promise);
    js_gc_protect(vm, &bound);
    JsValue onf = js_bound_native_new(ctx, on_f, bound);
    js_gc_protect(vm, &onf);
    JsValue onr = js_bound_native_new(ctx, on_r, bound);
    bool ok = js_is_function(onf) && js_is_function(onr) &&
              js_promise_then(ctx, js_value_promise(promise), onf, onr) != NULL;
    js_gc_unprotect(vm, &onf);
    js_gc_unprotect(vm, &bound);
    js_gc_unprotect(vm, &promise);
    return ok;
}

/* ---- module construction ---- */

/* Bare module cell for `specifier`, status FETCHING, nothing else set.
 * Not registered; NULL on OOM. */
static JsModule *module_alloc_placeholder(JsContext *ctx, JsString *specifier) {
    JsVm *vm = ctx->vm;
    JsValue specv = js_value_from_cell(&specifier->gc);
    js_gc_protect(vm, &specv);
    JsGcCell *cell = js_gc_new_cell(vm, JS_KIND_MODULE, sizeof(JsModule));
    js_gc_unprotect(vm, &specv);
    if (!cell)
        return NULL;
    JsModule *m = (JsModule *)cell;
    memset((char *)m + sizeof(JsGcCell), 0, sizeof *m - sizeof(JsGcCell));
    m->specifier = specifier;
    m->eval_error = js_undefined();
    m->status = JS_MOD_FETCHING;
    return m;
}

/* Parses + compiles `source` into m (exports object, body, import/star/dep
 * metadata). m must be rooted by the caller (registry or protected slot). */
static bool module_compile_source(JsContext *ctx, JsModule *m, const uint16_t *source,
                                  size_t source_len, ModError *err) {
    JsVm *vm = ctx->vm;
    JsValue mv = module_value(m);
    js_gc_protect(vm, &mv);

    /* exports (namespace) object. Real ESM's Module Namespace Exotic Object
     * has [[Prototype]] === null (not Object.prototype like an ordinary
     * object) — js_object_new(ctx) gives every new object Object.prototype
     * by default, so undo that here to match spec exactly. */
    JsValue exports = js_object_new(ctx);
    if (!js_is_object(exports)) {
        js_gc_unprotect(vm, &mv);
        err->oom = true;
        return false;
    }
    js_value_object(exports)->proto = js_undefined();
    value_module(mv)->exports = js_value_object(exports);

    JsArena arena;
    js_arena_init(&arena, vm);
    JsParseResult pr;
    JsFunctionCell *body = NULL;
    if (!js_parse_module(&arena, source, source_len, &pr)) {
        err->msg = pr.err_msg;
        err->pos = pr.err_pos;
    } else {
        JsCompileError ce;
        body = js_compile_module_body(ctx, pr.module, value_module(mv), &ce);
        if (!body) {
            err->msg = ce.msg;
            err->pos = ce.pos;
        }
    }
    js_arena_free(&arena);
    js_gc_unprotect(vm, &mv);
    if (!body)
        return false;
    m->body = body;
    return true;
}

/* ---- canonicalization ---- */

/*
 * Maps a raw specifier + referrer to the canonical (interned) specifier
 * that is the registry identity. NULL canonicalizer -> raw is canonical.
 * NULL on failure with *errmsg set (static ASCII).
 */
static JsString *canonicalize_spec(JsContext *ctx, JsModule *referrer, JsString *raw,
                                   const char **errmsg) {
    if (!ctx->canon)
        return raw;
    const uint16_t *out;
    size_t out_len;
    if (!ctx->canon(ctx->loader_ud, raw->units, raw->length,
                    referrer ? referrer->specifier->units : NULL,
                    referrer ? referrer->specifier->length : 0, &out, &out_len)) {
        *errmsg = "cannot resolve module specifier";
        return NULL;
    }
    JsValue v = js_atom(ctx->vm, out, out_len);
    if (!js_is_string(v)) {
        *errmsg = "out of memory";
        return NULL;
    }
    return js_value_string(v);
}

/* ---- shallow fetch (one module: loader -> compiled/adopted) ---- */

/* Marks m failed and rejects its fetch promise (double-settle no-ops). */
static void module_fetch_fail(JsContext *ctx, JsModule *m, JsValue reason, uint32_t pos) {
    m->status = JS_MOD_ERRORED;
    m->eval_error = reason;
    ctx->error_pos = pos;
    js_promise_reject(ctx, m->fetch_promise, reason);
}

static bool fetch_on_resolved(JsContext *ctx, JsValue bound, JsValue tv,
                              const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    JsModule *m = value_module(bound);
    JsValue resolved = argc > 0 ? args[0] : js_undefined();

    if (js_is_string(resolved)) {
        JsString *src = js_value_string(resolved);
        ModError err = {NULL, 0, false};
        if (!module_compile_source(ctx, m, src->units, src->length, &err)) {
            module_fetch_fail(ctx, m, mod_error_value(ctx, &err), err.pos);
            return true;
        }
    } else if (js_value_is_cell(resolved) &&
               js_value_cell(resolved)->kind == JS_KIND_BYTECODE) {
        JsBytecode *bc = (JsBytecode *)js_value_cell(resolved);
        const char *lerr = NULL;
        if (!js_bc_load_module(ctx, m, bc->bytes, bc->length, &lerr)) {
            module_fetch_fail(ctx, m, ascii_value(ctx, lerr ? lerr : "invalid module bytecode"), 0);
            return true;
        }
    } else if (js_value_is_cell(resolved) &&
               js_value_cell(resolved)->kind == JS_KIND_OBJECT) {
        /* synthetic leaf: the object IS the exports, adopted verbatim
         * (its own [[Prototype]] included) — no parse, no deps */
        m->exports = (JsObject *)js_value_cell(resolved);
        m->synthetic = true;
        m->status = JS_MOD_EVALUATED;
        js_promise_fulfill(ctx, m->fetch_promise, bound);
        return true;
    } else {
        module_fetch_fail(ctx, m,
                          ascii_value(ctx, "module loader must resolve with source text, "
                                           "module bytecode, or an exports object"),
                          0);
        return true;
    }
    m->status = JS_MOD_UNLINKED;
    js_promise_fulfill(ctx, m->fetch_promise, bound);
    return true;
}

static bool fetch_on_rejected(JsContext *ctx, JsValue bound, JsValue tv,
                              const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    module_fetch_fail(ctx, value_module(bound), argc > 0 ? args[0] : js_undefined(), 0);
    return true;
}

/*
 * The per-module shallow-fetch promise for canonical `spec`: settles once
 * the module's source/bytecode is compiled or its synthetic exports adopted
 * (dep_specs known; dependencies NOT yet fetched — the graph loader below
 * composes these). Fulfills with the module (disguised), rejects with the
 * load error. Exactly one loader call per canonical specifier, ever: an
 * in-flight FETCHING entry is joined via its stored promise, a settled
 * entry yields an already-settled one. Undefined only on OOM.
 */
static JsValue fetch_module_async(JsContext *ctx, JsModule *referrer, JsString *spec) {
    JsVm *vm = ctx->vm;
    JsModule *existing = registry_find(ctx, spec);
    if (existing) {
        if (existing->status == JS_MOD_FETCHING)
            return js_value_from_cell(&existing->fetch_promise->gc);
        if (existing->status == JS_MOD_ERRORED)
            return settled_promise(ctx, false, existing->eval_error);
        return settled_promise(ctx, true, module_value(existing));
    }
    if (!ctx->loader)
        return rejected_ascii(ctx, "no module loader set");

    JsModule *m = module_alloc_placeholder(ctx, spec);
    if (!m)
        return js_undefined();
    JsValue mv = module_value(m);
    js_gc_protect(vm, &mv);
    JsPromise *fp = js_promise_alloc(ctx);
    bool added = fp != NULL;
    if (added) {
        value_module(mv)->fetch_promise = fp;
        added = registry_add(ctx, value_module(mv));
    }
    js_gc_unprotect(vm, &mv);
    if (!added)
        return js_undefined();
    /* m (and fp through it) is now rooted by the registry */

    m = value_module(mv);
    JsValue loader_p = ctx->loader(ctx->loader_ud, ctx, spec->units, spec->length,
                                   referrer ? referrer->specifier->units : NULL,
                                   referrer ? referrer->specifier->length : 0);
    if (!js_is_promise(loader_p)) {
        module_fetch_fail(ctx, m, ascii_value(ctx, "module loader did not return a promise"), 0);
    } else if (!then_bound(ctx, loader_p, fetch_on_resolved, fetch_on_rejected, mv)) {
        module_fetch_fail(ctx, m, ascii_value(ctx, "out of memory"), 0);
    }
    return js_value_from_cell(&m->fetch_promise->gc);
}

/* ---- graph load (transitive fetch, shared-counter — cycle-proof) ---- */

/* state: { remaining: number, promise: op promise, root: canonical spec
 * string, seen: object whose props are this operation's processed specs } */

static bool graph_enqueue(JsContext *ctx, JsValue statev, JsModule *referrer,
                          JsString *raw_spec);

static void graph_fail(JsContext *ctx, JsObject *state, JsValue reason) {
    /* reason is typically a just-allocated string held only here; mstate_get
     * allocates and could otherwise collect it before the reject stores it. */
    js_gc_protect(ctx->vm, &reason);
    JsValue pv = mstate_get(ctx, state, "promise");
    if (js_is_promise(pv))
        js_promise_reject(ctx, js_value_promise(pv), reason); /* re-settle no-ops */
    js_gc_unprotect(ctx->vm, &reason);
}

static bool graph_on_fetched(JsContext *ctx, JsValue bound, JsValue tv,
                             const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    if (argc < 1)
        return true;
    JsObject *state = js_value_object(bound);
    JsModule *m = value_module(args[0]);
    /* discover this module's dependencies before decrementing, so the
     * counter can't hit zero with work still unprocessed */
    for (uint32_t i = 0; i < m->dep_spec_count; i++) {
        if (!graph_enqueue(ctx, bound, m, m->dep_specs[i])) {
            graph_fail(ctx, state, ascii_value(ctx, "out of memory"));
            return true;
        }
    }
    double rem = js_to_number_value(ctx, mstate_get(ctx, state, "remaining")) - 1;
    mstate_set(ctx, state, "remaining", js_number(rem));
    if (rem == 0) {
        JsValue rootspec = mstate_get(ctx, state, "root");
        JsModule *root = js_is_string(rootspec)
                             ? registry_find(ctx, js_value_string(rootspec))
                             : NULL;
        JsValue pv = mstate_get(ctx, state, "promise");
        if (root && js_is_promise(pv))
            js_promise_fulfill(ctx, js_value_promise(pv), module_value(root));
    }
    return true;
}

static bool graph_on_failed(JsContext *ctx, JsValue bound, JsValue tv,
                            const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    graph_fail(ctx, js_value_object(bound), argc > 0 ? args[0] : js_undefined());
    return true;
}

/*
 * Adds one module (by raw specifier, canonicalized against `referrer`;
 * already-canonical when referrer is NULL) to the operation: dedupes against
 * the op's seen set, bumps the counter, and chains discovery of its own
 * deps onto its shallow fetch. false only on OOM.
 */
static bool graph_enqueue(JsContext *ctx, JsValue statev, JsModule *referrer,
                          JsString *raw_spec) {
    JsObject *state = js_value_object(statev);
    const char *cerr = NULL;
    JsString *spec = referrer ? canonicalize_spec(ctx, referrer, raw_spec, &cerr) : raw_spec;
    if (!spec) {
        graph_fail(ctx, state, ascii_value(ctx, cerr));
        return true; /* op failed; not an OOM to propagate */
    }
    JsValue seenv = mstate_get(ctx, state, "seen");
    if (!js_is_object(seenv))
        return false;
    JsObject *seen = js_value_object(seenv);
    bool found;
    js_map_get(&seen->props, spec, &found);
    if (found)
        return true;
    if (!js_map_set(ctx->vm, &seen->props, spec, js_number(1)))
        return false;
    double rem = js_to_number_value(ctx, mstate_get(ctx, state, "remaining"));
    mstate_set(ctx, state, "remaining", js_number(rem + 1));
    JsValue fp = fetch_module_async(ctx, referrer, spec);
    if (!js_is_promise(fp))
        return false;
    return then_bound(ctx, fp, graph_on_fetched, graph_on_failed, statev);
}

/*
 * Promise that fulfills with the root module once the whole transitive
 * static-import graph rooted at canonical `root_spec` has been fetched
 * (compiled/adopted, not yet instantiated). Undefined only on OOM.
 */
static JsValue load_graph(JsContext *ctx, JsString *root_spec) {
    JsVm *vm = ctx->vm;
    JsValue rootv = js_value_from_cell(&root_spec->gc);
    js_gc_protect(vm, &rootv);
    JsValue statev = js_object_new(ctx);
    js_gc_protect(vm, &statev);
    JsValue seenv = js_object_new(ctx);
    js_gc_protect(vm, &seenv);
    JsPromise *op = js_promise_alloc(ctx);
    JsValue opv = op ? js_value_from_cell(&op->gc) : js_undefined();
    js_gc_protect(vm, &opv);
    bool ok = js_is_object(statev) && js_is_object(seenv) && op != NULL;
    if (ok) {
        JsObject *state = js_value_object(statev);
        mstate_set(ctx, state, "remaining", js_number(0));
        mstate_set(ctx, state, "promise", opv);
        mstate_set(ctx, state, "root", rootv);
        mstate_set(ctx, state, "seen", seenv);
        if (!graph_enqueue(ctx, statev, NULL, root_spec))
            js_promise_reject(ctx, op, ascii_value(ctx, "out of memory"));
    }
    js_gc_unprotect(vm, &opv);
    js_gc_unprotect(vm, &seenv);
    js_gc_unprotect(vm, &statev);
    js_gc_unprotect(vm, &rootv);
    return ok ? opv : js_undefined();
}

/* ---- instantiate (resolve dependency graph; sync, post-fetch) ---- */

static JsModule *instantiate(JsContext *ctx, JsModule *m, ModError *err);

static bool resolve_dep(JsContext *ctx, JsModule *referrer, JsString *raw_spec,
                        JsModule **out, ModError *err) {
    /* The graph load already fetched and registered every transitive
     * dependency before instantiate runs — this is a pure lookup. The
     * canonicalizer is required to be deterministic, so re-canonicalizing
     * the raw source-text specifier here hits the same registry key. */
    const char *cerr = NULL;
    JsString *spec = canonicalize_spec(ctx, referrer, raw_spec, &cerr);
    if (!spec) {
        err->msg = cerr;
        err->pos = 0;
        return false;
    }
    JsModule *dep = registry_find(ctx, spec);
    if (!dep || dep->status == JS_MOD_FETCHING) {
        err->msg = "internal error: dependency was not fetched";
        err->pos = 0;
        return false;
    }
    if (dep->status == JS_MOD_ERRORED) {
        err->msg = "dependency failed to load";
        err->pos = 0;
        return false;
    }
    dep = instantiate(ctx, dep, err);
    *out = dep;
    return dep != NULL;
}

static JsModule *instantiate(JsContext *ctx, JsModule *m, ModError *err) {
    if (m->status >= JS_MOD_LINKED || m->deps) /* already instantiated */
        return m;
    if (m->dep_spec_count == 0)
        return m;
    JsValue mv = js_value_from_cell(&m->gc);
    js_gc_protect(ctx->vm, &mv);
    JsModule **deps = js_realloc_raw(ctx->vm, NULL, 0,
                                     (size_t)m->dep_spec_count * sizeof(JsModule *));
    bool ok = deps != NULL;
    if (ok) {
        m = value_module(mv);
        m->deps = deps;
        m->dep_count = m->dep_spec_count;
        for (uint32_t i = 0; i < m->dep_spec_count; i++)
            m->deps[i] = NULL;
        for (uint32_t i = 0; i < m->dep_spec_count && ok; i++) {
            JsModule *dep;
            if (!resolve_dep(ctx, value_module(mv), value_module(mv)->dep_specs[i],
                             &dep, err)) {
                ok = false;
                break;
            }
            value_module(mv)->deps[i] = dep;
        }
    } else {
        err->oom = true;
    }
    m = value_module(mv);
    js_gc_unprotect(ctx->vm, &mv);
    return ok ? m : NULL;
}

/* ---- link (bind imports/stars to source modules) ---- */

static JsModule *dep_for_spec(JsModule *m, JsString *spec) {
    for (uint32_t i = 0; i < m->dep_count; i++)
        if (m->dep_specs[i] == spec)
            return m->deps[i];
    return NULL;
}

static bool link_module(JsContext *ctx, JsModule *m, ModError *err) {
    (void)ctx;
    if (m->status >= JS_MOD_LINKED || m->status == JS_MOD_LINKING)
        return true; /* linked or in-progress (cycle) */
    m->status = JS_MOD_LINKING;
    for (uint32_t i = 0; i < m->import_count; i++) {
        JsModule *src = dep_for_spec(m, m->imports[i].specifier);
        if (!src) {
            err->msg = "unresolved import";
            err->pos = 0;
            return false;
        }
        m->imports[i].source = src;
    }
    for (uint32_t i = 0; i < m->star_count; i++) {
        JsModule *src = dep_for_spec(m, m->stars[i].specifier);
        if (!src) {
            err->msg = "unresolved re-export";
            err->pos = 0;
            return false;
        }
        m->stars[i].source = src;
    }
    for (uint32_t i = 0; i < m->dep_count; i++)
        if (m->deps[i] && !link_module(ctx, m->deps[i], err))
            return false;
    m->status = JS_MOD_LINKED;
    return true;
}

/* ---- evaluate (promise-chained; deps strictly left-to-right) ---- */

/* Applies one re-export from `src` (already evaluated) into dst->exports. */
static bool apply_reexport(JsContext *ctx, JsModule *dst, const JsStarExport *re) {
    JsModule *src = re->source;
    if (!src || !src->exports)
        return true;
    JsObject *doo = dst->exports;
    if (re->kind == JS_REEXP_NS) {
        /* export * as ns: bind ns to the source namespace (live by reference) */
        return js_map_set(ctx->vm, &doo->props, re->exported,
                          js_value_from_cell(&src->exports->gc));
    }
    if (re->kind == JS_REEXP_NAMED) {
        bool found;
        JsValue v = js_map_get(&src->exports->props, re->imported, &found);
        return js_map_set(ctx->vm, &doo->props, re->exported, found ? v : js_undefined());
    }
    /* export * from: copy all named exports (excluding default) */
    JsObject *so = src->exports;
    for (uint32_t i = 0; i < so->props.capacity; i++) {
        JsMapEntry *e = &so->props.entries[i];
        if (!e->key || e->key == JS_MAP_TOMBSTONE || str_is_default(e->key))
            continue;
        if (!js_map_set(ctx->vm, &doo->props, e->key, e->value))
            return false;
    }
    return true;
}

/* chain state: { module, index: next dep to evaluate, promise } */

static JsValue evaluate(JsContext *ctx, JsModule *m);
static bool eval_step(JsContext *ctx, JsValue statev);

static void eval_fail_state(JsContext *ctx, JsObject *state, JsValue reason) {
    /* Root reason across the mstate_get allocations below; it is usually a
     * fresh string held only in this frame until stored/rejected. */
    js_gc_protect(ctx->vm, &reason);
    JsModule *m = value_module(mstate_get(ctx, state, "module"));
    m->status = JS_MOD_ERRORED;
    m->eval_error = reason;
    JsValue pv = mstate_get(ctx, state, "promise");
    if (js_is_promise(pv))
        js_promise_reject(ctx, js_value_promise(pv), reason);
    js_gc_unprotect(ctx->vm, &reason);
}

static bool eval_on_dep_done(JsContext *ctx, JsValue bound, JsValue tv,
                             const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    (void)args;
    (void)argc;
    *r = js_undefined();
    JsObject *state = js_value_object(bound);
    double i = js_to_number_value(ctx, mstate_get(ctx, state, "index"));
    mstate_set(ctx, state, "index", js_number(i + 1));
    eval_step(ctx, bound);
    return true;
}

static bool eval_on_fail(JsContext *ctx, JsValue bound, JsValue tv,
                         const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    eval_fail_state(ctx, js_value_object(bound), argc > 0 ? args[0] : js_undefined());
    return true;
}

static bool eval_on_body_done(JsContext *ctx, JsValue bound, JsValue tv,
                              const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    (void)args;
    (void)argc;
    *r = js_undefined();
    JsObject *state = js_value_object(bound);
    JsModule *m = value_module(mstate_get(ctx, state, "module"));
    m->status = JS_MOD_EVALUATED;
    JsValue pv = mstate_get(ctx, state, "promise");
    if (js_is_promise(pv))
        js_promise_fulfill(ctx, js_value_promise(pv), module_value(m));
    return true;
}

/*
 * Advances m's evaluation from state.index: already-settled dependency
 * evaluations are folded inline (so a fully-synchronous graph completes
 * within one call), a pending one parks the chain on its promise. After the
 * last dep: re-exports (their sources have now run), then the body — whose
 * own completion promise (js_run_module) may itself be pending on
 * top-level await.
 */
static bool eval_step(JsContext *ctx, JsValue statev) {
    JsObject *state = js_value_object(statev);
    JsModule *m = value_module(mstate_get(ctx, state, "module"));
    uint32_t i = (uint32_t)js_to_number_value(ctx, mstate_get(ctx, state, "index"));
    for (; i < m->dep_count; i++) {
        JsModule *dep = m->deps[i];
        if (!dep)
            continue;
        JsValue dp = evaluate(ctx, dep);
        int st = js_promise_state(dp);
        if (st == 1)
            continue;
        if (st == 2) {
            eval_fail_state(ctx, state, js_promise_result(dp));
            return true;
        }
        if (st < 0) {
            eval_fail_state(ctx, state, ascii_value(ctx, "out of memory"));
            return true;
        }
        mstate_set(ctx, state, "index", js_number(i));
        if (!then_bound(ctx, dp, eval_on_dep_done, eval_on_fail, statev))
            eval_fail_state(ctx, state, ascii_value(ctx, "out of memory"));
        return true;
    }
    mstate_set(ctx, state, "index", js_number(i));

    m = value_module(mstate_get(ctx, state, "module"));
    for (uint32_t s = 0; s < m->star_count; s++) {
        if (!apply_reexport(ctx, m, &m->stars[s])) {
            eval_fail_state(ctx, state, ascii_value(ctx, "out of memory"));
            return true;
        }
    }

    JsValue bodyv = js_value_from_cell(&m->body->gc);
    JsValue bp = js_run_module(ctx, bodyv);
    int st = js_promise_state(bp);
    if (st == 1) {
        m->status = JS_MOD_EVALUATED;
        JsValue pv = mstate_get(ctx, state, "promise");
        if (js_is_promise(pv))
            js_promise_fulfill(ctx, js_value_promise(pv), module_value(m));
        return true;
    }
    if (st == 2) {
        eval_fail_state(ctx, state, js_promise_result(bp));
        return true;
    }
    if (st < 0) {
        eval_fail_state(ctx, state, ascii_value(ctx, "out of memory"));
        return true;
    }
    /* body suspended on top-level await: finish when it settles */
    if (!then_bound(ctx, bp, eval_on_body_done, eval_on_fail, statev))
        eval_fail_state(ctx, state, ascii_value(ctx, "out of memory"));
    return true;
}

/*
 * Promise for m's evaluation: fulfills with the module once its deps (in
 * order) and its own body have run to true completion; rejects with the
 * thrown error. A module already EVALUATING yields an immediately-fulfilled
 * promise — the spec's cycle semantics (bindings visible, values maybe not
 * yet); as today, that also means a second concurrent evaluation of the
 * same module doesn't wait for the first.
 */
static JsValue evaluate(JsContext *ctx, JsModule *m) {
    JsVm *vm = ctx->vm;
    if (m->status == JS_MOD_EVALUATED)
        return settled_promise(ctx, true, module_value(m));
    if (m->status == JS_MOD_ERRORED)
        return settled_promise(ctx, false, m->eval_error);
    if (m->status == JS_MOD_EVALUATING)
        return settled_promise(ctx, true, module_value(m));
    m->status = JS_MOD_EVALUATING;

    JsValue mv = module_value(m);
    js_gc_protect(vm, &mv);
    JsValue statev = js_object_new(ctx);
    js_gc_protect(vm, &statev);
    JsPromise *ep = js_promise_alloc(ctx);
    JsValue epv = ep ? js_value_from_cell(&ep->gc) : js_undefined();
    js_gc_protect(vm, &epv);
    bool ok = js_is_object(statev) && ep != NULL;
    if (ok) {
        JsObject *state = js_value_object(statev);
        mstate_set(ctx, state, "module", mv);
        mstate_set(ctx, state, "index", js_number(0));
        mstate_set(ctx, state, "promise", epv);
        eval_step(ctx, statev);
    }
    js_gc_unprotect(vm, &epv);
    js_gc_unprotect(vm, &statev);
    js_gc_unprotect(vm, &mv);
    return ok ? epv : js_undefined();
}

/* ---- public entry ---- */

static bool root_on_eval_done(JsContext *ctx, JsValue bound, JsValue tv,
                              const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    JsPromise *done = js_value_promise(bound);
    if (argc > 0) {
        JsModule *m = value_module(args[0]);
        js_promise_fulfill(ctx, done, js_value_from_cell(&m->exports->gc));
    }
    return true;
}

static bool root_on_eval_fail(JsContext *ctx, JsValue bound, JsValue tv,
                              const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    js_promise_reject(ctx, js_value_promise(bound), argc > 0 ? args[0] : js_undefined());
    return true;
}

/* graph fetched: run the synchronous instantiate/link, then evaluation */
static bool root_on_fetched(JsContext *ctx, JsValue bound, JsValue tv,
                            const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    JsPromise *done = js_value_promise(bound);
    if (argc < 1)
        return true;
    JsModule *root = value_module(args[0]);

    ModError err = {NULL, 0, false};
    JsModule *inst = instantiate(ctx, root, &err);
    if (inst && !link_module(ctx, inst, &err))
        inst = NULL;
    if (!inst) {
        js_promise_reject(ctx, done, mod_error_value(ctx, &err));
        return true;
    }

    JsValue ep = evaluate(ctx, root);
    int st = js_promise_state(ep);
    if (st == 1) {
        js_promise_fulfill(ctx, done, js_value_from_cell(&root->exports->gc));
    } else if (st == 2) {
        js_promise_reject(ctx, done, js_promise_result(ep));
    } else if (st < 0 ||
               !then_bound(ctx, ep, root_on_eval_done, root_on_eval_fail, bound)) {
        js_promise_reject(ctx, done, ascii_value(ctx, "out of memory"));
    }
    return true;
}

JsValue js_eval_module(JsContext *ctx, const uint16_t *specifier, size_t spec_len) {
    JsVm *vm = ctx->vm;
    JsPromise *done = js_promise_alloc(ctx);
    if (!done)
        return js_undefined();
    JsValue donev = js_value_from_cell(&done->gc);
    js_gc_protect(vm, &donev);

    JsValue specv = js_atom(vm, specifier, spec_len);
    if (!js_is_string(specv)) {
        js_promise_reject(ctx, js_value_promise(donev), ascii_value(ctx, "out of memory"));
        js_gc_unprotect(vm, &donev);
        return donev;
    }
    js_gc_protect(vm, &specv);

    const char *cerr = NULL;
    JsString *canon = canonicalize_spec(ctx, NULL, js_value_string(specv), &cerr);
    bool wired = false;
    if (canon) {
        JsValue canonv = js_value_from_cell(&canon->gc);
        js_gc_protect(vm, &canonv);
        JsValue gp = load_graph(ctx, js_value_string(canonv));
        wired = js_is_promise(gp) &&
                then_bound(ctx, gp, root_on_fetched, root_on_eval_fail, donev);
        js_gc_unprotect(vm, &canonv);
        cerr = "out of memory";
    }
    if (!wired)
        js_promise_reject(ctx, js_value_promise(donev), ascii_value(ctx, cerr));

    js_gc_unprotect(vm, &specv);
    /* drain what can complete now, so an all-synchronous graph (settled
     * loader promises throughout) finishes within this call */
    js_run_jobs(ctx);
    js_gc_unprotect(vm, &donev);
    return donev;
}

JsValue js_module_dynamic_import(JsContext *ctx, JsModule *referrer, JsValue specv) {
    JsVm *vm = ctx->vm;
    JsPromise *done = js_promise_alloc(ctx);
    if (!done)
        return js_undefined();
    JsValue donev = js_value_from_cell(&done->gc);
    js_gc_protect(vm, &donev);

    /* ToString + intern the specifier */
    js_gc_protect(vm, &specv);
    JsString *ss = js_to_string_cell(ctx, specv, 0);
    js_gc_unprotect(vm, &specv);
    JsString *spec = NULL;
    if (ss) {
        JsValue ssv = js_value_from_cell(&ss->gc);
        js_gc_protect(vm, &ssv);
        spec = js_intern_cell(vm, ss);
        js_gc_unprotect(vm, &ssv);
    }
    if (!spec) {
        js_promise_reject(ctx, js_value_promise(donev), ascii_value(ctx, "out of memory"));
        js_gc_unprotect(vm, &donev);
        return donev;
    }

    const char *cerr = NULL;
    JsString *canon = canonicalize_spec(ctx, referrer, spec, &cerr);
    bool wired = false;
    if (canon) {
        JsValue canonv = js_value_from_cell(&canon->gc);
        js_gc_protect(vm, &canonv);
        JsValue gp = load_graph(ctx, js_value_string(canonv));
        /* graph fetched -> instantiate/link/evaluate -> settle with exports:
         * exactly the root pipeline js_eval_module uses */
        wired = js_is_promise(gp) &&
                then_bound(ctx, gp, root_on_fetched, root_on_eval_fail, donev);
        js_gc_unprotect(vm, &canonv);
        cerr = "out of memory";
    }
    if (!wired)
        js_promise_reject(ctx, js_value_promise(donev), ascii_value(ctx, cerr));
    js_gc_unprotect(vm, &donev);
    return donev;
}

/* ---- bytecode producer (host-side cache tooling) ---- */

bool js_bytecode_compile_module(JsContext *ctx, const uint16_t *specifier,
                                size_t spec_len, const uint16_t *source,
                                size_t source_len, uint8_t **out, size_t *out_len,
                                const char **err_msg, uint32_t *err_pos) {
    JsVm *vm = ctx->vm;
    *err_msg = NULL;
    *err_pos = 0;

    JsValue specv = js_atom(vm, specifier, spec_len);
    if (!js_is_string(specv)) {
        *err_msg = "out of memory";
        return false;
    }
    js_gc_protect(vm, &specv);

    /* Compile this module alone — unregistered, no resolution, linking, or
     * evaluation — so it can be cached independently of its deps. */
    JsModule *m = module_alloc_placeholder(ctx, js_value_string(specv));
    if (!m) {
        js_gc_unprotect(vm, &specv);
        *err_msg = "out of memory";
        return false;
    }
    JsValue mv = module_value(m);
    js_gc_protect(vm, &mv);
    ModError err = {NULL, 0, false};
    bool ok = module_compile_source(ctx, m, source, source_len, &err);
    if (!ok) {
        *err_msg = err.oom ? "out of memory" : (err.msg ? err.msg : "module compile error");
        *err_pos = err.pos;
    } else {
        ok = js_bc_serialize_module(ctx, value_module(mv), out, out_len);
        if (!ok)
            *err_msg = "bytecode serialization failed";
    }
    js_gc_unprotect(vm, &mv);
    js_gc_unprotect(vm, &specv);
    return ok;
}

JsValue js_module_get_export(JsContext *ctx, JsValue ns, const uint16_t *name,
                             size_t name_len) {
    if (!js_is_object(ns))
        return js_undefined();
    JsValue key = js_atom(ctx->vm, name, name_len);
    if (!js_is_string(key))
        return js_undefined();
    return js_object_get(ctx->vm, ns, key);
}
