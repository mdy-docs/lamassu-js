#include "js_bytecode.h" /* js_gc_mark_jobs */
#include "lamassu_internal.h"
#include "js_date.h"
#include "js_mapobj.h"
#include "js_setobj.h"
#ifdef LAMASSU_HAS_REGEX
#include "js_regexp.h"
#endif

/*
 * Stop-the-world mark-sweep. Marking uses an explicit stack (no C
 * recursion); if the stack cannot grow, the cycle is abandoned — every
 * cell is treated as live and the threshold backs off, so an OOM during
 * GC is never fatal and never frees a reachable cell.
 */

static bool js_mark_push(JsVm *vm, JsGcCell *c) {
    if (vm->mark_len == vm->mark_cap) {
        size_t ncap = vm->mark_cap ? vm->mark_cap * 2 : 64;
        JsGcCell **ns = js_realloc_raw(vm, vm->mark_stack,
                                       vm->mark_cap * sizeof *ns, ncap * sizeof *ns);
        if (!ns) {
            vm->mark_overflow = true;
            return false;
        }
        vm->mark_stack = ns;
        vm->mark_cap = ncap;
    }
    vm->mark_stack[vm->mark_len++] = c;
    return true;
}

void js_gc_mark_cell(JsVm *vm, JsGcCell *c) {
    if (!c || c->mark)
        return;
    c->mark = 1;
    /* strings and bytecode buffers have no children */
    if (c->kind != JS_KIND_STRING && c->kind != JS_KIND_BYTECODE)
        js_mark_push(vm, c);
}

void js_gc_mark_value(JsVm *vm, JsValue v) {
    if (js_value_is_cell(v))
        js_gc_mark_cell(vm, js_value_cell(v));
}

static void js_gc_trace(JsVm *vm) {
    while (vm->mark_len) {
        JsGcCell *c = vm->mark_stack[--vm->mark_len];
        switch ((JsGcKind)c->kind) {
        case JS_KIND_OBJECT: {
            JsObject *o = (JsObject *)c;
            for (uint32_t i = 0; i < o->props.capacity; i++) {
                JsMapEntry *e = &o->props.entries[i];
                if (e->key && e->key != JS_MAP_TOMBSTONE) {
                    js_gc_mark_cell(vm, &e->key->gc);
                    js_gc_mark_value(vm, e->value);
                }
            }
            for (uint32_t i = 0; i < o->elem_count; i++)
                js_gc_mark_value(vm, o->elems[i]);
            js_gc_mark_value(vm, o->proto);
#ifdef LAMASSU_HAS_REGEX
            if (o->obj_kind == JS_OBJ_REGEXP)
                js_regexp_mark(vm, o);
#endif
            if (o->obj_kind == JS_OBJ_MAP)
                js_mapobj_mark(vm, o);
            if (o->obj_kind == JS_OBJ_SET)
                js_setobj_mark(vm, o);
            break;
        }
        case JS_KIND_FUNCTION: {
            JsFunctionCell *f = (JsFunctionCell *)c;
            for (uint32_t i = 0; i < f->const_count; i++)
                js_gc_mark_value(vm, f->consts[i]);
            if (f->name)
                js_gc_mark_cell(vm, &f->name->gc);
            if (f->module)
                js_gc_mark_cell(vm, &f->module->gc);
            break;
        }
        case JS_KIND_MODULE:
            js_gc_mark_module(vm, (JsModule *)c);
            break;
        case JS_KIND_FIBER: {
            JsFiber *fb = (JsFiber *)c;
            for (uint32_t i = 0; i < fb->sp; i++)
                js_gc_mark_value(vm, fb->stack[i]);
            for (uint32_t i = 0; i < fb->frame_count; i++)
                js_gc_mark_cell(vm, &fb->frames[i].closure->gc);
            for (JsUpvalue *uv = fb->open_upvals; uv; uv = uv->next_open)
                js_gc_mark_cell(vm, &uv->gc);
            if (fb->caller)
                js_gc_mark_cell(vm, &fb->caller->gc);
            if (fb->result_promise)
                js_gc_mark_cell(vm, &fb->result_promise->gc);
            js_gc_mark_value(vm, fb->error);
            break;
        }
        case JS_KIND_PROMISE: {
            JsPromise *p = (JsPromise *)c;
            js_gc_mark_value(vm, p->value);
            for (JsReaction *rx = p->reactions; rx; rx = rx->next) {
                js_gc_mark_value(vm, rx->on_fulfilled);
                js_gc_mark_value(vm, rx->on_rejected);
                if (rx->result)
                    js_gc_mark_cell(vm, &rx->result->gc);
                if (rx->fiber)
                    js_gc_mark_cell(vm, &rx->fiber->gc);
            }
            break;
        }
        case JS_KIND_CLOSURE: {
            JsClosure *cl = (JsClosure *)c;
            js_gc_mark_cell(vm, &cl->fn->gc);
            js_gc_mark_value(vm, cl->this_val);
            if (cl->prototype_obj)
                js_gc_mark_cell(vm, &cl->prototype_obj->gc);
            for (uint16_t i = 0; i < cl->n_upvals; i++) {
                if (cl->upvals[i])
                    js_gc_mark_cell(vm, &cl->upvals[i]->gc);
            }
            break;
        }
        case JS_KIND_UPVALUE: {
            JsUpvalue *uv = (JsUpvalue *)c;
            js_gc_mark_value(vm, uv->closed);
            /* while open, the live value sits in the fiber's stack */
            if (uv->open)
                js_gc_mark_cell(vm, &uv->fiber->gc);
            break;
        }
        case JS_KIND_NATIVE: {
            JsNative *nf = (JsNative *)c;
            if (nf->name)
                js_gc_mark_cell(vm, &nf->name->gc);
            if (nf->statics)
                js_gc_mark_cell(vm, &nf->statics->gc);
            if (nf->prototype)
                js_gc_mark_cell(vm, &nf->prototype->gc);
            if (nf->is_bound)
                js_gc_mark_value(vm, nf->bound);
            break;
        }
        case JS_KIND_STRING:
        case JS_KIND_BYTECODE:
            break;
        }
    }
}

