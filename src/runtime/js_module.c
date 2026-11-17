/*
 * ES modules: an async fetch graph feeding the synchronous
 * prepare -> link -> evaluate pipeline.
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
 * transitive dependency has arrived does it run the synchronous
 * prepare and link. Evaluation is V8's (see "evaluate"): the bodies run in
 * one walk, dependencies first and left to right, and a body that suspends
 * at a top-level await holds back the modules that import it, not its
 * siblings, and keeps the graph's completion pending until it has finished.
 *
 * No walk over a graph recurses on the C stack: each keeps its path in the
 * heap, and the ones along imports stop at JS_MAX_MODULE_DEPTH modules with
 * V8's RangeError (see "walking the graph").
 *
 * Orchestration reuses the promise machinery itself: state objects + bound
 * natives via js_promise_then, the same idiom as Promise.all in
 * js_promise.c. JsModule cells ride through captures disguised as JsValues
 * (they tag as objects via js_value_from_cell) — never hand one to script.
 *
 * Live bindings: an importer reads the exports object of the module that owns
 * the binding at each use (GET_IMPORT), wherever the name came through, so a
 * write in that module is observed. Linking is V8's: ResolveExport finds the
 * binding behind every imported and re-exported name (with its three
 * SyntaxErrors), strongly connected component by component, and
 * FetchStarExports gathers what `export *` brings into a namespace somebody
 * takes. A re-exported name is a slot in the re-exporter's namespace that the
 * owning module rewrites at every write of the export (a mirror), so
 * namespaces are as live as imports.
 */
#include "js_bytecode.h"
#include "js_error.h"
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
    if (m->fiber)
        js_gc_mark_cell(vm, &m->fiber->gc);
    js_gc_mark_value(vm, m->eval_error);
    js_gc_mark_value(vm, m->returned);
    if (m->top_capability)
        js_gc_mark_cell(vm, &m->top_capability->gc);
    if (m->cycle_root)
        js_gc_mark_cell(vm, &m->cycle_root->gc);
    for (uint32_t i = 0; i < m->async_parent_count; i++)
        js_gc_mark_cell(vm, &m->async_parents[i]->gc);
    for (uint32_t i = 0; i < m->import_count; i++) {
        if (m->imports[i].specifier)
            js_gc_mark_cell(vm, &m->imports[i].specifier->gc);
        if (m->imports[i].imported_name)
            js_gc_mark_cell(vm, &m->imports[i].imported_name->gc);
        if (m->imports[i].source)
            js_gc_mark_cell(vm, &m->imports[i].source->gc);
        if (m->imports[i].requested)
            js_gc_mark_cell(vm, &m->imports[i].requested->gc);
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
    for (uint32_t i = 0; i < m->binding_count; i++) {
        js_gc_mark_cell(vm, &m->bindings[i].name->gc);
        js_gc_mark_cell(vm, &m->bindings[i].origin->gc);
        if (m->bindings[i].origin_name)
            js_gc_mark_cell(vm, &m->bindings[i].origin_name->gc);
    }
    /* a mirror's namespace is held weakly (js_module_mirrors_prune); its keys are not */
    for (uint32_t i = 0; i < m->mirror_count; i++)
        js_gc_mark_cell(vm, &m->mirrors[i].name->gc);
    for (uint32_t i = 0; i < m->mirrored.used; i++) {
        JsString *k = m->mirrored.entries[i].key;
        if (k && k != JS_MAP_TOMBSTONE)
            js_gc_mark_cell(vm, &k->gc);
    }
}

void js_module_free_cell(JsVm *vm, JsModule *m) {
    js_realloc_raw(vm, m->imports, (size_t)m->import_count * sizeof(JsModuleImport), 0);
    js_realloc_raw(vm, m->stars, (size_t)m->star_count * sizeof(JsStarExport), 0);
    js_realloc_raw(vm, m->dep_specs, (size_t)m->dep_spec_count * sizeof(JsString *), 0);
    js_realloc_raw(vm, m->deps, (size_t)m->dep_count * sizeof(JsModule *), 0);
    js_realloc_raw(vm, m->bindings, (size_t)m->binding_cap * sizeof(JsExportBinding), 0);
    js_realloc_raw(vm, m->mirrors, (size_t)m->mirror_cap * sizeof(JsExportMirror), 0);
    js_realloc_raw(vm, m->async_parents, (size_t)m->async_parent_cap * sizeof(JsModule *), 0);
    js_map_free(vm, &m->bound);
    js_map_free(vm, &m->mirrored);
}

/* ---- export mirrors (the owning module's end of a re-exported name) ---- */

/* O's export KEY shows in the namespace EXPORTS under NAME from now on. False on OOM. */
static bool add_mirror(JsVm *vm, JsModule *o, JsString *key, JsObject *exports, JsString *name) {
    if (o->mirror_count == o->mirror_cap) {
        uint32_t ncap = o->mirror_cap ? o->mirror_cap * 2 : 4;
        JsExportMirror *na = js_realloc_raw(vm, o->mirrors,
                                            (size_t)o->mirror_cap * sizeof(JsExportMirror),
                                            (size_t)ncap * sizeof(JsExportMirror));
        if (!na)
            return false;
        if (!o->mirror_cap) { /* its first: the collector prunes the modules on this list */
            o->mirror_next = vm->mirror_modules;
            vm->mirror_modules = o;
        }
        o->mirrors = na;
        o->mirror_cap = ncap;
    }
    bool found;
    JsValue head = js_map_get(&o->mirrored, key, &found);
    uint32_t next = found ? (uint32_t)js_get_number(head) : 0;
    if (!js_map_set(vm, &o->mirrored, key, js_number((double)o->mirror_count + 1)))
        return false;
    JsExportMirror *mr = &o->mirrors[o->mirror_count++];
    mr->exports = exports;
    mr->name = name;
    mr->key = key;
    mr->next = next;
    return true;
}

/* M wrote V to its export NAME (JS_OP_SET_EXPORT, JS_OP_INIT_EXPORT): the namespaces that
 * re-export it show the same. The slots exist, so nothing is allocated. */
void js_module_export_written(JsVm *vm, JsModule *m, JsString *name, JsValue v) {
    bool found;
    JsValue head = js_map_get(&m->mirrored, name, &found);
    if (!found)
        return;
    for (uint32_t i = (uint32_t)js_get_number(head); i; i = m->mirrors[i - 1].next)
        (void)js_map_set(vm, &m->mirrors[i - 1].exports->props, m->mirrors[i - 1].name, v);
}

/* Between marking and the sweep: a mirror whose namespace is about to be freed goes (the
 * namespace of an evicted re-exporter nobody holds any more), and so does a module about to be
 * freed itself from the list. Nothing is allocated: the chains are rebuilt in place. */
void js_module_mirrors_prune(JsVm *vm) {
    JsModule **link = &vm->mirror_modules;
    while (*link) {
        JsModule *m = *link;
        if (!m->gc.mark) {
            *link = m->mirror_next;
            continue;
        }
        link = &m->mirror_next;
        uint32_t kept = 0;
        for (uint32_t i = 0; i < m->mirror_count; i++)
            if (m->mirrors[i].exports->gc.mark)
                m->mirrors[kept++] = m->mirrors[i];
        if (kept == m->mirror_count)
            continue;
        m->mirror_count = kept;
        for (uint32_t i = 0; i < m->mirrored.used; i++) {
            JsString *k = m->mirrored.entries[i].key;
            if (k && k != JS_MAP_TOMBSTONE)
                m->mirrored.entries[i].value = js_number(0);
        }
        for (uint32_t i = 0; i < kept; i++) {
            bool found;
            JsValue head = js_map_get(&m->mirrored, m->mirrors[i].key, &found);
            m->mirrors[i].next = found ? (uint32_t)js_get_number(head) : 0;
            (void)js_map_set(vm, &m->mirrored, m->mirrors[i].key, js_number((double)i + 1));
        }
    }
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
    js_map_free(ctx->vm, &ctx->module_index);
    ctx->modules = NULL;
    ctx->module_count = ctx->module_cap = 0;
}

/* ---- registry ---- */

static JsValue module_value(JsModule *m);
static JsModule *value_module(JsValue v);

static JsModule *registry_find(JsContext *ctx, JsString *specifier) {
    bool found;
    JsValue v = js_map_get(&ctx->module_index, specifier, &found);
    return found ? value_module(v) : NULL;
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
    /* Index first: if this OOMs, leave the array untouched so the two stay in
     * sync (a half-added module would be invisible to registry_find). */
    if (!js_map_set(ctx->vm, &ctx->module_index, m->specifier, module_value(m)))
        return false;
    ctx->modules[ctx->module_count++] = m;
    return true;
}

/* ---- eviction (lamassu.h) ---- */

size_t js_context_module_count(const JsContext *ctx) { return ctx->module_count; }

JsValue js_context_module_specifier(const JsContext *ctx, size_t index) {
    if (index >= ctx->module_count || !ctx->modules[index] || !ctx->modules[index]->specifier)
        return js_undefined();
    return js_value_from_cell(&ctx->modules[index]->specifier->gc);
}

bool js_context_evict_module(JsContext *ctx, const uint16_t *specifier, size_t spec_len) {
    JsValue key = js_atom(ctx->vm, specifier, spec_len);
    if (!js_is_string(key))
        return false;
    JsModule *root = registry_find(ctx, js_value_string(key));
    if (!root)
        return false;
    uint32_t n = ctx->module_count;
    /* the VM's allocator, like the rest of the engine (no libc malloc) */
    uint8_t *mark = js_realloc_raw(ctx->vm, NULL, 0, n ? n : 1);
    if (!mark)
        return false;
    memset(mark, 0, n ? n : 1);
    for (uint32_t i = 0; i < n; i++)
        if (ctx->modules[i] == root)
            mark[i] = 1;
    /* dependents, to a fixpoint: a module goes when any of its deps goes */
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t i = 0; i < n; i++) {
            if (mark[i])
                continue;
            JsModule *m = ctx->modules[i];
            for (uint32_t d = 0; d < m->dep_count && !mark[i]; d++)
                for (uint32_t j = 0; j < n; j++)
                    if (mark[j] && m->deps[d] == ctx->modules[j]) { mark[i] = 1; changed = true; break; }
        }
    }
    /* nothing in flight may go */
    for (uint32_t i = 0; i < n; i++)
        if (mark[i]) {
            const JsModule *m = ctx->modules[i];
            uint8_t st = m->status;
            /* a module on an evaluation's stack, or in the list of those about to run, may have
             * failed already (ERRORED) and is still held there */
            if (st == JS_MOD_FETCHING || st == JS_MOD_LINKING || st == JS_MOD_EVALUATING ||
                st == JS_MOD_EVALUATING_ASYNC || m->eval_pass || m->exec_mark) {
                js_realloc_raw(ctx->vm, mark, n ? n : 1, 0);
                return false;
            }
        }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (mark[i]) {
            js_map_delete(&ctx->module_index, ctx->modules[i]->specifier);
            continue;
        }
        ctx->modules[kept++] = ctx->modules[i];
    }
    ctx->module_count = kept;
    js_realloc_raw(ctx->vm, mark, n ? n : 1, 0);
    return true;
}

/* ---- small helpers ---- */

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
    return js_error_from_ascii(ctx, msg); /* Error object; string on OOM */
}

