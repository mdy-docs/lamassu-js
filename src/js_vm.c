#include "js_bytecode.h" /* js_builtins_init */
#include "jsvm_internal.h"

#define JS_GC_DEFAULT_THRESHOLD ((size_t)256 * 1024)

#ifndef JSVM_FREESTANDING
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
#endif

void *js_realloc_raw(JsVm *vm, void *ptr, size_t old_size, size_t new_size) {
    if (new_size == 0) {
        if (ptr) {
            vm->realloc_fn(vm->alloc_ud, ptr, old_size, 0);
            vm->bytes_live -= old_size;
        }
        return NULL;
    }
    void *p = vm->realloc_fn(vm->alloc_ud, ptr, old_size, new_size);
    if (p)
        vm->bytes_live = vm->bytes_live - old_size + new_size;
    return p;
}

JsVm *js_vm_new(const JsVmConfig *cfg) {
    JsReallocFn fn = cfg ? cfg->realloc_fn : NULL;
    void *ud = cfg ? cfg->alloc_ud : NULL;
#ifndef JSVM_FREESTANDING
    if (!fn)
        fn = js_default_realloc;
#endif
    if (!fn)
        return NULL;

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
    JsValue globals = js_object_new(vm); /* safe point */
    if (!js_is_object(globals))
        return NULL;
    /* Raw allocation cannot collect, so `globals` stays alive unrooted. */
    JsContext *ctx = js_realloc_raw(vm, NULL, 0, sizeof *ctx);
    if (!ctx)
        return NULL;
    ctx->vm = vm;
    ctx->globals = js_value_object(globals);
    ctx->string_methods = NULL;
    ctx->array_methods = NULL;
    ctx->number_methods = NULL;
    ctx->fiber = NULL;
    ctx->fuel = 0;
    ctx->error_pos = 0;
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
    return ctx;
}

void js_context_free(JsContext *ctx) {
    if (!ctx)
        return;
    *ctx->prev_link = ctx->next;
    if (ctx->next)
        ctx->next->prev_link = ctx->prev_link;
    js_realloc_raw(ctx->vm, ctx, sizeof *ctx, 0);
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
