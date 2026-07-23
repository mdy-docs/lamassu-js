#include "js_bytecode.h" /* js_builtins_init */
#include "lamassu_internal.h"

#define JS_GC_DEFAULT_THRESHOLD ((size_t)256 * 1024)

#include <stdlib.h>

static void *js_default_realloc(void *ud, void *ptr, size_t old_size, size_t new_size) {
    (void)ud;
    (void)old_size;
    if (new_size == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, new_size);
}

void *js_realloc_raw(JsVm *vm, void *ptr, size_t old_size, size_t new_size) {
    if (new_size == 0) {
        if (ptr) {
            vm->realloc_fn(vm->alloc_ud, ptr, old_size, 0);
            vm->bytes_live -= old_size;
        }
        return NULL;
    }
    /* Enforce the heap limit at the single byte-accounting choke point so that
     * bulk allocations (array/string buffers, hash tables) are capped too, not
     * just cell headers. Only net growth can breach the limit; a shrink or a
     * move to a smaller size never does. Skip while the collector is running:
     * its own bookkeeping must not recurse into a collect, and sweep only frees
     * (shrinks) anyway. */
    if (vm->heap_limit && new_size > old_size && !vm->gc_running) {
        size_t grow = new_size - old_size;
        if (vm->bytes_live + grow > vm->heap_limit) {
            js_gc_collect(vm);
            if (vm->bytes_live + grow > vm->heap_limit)
                return NULL;
        }
    }
    void *p = vm->realloc_fn(vm->alloc_ud, ptr, old_size, new_size);
    if (p)
        vm->bytes_live = vm->bytes_live - old_size + new_size;
    return p;
}

JsVm *js_vm_new(const JsVmConfig *cfg) {
    JsReallocFn fn = cfg ? cfg->realloc_fn : NULL;
    void *ud = cfg ? cfg->alloc_ud : NULL;
    if (!fn)
        fn = js_default_realloc;

    JsVm *vm = fn(ud, NULL, 0, sizeof *vm);
    if (!vm)
        return NULL;
    memset(vm, 0, sizeof *vm);
    vm->realloc_fn = fn;
    vm->alloc_ud = ud;
    vm->bytes_live = sizeof *vm;
    vm->gc_threshold_init =
        cfg && cfg->gc_threshold ? cfg->gc_threshold : JS_GC_DEFAULT_THRESHOLD;
    vm->gc_threshold = vm->gc_threshold_init;
    vm->gc_stress = cfg && cfg->gc_stress;
    vm->heap_limit = cfg ? cfg->heap_limit : 0;
    vm->rng_state = cfg && cfg->rng_seed ? cfg->rng_seed
                                         : UINT64_C(0x9E3779B97F4A7C15);
    return vm;
}

void js_vm_free(JsVm *vm) {
    if (!vm)
        return;
    js_jobs_free_all(vm); /* microtask queue holds raw job nodes */
    while (vm->contexts)
        js_context_free(vm->contexts);
    JsGcCell *c = vm->cells;
    while (c) {
        JsGcCell *next = c->next;
        js_gc_free_cell(vm, c, false); /* atom table torn down below */
        c = next;
    }
    vm->cells = NULL;
    js_atoms_free(vm);
    js_realloc_raw(vm, vm->roots, vm->roots_cap * sizeof *vm->roots, 0);
    js_realloc_raw(vm, vm->mark_stack, vm->mark_cap * sizeof *vm->mark_stack, 0);
    JsReallocFn fn = vm->realloc_fn;
    void *ud = vm->alloc_ud;
    fn(ud, vm, sizeof *vm, 0);
}

size_t js_vm_allocated_bytes(const JsVm *vm) {
    return vm->bytes_live;
}

size_t js_gc_live_cells(const JsVm *vm) {
    return vm->cell_count;
}

static bool js_define_global(JsVm *vm, JsObject *globals, const char *name,
                             JsValue v) {
    uint16_t units[16];
    size_t n = 0;
    while (name[n] && n < 16) {
        units[n] = (uint16_t)(unsigned char)name[n];
        n++;
    }
    JsValue key = js_atom(vm, units, n);
    if (!js_is_string(key))
        return false;
    return js_map_set(vm, &globals->props, js_value_string(key), v);
}