void js_gc_free_cell(JsVm *vm, JsGcCell *c, bool remove_atoms) {
    size_t size = 0;
    switch ((JsGcKind)c->kind) {
    case JS_KIND_STRING: {
        JsString *s = (JsString *)c;
        if (remove_atoms && s->interned)
            js_atoms_remove(vm, s);
        size = sizeof *s + (size_t)s->length * sizeof(uint16_t);
        break;
    }
    case JS_KIND_OBJECT: {
        JsObject *o = (JsObject *)c;
        js_map_free(vm, &o->props);
        js_realloc_raw(vm, o->elems, (size_t)o->elem_cap * sizeof(JsValue), 0);
        size = sizeof *o;
#ifdef LAMASSU_HAS_REGEX
        if (o->obj_kind == JS_OBJ_REGEXP)
            size = js_regexp_release(vm, o);
#endif
        if (o->obj_kind == JS_OBJ_DATE)
            size = sizeof(JsDateObject);
        if (o->obj_kind == JS_OBJ_MAP)
            size = js_mapobj_release(vm, o);
        if (o->obj_kind == JS_OBJ_SET)
            size = js_setobj_release(vm, o);
        break;
    }
    case JS_KIND_FUNCTION: {
        JsFunctionCell *f = (JsFunctionCell *)c;
        js_realloc_raw(vm, f->code, f->code_cap, 0);
        js_realloc_raw(vm, f->consts, (size_t)f->const_cap * sizeof(JsValue), 0);
        js_realloc_raw(vm, f->lines, (size_t)f->line_cap * sizeof(JsLineEntry), 0);
        js_realloc_raw(vm, f->upvals, (size_t)f->upval_cap * sizeof(JsUpvalDesc), 0);
        if (f->insn_boundary)
            js_realloc_raw(vm, f->insn_boundary, f->code_len, 0);
        size = sizeof *f;
        break;
    }
    case JS_KIND_FIBER: {
        JsFiber *fb = (JsFiber *)c;
        js_realloc_raw(vm, fb->stack, (size_t)fb->stack_cap * sizeof(JsValue), 0);
        js_realloc_raw(vm, fb->frames, (size_t)fb->frame_cap * sizeof(JsFrame), 0);
        js_realloc_raw(vm, fb->trys, (size_t)fb->try_cap * sizeof(JsTryEntry), 0);
        size = sizeof *fb;
        break;
    }
    case JS_KIND_CLOSURE: {
        JsClosure *cl = (JsClosure *)c;
        size = sizeof *cl + (size_t)cl->n_upvals * sizeof(JsUpvalue *);
        break;
    }
    case JS_KIND_UPVALUE:
        size = sizeof(JsUpvalue);
        break;
    case JS_KIND_NATIVE:
        size = sizeof(JsNative);
        break;
    case JS_KIND_PROMISE: {
        JsPromise *p = (JsPromise *)c;
        while (p->reactions) {
            JsReaction *rx = p->reactions;
            p->reactions = rx->next;
            js_realloc_raw(vm, rx, sizeof *rx, 0);
        }
        size = sizeof(JsPromise);
        break;
    }
    case JS_KIND_MODULE:
        js_module_free_cell(vm, (JsModule *)c);
        size = sizeof(JsModule);
        break;
    case JS_KIND_BYTECODE: {
        JsBytecode *bc = (JsBytecode *)c;
        size = sizeof *bc + bc->length;
        break;
    }
    }
    vm->cell_count--;
    js_realloc_raw(vm, c, size, 0);
}