static JsValue mod_error_value(JsContext *ctx, const JsModError *err) {
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

/* False on OOM: the state object is then missing a field its reader needs. */
static bool mstate_set(JsContext *ctx, JsObject *st, const char *k, JsValue v) {
    return js_object_set_ascii(ctx, st, k, v);
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
JsModule *js_module_alloc_placeholder(JsContext *ctx, JsString *specifier) {
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
    m->returned = js_undefined();
    m->status = JS_MOD_FETCHING;
    return m;
}

/* import.meta for M: an object with no prototype, as V8 makes it, whose `url` is M's specifier
 * (the host's part: V8 leaves the properties to its embedder), held as the namespace of a
 * synthetic module M's pseudo-import binds to. NULL on OOM. */
static JsModule *meta_module(JsContext *ctx, JsModule *m) {
    JsVm *vm = ctx->vm;
    JsValue mv = js_value_from_cell(&m->gc);
    js_gc_protect(vm, &mv);
    JsModule *mm = js_module_alloc_placeholder(ctx, m->specifier);
    JsValue mmv = mm ? js_value_from_cell(&mm->gc) : js_undefined();
    js_gc_protect(vm, &mmv);
    JsValue meta = mm ? js_object_new(ctx) : js_undefined();
    bool ok = js_is_object(meta);
    if (ok) {
        js_value_object(meta)->proto = js_undefined();
        mm->exports = js_value_object(meta);
        mm->synthetic = true;
        mm->status = JS_MOD_EVALUATED;
        mm->link_state = 2;
        ok = js_object_set_ascii(ctx, mm->exports, "url", js_value_from_cell(&m->specifier->gc));
    }
    js_gc_unprotect(vm, &mmv);
    js_gc_unprotect(vm, &mv);
    return ok ? mm : NULL;
}

/* ---- canonicalization ---- */

/*
 * Maps a raw specifier + referrer to the canonical (interned) specifier
 * that is the registry identity. NULL canonicalizer -> raw is canonical.
 * NULL on failure with *errmsg set (static ASCII).
 */
/* The length of a module request's specifier: what comes before its import attributes, which
 * the compiler appends as "\0key\0value" pairs (a module and a JSON module of one specifier are
 * two requests, two records) */
static uint32_t spec_len(const JsString *s) {
    uint32_t n = 0;
    while (n < s->length && s->units[n] != 0)
        n++;
    return n;
}

static JsString *canonicalize_spec(JsContext *ctx, JsModule *referrer, JsString *raw,
                                   const char **errmsg) {
    if (!ctx->canon)
        return raw;
    const uint16_t *out;
    size_t out_len;
    uint32_t base = spec_len(raw), rlen = referrer ? spec_len(referrer->specifier) : 0;
    if (!ctx->canon(ctx->loader_ud, raw->units, base, referrer ? referrer->specifier->units : NULL,
                    rlen, &out, &out_len)) {
        *errmsg = "cannot resolve module specifier";
        return NULL;
    }
    JsValue v;
    if (base == raw->length) {
        v = js_atom(ctx->vm, out, out_len);
    } else { /* the canonical specifier, its attributes after it again */
        size_t total = out_len + (raw->length - base);
        uint16_t *u = js_realloc_raw(ctx->vm, NULL, 0, total * sizeof *u);
        if (!u) {
            *errmsg = "out of memory";
            return NULL;
        }
        memcpy(u, out, out_len * sizeof *u);
        memcpy(u + out_len, raw->units + base, (raw->length - base) * sizeof *u);
        JsValue rawv = js_value_from_cell(&raw->gc);
        js_gc_protect(ctx->vm, &rawv);
        v = js_atom(ctx->vm, u, total);
        js_gc_unprotect(ctx->vm, &rawv);
        js_realloc_raw(ctx->vm, u, total * sizeof *u, 0);
    }
    if (!js_is_string(v)) {
        *errmsg = "out of memory";
        return NULL;
    }
    return js_value_string(v);
}

/* ---- shallow fetch (one module: loader -> compiled/adopted) ---- */

/* Marks m failed and rejects its fetch promise (double-settle no-ops). */
static void module_fetch_fail(JsContext *ctx, JsModule *m, JsValue reason, uint32_t pos) {
    m->status = JS_MOD_LOAD_FAILED;
    m->eval_error = reason;
    ctx->error_pos = pos;
    ctx->error_trace_count = 0; /* a load failure has no frames; see module_compile_fail */
    ctx->raise_fresh = false;   /* and the trace is no longer the raise's it was of */
    ctx->raise_trace = NULL;
    js_promise_reject(ctx, m->fetch_promise, reason);
}

/* A syntax error has a place: one trace frame naming the module and the offset,
 * so a host can report file and line without a running stack. */
static void module_compile_fail(JsContext *ctx, JsModule *m, JsValue reason, uint32_t pos) {
    js_error_set_origin(ctx, reason, m->specifier, pos); /* kept with the error: js_error_stack */
    module_fetch_fail(ctx, m, reason, pos);
    ctx->error_trace[0].specifier = m->specifier ? js_value_from_cell(&m->specifier->gc) : js_undefined();
    ctx->error_trace[0].function = js_undefined();
    ctx->error_trace[0].pos = pos;
    ctx->error_trace_count = 1;
}

/* A module request with import attributes: `type: "json"` makes a JSON module (a synthetic module
 * whose default export is the text the loader gave, parsed as JSON.parse does), as V8 does;
 * any other attribute, or type, is refused with the TypeError Node gives. */
static bool fetch_with_attributes(JsContext *ctx, JsModule *m, JsValue bound, JsValue resolved) {
    JsString *spec = m->specifier;
    uint32_t base = spec_len(spec), i = base;
    JsValue rv = resolved;
    js_gc_protect(ctx->vm, &rv);
    bool json = false;
    JsValue reason = js_undefined();
    while (i < spec->length && js_is_undefined(reason)) { /* "\0key\0value" pairs */
        uint32_t k0 = i + 1, k1 = k0;
        while (k1 < spec->length && spec->units[k1] != 0)
            k1++;
        uint32_t v0 = k1 + 1, v1 = v0;
        while (v1 < spec->length && spec->units[v1] != 0)
            v1++;
        static const uint16_t ktype[] = {'t', 'y', 'p', 'e'}, vjson[] = {'j', 's', 'o', 'n'};
        bool is_type = k1 - k0 == 4 && !memcmp(spec->units + k0, ktype, sizeof ktype);
        if (is_type && v1 - v0 == 4 && !memcmp(spec->units + v0, vjson, sizeof vjson)) {
            json = true;
        } else {
            JsString *ks = js_string_cell_new(ctx->vm, spec->units + k0, k1 - k0);
            JsValue kv = ks ? js_value_from_cell(&ks->gc) : js_undefined();
            js_gc_protect(ctx->vm, &kv);
            JsString *vs = ks ? js_string_cell_new(ctx->vm, spec->units + v0, v1 - v0) : NULL;
            reason = vs ? js_error_fmt(ctx, "TypeError: Import attribute \"%S\" with value \"%S\" is not supported", ks, vs)
                        : js_oom_value(ctx->vm);
            js_gc_unprotect(ctx->vm, &kv);
        }
        i = v1;
    }
    if (js_is_undefined(reason) && json && !js_is_string(resolved)) {
        JsString *bs = js_string_cell_new(ctx->vm, spec->units, base);
        reason = bs ? js_error_fmt(ctx, "TypeError: Module \"%S\" is not of type \"json\"", bs)
                    : js_oom_value(ctx->vm);
    }
    JsValue value = js_undefined();
    js_gc_protect(ctx->vm, &value);
    if (js_is_undefined(reason) && !js_json_parse_text(ctx, resolved, &value))
        reason = value; /* JSON.parse's SyntaxError */
    if (js_is_undefined(reason)) {
        JsValue ns = js_object_new(ctx);
        if (js_is_object(ns)) {
            JsObject *o = js_value_object(ns);
            o->proto = js_undefined();
            if (js_object_set_ascii(ctx, o, "default", value)) {
                o->obj_flags |= JS_OBJ_SEALED | JS_OBJ_NAMESPACE; /* a namespace: sealed, read-only */
                m->exports = o;
                m->synthetic = true;
                m->status = JS_MOD_EVALUATED;
                js_promise_fulfill(ctx, m->fetch_promise, bound);
            } else {
                reason = js_oom_value(ctx->vm);
            }
        } else {
            reason = js_oom_value(ctx->vm);
        }
    }
    if (!js_is_undefined(reason))
        module_fetch_fail(ctx, m, reason, 0);
    js_gc_unprotect(ctx->vm, &value);
    js_gc_unprotect(ctx->vm, &rv);
    return true;
}

static bool fetch_on_resolved(JsContext *ctx, JsValue bound, JsValue tv,
                              const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    JsModule *m = value_module(bound);
    JsValue resolved = argc > 0 ? args[0] : js_undefined();

    if (spec_len(m->specifier) < m->specifier->length) /* import attributes */
        return fetch_with_attributes(ctx, m, bound, resolved);
    if (js_is_string(resolved)) {
        JsString *src = js_value_string(resolved);
        /* Source needs a parser, which a runtime-only build does not link.
         * Precompiled bytecode (the branch below) is unaffected. */
        if (!ctx->compile_source) {
            module_fetch_fail(ctx, m,
                              ascii_value(ctx, "source modules unavailable in this build "
                                               "(precompile to bytecode)"),
                              0);
            return true;
        }
        JsModError err = {NULL, 0, false};
        if (!ctx->compile_source(ctx, m, src->units, src->length, &err)) {
            /* a SyntaxError object, as V8 rejects with; the message is the compiler's text */
            JsValue reason = err.oom ? mod_error_value(ctx, &err)
                                     : js_error_new_ascii(ctx, JS_ERROR_SYNTAX, err.msg ? err.msg : "module error");
            if (js_is_undefined(reason))
                reason = js_oom_value(ctx->vm);
            module_compile_fail(ctx, m, reason, err.pos);
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
        if (existing->status == JS_MOD_LOAD_FAILED)
            return settled_promise(ctx, false, existing->eval_error);
        /* one whose evaluation threw is there, as V8 has it: the graph loads and links, and
         * the error is thrown again where the module would run (eval_required) */
        return settled_promise(ctx, true, module_value(existing));
    }
    if (!ctx->loader)
        return rejected_ascii(ctx, "no module loader set");

    JsModule *m = js_module_alloc_placeholder(ctx, spec);
    if (!m)
        return js_undefined();
    m->entry = !referrer && spec == ctx->entry_spec;
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
    /* the loader is asked for the specifier; the attributes say what to make of its answer */
    JsValue loader_p = ctx->loader(ctx->loader_ud, ctx, spec->units, spec_len(spec),
                                   referrer ? referrer->specifier->units : NULL,
                                   referrer ? spec_len(referrer->specifier) : 0);
    if (!js_is_promise(loader_p)) {
        module_fetch_fail(ctx, m, ascii_value(ctx, "module loader did not return a promise"), 0);
    } else if (!then_bound(ctx, loader_p, fetch_on_resolved, fetch_on_rejected, mv)) {
        module_fetch_fail(ctx, m, ascii_value(ctx, "out of memory"), 0);
    }
    return js_value_from_cell(&m->fetch_promise->gc);
}

/* ---- walking the graph ---- */

/*
 * The walks along a graph's imports (the graph load over what is fetched already, prepare_graph,
 * finish_link, the evaluation) go depth first,
 * as V8's do, but keep their path in an array of the VM's allocator where V8 recurses. How deep
 * a graph may be is then not a matter of the stack the host gave the thread (512 KB on a
 * secondary thread of macOS or iOS), and a graph too deep is an error, never a crash. V8 throws
 * this RangeError when its stack runs out: with the stack Node gives it, on a chain of about
 * 3100 modules. The limit here is of that order, and the same on every platform.
 *
 * The walks along re-exports (resolve_export, fetch_star_exports) keep their path the same way
 * and have no limit: V8 follows a chain of `export *` some ten thousand modules long, and
 * crashes on a longer one.
 */
#define JS_MAX_MODULE_DEPTH 3000

static const char k_too_deep[] = "RangeError: Maximum call stack size exceeded";

typedef struct WalkFrame {
    JsModule *m;
    uint32_t i; /* the next of its dependencies to look at */
} WalkFrame;

typedef struct Walk {
    WalkFrame *frames; /* the path from the root to the module the walk is at */
    uint32_t depth, cap;
} Walk;

/* M is the next module of the path. False on OOM. */
static bool walk_push(JsVm *vm, Walk *w, JsModule *m) {
    if (w->depth == w->cap) {
        uint32_t ncap = w->cap ? w->cap * 2 : 16;
        WalkFrame *na = js_realloc_raw(vm, w->frames, (size_t)w->cap * sizeof(WalkFrame),
                                       (size_t)ncap * sizeof(WalkFrame));
        if (!na)
            return false;
        w->frames = na;
        w->cap = ncap;
    }
    w->frames[w->depth].m = m;
    w->frames[w->depth].i = 0;
    w->depth++;
    return true;
}

static void walk_free(JsVm *vm, Walk *w) {
    js_realloc_raw(vm, w->frames, (size_t)w->cap * sizeof(WalkFrame), 0);
}

/* ---- graph load (the specification's LoadRequestedModules; cycle-proof) ---- */

/*
 * One operation fetches everything a root requires, however far down. Its state is an object:
 * { remaining: the fetches it waits for, promise: the operation's, root: the canonical specifier,
 * seen: an object whose properties are the specifiers the operation has been to }.
 *
 * A module that is fetched already is gone into at once, and its requests after it, depth first
 * (InnerModuleLoading); only a module the loader has yet to answer for takes a job. So a graph
 * that is all there is loaded when load_graph returns, whatever its depth, and an import() of
 * one settles as many jobs later as an import() of any other: in the order of the calls.
 */

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
                             const JsValue *args, int argc, JsValue *r);

static bool graph_on_failed(JsContext *ctx, JsValue bound, JsValue tv,
                            const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    graph_fail(ctx, js_value_object(bound), argc > 0 ? args[0] : js_undefined());
    return true;
}

/*
 * The operation comes to one request (a raw specifier, canonicalized against `referrer`;
 * already canonical when referrer is NULL). One it has been to is passed. A module that is
 * fetched comes back in *LOADED, for the caller to go into; one that could not be loaded fails
 * the operation; any other is asked of the loader, or joined if it was already, and the
 * operation waits for it. false only on OOM.
 */
static bool graph_request(JsContext *ctx, JsValue statev, JsModule *referrer,
                          JsString *raw_spec, JsModule **loaded) {
    JsObject *state = js_value_object(statev);
    *loaded = NULL;
    const char *cerr = NULL;
    JsString *spec = referrer ? canonicalize_spec(ctx, referrer, raw_spec, &cerr) : raw_spec;
    if (!spec) {
        graph_fail(ctx, state, ascii_value(ctx, cerr));
        return true; /* op failed; not an OOM to propagate */
    }
    /*
     * The canonical specifier is a fresh cell held nowhere but here until
     * the seen-set has it, and reading the seen-set allocates (its key is a
     * cell too). A collection there swept it, and the loader was then handed
     * a freed string's units — a use-after-free that surfaced as
     * "unexpected character" from the lexer, on a build with enough live
     * cells for the collector to run at that moment.
     */
    JsValue specv = js_value_from_cell(&spec->gc);
    js_gc_protect(ctx->vm, &specv);
    bool ok = true;
    JsValue seenv = mstate_get(ctx, state, "seen");
    if (!js_is_object(seenv)) {
        ok = false;
    } else {
        JsObject *seen = js_value_object(seenv);
        bool found;
        js_map_get(&seen->props, spec, &found);
        if (!found) {
            JsModule *existing = registry_find(ctx, spec);
            if (!js_map_set(ctx->vm, &seen->props, spec, js_number(1))) {
                ok = false;
            } else if (existing && existing->status == JS_MOD_LOAD_FAILED) {
                graph_fail(ctx, state, existing->eval_error);
            } else if (existing && existing->status != JS_MOD_FETCHING) {
                *loaded = existing;
            } else {
                double rem = js_primitive_to_number(mstate_get(ctx, state, "remaining"));
                mstate_set(ctx, state, "remaining", js_number(rem + 1));
                JsValue fp = fetch_module_async(ctx, referrer, spec);
                ok = js_is_promise(fp) && then_bound(ctx, fp, graph_on_fetched, graph_on_failed, statev);
            }
        }
    }
    js_gc_unprotect(ctx->vm, &specv);
    return ok;
}

/* M is fetched: the operation goes on to what M requires, and from each of those that is
 * fetched to what it requires, depth first, the path in the heap (see Walk). false on OOM. */
static bool graph_visit(JsContext *ctx, JsValue statev, JsModule *m) {
    JsVm *vm = ctx->vm;
    Walk w = {NULL, 0, 0};
    bool ok = walk_push(vm, &w, m);
    while (ok && w.depth) {
        WalkFrame *f = &w.frames[w.depth - 1];
        if (f->i == f->m->dep_spec_count) {
            w.depth--;
            continue;
        }
        JsModule *from = f->m, *loaded = NULL;
        ok = graph_request(ctx, statev, from, from->dep_specs[f->i++], &loaded);
        if (ok && loaded)
            ok = walk_push(vm, &w, loaded);
    }
    walk_free(vm, &w);
    return ok;
}

/* Nothing left to wait for: the operation's promise is fulfilled with the root (to no effect
 * when the operation has failed). */
static void graph_settle(JsContext *ctx, JsObject *state) {
    if (js_primitive_to_number(mstate_get(ctx, state, "remaining")) != 0)
        return;
    JsValue rootspec = mstate_get(ctx, state, "root");
    JsModule *root = js_is_string(rootspec) ? registry_find(ctx, js_value_string(rootspec)) : NULL;
    JsValue pv = mstate_get(ctx, state, "promise");
    if (root && js_is_promise(pv))
        js_promise_fulfill(ctx, js_value_promise(pv), module_value(root));
}

static bool graph_on_fetched(JsContext *ctx, JsValue bound, JsValue tv,
                             const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    if (argc < 1)
        return true;
    JsObject *state = js_value_object(bound);
    /* this module's requests before the count goes down, so it cannot come to zero with work
     * still to find */
    if (!graph_visit(ctx, bound, value_module(args[0]))) {
        graph_fail(ctx, state, ascii_value(ctx, "out of memory"));
        return true;
    }
    double rem = js_primitive_to_number(mstate_get(ctx, state, "remaining")) - 1;
    mstate_set(ctx, state, "remaining", js_number(rem));
    graph_settle(ctx, state);
    return true;
}

/*
 * Promise that fulfills with the root module once the whole transitive
 * static-import graph rooted at canonical `root_spec` has been fetched
 * (compiled/adopted, not yet instantiated): before this returns when there
 * was nothing left to fetch. Undefined only on OOM.
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
        JsModule *loaded = NULL;
        bool set = mstate_set(ctx, state, "remaining", js_number(0)) &&
                   mstate_set(ctx, state, "promise", opv) &&
                   mstate_set(ctx, state, "root", rootv) &&
                   mstate_set(ctx, state, "seen", seenv);
        if (!set || !graph_request(ctx, statev, NULL, js_value_string(rootv), &loaded) ||
            (loaded && !graph_visit(ctx, statev, loaded)))
            js_promise_reject(ctx, js_value_promise(opv), ascii_value(ctx, "out of memory"));
        else if (loaded)
            graph_settle(ctx, js_value_object(statev));
    }
    js_gc_unprotect(vm, &opv);
    js_gc_unprotect(vm, &seenv);
    js_gc_unprotect(vm, &statev);
    js_gc_unprotect(vm, &rootv);
    return ok ? opv : js_undefined();
}

/* ---- prepare (the modules behind the specifiers; sync, post-fetch) ---- */

static JsModule *dep_for_spec(JsModule *m, JsString *spec) {
    for (uint32_t i = 0; i < m->dep_count; i++)
        if (m->dep_specs[i] == spec)
            return m->deps[i];
    return NULL;
}

/*
 * V8's PrepareInstantiate for one module: each of its requests gets its module, and every
 * import and re-export the module its specifier names. What a name resolves to is finish_link's.
 * The graph load already fetched and registered every transitive dependency, so finding a
 * request's module is a lookup: the canonicalizer is required to be deterministic, and
 * canonicalizing the specifier of the source text again gives the registry's key.
 */
static bool prepare_module(JsContext *ctx, JsModule *m, JsModError *err) {
    err->pos = 0;
    m->status = JS_MOD_LINKING;
    if (m->dep_spec_count) {
        JsModule **deps = js_realloc_raw(ctx->vm, NULL, 0,
                                         (size_t)m->dep_spec_count * sizeof(JsModule *));
        if (!deps) {
            err->oom = true;
            return false;
        }
        for (uint32_t i = 0; i < m->dep_spec_count; i++)
            deps[i] = NULL;
        m->deps = deps;
        m->dep_count = m->dep_spec_count;
    }
    for (uint32_t i = 0; i < m->dep_spec_count; i++) {
        const char *cerr = NULL;
        JsString *spec = canonicalize_spec(ctx, m, m->dep_specs[i], &cerr);
        if (!spec) {
            err->msg = cerr;
            return false;
        }
        JsModule *dep = registry_find(ctx, spec);
        if (!dep || dep->status == JS_MOD_FETCHING) {
            err->msg = "internal error: dependency was not fetched";
            return false;
        }
        if (dep->status == JS_MOD_LOAD_FAILED) {
            err->msg = "dependency failed to load";
            return false;
        }
        m->deps[i] = dep;
    }
    for (uint32_t i = 0; i < m->star_count; i++) {
        JsStarExport *re = &m->stars[i];
        if (re->kind == JS_REEXP_LOCAL)
            continue;
        JsModule *src = dep_for_spec(m, re->specifier);
        if (!src) {
            err->msg = "unresolved re-export";
            return false;
        }
        re->source = src;
    }
    for (uint32_t i = 0; i < m->import_count; i++) {
        if (m->imports[i].kind == JS_IMPORT_META) {
            if (!m->imports[i].source && !(m->imports[i].source = meta_module(ctx, m))) {
                err->oom = true;
                return false;
            }
            continue;
        }
        JsModule *src = dep_for_spec(m, m->imports[i].specifier);
        if (!src) {
            err->msg = "unresolved import";
            return false;
        }
        /* a named or default import is bound to the module that owns the binding once
         * finish_link has resolved it; a namespace or side-effect one to the source itself */
        if (m->imports[i].imported_name && !m->imports[i].requested)
            m->imports[i].requested = m->imports[i].imported_name;
        m->imports[i].source = src;
    }
    return true;
}

/*
 * Prepares every module under ROOT that is not prepared yet, depth first. False with *ERR, and
 * then none of them is: like V8, which takes a graph whose instantiation failed back to
 * unlinked, so importing it again starts over (and fails the same way).
 */
static bool prepare_graph(JsContext *ctx, JsModule *root, JsModError *err) {
    JsVm *vm = ctx->vm;
    if (root->status != JS_MOD_UNLINKED)
        return true; /* prepared before, or adopted (a synthetic module) */
    JsValue rootv = module_value(root);
    js_gc_protect(vm, &rootv); /* the others are the registry's */
    Walk w = {NULL, 0, 0};
    JsModule **made = NULL; /* every module this pass prepared */
    uint32_t made_count = 0, made_cap = 0;
    bool ok = true;
    JsModule *next = root;
    while (ok && (next || w.depth)) {
        if (next) {
            if (w.depth == JS_MAX_MODULE_DEPTH) {
                err->msg = k_too_deep;
                err->pos = 0;
                ok = false;
                break;
            }
            if (made_count == made_cap) {
                uint32_t ncap = made_cap ? made_cap * 2 : 16;
                JsModule **na = js_realloc_raw(vm, made, (size_t)made_cap * sizeof(JsModule *),
                                               (size_t)ncap * sizeof(JsModule *));
                if (na) {
                    made = na;
                    made_cap = ncap;
                }
            }
            if (made_count == made_cap || !walk_push(vm, &w, next)) {
                err->oom = true;
                ok = false;
                break;
            }
            made[made_count++] = next;
            ok = prepare_module(ctx, next, err);
            next = NULL;
            continue;
        }
        WalkFrame *f = &w.frames[w.depth - 1];
        while (f->i < f->m->dep_count && !next) {
            JsModule *dep = f->m->deps[f->i++];
            if (dep && dep->status == JS_MOD_UNLINKED)
                next = dep;
        }
        if (!next)
            w.depth--;
    }
    for (uint32_t i = 0; i < made_count; i++) {
        JsModule *m = made[i];
        if (ok) {
            m->status = JS_MOD_LINKED;
            continue;
        }
        m->status = JS_MOD_UNLINKED;
        js_realloc_raw(vm, m->deps, (size_t)m->dep_count * sizeof(JsModule *), 0);
        m->deps = NULL;
        m->dep_count = 0;
    }
    js_realloc_raw(vm, made, (size_t)made_cap * sizeof(JsModule *), 0);
    walk_free(vm, &w);
    js_gc_unprotect(vm, &rootv);
    return ok;
}

/* ---- the exports table: which binding a name is (V8's cells) ---- */

/* The binding M exports as NAME, if its table has one: a name linking bound to another binding,
 * or M's own (the key is in its exports object). A binding is the module that owns it and its
 * name there; no name is that module's namespace (V8's namespace cell, one per module: two
 * modules re-exporting the namespace of a third export one binding). */
static bool lookup_cell(JsModule *m, JsString *name, JsModule **origin, JsString **oname) {
    bool found;
    JsValue at = js_map_get(&m->bound, name, &found);
    if (found) {
        JsExportBinding *b = &m->bindings[(uint32_t)js_get_number(at)];
        *origin = b->origin;
        *oname = b->origin_name;
        return true;
    }
    if (!m->exports)
        return false;
    js_map_get(&m->exports->props, name, &found);
    if (!found)
        return false;
    *origin = m;
    *oname = name;
    return true;
}

/* Puts the bindings of M not yet in its namespace there: the slot takes the value the binding
 * has now (the TDZ marker as well: a read through the namespace then throws as a read of the
 * binding does) and the owning module mirrors every later write into it. A synthetic module's
 * object is the host's, written behind our back: its values are copied again when M runs
 * (refresh_synthetic). False on OOM. */
static bool commit_bindings(JsContext *ctx, JsModule *m) {
    JsVm *vm = ctx->vm;
    while (m->binding_committed < m->binding_count) {
        JsExportBinding *b = &m->bindings[m->binding_committed];
        if (!b->alias && !b->hidden) {
            JsModule *o = b->origin;
            bool found = false;
            JsValue v = js_undefined();
            if (!o->exports)
                ;
            else if (!b->origin_name) /* its namespace: never written again */
                v = js_value_from_cell(&o->exports->gc);
            else
                v = js_map_get(&o->exports->props, b->origin_name, &found);
            if (b->origin_name && !found)
                v = js_undefined();
            if (!m->exports || !js_map_set(vm, &m->exports->props, b->name, v))
                return false;
            if (b->origin_name && !o->synthetic &&
                !add_mirror(vm, o, b->origin_name, m->exports, b->name))
                return false;
        }
        m->binding_committed++;
    }
    return true;
}

static void refresh_synthetic(JsContext *ctx, JsModule *m) {
    for (uint32_t i = 0; i < m->binding_committed; i++) {
        JsExportBinding *b = &m->bindings[i];
        if (b->alias || b->hidden || !b->origin_name || !b->origin->synthetic ||
            !b->origin->exports)
            continue;
        bool found;
        JsValue v = js_map_get(&b->origin->exports->props, b->origin_name, &found);
        (void)js_map_set(ctx->vm, &m->exports->props, b->name, found ? v : js_undefined());
    }
}

/* V8's Put into M's exports table: NAME is the binding ONAME of ORIGIN. A module still being
 * linked keeps it out of its namespace until its component is done (a failed link takes it
 * back: reset_bindings). False on OOM. */
static bool put_binding(JsContext *ctx, JsModule *m, JsString *name, JsModule *origin,
                        JsString *oname, bool alias) {
    JsVm *vm = ctx->vm;
    if (m->binding_count == m->binding_cap) {
        uint32_t ncap = m->binding_cap ? m->binding_cap * 2 : 4;
        JsExportBinding *na = js_realloc_raw(vm, m->bindings,
                                             (size_t)m->binding_cap * sizeof(JsExportBinding),
                                             (size_t)ncap * sizeof(JsExportBinding));
        if (!na)
            return false;
        m->bindings = na;
        m->binding_cap = ncap;
    }
    if (!js_map_set(vm, &m->bound, name, js_number((double)m->binding_count)))
        return false;
    JsExportBinding *b = &m->bindings[m->binding_count++];
    b->name = name;
    b->origin = origin;
    b->origin_name = oname;
    b->alias = alias;
    b->hidden = m->ns_ready;
    return m->link_state != 2 || commit_bindings(ctx, m);
}

static void reset_bindings(JsVm *vm, JsModule *m) {
    m->binding_count = m->binding_committed = 0;
    js_map_free(vm, &m->bound);
    m->ns_ready = false;
}

/* ---- the names `export *` brings (V8's FetchStarExports) ---- */

typedef struct MoreExport {
    JsString *name;
    JsModule *origin;
    JsString *oname;
    bool ambiguous;
} MoreExport;

typedef struct MoreExports {
    JsMap index; /* name -> position in list */
    MoreExport *list;
    uint32_t count, cap;
} MoreExports;

typedef struct FetchFrame {
    JsModule *m;
    uint32_t i;       /* the `export *` it is at */
    MoreExports more; /* what they brought so far */
} FetchFrame;

typedef struct FetchWalk {
    JsModule **visited;
    uint32_t count, cap;
    FetchFrame *frames; /* the path of the walk (see Walk) */
    uint32_t depth, frame_cap;
} FetchWalk;

/* One name of a module M re-exports with `export *`: left out when it is `default` or M has it
 * already, and marked ambiguous when another star gave the name a different binding. */
static bool more_add(JsVm *vm, JsModule *m, MoreExports *more, JsString *name, JsModule *origin,
                     JsString *oname) {
    if (str_is_default(name))
        return true;
    bool found;
    js_map_get(&m->bound, name, &found);
    if (!found && m->exports)
        js_map_get(&m->exports->props, name, &found);
    if (found)
        return true;
    JsValue at = js_map_get(&more->index, name, &found);
    if (found) {
        MoreExport *e = &more->list[(uint32_t)js_get_number(at)];
        if (e->origin != origin || e->oname != oname)
            e->ambiguous = true;
        return true;
    }
    if (more->count == more->cap) {
        uint32_t ncap = more->cap ? more->cap * 2 : 8;
        MoreExport *na = js_realloc_raw(vm, more->list, (size_t)more->cap * sizeof(MoreExport),
                                        (size_t)ncap * sizeof(MoreExport));
        if (!na)
            return false;
        more->list = na;
        more->cap = ncap;
    }
    if (!js_map_set(vm, &more->index, name, js_number((double)more->count)))
        return false;
    MoreExport *e = &more->list[more->count++];
    e->name = name;
    e->origin = origin;
    e->oname = oname;
    e->ambiguous = false;
    return true;
}

/* The names REQ exports go to MORE, for M that re-exports it with `export *`. False on OOM. */
static bool more_add_all(JsVm *vm, JsModule *m, MoreExports *more, JsModule *req) {
    bool ok = true;
    for (uint32_t k = 0; ok && k < req->binding_count; k++)
        ok = more_add(vm, m, more, req->bindings[k].name, req->bindings[k].origin,
                      req->bindings[k].origin_name);
    for (uint32_t k = 0; ok && req->exports && k < req->exports->props.used; k++) {
        JsString *key = req->exports->props.entries[k].key;
        if (!key || key == JS_MAP_TOMBSTONE)
            continue;
        bool bound;
        js_map_get(&req->bound, key, &bound);
        if (!bound) /* its own binding */
            ok = more_add(vm, m, more, key, req, key);
    }
    return ok;
}

static void more_free(JsVm *vm, MoreExports *more) {
    js_realloc_raw(vm, more->list, (size_t)more->cap * sizeof(MoreExport), 0);
    js_map_free(vm, &more->index);
}

/* The walk comes to M: 1 when M joined its path, 0 when there is nothing to gather for it (its
 * namespace was made already, or this walk has been there), -1 on OOM. */
static int fetch_enter(JsVm *vm, FetchWalk *w, JsModule *m) {
    if (m->ns_ready || m->synthetic || m->fetch_mark)
        return 0;
    if (w->count == w->cap) {
        uint32_t ncap = w->cap ? w->cap * 2 : 8;
        JsModule **na = js_realloc_raw(vm, w->visited, (size_t)w->cap * sizeof(JsModule *),
                                       (size_t)ncap * sizeof(JsModule *));
        if (!na)
            return -1;
        w->visited = na;
        w->cap = ncap;
    }
    if (w->depth == w->frame_cap) {
        uint32_t ncap = w->frame_cap ? w->frame_cap * 2 : 8;
        FetchFrame *na = js_realloc_raw(vm, w->frames, (size_t)w->frame_cap * sizeof(FetchFrame),
                                        (size_t)ncap * sizeof(FetchFrame));
        if (!na)
            return -1;
        w->frames = na;
        w->frame_cap = ncap;
    }
    w->visited[w->count++] = m;
    m->fetch_mark = true;
    FetchFrame *f = &w->frames[w->depth++];
    memset(f, 0, sizeof *f);
    f->m = m;
    return 1;
}

/*
 * V8's FetchStarExports, with its order and its shortcuts, which decide what a namespace shows:
 * the modules M re-exports with `export *` first, then every name of their tables M lacks, but
 * for `default` and the names two stars give different bindings. A module whose namespace was
 * made already is taken as it is, and one this walk is in (a cycle) as far as it has got.
 * V8 recurses; the path is in W here (see Walk), so a chain of `export *` of any length is
 * followed.
 */
static bool fetch_star_exports(JsContext *ctx, FetchWalk *w, JsModule *root) {
    JsVm *vm = ctx->vm;
    bool ok = fetch_enter(vm, w, root) >= 0;
    while (ok && w->depth) {
        FetchFrame *f = &w->frames[w->depth - 1];
        JsModule *m = f->m;
        bool deeper = false;
        while (ok && !deeper && f->i < m->star_count) {
            JsModule *req = m->stars[f->i].source;
            if (m->stars[f->i].kind == JS_REEXP_ALL && req) {
                /* its own stars first: the walk comes back to this one when they are done */
                int entered = fetch_enter(vm, w, req);
                if (entered) {
                    ok = entered > 0;
                    deeper = true;
                    break;
                }
                ok = more_add_all(vm, m, &f->more, req);
            }
            f->i++;
        }
        if (!ok || deeper)
            continue;
        for (uint32_t k = 0; ok && k < f->more.count; k++)
            if (!f->more.list[k].ambiguous)
                ok = put_binding(ctx, m, f->more.list[k].name, f->more.list[k].origin,
                                 f->more.list[k].oname, false);
        more_free(vm, &f->more);
        w->depth--;
    }
    while (w->depth) /* a walk that failed: what its path had gathered */
        more_free(vm, &w->frames[--w->depth].more);
    return ok;
}

/* M's namespace is about to be taken (V8's GetModuleNamespace): the names its `export *` bring
 * join it, once. False on OOM. */
static bool ensure_namespace(JsContext *ctx, JsModule *m) {
    m->ns_wanted = true;
    if (m->ns_ready || m->synthetic)
        return true;
    FetchWalk w;
    memset(&w, 0, sizeof w);
    bool ok = fetch_star_exports(ctx, &w, m);
    for (uint32_t i = 0; i < w.count; i++)
        w.visited[i]->fetch_mark = false;
    js_realloc_raw(ctx->vm, w.visited, (size_t)w.cap * sizeof(JsModule *), 0);
    js_realloc_raw(ctx->vm, w.frames, (size_t)w.frame_cap * sizeof(FetchFrame), 0);
    if (ok)
        m->ns_ready = true;
    return ok;
}

/* ---- resolve and instantiate (V8's FinishInstantiate) ---- */

typedef struct ResolvePair {
    JsModule *m;
    JsString *name;
} ResolvePair;

/* One step of a resolution: the name NAME of M, asked for through SPEC */
typedef struct ResolveFrame {
    JsModule *m;
    JsString *spec, *name;
    bool must;
    uint8_t stage; /* RESOLVE_* */
    uint32_t i;    /* the next `export *` to ask */
    JsModule *uo;  /* the binding the `export *` asked so far agree on */
    JsString *un;
} ResolveFrame;

enum {
    RESOLVE_ENTER, /* nothing looked at yet */
    RESOLVE_NAMED, /* waiting for the name an `export { a as NAME } from` stands for */
    RESOLVE_STARS, /* going through the `export *` */
    RESOLVE_STAR,  /* waiting for what one of them has */
};

typedef struct LinkPass {
    JsContext *ctx;
    JsValue *error;     /* the caller's rooted slot: what failed */
    Walk walk;          /* the path of finish_link */
    JsModule **stack;   /* the modules being linked, as Tarjan's algorithm stacks them */
    uint32_t depth, cap;
    uint32_t next_index;
    ResolvePair *set;   /* the (module, name) pairs one resolution is in: open addressing */
    uint32_t set_count, set_cap;
    ResolveFrame *resolving; /* the path of one resolution (see Walk) */
    uint32_t resolve_depth, resolve_cap;
} LinkPass;

static uint32_t pair_hash(const JsModule *m, const JsString *name) {
    uint64_t h = ((uint64_t)(uintptr_t)m >> 3) * 0x9E3779B97F4A7C15u;
    h ^= ((uint64_t)(uintptr_t)name >> 3) * 0xC2B2AE3D27D4EB4Fu;
    return (uint32_t)(h ^ (h >> 29));
}

static void set_clear(LinkPass *lp) {
    if (lp->set_count)
        memset(lp->set, 0, (size_t)lp->set_cap * sizeof(ResolvePair));
    lp->set_count = 0;
}

/* 1 when (M, NAME) is in the set already, 0 when it was added, -1 on OOM */
static int set_add(LinkPass *lp, JsModule *m, JsString *name) {
    JsVm *vm = lp->ctx->vm;
    if ((lp->set_count + 1) * 2 > lp->set_cap) {
        uint32_t ncap = lp->set_cap ? lp->set_cap * 2 : 16;
        ResolvePair *ns = js_realloc_raw(vm, NULL, 0, (size_t)ncap * sizeof(ResolvePair));
        if (!ns)
            return -1;
        memset(ns, 0, (size_t)ncap * sizeof(ResolvePair));
        for (uint32_t i = 0; i < lp->set_cap; i++) {
            if (!lp->set[i].m)
                continue;
            uint32_t at = pair_hash(lp->set[i].m, lp->set[i].name) & (ncap - 1);
            while (ns[at].m)
                at = (at + 1) & (ncap - 1);
            ns[at] = lp->set[i];
        }
        js_realloc_raw(vm, lp->set, (size_t)lp->set_cap * sizeof(ResolvePair), 0);
        lp->set = ns;
        lp->set_cap = ncap;
    }
    uint32_t at = pair_hash(m, name) & (lp->set_cap - 1);
    while (lp->set[at].m) {
        if (lp->set[at].m == m && lp->set[at].name == name)
            return 1;
        at = (at + 1) & (lp->set_cap - 1);
    }
    lp->set[at].m = m;
    lp->set[at].name = name;
    lp->set_count++;
    return 0;
}

/* One of V8's three link errors, with the specifier as the module wrote it (its import
 * attributes are not part of it). */
static int link_error(LinkPass *lp, const char *fmt, JsString *spec, JsString *name,
                      bool spec_first) {
    JsContext *ctx = lp->ctx;
    JsString *shown = js_string_cell_new(ctx->vm, spec->units, spec_len(spec));
    if (!shown)
        *lp->error = js_oom_value(ctx->vm);
    else if (spec_first)
        *lp->error = js_error_fmt(ctx, fmt, shown, name);
    else
        *lp->error = js_error_fmt(ctx, fmt, name, shown);
    return -1;
}

static int link_oom(LinkPass *lp) {
    *lp->error = js_oom_value(lp->ctx->vm);
    return -1;
}

/* The resolution goes on to NAME of M. False on OOM. */
static bool resolve_push(LinkPass *lp, JsModule *m, JsString *spec, JsString *name, bool must) {
    if (lp->resolve_depth == lp->resolve_cap) {
        uint32_t ncap = lp->resolve_cap ? lp->resolve_cap * 2 : 8;
        ResolveFrame *na = js_realloc_raw(lp->ctx->vm, lp->resolving,
                                          (size_t)lp->resolve_cap * sizeof(ResolveFrame),
                                          (size_t)ncap * sizeof(ResolveFrame));
        if (!na)
            return false;
        lp->resolving = na;
        lp->resolve_cap = ncap;
    }
    ResolveFrame *f = &lp->resolving[lp->resolve_depth++];
    f->m = m;
    f->spec = spec;
    f->name = name;
    f->must = must;
    f->stage = RESOLVE_ENTER;
    f->i = 0;
    f->uo = NULL;
    f->un = NULL;
    return true;
}

/*
 * V8's ResolveExport, step for step, since its order decides which error a graph gets: the
 * binding M exports as NAME, asked for through the specifier SPEC. 1 with the binding; 0 when
 * there is none and MUST is false; -1 with the error.
 *
 *  1. a name in the module's table is that binding (its own, or one resolved before);
 *  2. a (module, name) this resolution is in already is a cycle;
 *  3. `export { a as NAME } from`: the binding of `a` there, which must exist; and
 *     `export * as NAME from` (as `import * as ns` then `export { ns as NAME }`): the
 *     namespace of the module named;
 *  4. else the `export *` in order, never for `default`: the one binding they agree on, and
 *     two different ones are an error whoever asks (a name looked for through an `export *`
 *     need not be there: the only case of MUST false);
 *  5. else there is none.
 *
 * V8 recurses from module to module in steps 3 and 4. Here the modules a resolution is going
 * through are lp->resolving (see Walk), each with the step it is at, and R, RO and RN are what
 * the one that just ended gave the one that asked.
 */
static int resolve_export(LinkPass *lp, JsModule *m, JsString *spec, JsString *name, bool must,
                          JsModule **origin, JsString **oname) {
    JsContext *ctx = lp->ctx;
    int r = 0;
    JsModule *ro = NULL;
    JsString *rn = NULL;
    lp->resolve_depth = 0;
    if (!resolve_push(lp, m, spec, name, must))
        return link_oom(lp);
    while (lp->resolve_depth) {
        ResolveFrame *f = &lp->resolving[lp->resolve_depth - 1];
        m = f->m;
        if (f->stage == RESOLVE_ENTER) {
            if (lookup_cell(m, f->name, &ro, &rn)) {
                r = 1;
                lp->resolve_depth--;
                continue;
            }
            if (!f->spec)
                f->spec = m->specifier;
            f->stage = RESOLVE_STARS;
            f->i = m->star_count; /* none to ask, unless step 4 is reached */
            if (!m->synthetic) {
                int seen = set_add(lp, m, f->name);
                if (seen < 0)
                    return link_oom(lp);
                if (seen) {
                    if (f->must)
                        return link_error(lp, "SyntaxError: Detected cycle while resolving name '%S' in '%S'",
                                          f->spec, f->name, false);
                    r = 0;
                    lp->resolve_depth--;
                    continue;
                }
                JsStarExport *named = NULL;
                for (uint32_t i = 0; i < m->star_count && !named; i++) {
                    JsStarExport *re = &m->stars[i];
                    if ((re->kind == JS_REEXP_NAMED || re->kind == JS_REEXP_NS) &&
                        re->exported == f->name && re->source)
                        named = re;
                }
                if (named && named->kind == JS_REEXP_NS) {
                    ro = named->source;
                    rn = NULL;
                    if (!put_binding(ctx, m, f->name, ro, rn, false))
                        return link_oom(lp);
                    r = 1;
                    lp->resolve_depth--;
                    continue;
                }
                if (named) {
                    f->stage = RESOLVE_NAMED;
                    if (!resolve_push(lp, named->source, named->specifier, named->imported, true))
                        return link_oom(lp);
                    continue;
                }
                if (!str_is_default(f->name))
                    f->i = 0;
            }
        } else if (f->stage == RESOLVE_NAMED) {
            /* it was there: one that must be and is not ends the resolution with its error */
            if (!put_binding(ctx, m, f->name, ro, rn, false))
                return link_oom(lp);
            r = 1;
            lp->resolve_depth--;
            continue;
        } else if (f->stage == RESOLVE_STAR) {
            if (r && !f->uo) {
                f->uo = ro;
                f->un = rn;
            } else if (r && (f->uo != ro || f->un != rn)) {
                return link_error(lp, "SyntaxError: The requested module '%S' contains conflicting star exports for name '%S'",
                                  f->spec, f->name, true);
            }
            f->stage = RESOLVE_STARS;
        }
        JsStarExport *star = NULL;
        while (f->i < m->star_count && !star) {
            JsStarExport *re = &m->stars[f->i++];
            if (re->kind == JS_REEXP_ALL && re->source)
                star = re;
        }
        if (star) {
            f->stage = RESOLVE_STAR;
            if (!resolve_push(lp, star->source, star->specifier, f->name, false))
                return link_oom(lp);
            continue;
        }
        if (f->uo) {
            ro = f->uo;
            rn = f->un;
            if (!put_binding(ctx, m, f->name, ro, rn, false))
                return link_oom(lp);
            r = 1;
        } else if (f->must) {
            return link_error(lp, "SyntaxError: The requested module '%S' does not provide an export named '%S'",
                              f->spec, f->name, true);
        } else {
            r = 0;
        }
        lp->resolve_depth--;
    }
    *origin = ro;
    *oname = rn;
    return r;
}

/*
 * V8's RunInitializationCode and the end of a module's linking: its function declarations are
 * made and its exported ones bound before any body runs (so in a cycle the module that runs
 * first can already call the functions the others export: the body runs up to
 * JS_OP_MODULE_READY and parks there, m->fiber, for evaluation to resume), the namespaces it
 * takes are made, and the names it re-exports go into its own.
 */
static bool module_ready(LinkPass *lp, JsModule *m) {
    JsContext *ctx = lp->ctx;
    if (m->body && !m->synthetic && m->status == JS_MOD_LINKED && !m->fiber) {
        JsFiber *fb = js_module_instantiate_body(ctx, m->body, lp->error);
        if (!fb) {
            m->status = JS_MOD_LOAD_FAILED; /* it cannot be made: OOM, or the frame limit */
            m->eval_error = *lp->error;
            return false;
        }
        m->fiber = fb;
    }
    bool ok = true;
    for (uint32_t i = 0; ok && i < m->import_count; i++)
        if (m->imports[i].kind == JS_IMPORT_NS && m->imports[i].source)
            ok = ensure_namespace(ctx, m->imports[i].source);
    for (uint32_t i = 0; ok && i < m->star_count; i++)
        if (m->stars[i].kind == JS_REEXP_NS && m->stars[i].source)
            ok = ensure_namespace(ctx, m->stars[i].source);
    if (ok && m->ns_wanted) /* asked before a failed link took its names back */
        ok = ensure_namespace(ctx, m);
    ok = ok && commit_bindings(ctx, m);
    if (!ok)
        link_oom(lp);
    return ok;
}

/*
 * V8's FinishInstantiate: depth first, a module's dependencies before it; then the binding of
 * every name it imports and of every name it re-exports by name, each with a resolve set of its
 * own; and when the module closes a strongly connected component, module_ready for the
 * component, last entered first. A module linked already is left alone, so a later graph (a
 * dynamic import) only links what it brings. V8 recurses; the path is lp->walk here (see Walk).
 *
 * link_enter: the walk comes to M, which joins the path and Tarjan's stack.
 */
static bool link_enter(LinkPass *lp, JsModule *m) {
    JsVm *vm = lp->ctx->vm;
    if (lp->walk.depth == JS_MAX_MODULE_DEPTH) {
        *lp->error = js_error_from_ascii(lp->ctx, k_too_deep);
        return false;
    }
    if (lp->depth == lp->cap) {
        uint32_t ncap = lp->cap ? lp->cap * 2 : 16;
        JsModule **na = js_realloc_raw(vm, lp->stack, (size_t)lp->cap * sizeof(JsModule *),
                                       (size_t)ncap * sizeof(JsModule *));
        if (!na)
            return link_oom(lp) > 0;
        lp->stack = na;
        lp->cap = ncap;
    }
    if (!walk_push(vm, &lp->walk, m))
        return link_oom(lp) > 0;
    reset_bindings(vm, m);
    lp->stack[lp->depth++] = m;
    m->link_state = 1;
    m->dfs_index = m->dfs_ancestor = lp->next_index++;
    return true;
}

/* link_leave: M's dependencies are linked, and now its names are. */
static bool link_leave(LinkPass *lp, JsModule *m) {
    /* the further names of its own exports: one binding each (export { x as a, x as b }) */
    for (uint32_t i = 0; i < m->star_count; i++) {
        JsStarExport *re = &m->stars[i];
        if (re->kind == JS_REEXP_LOCAL && !put_binding(lp->ctx, m, re->exported, m, re->imported, true))
            return link_oom(lp) > 0;
    }
    for (uint32_t i = 0; i < m->import_count; i++) {
        JsModuleImport *imp = &m->imports[i];
        if (!imp->requested || imp->kind == JS_IMPORT_META)
            continue;
        JsModule *src = dep_for_spec(m, imp->specifier), *origin;
        JsString *oname;
        if (!src)
            continue;
        set_clear(lp);
        if (resolve_export(lp, src, imp->specifier, imp->requested, true, &origin, &oname) <= 0)
            return false;
        imp->source = origin;
        imp->imported_name = oname;
    }
    for (uint32_t i = 0; i < m->star_count; i++) {
        JsModule *origin;
        JsString *oname;
        if (m->stars[i].kind != JS_REEXP_NAMED && m->stars[i].kind != JS_REEXP_NS)
            continue;
        set_clear(lp);
        if (resolve_export(lp, m, NULL, m->stars[i].exported, true, &origin, &oname) <= 0)
            return false;
    }
    if (m->dfs_ancestor == m->dfs_index) {
        JsModule *a;
        do {
            a = lp->stack[lp->depth - 1];
            if (!module_ready(lp, a))
                return false;
            lp->depth--;
            a->link_state = 2;
        } while (a != m);
    }
    return true;
}

/* False with *lp->error: the caller takes the modules still on the stack back to unlinked
 * (link_graph). */
static bool finish_link(LinkPass *lp, JsModule *root) {
    if (root->link_state)
        return true;
    if (!link_enter(lp, root))
        return false;
    while (lp->walk.depth) {
        WalkFrame *f = &lp->walk.frames[lp->walk.depth - 1];
        JsModule *m = f->m, *next = NULL;
        while (f->i < m->dep_count && !next) {
            JsModule *dep = m->deps[f->i++];
            if (!dep)
                continue;
            if (!dep->link_state)
                next = dep;
            else if (dep->link_state == 1 && dep->dfs_ancestor < m->dfs_ancestor)
                m->dfs_ancestor = dep->dfs_ancestor; /* one of the path, or of a component still open */
        }
        if (next) {
            if (!link_enter(lp, next))
                return false;
            continue;
        }
        if (!link_leave(lp, m))
            return false;
        lp->walk.depth--;
        if (lp->walk.depth && m->link_state == 1) { /* its component closes further up */
            JsModule *from = lp->walk.frames[lp->walk.depth - 1].m;
            if (m->dfs_ancestor < from->dfs_ancestor)
                from->dfs_ancestor = m->dfs_ancestor;
        }
    }
    return true;
}

/* Links the graph under ROOT. False with *ERROR, and then nothing of the modules whose linking
 * did not finish stays: importing one again links it again, and fails the same way. */
static bool link_graph(JsContext *ctx, JsModule *root, JsValue *error) {
    LinkPass lp;
    memset(&lp, 0, sizeof lp);
    lp.ctx = ctx;
    lp.error = error;
    bool ok = finish_link(&lp, root);
    if (!ok) {
        for (uint32_t i = 0; i < lp.depth; i++) {
            lp.stack[i]->link_state = 0;
            reset_bindings(ctx->vm, lp.stack[i]);
        }
    }
    walk_free(ctx->vm, &lp.walk);
    js_realloc_raw(ctx->vm, lp.stack, (size_t)lp.cap * sizeof(JsModule *), 0);
    js_realloc_raw(ctx->vm, lp.set, (size_t)lp.set_cap * sizeof(ResolvePair), 0);
    js_realloc_raw(ctx->vm, lp.resolving, (size_t)lp.resolve_cap * sizeof(ResolveFrame), 0);
    return ok;
}

/* ---- evaluate (V8's SourceTextModule::Evaluate and what follows it) ---- */

/*
 * A graph's bodies run in one walk, depth first and each module after its dependencies, with no
 * job run between two of them: the microtasks they queue run after the last. A module with a
 * top-level await (and so every module that imports one, however far up) does not hold the walk:
 * its body runs to its first await that waits, or is put off until the dependencies it waits for
 * are done (pending_async_deps, and it is one of their async_parents), and the walk goes on with
 * its siblings. When the body of a module that waited ends, the modules that waited for nothing
 * else run, in the order the walk met them (async_ordinal), and so on up to the root; when it
 * throws, every module that waited for it has failed with the same error.
 *
 * A module of a cycle is done when the module its cycle closed at is (cycle_root), so that is
 * what a later importer of any of them waits for. What evaluating a module from the top comes to
 * is one promise per module (top_capability), fulfilled with undefined: a second import of a
 * module that is still waiting gets the same promise, not a namespace whose bindings are not
 * there yet. A module whose evaluation threw throws the same error again wherever a later graph
 * comes to run it, after the modules before it have run.
 *
 * V8 recurses from a module into its dependencies (InnerModuleEvaluation); the path is in the
 * heap here (see Walk), with Tarjan's stack next to it as in finish_link.
 *
 * One state V8 cannot be in: a native called by a module's body may run the jobs (js_run_jobs),
 * and so another evaluation inside the walk. A module on another evaluation's path is then
 * passed as one of a cycle is, a module on a path that a job fails makes its walk fail when it
 * is back at it, and one that a job frees of what it waited for runs when its walk leaves it.
 */

typedef struct EvalPass {
    JsContext *ctx;
    Walk walk;
    JsModule **stack; /* Tarjan's: the modules whose component is still open */
    uint32_t depth, cap;
    uint32_t next_index;
    uint32_t id;
    JsValue *error;
} EvalPass;

static void async_fulfilled(JsContext *ctx, JsModule *m);
static void async_rejected(JsContext *ctx, JsModule *m, JsValue err);

static bool module_has_tla(const JsModule *m) {
    return m->body && !m->synthetic && (m->body->fn_flags & JS_FN_ASYNC) != 0;
}

static void async_parents_free(JsVm *vm, JsModule *m) {
    js_realloc_raw(vm, m->async_parents, (size_t)m->async_parent_cap * sizeof(JsModule *), 0);
    m->async_parents = NULL;
    m->async_parent_count = m->async_parent_cap = 0;
}

/* V8's RecordError: M's evaluation threw ERR, and nothing of its body is left to run. */
static void record_error(JsModule *m, JsValue err) {
    m->status = JS_MOD_ERRORED;
    m->eval_error = err;
    m->fiber = NULL;
}

/* M's evaluation is done, and so whatever of it waited. */
static void module_done(JsContext *ctx, JsModule *m) {
    if (m->async_ordinal)
        m->async_ordinal = 1;
    if (m->status == JS_MOD_EVALUATING_ASYNC)
        m->status = JS_MOD_EVALUATED;
    if (m->top_capability)
        js_promise_fulfill(ctx, m->top_capability, js_undefined());
}

/* V8's ExecuteModule for a module without a top-level await: its body runs to its end. False
 * with *ERROR when it throws. */
static bool execute_sync(JsContext *ctx, JsModule *m, JsValue *error) {
    refresh_synthetic(ctx, m); /* what it re-exports of the host's objects, as they are now */
    JsFiber *fb = m->fiber;    /* parked at the end of its instantiation (module_ready) */
    if (!fb)
        return true;
    m->fiber = NULL; /* the fiber roots itself while it runs */
    JsValue bp = js_module_finish_body(ctx, fb);
    int st = js_promise_state(bp);
    if (st == 1) {
        if (m->entry)
            m->returned = js_promise_result(bp);
        return true;
    }
    *error = st == 2 ? js_promise_result(bp) : ascii_value(ctx, "out of memory");
    return false;
}

static bool async_on_fulfilled(JsContext *ctx, JsValue bound, JsValue tv,
                               const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    JsModule *m = value_module(bound);
    if (m->entry && argc > 0 && m->status != JS_MOD_ERRORED)
        m->returned = args[0];
    async_fulfilled(ctx, m);
    return true;
}

static bool async_on_rejected(JsContext *ctx, JsValue bound, JsValue tv,
                              const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    async_rejected(ctx, value_module(bound), argc > 0 ? args[0] : js_undefined());
    return true;
}

/* V8's ExecuteAsyncModule: the body of a module with a top-level await runs as far as it goes
 * now, and async_fulfilled or async_rejected takes it from where it ends, a job later even when
 * that is at once. False on OOM, and then nothing will. */
static bool execute_async(JsContext *ctx, JsModule *m) {
    JsVm *vm = ctx->vm;
    JsValue mv = module_value(m);
    js_gc_protect(vm, &mv);
    refresh_synthetic(ctx, m);
    JsFiber *fb = m->fiber;
    JsValue bp;
    if (fb) {
        m->fiber = NULL; /* the fiber roots itself while it runs, and its promise after */
        bp = js_module_finish_body(ctx, fb);
    } else {
        bp = settled_promise(ctx, true, js_undefined());
    }
    bool ok = js_is_promise(bp) && then_bound(ctx, bp, async_on_fulfilled, async_on_rejected, mv);
    js_gc_unprotect(vm, &mv);
    return ok;
}

/* M waits for REQUIRED. False on OOM. */
static bool add_async_parent(JsVm *vm, JsModule *required, JsModule *m) {
    if (required->async_parent_count == required->async_parent_cap) {
        uint32_t ncap = required->async_parent_cap ? required->async_parent_cap * 2 : 4;
        JsModule **na = js_realloc_raw(vm, required->async_parents,
                                       (size_t)required->async_parent_cap * sizeof(JsModule *),
                                       (size_t)ncap * sizeof(JsModule *));
        if (!na)
            return false;
        required->async_parents = na;
        required->async_parent_cap = ncap;
    }
    required->async_parents[required->async_parent_count++] = m;
    m->pending_async_deps++;
    return true;
}

static bool eval_oom(EvalPass *ep) {
    *ep->error = ascii_value(ep->ctx, "out of memory");
    return false;
}

/* eval_enter: the walk comes to M, which joins the path and Tarjan's stack. */
static bool eval_enter(EvalPass *ep, JsModule *m) {
    JsVm *vm = ep->ctx->vm;
    if (ep->walk.depth == JS_MAX_MODULE_DEPTH) {
        *ep->error = ascii_value(ep->ctx, k_too_deep);
        return false;
    }
    if (ep->depth == ep->cap) {
        uint32_t ncap = ep->cap ? ep->cap * 2 : 16;
        JsModule **na = js_realloc_raw(vm, ep->stack, (size_t)ep->cap * sizeof(JsModule *),
                                       (size_t)ncap * sizeof(JsModule *));
        if (!na)
            return eval_oom(ep);
        ep->stack = na;
        ep->cap = ncap;
    }
    if (!walk_push(vm, &ep->walk, m))
        return eval_oom(ep);
    ep->stack[ep->depth++] = m;
    m->status = JS_MOD_EVALUATING;
    m->eval_pass = ep->id;
    m->dfs_index = m->dfs_ancestor = ep->next_index++;
    m->pending_async_deps = 0;
    return true;
}

/* M requires DEP, which the walk has been to, now or before. False with *ep->error when that
 * makes M fail. */
static bool eval_required(EvalPass *ep, JsModule *m, JsModule *dep) {
    if (dep->status == JS_MOD_EVALUATING) {
        /* of the path, or of a component still open; one of another evaluation's is passed */
        if (dep->eval_pass == ep->id && dep->dfs_ancestor < m->dfs_ancestor)
            m->dfs_ancestor = dep->dfs_ancestor;
    } else if (dep->status == JS_MOD_EVALUATING_ASYNC || dep->status == JS_MOD_EVALUATED) {
        if (dep->cycle_root)
            dep = dep->cycle_root;
    } else if (dep->status != JS_MOD_ERRORED) {
        return true; /* nothing to run or to wait for */
    }
    if (dep->status == JS_MOD_ERRORED) {
        *ep->error = dep->eval_error;
        return false;
    }
    if (dep->async_ordinal >= 2 && !add_async_parent(ep->ctx->vm, dep, m))
        return eval_oom(ep);
    return true;
}

/* eval_leave: M's dependencies have run or wait, and now M runs, or waits for them. */
static bool eval_leave(EvalPass *ep, JsModule *m) {
    JsContext *ctx = ep->ctx;
    if (m->pending_async_deps > 0 || module_has_tla(m)) {
        m->async_ordinal = 2 + ctx->async_evaluations++;
        if (m->pending_async_deps == 0 && !execute_async(ctx, m))
            return eval_oom(ep);
    } else if (!execute_sync(ctx, m, ep->error)) {
        return false;
    }
    if (m->dfs_ancestor == m->dfs_index) { /* its component closes here */
        JsModule *a;
        do {
            a = ep->stack[--ep->depth];
            a->eval_pass = 0;
            a->cycle_root = m;
            if (a->status == JS_MOD_EVALUATING)
                a->status = a->async_ordinal >= 2 ? JS_MOD_EVALUATING_ASYNC : JS_MOD_EVALUATED;
        } while (a != m);
    }
    return true;
}

/* V8's InnerModuleEvaluation of ROOT, which is linked and no more. False with *ep->error: the
 * caller fails the modules still on the stack (evaluate). */
static bool inner_evaluation(EvalPass *ep, JsModule *root) {
    if (!eval_enter(ep, root))
        return false;
    while (ep->walk.depth) {
        WalkFrame *f = &ep->walk.frames[ep->walk.depth - 1];
        JsModule *m = f->m, *next = NULL;
        if (m->status != JS_MOD_EVALUATING) { /* a job failed it while a body below it ran */
            *ep->error = m->eval_error;
            return false;
        }
        while (f->i < m->dep_count && !next) {
            JsModule *dep = m->deps[f->i];
            if (dep && dep->status == JS_MOD_LINKED) {
                next = dep; /* f->i stays: back from it, M requires it as it then is */
            } else {
                f->i++;
                if (dep && !eval_required(ep, m, dep))
                    return false;
            }
        }
        if (next) {
            if (!eval_enter(ep, next))
                return false;
            continue;
        }
        if (!eval_leave(ep, m))
            return false;
        ep->walk.depth--;
    }
    return true;
}

/* The modules a dependency's end made ready to run. */
typedef struct ExecList {
    JsModule **v;
    uint32_t count, cap;
} ExecList;

/* V8's GatherAvailableAncestors, a module at a time: the modules that waited for FROM wait for
 * one dependency less, and those that wait for none join the list. */
static void gather_parents(JsContext *ctx, JsModule *from, ExecList *l) {
    JsVm *vm = ctx->vm;
    for (uint32_t i = from->async_parent_count; i-- > 0;) {
        JsModule *p = from->async_parents[i];
        if (p->status == JS_MOD_EVALUATING && p->async_ordinal == 0) {
            /* on the path of a walk (a native of a body ran this job): it runs when the walk
             * leaves it */
            if (p->pending_async_deps)
                p->pending_async_deps--;
            continue;
        }
        if (p->exec_mark || p->status == JS_MOD_ERRORED ||
            (p->cycle_root && p->cycle_root->status == JS_MOD_ERRORED) ||
            (!p->cycle_root && p->status != JS_MOD_EVALUATING))
            continue;
        if (!p->pending_async_deps || --p->pending_async_deps)
            continue;
        if (l->count == l->cap) {
            uint32_t ncap = l->cap ? l->cap * 2 : 8;
            JsModule **na = js_realloc_raw(vm, l->v, (size_t)l->cap * sizeof(JsModule *),
                                           (size_t)ncap * sizeof(JsModule *));
            if (!na) {
                async_rejected(ctx, p, ascii_value(ctx, "out of memory"));
                continue;
            }
            l->v = na;
            l->cap = ncap;
        }
        p->exec_mark = true;
        l->v[l->count++] = p;
    }
    async_parents_free(vm, from);
}

/* The list in the order its modules began to wait (heapsort: the engine has no libc). */
static void exec_list_sort(ExecList *l) {
    JsModule **v = l->v;
    uint32_t n = l->count;
    for (uint32_t start = n / 2; start-- > 0;) {
        for (uint32_t root = start;;) {
            uint32_t c = 2 * root + 1;
            if (c >= n)
                break;
            if (c + 1 < n && v[c + 1]->async_ordinal > v[c]->async_ordinal)
                c++;
            if (v[root]->async_ordinal >= v[c]->async_ordinal)
                break;
            JsModule *t = v[root];
            v[root] = v[c];
            v[c] = t;
            root = c;
        }
    }
    for (uint32_t end = n; end-- > 1;) {
        JsModule *t = v[0];
        v[0] = v[end];
        v[end] = t;
        for (uint32_t root = 0;;) {
            uint32_t c = 2 * root + 1;
            if (c >= end)
                break;
            if (c + 1 < end && v[c + 1]->async_ordinal > v[c]->async_ordinal)
                c++;
            if (v[root]->async_ordinal >= v[c]->async_ordinal)
                break;
            JsModule *u = v[root];
            v[root] = v[c];
            v[c] = u;
            root = c;
        }
    }
}

/* V8's AsyncModuleExecutionFulfilled: the body of M, which waited, has ended. */
static void async_fulfilled(JsContext *ctx, JsModule *m) {
    JsVm *vm = ctx->vm;
    if (m->status == JS_MOD_ERRORED)
        return;
    JsValue mv = module_value(m);
    js_gc_protect(vm, &mv);
    module_done(ctx, m);

    /* a module that waits for none of its dependencies any more runs now; one without a
     * top-level await is then done too, and the modules that waited for it are looked at with it */
    ExecList l = {NULL, 0, 0};
    gather_parents(ctx, m, &l);
    for (uint32_t i = 0; i < l.count; i++)
        if (!module_has_tla(l.v[i]))
            gather_parents(ctx, l.v[i], &l);
    exec_list_sort(&l);

    JsValue err = js_undefined();
    js_gc_protect(vm, &err);
    for (uint32_t i = 0; i < l.count; i++) {
        JsModule *x = l.v[i];
        x->exec_mark = false; /* until here it could not be evicted */
        if (x->status == JS_MOD_ERRORED)
            continue; /* one that ran before it in this list threw */
        if (module_has_tla(x)) {
            if (!execute_async(ctx, x))
                async_rejected(ctx, x, ascii_value(ctx, "out of memory"));
        } else if (!execute_sync(ctx, x, &err)) {
            async_rejected(ctx, x, err);
        } else {
            module_done(ctx, x);
        }
    }
    js_gc_unprotect(vm, &err);
    js_realloc_raw(vm, l.v, (size_t)l.cap * sizeof(JsModule *), 0);
    js_gc_unprotect(vm, &mv);
}

/* V8's AsyncModuleExecutionRejected: M, which waited, threw ERR, or a dependency it waited for
 * did; so have the modules that wait for M, and theirs (V8 recurses; the path is a Walk). */
static void async_rejected(JsContext *ctx, JsModule *m, JsValue err) {
    JsVm *vm = ctx->vm;
    js_gc_protect(vm, &err);
    Walk w = {NULL, 0, 0};
    JsModule *next = m;
    for (;;) {
        if (next) {
            JsModule *x = next;
            next = NULL;
            if (x->status != JS_MOD_ERRORED) {
                record_error(x, err);
                if (x->async_ordinal)
                    x->async_ordinal = 1;
                if (x->top_capability)
                    js_promise_reject(ctx, x->top_capability, err);
                if (!walk_push(vm, &w, x))
                    break; /* OOM: the modules that wait for it go on waiting */
            }
        }
        if (!w.depth)
            break;
        WalkFrame *f = &w.frames[w.depth - 1];
        if (f->i < f->m->async_parent_count) {
            next = f->m->async_parents[f->i++];
        } else {
            async_parents_free(vm, f->m);
            w.depth--;
        }
    }
    walk_free(vm, &w);
    js_gc_unprotect(vm, &err);
}

/*
 * V8's Module::Evaluate: the promise of M's evaluation from the top, fulfilled with undefined
 * once M, with all it imports, has run to its end, and rejected with what was thrown. Undefined
 * on OOM.
 *
 * Where V8 stops the process: a module of a cycle that ran, when the module its cycle closed at
 * failed later (after a top-level await) and was never asked for from the top. The specification
 * says what this gives, the cycle root's error, and that is what it gives here.
 */
static JsValue evaluate(JsContext *ctx, JsModule *m) {
    JsVm *vm = ctx->vm;
    if (m->status == JS_MOD_EVALUATING) /* asked for by a job a native of a body on the path ran */
        return settled_promise(ctx, true, js_undefined());
    if ((m->status == JS_MOD_EVALUATING_ASYNC || m->status == JS_MOD_EVALUATED) && m->cycle_root)
        m = m->cycle_root;
    if (m->top_capability)
        return js_value_from_cell(&m->top_capability->gc);
    if (m->status == JS_MOD_ERRORED)
        return settled_promise(ctx, false, m->eval_error);
    if (m->status != JS_MOD_LINKED && m->status != JS_MOD_EVALUATING_ASYNC &&
        m->status != JS_MOD_EVALUATED)
        return rejected_ascii(ctx, "internal error: module is not linked");

    JsValue mv = module_value(m);
    js_gc_protect(vm, &mv);
    JsPromise *cap = js_promise_alloc(ctx);
    if (!cap) {
        js_gc_unprotect(vm, &mv);
        return js_undefined();
    }
    m->top_capability = cap;
    JsValue capv = js_value_from_cell(&cap->gc);
    js_gc_protect(vm, &capv);
    bool ok = true;
    if (m->status == JS_MOD_LINKED) {
        JsValue err = js_undefined();
        js_gc_protect(vm, &err);
        EvalPass ep;
        memset(&ep, 0, sizeof ep);
        ep.ctx = ctx;
        ep.error = &err;
        if (++ctx->eval_passes == 0)
            ctx->eval_passes = 1;
        ep.id = ctx->eval_passes;
        ok = inner_evaluation(&ep, m);
        if (!ok) {
            /* what was thrown is the error of every module the walk had not finished with */
            for (uint32_t i = 0; i < ep.depth; i++) {
                JsModule *a = ep.stack[i];
                a->eval_pass = 0;
                record_error(a, err);
                if (a->async_ordinal)
                    a->async_ordinal = 1;
                async_parents_free(vm, a);
            }
            if (m->status != JS_MOD_ERRORED) /* the walk did not even start (OOM) */
                record_error(m, err);
            js_promise_reject(ctx, js_value_promise(capv), err);
        }
        walk_free(vm, &ep.walk);
        js_realloc_raw(vm, ep.stack, (size_t)ep.cap * sizeof(JsModule *), 0);
        js_gc_unprotect(vm, &err);
    }
    if (ok && m->async_ordinal < 2 && m->status != JS_MOD_ERRORED)
        js_promise_fulfill(ctx, js_value_promise(capv), js_undefined());
    js_gc_unprotect(vm, &capv);
    js_gc_unprotect(vm, &mv);
    return capv;
}

/* ---- public entry ---- */

/* What the root's promise fulfills with: the namespace, or for js_eval_entry what the body returned. */
static void root_fulfill(JsContext *ctx, JsPromise *done, JsModule *root, bool entry) {
    if (entry)
        js_promise_fulfill(ctx, done, root->returned);
    else if (!ensure_namespace(ctx, root)) /* as V8 makes a namespace: when it is first taken */
        js_promise_reject(ctx, done, ascii_value(ctx, "out of memory"));
    else
        js_promise_fulfill(ctx, done, js_value_from_cell(&root->exports->gc));
}

/* The root's evaluation is done: BOUND is { promise, module, entry } (root_wait). */
static bool root_on_eval_done(JsContext *ctx, JsValue bound, JsValue tv,
                              const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    (void)args;
    (void)argc;
    *r = js_undefined();
    JsObject *st = js_value_object(bound);
    JsValue pv = mstate_get(ctx, st, "promise");
    JsValue mv = mstate_get(ctx, st, "module");
    bool entry = mstate_get(ctx, st, "entry").bits == JS_SPECIAL_TRUE;
    if (js_is_promise(pv) && js_is_object(mv))
        root_fulfill(ctx, js_value_promise(pv), value_module(mv), entry);
    return true;
}

static bool root_on_eval_fail_state(JsContext *ctx, JsValue bound, JsValue tv,
                                    const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    JsValue pv = mstate_get(ctx, js_value_object(bound), "promise");
    if (js_is_promise(pv))
        js_promise_reject(ctx, js_value_promise(pv), argc > 0 ? args[0] : js_undefined());
    return true;
}

static bool root_on_eval_fail(JsContext *ctx, JsValue bound, JsValue tv,
                              const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    js_promise_reject(ctx, js_value_promise(bound), argc > 0 ? args[0] : js_undefined());
    return true;
}

/* DONE settles as the evaluation EP of ROOT does, a job after it. False on OOM. */
static bool root_wait(JsContext *ctx, JsValue donev, JsModule *root, JsValue ep, bool entry) {
    JsVm *vm = ctx->vm;
    JsValue mv = module_value(root);
    js_gc_protect(vm, &donev);
    js_gc_protect(vm, &mv);
    js_gc_protect(vm, &ep);
    JsValue statev = js_object_new(ctx);
    js_gc_protect(vm, &statev);
    bool ok = js_is_object(statev);
    if (ok) {
        JsObject *st = js_value_object(statev);
        ok = mstate_set(ctx, st, "promise", donev) && mstate_set(ctx, st, "module", mv) &&
             mstate_set(ctx, st, "entry", js_bool(entry)) &&
             then_bound(ctx, ep, root_on_eval_done, root_on_eval_fail_state, statev);
    }
    js_gc_unprotect(vm, &statev);
    js_gc_unprotect(vm, &ep);
    js_gc_unprotect(vm, &mv);
    js_gc_unprotect(vm, &donev);
    return ok;
}

/* graph fetched: the synchronous prepare and link, then evaluation */
static void root_fetched(JsContext *ctx, JsValue bound, const JsValue *args, int argc, bool entry) {
    JsPromise *done = js_value_promise(bound);
    if (argc < 1)
        return;
    JsModule *root = value_module(args[0]);

    JsModError err = {NULL, 0, false};
    if (!prepare_graph(ctx, root, &err)) {
        js_promise_reject(ctx, done, mod_error_value(ctx, &err));
        return;
    }
    JsValue ierr = js_undefined();
    js_gc_protect(ctx->vm, &ierr);
    bool envs = link_graph(ctx, root, &ierr);
    if (!envs)
        js_promise_reject(ctx, done, ierr);
    js_gc_unprotect(ctx->vm, &ierr);
    if (!envs)
        return;

    /* a job after the evaluation's promise settles, even when it is settled already: the
     * specification's ContinueDynamicImport, and so what an import() comes to keeps its place
     * among the other jobs whether or not the module was evaluated before */
    JsValue ep = evaluate(ctx, root);
    if (!js_is_promise(ep) || !root_wait(ctx, bound, root, ep, entry))
        js_promise_reject(ctx, js_value_promise(bound), ascii_value(ctx, "out of memory"));
}

static bool root_on_fetched(JsContext *ctx, JsValue bound, JsValue tv,
                            const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    root_fetched(ctx, bound, args, argc, false);
    return true;
}

static bool entry_on_fetched(JsContext *ctx, JsValue bound, JsValue tv,
                             const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    root_fetched(ctx, bound, args, argc, true);
    return true;
}

static JsValue eval_root(JsContext *ctx, const uint16_t *specifier, size_t spec_len, bool entry) {
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
        /* the root's record is made inside load_graph, synchronously: mark it there */
        if (entry)
            ctx->entry_spec = js_value_string(canonv);
        JsValue gp = load_graph(ctx, js_value_string(canonv));
        ctx->entry_spec = NULL;
        wired = js_is_promise(gp) &&
                then_bound(ctx, gp, entry ? entry_on_fetched : root_on_fetched, root_on_eval_fail, donev);
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

JsValue js_eval_module(JsContext *ctx, const uint16_t *specifier, size_t spec_len) {
    return eval_root(ctx, specifier, spec_len, false);
}

JsValue js_eval_entry(JsContext *ctx, const uint16_t *specifier, size_t spec_len) {
    return eval_root(ctx, specifier, spec_len, true);
}

/* import()'s options: the `with` attributes, appended to SPEC as the compiler appends a static
 * import's ("\0key\0value", by key; only `type`, the one V8 hands its host). The new request in
 * *OUT; false with the TypeError V8 gives in *ERR. SPEC and OPTS are rooted by the caller. */
static bool request_with_options(JsContext *ctx, JsString *spec, JsValue opts, JsString **out, JsValue *err) {
    *out = spec;
    if (js_is_undefined(opts))
        return true;
    if (!js_is_object(opts) && !js_is_function(opts) && !js_is_promise(opts)) {
        *err = js_error_fmt(ctx, "TypeError: The second argument to import() must be an object");
        return false;
    }
    static const uint16_t kwith[] = {'w', 'i', 't', 'h'}, ktype[] = {'t', 'y', 'p', 'e'};
    JsValue key = js_atom(ctx->vm, kwith, 4), with = js_undefined(), type = js_undefined();
    if (!js_is_string(key) || !js_get_property(ctx, opts, key, &with)) {
        *err = js_is_undefined(ctx->pending_error) ? js_oom_value(ctx->vm) : js_take_error(ctx);
        return false;
    }
    if (js_is_undefined(with))
        return true;
    if (!js_is_object(with) && !js_is_function(with) && !js_is_promise(with)) {
        *err = js_error_fmt(ctx, "TypeError: The 'with' option must be an object");
        return false;
    }
    js_gc_protect(ctx->vm, &with);
    /* every attribute must be a string; the ones the host does not know are dropped, as V8 does */
    JsValue names = js_undefined();
    bool ok = true;
    if (js_is_object(with)) {
        JsObject *w = js_value_object(with);
        for (uint32_t i = 0; ok && i < w->props.used; i++) {
            JsString *k = w->props.entries[i].key;
            if (!k || k == JS_MAP_TOMBSTONE || js_prop_hidden_at(w, i))
                continue;
            JsValue v;
            ok = js_get_property(ctx, with, js_value_from_cell(&k->gc), &v);
            if (ok && !js_is_string(v)) {
                *err = js_error_fmt(ctx, "TypeError: Import attribute value must be a string");
                js_gc_unprotect(ctx->vm, &with);
                return false;
            }
            if (ok && k->length == 4 && !memcmp(k->units, ktype, sizeof ktype))
                type = v;
        }
    }
    (void)names;
    js_gc_unprotect(ctx->vm, &with);
    if (!ok) {
        *err = js_is_undefined(ctx->pending_error) ? js_oom_value(ctx->vm) : js_take_error(ctx);
        return false;
    }
    if (js_is_undefined(type))
        return true;
    JsString *ts = js_value_string(type);
    size_t total = (size_t)spec->length + 6 + ts->length;
    uint16_t *u = js_realloc_raw(ctx->vm, NULL, 0, total * sizeof *u);
    if (!u) {
        *err = js_oom_value(ctx->vm);
        return false;
    }
    memcpy(u, spec->units, spec->length * sizeof *u);
    u[spec->length] = 0;
    memcpy(u + spec->length + 1, ktype, sizeof ktype);
    u[spec->length + 5] = 0;
    memcpy(u + spec->length + 6, ts->units, ts->length * sizeof *u);
    JsValue typev = type;
    js_gc_protect(ctx->vm, &typev);
    JsValue a = js_atom(ctx->vm, u, total);
    js_gc_unprotect(ctx->vm, &typev);
    js_realloc_raw(ctx->vm, u, total * sizeof *u, 0);
    if (!js_is_string(a)) {
        *err = js_oom_value(ctx->vm);
        return false;
    }
    *out = js_value_string(a);
    return true;
}

JsValue js_module_dynamic_import(JsContext *ctx, JsModule *referrer, JsValue specv) {
    JsVm *vm = ctx->vm;
    JsPromise *done = js_promise_alloc(ctx);
    if (!done)
        return js_undefined();
    JsValue donev = js_value_from_cell(&done->gc);
    js_gc_protect(vm, &donev);

    /* the compiler hands [specifier, options] */
    JsValue optsv = js_undefined();
    if (js_is_object(specv) && js_value_object(specv)->obj_kind == JS_OBJ_ARRAY &&
        js_value_object(specv)->elem_count == 2) {
        optsv = js_value_object(specv)->elems[1];
        specv = js_value_object(specv)->elems[0];
    }
    js_gc_protect(vm, &optsv);

    /* ToString + intern the specifier */
    js_gc_protect(vm, &specv);
    JsString *ss = js_op_to_string(ctx, specv); /* its toString may run, and throw */
    js_gc_unprotect(vm, &specv);
    if (!ss) {
        js_gc_unprotect(vm, &optsv);
        js_promise_reject(ctx, js_value_promise(donev), js_take_error(ctx));
        js_gc_unprotect(vm, &donev);
        return donev;
    }
    JsString *spec = NULL;
    {
        JsValue ssv = js_value_from_cell(&ss->gc);
        js_gc_protect(vm, &ssv);
        JsValue oerr = js_undefined();
        bool nul = spec_len(ss) < ss->length;
        if (nul || !request_with_options(ctx, ss, optsv, &ss, &oerr)) {
            if (nul)
                oerr = js_error_fmt(ctx, "TypeError: A module specifier may not contain a NUL character");
            js_gc_unprotect(vm, &ssv);
            js_gc_unprotect(vm, &optsv);
            js_promise_reject(ctx, js_value_promise(donev), oerr);
            js_gc_unprotect(vm, &donev);
            return donev;
        }
        ssv = js_value_from_cell(&ss->gc);
        spec = js_intern_cell(vm, ss);
        js_gc_unprotect(vm, &ssv);
    }
    js_gc_unprotect(vm, &optsv);
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

JsValue js_module_get_export(JsContext *ctx, JsValue ns, const uint16_t *name,
                             size_t name_len) {
    if (!js_is_object(ns))
        return js_undefined();
    JsValue key = js_atom(ctx->vm, name, name_len);
    if (!js_is_string(key))
        return js_undefined();
    return js_object_get(ctx->vm, ns, key);
}