JsContext *js_context_new(JsVm *vm) {
    /* Raw allocation cannot collect, so it's safe to build ctx here, before
     * globals — js_object_new needs a JsContext (globals' [[Prototype]] is
     * ctx->object_proto), and this is the only way to have one to give it.
     * ctx isn't linked into vm->contexts (so not GC-visited) until after
     * globals is created below, so its not-yet-initialized fields are never
     * read by a collection that allocation might trigger. */
    JsContext *ctx = js_realloc_raw(vm, NULL, 0, sizeof *ctx);
    if (!ctx)
        return NULL;
    ctx->vm = vm;
    ctx->string_methods = NULL;
    ctx->number_methods = NULL;
    ctx->promise_methods = NULL;
    ctx->object_proto = NULL;
    ctx->array_proto = NULL;
    ctx->regexp_proto = NULL;
    ctx->date_proto = NULL;
    ctx->map_proto = NULL;
    ctx->set_proto = NULL;
    ctx->repl_scope = NULL;
    ctx->repl_const = NULL;
    ctx->modules = NULL;
    ctx->module_count = ctx->module_cap = 0;
    ctx->loader = NULL;
    ctx->canon = NULL;
    ctx->loader_ud = NULL;
    ctx->fiber = NULL;
    ctx->fuel = 0;
    ctx->error_pos = 0;
    ctx->reentry_depth = 0;

    JsValue globals = js_object_new(ctx); /* safe point; ctx->object_proto is
                                           * still NULL, so this correctly
                                           * comes back prototype-less — see
                                           * the globalThis wiring below */
    if (!js_is_object(globals)) {
        js_realloc_raw(vm, ctx, sizeof *ctx, 0);
        return NULL;
    }
    ctx->globals = js_value_object(globals);
    ctx->next = vm->contexts;
    ctx->prev_link = &vm->contexts;
    if (vm->contexts)
        vm->contexts->prev_link = &ctx->next;
    vm->contexts = ctx;
    /* Global constants (globals are rooted via the context now). */
    if (!js_define_global(vm, ctx->globals, "undefined", js_undefined()) ||
        !js_define_global(vm, ctx->globals, "NaN", js_number(__builtin_nan(""))) ||
        !js_define_global(vm, ctx->globals, "Infinity", js_number(__builtin_inf())) ||
        !js_define_global(vm, ctx->globals, "globalThis",
                          js_value_from_cell(&ctx->globals->gc))) {
        js_context_free(ctx);
        return NULL;
    }
    if (!js_builtins_init(ctx)) {
        js_context_free(ctx);
        return NULL;
    }
    /* globalThis is ctx->globals itself, created above before
     * ctx->object_proto existed (js_builtins_init creates it) — real JS has
     * it inherit Object.prototype like any other ordinary object, so wire
     * it up now that one exists. */
    ctx->globals->proto = js_value_from_cell(&ctx->object_proto->gc);
    return ctx;
}

void js_context_free(JsContext *ctx) {
    if (!ctx)
        return;
    js_modules_free_registry(ctx); /* module cells are freed via the GC sweep */
    *ctx->prev_link = ctx->next;
    if (ctx->next)
        ctx->next->prev_link = ctx->prev_link;
    js_realloc_raw(ctx->vm, ctx, sizeof *ctx, 0);
}

void js_set_module_loader(JsContext *ctx, JsModuleLoader load,
                          JsModuleCanonicalizer canon, void *ud) {
    ctx->loader = load;
    ctx->canon = canon;
    ctx->loader_ud = ud;
}

JsValue js_context_globals(JsContext *ctx) {
    return js_value_from_cell(&ctx->globals->gc);
}

uint32_t js_context_error_pos(const JsContext *ctx) {
    return ctx->error_pos;
}

void js_context_set_fuel(JsContext *ctx, uint64_t fuel) {
    ctx->fuel = fuel;
}
