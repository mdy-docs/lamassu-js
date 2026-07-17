/*
 * ES modules: the compile -> instantiate -> link -> evaluate pipeline.
 *
 * Each module is a GC cell holding a compiled body, a live `exports` object
 * (exported bindings are properties of it), import descriptors, and star
 * re-export sources. Modules are cached by resolved specifier, so cyclic and
 * diamond graphs resolve to shared records. Evaluation runs bodies in
 * dependency order; a module re-entered while EVALUATING (a cycle) is
 * skipped, matching the spec's "bindings visible, values maybe not yet"
 * behavior.
 *
 * Live bindings: an importer reads `source.exports[name]` at each use
 * (GET_IMPORT), so a mutation in the exporting module is observed. Star
 * re-exports are snapshot-copied once the source has evaluated.
 */
#include "js_bytecode.h"
#include "jsvm_internal.h"

/* ---- GC helpers ---- */

void js_gc_mark_module(JsVm *vm, JsModule *m) {
    if (m->specifier)
        js_gc_mark_cell(vm, &m->specifier->gc);
    if (m->body)
        js_gc_mark_cell(vm, &m->body->gc);
    if (m->exports)
        js_gc_mark_cell(vm, &m->exports->gc);
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

/* ---- compile ---- */

typedef struct {
    const char *msg;
    uint32_t pos;
    bool oom;
} ModError;

/*
 * Returns the cached module for `specifier`, else parses+compiles `source`
 * into a fresh (unlinked) module and caches it. The returned module is not
 * yet rooted by anything but the registry (which is a GC root).
 */
static JsModule *module_get_or_compile(JsContext *ctx, JsString *specifier,
                                       const uint16_t *source, size_t source_len,
                                       ModError *err) {
    JsModule *existing = registry_find(ctx, specifier);
    if (existing)
        return existing;

    JsVm *vm = ctx->vm;
    /* create the module cell first, rooted via a protected namespace slot */
    JsValue specv = js_value_from_cell(&specifier->gc);
    js_gc_protect(vm, &specv);
    JsGcCell *cell = js_gc_new_cell(vm, JS_KIND_MODULE, sizeof(JsModule));
    if (!cell) {
        js_gc_unprotect(vm, &specv);
        err->oom = true;
        return NULL;
    }
    JsModule *m = (JsModule *)cell;
    memset((char *)m + sizeof(JsGcCell), 0, sizeof *m - sizeof(JsGcCell));
    m->specifier = js_value_string(specv);
    m->eval_error = js_undefined();
    m->status = JS_MOD_UNLINKED;
    JsValue mv = js_value_from_cell(cell);
    js_gc_protect(vm, &mv);

    /* exports (namespace) object */
    JsValue exports = js_object_new(vm);
    if (!js_is_object(exports)) {
        js_gc_unprotect(vm, &mv);
        js_gc_unprotect(vm, &specv);
        err->oom = true;
        return NULL;
    }
    ((JsModule *)js_value_cell(mv))->exports = js_value_object(exports);

    /* parse + compile the body */
    JsArena arena;
    js_arena_init(&arena, vm);
    JsParseResult pr;
    JsFunctionCell *body = NULL;
    if (!js_parse_module(&arena, source, source_len, &pr)) {
        err->msg = pr.err_msg;
        err->pos = pr.err_pos;
    } else {
        JsCompileError ce;
        body = js_compile_module_body(ctx, pr.module, (JsModule *)js_value_cell(mv), &ce);
        if (!body) {
            err->msg = ce.msg;
            err->pos = ce.pos;
        }
    }
    js_arena_free(&arena);
    if (!body) {
        js_gc_unprotect(vm, &mv);
        js_gc_unprotect(vm, &specv);
        return NULL;
    }
    m = (JsModule *)js_value_cell(mv);
    m->body = body;

    if (!registry_add(ctx, m)) {
        js_gc_unprotect(vm, &mv);
        js_gc_unprotect(vm, &specv);
        err->oom = true;
        return NULL;
    }
    js_gc_unprotect(vm, &mv);
    js_gc_unprotect(vm, &specv);
    return m;
}

/* ---- instantiate (resolve dependency graph) ---- */

static JsModule *instantiate(JsContext *ctx, JsModule *m, ModError *err);

static bool resolve_dep(JsContext *ctx, JsModule *referrer, JsString *raw_spec,
                        JsModule **out, ModError *err) {
    if (!ctx->resolver) {
        err->msg = "no module resolver set";
        err->pos = 0;
        return false;
    }
    const uint16_t *out_spec, *out_source;
    size_t out_spec_len, out_len;
    if (!ctx->resolver(ctx->resolver_ud, raw_spec->units, raw_spec->length,
                       referrer->specifier->units, referrer->specifier->length,
                       &out_spec, &out_spec_len, &out_source, &out_len)) {
        err->msg = "module not found";
        err->pos = 0;
        return false;
    }
    JsString *canon = js_value_string(js_atom(ctx->vm, out_spec, out_spec_len));
    if (!canon) {
        err->oom = true;
        return false;
    }
    JsValue canonv = js_value_from_cell(&canon->gc);
    js_gc_protect(ctx->vm, &canonv);
    JsModule *dep = module_get_or_compile(ctx, js_value_string(canonv), out_source, out_len, err);
    if (dep)
        dep = instantiate(ctx, dep, err);
    js_gc_unprotect(ctx->vm, &canonv);
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
        m = (JsModule *)js_value_cell(mv);
        m->deps = deps;
        m->dep_count = m->dep_spec_count;
        for (uint32_t i = 0; i < m->dep_spec_count; i++)
            m->deps[i] = NULL;
        for (uint32_t i = 0; i < m->dep_spec_count && ok; i++) {
            JsModule *dep;
            if (!resolve_dep(ctx, (JsModule *)js_value_cell(mv),
                             ((JsModule *)js_value_cell(mv))->dep_specs[i], &dep, err)) {
                ok = false;
                break;
            }
            ((JsModule *)js_value_cell(mv))->deps[i] = dep;
        }
    } else {
        err->oom = true;
    }
    m = (JsModule *)js_value_cell(mv);
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

/* ---- evaluate (run bodies in dependency order) ---- */

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

static bool evaluate(JsContext *ctx, JsModule *m, JsValue *error) {
    if (m->status == JS_MOD_EVALUATED)
        return true;
    if (m->status == JS_MOD_ERRORED) {
        *error = m->eval_error;
        return false;
    }
    if (m->status == JS_MOD_EVALUATING)
        return true; /* cycle: already running */
    m->status = JS_MOD_EVALUATING;

    JsValue mv = js_value_from_cell(&m->gc);
    js_gc_protect(ctx->vm, &mv);

    bool ok = true;
    /* dependencies first */
    for (uint32_t i = 0; i < m->dep_count && ok; i++) {
        JsModule *dep = ((JsModule *)js_value_cell(mv))->deps[i];
        if (dep && !evaluate(ctx, dep, error)) {
            ok = false;
            break;
        }
    }
    /* re-exports: snapshot from each source (which has now evaluated) */
    m = (JsModule *)js_value_cell(mv);
    for (uint32_t i = 0; i < m->star_count && ok; i++) {
        if (!apply_reexport(ctx, m, &m->stars[i])) {
            JsString *s = js_ascii_cell(ctx->vm, "out of memory");
            *error = s ? js_value_from_cell(&s->gc) : js_undefined();
            ok = false;
        }
    }
    /* run the module body (populates exports); drains microtasks */
    if (ok) {
        m = (JsModule *)js_value_cell(mv);
        JsValue result;
        JsValue bodyv = js_value_from_cell(&m->body->gc);
        if (!js_run_module(ctx, bodyv, &result)) {
            *error = result;
            ok = false;
        }
    }
    m = (JsModule *)js_value_cell(mv);
    if (ok) {
        m->status = JS_MOD_EVALUATED;
    } else {
        m->status = JS_MOD_ERRORED;
        m->eval_error = *error;
    }
    js_gc_unprotect(ctx->vm, &mv);
    return ok;
}

/* ---- public entry ---- */

JsValue js_eval_module(JsContext *ctx, const uint16_t *specifier, size_t spec_len,
                       const uint16_t *source, size_t source_len, bool *ok,
                       const char **err_msg, uint32_t *err_pos) {
    JsVm *vm = ctx->vm;
    *ok = true;
    *err_msg = NULL;
    *err_pos = 0;

    JsString *spec = js_value_string(js_atom(vm, specifier, spec_len));
    if (!spec) {
        *ok = false;
        *err_msg = "out of memory";
        return js_undefined();
    }
    JsValue specv = js_value_from_cell(&spec->gc);
    js_gc_protect(vm, &specv);

    ModError err = {NULL, 0, false};
    JsModule *root = module_get_or_compile(ctx, js_value_string(specv), source, source_len, &err);
    if (root)
        root = instantiate(ctx, root, &err);
    if (root && !link_module(ctx, root, &err))
        root = NULL;
    if (!root) {
        js_gc_unprotect(vm, &specv);
        *ok = false;
        *err_msg = err.oom ? "out of memory" : (err.msg ? err.msg : "module error");
        *err_pos = err.pos;
        return js_undefined();
    }

    JsValue rootv = js_value_from_cell(&root->gc);
    js_gc_protect(vm, &rootv);
    JsValue error;
    bool ev = evaluate(ctx, (JsModule *)js_value_cell(rootv), &error);
    JsValue ns = ev ? js_value_from_cell(&((JsModule *)js_value_cell(rootv))->exports->gc)
                    : error;
    js_gc_unprotect(vm, &rootv);
    js_gc_unprotect(vm, &specv);
    *ok = ev;
    return ns;
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