void js_gc_collect(JsVm *vm) {
    if (vm->gc_running)
        return;
    vm->gc_running = true;
    vm->mark_overflow = false;

    for (size_t i = 0; i < vm->roots_len; i++)
        js_gc_mark_value(vm, *vm->roots[i]);
    for (JsContext *ctx = vm->contexts; ctx; ctx = ctx->next) {
        js_gc_mark_cell(vm, &ctx->globals->gc);
        if (ctx->string_methods)
            js_gc_mark_cell(vm, &ctx->string_methods->gc);
        if (ctx->number_methods)
            js_gc_mark_cell(vm, &ctx->number_methods->gc);
        if (ctx->promise_methods)
            js_gc_mark_cell(vm, &ctx->promise_methods->gc);
        if (ctx->object_proto)
            js_gc_mark_cell(vm, &ctx->object_proto->gc);
        if (ctx->array_proto)
            js_gc_mark_cell(vm, &ctx->array_proto->gc);
        if (ctx->regexp_proto)
            js_gc_mark_cell(vm, &ctx->regexp_proto->gc);
        if (ctx->date_proto)
            js_gc_mark_cell(vm, &ctx->date_proto->gc);
        if (ctx->map_proto)
            js_gc_mark_cell(vm, &ctx->map_proto->gc);
        if (ctx->set_proto)
            js_gc_mark_cell(vm, &ctx->set_proto->gc);
        if (ctx->repl_scope)
            js_gc_mark_cell(vm, &ctx->repl_scope->gc);
        if (ctx->repl_const)
            js_gc_mark_cell(vm, &ctx->repl_const->gc);
        js_gc_mark_module_registry(ctx);
        if (ctx->fiber)
            js_gc_mark_cell(vm, &ctx->fiber->gc);
    }
    js_gc_mark_jobs(vm);
    js_gc_trace(vm);

    if (vm->mark_overflow) {
        for (JsGcCell *c = vm->cells; c; c = c->next)
            c->mark = 0;
        vm->gc_threshold = vm->bytes_live * 2;
        vm->gc_running = false;
        return;
    }

    JsGcCell **link = &vm->cells;
    while (*link) {
        JsGcCell *c = *link;
        if (c->mark) {
            c->mark = 0;
            link = &c->next;
        } else {
            *link = c->next;
            js_gc_free_cell(vm, c, true);
        }
    }

    size_t want = vm->bytes_live * 2;
    vm->gc_threshold = want > vm->gc_threshold_init ? want : vm->gc_threshold_init;
    vm->gc_running = false;
}

void js_gc_maybe(JsVm *vm) {
    if (vm->gc_running)
        return;
    if (vm->gc_stress || vm->bytes_live >= vm->gc_threshold)
        js_gc_collect(vm);
}

JsGcCell *js_gc_new_cell(JsVm *vm, JsGcKind kind, size_t size) {
    js_gc_maybe(vm);
    /* The heap limit is now enforced uniformly in js_realloc_raw (which this
     * allocation goes through), so it covers cell headers and bulk buffers
     * alike — no bespoke pre-check needed here. */
    JsGcCell *c = js_realloc_raw(vm, NULL, 0, size);
    if (!c) {
        js_gc_collect(vm);
        c = js_realloc_raw(vm, NULL, 0, size);
        if (!c)
            return NULL;
    }
    c->kind = (uint8_t)kind;
    c->mark = 0;
    c->next = vm->cells;
    vm->cells = c;
    vm->cell_count++;
    return c;
}

bool js_gc_protect(JsVm *vm, JsValue *slot) {
    if (vm->roots_len == vm->roots_cap) {
        size_t ncap = vm->roots_cap ? vm->roots_cap * 2 : 16;
        JsValue **nr = js_realloc_raw(vm, vm->roots,
                                      vm->roots_cap * sizeof *nr, ncap * sizeof *nr);
        if (!nr)
            return false;
        vm->roots = nr;
        vm->roots_cap = ncap;
    }
    vm->roots[vm->roots_len++] = slot;
    return true;
}

void js_gc_unprotect(JsVm *vm, JsValue *slot) {
    for (size_t i = vm->roots_len; i-- > 0;) {
        if (vm->roots[i] == slot) {
            vm->roots[i] = vm->roots[--vm->roots_len];
            return;
        }
    }
}
