/*
 * Promises, the microtask queue, and the Promise standard library.
 *
 * A promise settles once; its subscriber list (reactions) is drained into
 * the microtask FIFO on settlement, so reactions always run on a later tick
 * (spec ordering). `await` registers a reaction that resumes a suspended
 * async fiber (js_resume_fiber in the interpreter).
 *
 * GC: promise cells and their pending reactions are traced by the collector
 * (js_gc.c). The job queue and the currently-running job are VM roots
 * (js_gc_mark_jobs). Values held across an allocation or a js_call are kept
 * reachable by one of those, or protected explicitly.
 */
#include "js_bytecode.h"
#include "lamassu_internal.h"

#define RX_THEN 0
#define RX_AWAIT 1

/* ---- allocation ---- */

JsPromise *js_promise_alloc(JsContext *ctx) {
    JsGcCell *c = js_gc_new_cell(ctx->vm, JS_KIND_PROMISE, sizeof(JsPromise));
    if (!c)
        return NULL;
    JsPromise *p = (JsPromise *)c;
    p->state = JS_PROMISE_PENDING;
    p->value = js_undefined();
    p->reactions = NULL;
    return p;
}

/* ---- microtask queue ---- */

static JsJob *job_alloc(JsVm *vm) {
    JsJob *job;
    if (vm->jobs_free) {
        job = vm->jobs_free;
        vm->jobs_free = job->next;
    } else {
        job = js_realloc_raw(vm, NULL, 0, sizeof(JsJob));
        if (!job)
            return NULL;
    }
    job->next = NULL;
    job->value = js_undefined();
    job->on_fulfilled = js_undefined();
    job->on_rejected = js_undefined();
    job->result = NULL;
    job->fiber = NULL;
    job->is_throw = false;
    return job;
}

static void job_enqueue(JsVm *vm, JsJob *job) {
    job->next = NULL;
    if (vm->jobs_tail)
        vm->jobs_tail->next = job;
    else
        vm->jobs_head = job;
    vm->jobs_tail = job;
}

static void job_recycle(JsVm *vm, JsJob *job) {
    job->value = js_undefined();
    job->on_fulfilled = js_undefined();
    job->on_rejected = js_undefined();
    job->result = NULL;
    job->fiber = NULL;
    job->next = vm->jobs_free;
    vm->jobs_free = job;
}

void js_gc_mark_jobs(JsVm *vm) {
    for (JsJob *j = vm->jobs_head; j; j = j->next) {
        js_gc_mark_value(vm, j->value);
        js_gc_mark_value(vm, j->on_fulfilled);
        js_gc_mark_value(vm, j->on_rejected);
        if (j->result)
            js_gc_mark_cell(vm, &j->result->gc);
        if (j->fiber)
            js_gc_mark_cell(vm, &j->fiber->gc);
    }
    /* the running-job stack: every job with a handler still on the C stack */
    for (JsJob *j = vm->running_job; j; j = j->next) {
        js_gc_mark_value(vm, j->value);
        js_gc_mark_value(vm, j->on_fulfilled);
        js_gc_mark_value(vm, j->on_rejected);
        if (j->result)
            js_gc_mark_cell(vm, &j->result->gc);
        if (j->fiber)
            js_gc_mark_cell(vm, &j->fiber->gc);
    }
}

void js_jobs_free_all(JsVm *vm) {
    for (JsJob *j = vm->jobs_head; j;) {
        JsJob *n = j->next;
        js_realloc_raw(vm, j, sizeof *j, 0);
        j = n;
    }
    for (JsJob *j = vm->jobs_free; j;) {
        JsJob *n = j->next;
        js_realloc_raw(vm, j, sizeof *j, 0);
        j = n;
    }
    vm->jobs_head = vm->jobs_tail = vm->jobs_free = NULL;
}

/* ---- settlement ---- */

/* Schedules a job from a reaction + settlement. Consumes nothing. */
static void schedule_reaction(JsContext *ctx, JsReaction *rx, bool fulfilled,
                              JsValue value) {
    JsJob *job = job_alloc(ctx->vm);
    if (!job)
        return; /* OOM: drop the reaction (degradation, not a crash) */
    job->kind = rx->kind;
    job->fulfilled = fulfilled;
    job->value = value;
    if (rx->kind == RX_THEN) {
        job->on_fulfilled = rx->on_fulfilled;
        job->on_rejected = rx->on_rejected;
        job->result = rx->result;
    } else {
        job->fiber = rx->fiber;
        job->is_throw = !fulfilled;
    }
    job_enqueue(ctx->vm, job);
}

static void settle(JsContext *ctx, JsPromise *p, uint8_t state, JsValue value) {
    if (p->state != JS_PROMISE_PENDING)
        return;
    p->value = value; /* rooted via p from here on */
    p->state = state;
    bool fulfilled = state == JS_PROMISE_FULFILLED;
    /* Reactions are prepended in add_reaction (O(1)), so the list is in reverse
     * attachment order. Reverse it here — pure pointer surgery, no allocation
     * and thus no GC safe point — so handlers fire FIFO, as the spec requires.
     * Every reaction stays on one of the two lists throughout, so all remain
     * reachable from p. */
    JsReaction *fifo = NULL;
    while (p->reactions) {
        JsReaction *rx = p->reactions;
        p->reactions = rx->next;
        rx->next = fifo;
        fifo = rx;
    }
    p->reactions = fifo;
    /* Keep each reaction linked to p (so it stays traced) until its job is
     * enqueued, then unlink and free it. */
    while (p->reactions) {
        JsReaction *rx = p->reactions;
        schedule_reaction(ctx, rx, fulfilled, value); /* GC-safe: rx still on p */
        p->reactions = rx->next;
        js_realloc_raw(ctx->vm, rx, sizeof *rx, 0);
    }
}

void js_promise_fulfill(JsContext *ctx, JsPromise *p, JsValue value) {
    settle(ctx, p, JS_PROMISE_FULFILLED, value);
}

void js_promise_reject(JsContext *ctx, JsPromise *p, JsValue reason) {
    settle(ctx, p, JS_PROMISE_REJECTED, reason);
}

/* Registers a reaction; if already settled, schedules it immediately. */
static void add_reaction(JsContext *ctx, JsPromise *p, JsReaction *rx) {
    if (p->state == JS_PROMISE_PENDING) {
        rx->next = p->reactions;
        p->reactions = rx;
        return;
    }
    schedule_reaction(ctx, rx, p->state == JS_PROMISE_FULFILLED, p->value);
    js_realloc_raw(ctx->vm, rx, sizeof *rx, 0);
}

static JsReaction *reaction_alloc(JsContext *ctx) {
    JsReaction *rx = js_realloc_raw(ctx->vm, NULL, 0, sizeof(JsReaction));
    if (!rx)
        return NULL;
    rx->next = NULL;
    rx->kind = RX_THEN;
    rx->on_fulfilled = js_undefined();
    rx->on_rejected = js_undefined();
    rx->result = NULL;
    rx->fiber = NULL;
    return rx;
}

/*
 * Resolution procedure: if value is a promise, p adopts its eventual state;
 * otherwise p fulfills with value.
 */
void js_promise_resolve_with(JsContext *ctx, JsPromise *p, JsValue value) {
    if (!js_is_promise(value)) {
        js_promise_fulfill(ctx, p, value);
        return;
    }
    JsPromise *q = js_value_promise(value);
    if (q == p) { /* resolving with itself: reject per spec */
        JsString *s = js_ascii_cell(ctx->vm, "TypeError: chaining cycle detected");
        js_promise_reject(ctx, p, s ? js_value_from_cell(&s->gc) : js_undefined());
        return;
    }
    /* pass-through then-reaction on q whose derived promise is p */
    JsValue pv = js_value_from_cell(&p->gc);
    JsValue qv = value;
    js_gc_protect(ctx->vm, &pv);
    js_gc_protect(ctx->vm, &qv);
    JsReaction *rx = reaction_alloc(ctx);
    if (rx) {
        rx->kind = RX_THEN;
        rx->result = js_value_promise(pv);
        add_reaction(ctx, js_value_promise(qv), rx);
    }
    js_gc_unprotect(ctx->vm, &qv);
    js_gc_unprotect(ctx->vm, &pv);
}

JsPromise *js_promise_then(JsContext *ctx, JsPromise *p, JsValue on_f, JsValue on_r) {
    /* Root p and the handlers BEFORE the first allocation (js_promise_alloc
     * is a GC safe point and the handlers may be otherwise unreferenced). */
    JsValue pv = js_value_from_cell(&p->gc);
    js_gc_protect(ctx->vm, &pv);
    js_gc_protect(ctx->vm, &on_f);
    js_gc_protect(ctx->vm, &on_r);
    JsPromise *r = js_promise_alloc(ctx);
    JsPromise *result = NULL;
    if (r) {
        JsValue rv = js_value_from_cell(&r->gc);
        js_gc_protect(ctx->vm, &rv);
        JsReaction *rx = reaction_alloc(ctx);
        if (rx) {
            rx->kind = RX_THEN;
            rx->on_fulfilled = on_f;
            rx->on_rejected = on_r;
            rx->result = js_value_promise(rv);
            add_reaction(ctx, js_value_promise(pv), rx);
            result = js_value_promise(rv);
        }
        js_gc_unprotect(ctx->vm, &rv);
    }
    js_gc_unprotect(ctx->vm, &on_r);
    js_gc_unprotect(ctx->vm, &on_f);
    js_gc_unprotect(ctx->vm, &pv);
    return result;
}

bool js_promise_await(JsContext *ctx, JsPromise *p, JsFiber *fiber) {
    JsReaction *rx = reaction_alloc(ctx);
    if (!rx)
        return false;
    rx->kind = RX_AWAIT;
    rx->fiber = fiber; /* fiber is ctx->fiber here, so it stays rooted */
    add_reaction(ctx, p, rx);
    return true;
}

/* ---- running microtasks ---- */

static void run_then_job(JsContext *ctx, JsJob *job) {
    JsValue handler = job->fulfilled ? job->on_fulfilled : job->on_rejected;
    JsPromise *r = job->result;
    if (!js_is_function(handler)) {
        if (!r)
            return; /* pass-through with no derived promise */
        if (job->fulfilled)
            js_promise_resolve_with(ctx, r, job->value);
        else
            js_promise_reject(ctx, r, job->value);
        return;
    }
    JsValue res;
    bool ok = js_call(ctx, handler, js_undefined(), &job->value, 1, &res);
    if (!r)
        return;
    if (ok)
        js_promise_resolve_with(ctx, r, res);
    else
        js_promise_reject(ctx, r, res);
}

void js_run_jobs(JsContext *ctx) {
    JsVm *vm = ctx->vm;
    while (vm->jobs_head) {
        JsJob *job = vm->jobs_head;
        vm->jobs_head = job->next;
        if (!vm->jobs_head)
            vm->jobs_tail = NULL;
        /* Reentrancy: a job handler may drain jobs itself (e.g. a module
         * continuation running a body via js_run_module). running_job is a
         * stack, linked through the popped job's now-free `next`, so every
         * in-flight job stays rooted across nested drains. */
        job->next = vm->running_job;
        vm->running_job = job;
        if (job->kind == RX_AWAIT)
            js_resume_fiber(ctx, job->fiber, job->value, job->is_throw);
        else
            run_then_job(ctx, job);
        vm->running_job = job->next;
        job_recycle(vm, job);
    }
}

bool js_has_pending_jobs(const JsContext *ctx) {
    return ctx->vm->jobs_head != NULL;
}

/* ---- host API ---- */

JsValue js_promise_new(JsContext *ctx) {
    JsPromise *p = js_promise_alloc(ctx);
    return p ? js_value_from_cell(&p->gc) : js_undefined();
}

bool js_resolve(JsContext *ctx, JsValue promise, JsValue value) {
    if (!js_is_promise(promise))
        return false;
    js_promise_resolve_with(ctx, js_value_promise(promise), value);
    return true;
}

bool js_reject(JsContext *ctx, JsValue promise, JsValue reason) {
    if (!js_is_promise(promise))
        return false;
    js_promise_reject(ctx, js_value_promise(promise), reason);
    return true;
}

int js_promise_state(JsValue v) {
    if (!js_is_promise(v))
        return -1;
    return (int)js_value_promise(v)->state;
}

JsValue js_promise_result(JsValue v) {
    if (!js_is_promise(v))
        return js_undefined();
    JsPromise *p = js_value_promise(v);
    return p->state == JS_PROMISE_PENDING ? js_undefined() : p->value;
}

/* ===================================================================
 * JS-level Promise library
 * =================================================================== */

#define ARG(i) ((i) < argc ? args[i] : js_undefined())

static bool nthrow(JsContext *ctx, JsValue *r, const char *msg) {
    JsString *s = js_ascii_cell(ctx->vm, msg);
    *r = s ? js_value_from_cell(&s->gc) : js_undefined();
    return false;
}

/* Coerces a value to a promise: passes promises through, wraps others in a
 * fulfilled promise. NULL on OOM. */
static JsPromise *coerce_promise(JsContext *ctx, JsValue v) {
    if (js_is_promise(v))
        return js_value_promise(v);
    JsPromise *p = js_promise_alloc(ctx);
    if (!p)
        return NULL;
    js_promise_fulfill(ctx, p, v);
    return p;
}

/* ---- prototype methods (this = promise) ---- */

static bool pm_then(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (!js_is_promise(tv))
        return nthrow(ctx, r, "TypeError: Promise.prototype.then called on non-promise");
    JsPromise *res = js_promise_then(ctx, js_value_promise(tv), ARG(0), ARG(1));
    if (!res)
        return nthrow(ctx, r, "out of memory");
    *r = js_value_from_cell(&res->gc);
    return true;
}

static bool pm_catch(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (!js_is_promise(tv))
        return nthrow(ctx, r, "TypeError: Promise.prototype.catch called on non-promise");
    JsPromise *res = js_promise_then(ctx, js_value_promise(tv), js_undefined(), ARG(0));
    if (!res)
        return nthrow(ctx, r, "out of memory");
    *r = js_value_from_cell(&res->gc);
    return true;
}

/* finally: run onFinally, then pass the settlement through unchanged — unless
 * onFinally returns a thenable, in which case the original settlement is
 * deferred until that thenable settles (and its rejection overrides). */

/* After onFinally's returned thenable fulfills, re-emit the captured original
 * fulfillment value. */
static bool finally_value_passthrough(JsContext *ctx, JsValue bound, JsValue tv,
                                      const JsValue *args, int argc, JsValue *r) {
    (void)ctx; (void)tv; (void)args; (void)argc;
    *r = bound; /* the original fulfillment value */
    return true;
}

/* After onFinally's returned thenable fulfills, re-throw the captured original
 * rejection reason. */
static bool finally_reason_rethrow(JsContext *ctx, JsValue bound, JsValue tv,
                                   const JsValue *args, int argc, JsValue *r) {
    (void)ctx; (void)tv; (void)args; (void)argc;
    *r = bound; /* the original rejection reason */
    return false;
}

/* Runs onFinally (bound). Returns true if the caller should fall through to
 * passing the original settlement along; false if *chain has been set to a
 * derived promise the caller must adopt, or if onFinally threw (in which case
 * *threw is set and *r holds the thrown value). `passthrough` re-emits the
 * original settlement once onFinally's returned thenable settles. */
static bool finally_run(JsContext *ctx, JsValue bound, JsValue orig,
                        JsBoundFn passthrough, JsValue *chain,
                        bool *threw, JsValue *r) {
    JsVm *vm = ctx->vm;
    *threw = false;
    *chain = js_undefined();
    if (!js_is_function(bound))
        return true; /* no onFinally: pass the original settlement straight through */
    JsValue result;
    if (!js_call(ctx, bound, js_undefined(), NULL, 0, &result)) {
        *threw = true;
        *r = result; /* onFinally threw — that rejection wins */
        return false;
    }
    if (!js_is_promise(result))
        return true; /* non-thenable return is ignored; pass original through */
    /* onFinally returned a thenable: wait for it, then re-emit `orig`. */
    js_gc_protect(vm, &result);
    js_gc_protect(vm, &orig);
    JsValue passer = js_bound_native_new(ctx, passthrough, orig);
    JsPromise *chained = js_is_function(passer)
        ? js_promise_then(ctx, js_value_promise(result), passer, js_undefined())
        : NULL;
    js_gc_unprotect(vm, &orig);
    js_gc_unprotect(vm, &result);
    if (!chained) {
        JsString *s = js_ascii_cell(vm, "out of memory");
        *threw = true;
        *r = s ? js_value_from_cell(&s->gc) : js_undefined();
        return false;
    }
    *chain = js_value_from_cell(&chained->gc);
    return false;
}

static bool finally_on_fulfill(JsContext *ctx, JsValue bound, JsValue tv,
                               const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue chain;
    bool threw;
    if (finally_run(ctx, bound, ARG(0), finally_value_passthrough, &chain, &threw, r)) {
        *r = ARG(0); /* pass the fulfillment value through */
        return true;
    }
    if (threw)
        return false; /* onFinally threw / OOM: reject with *r */
    *r = chain; /* adopt the derived promise (resolve_with waits on it) */
    return true;
}

static bool finally_on_reject(JsContext *ctx, JsValue bound, JsValue tv,
                              const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue chain;
    bool threw;
    if (finally_run(ctx, bound, ARG(0), finally_reason_rethrow, &chain, &threw, r)) {
        *r = ARG(0); /* re-throw the rejection reason */
        return false;
    }
    if (threw)
        return false; /* onFinally threw / OOM: reject with *r */
    /* Adopt the derived promise: it re-throws the original reason on fulfil, or
     * forwards onFinally's own rejection — either way the result rejects. */
    *r = chain;
    return true;
}

static bool pm_finally(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (!js_is_promise(tv))
        return nthrow(ctx, r, "TypeError: Promise.prototype.finally called on non-promise");
    JsValue onf = ARG(0);
    JsValue f = js_bound_native_new(ctx, finally_on_fulfill, onf);
    if (!js_is_function(f))
        return nthrow(ctx, r, "out of memory");
    js_gc_protect(ctx->vm, &f);
    JsValue rj = js_bound_native_new(ctx, finally_on_reject, onf);
    js_gc_unprotect(ctx->vm, &f);
    if (!js_is_function(rj))
        return nthrow(ctx, r, "out of memory");
    JsPromise *res = js_promise_then(ctx, js_value_promise(tv), f, rj);
    if (!res)
        return nthrow(ctx, r, "out of memory");
    *r = js_value_from_cell(&res->gc);
    return true;
}

/* ---- statics ---- */

static bool ps_resolve(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    if (js_is_promise(ARG(0))) {
        *r = ARG(0);
        return true;
    }
    JsPromise *p = js_promise_alloc(ctx);
    if (!p)
        return nthrow(ctx, r, "out of memory");
    js_promise_resolve_with(ctx, p, ARG(0));
    *r = js_value_from_cell(&p->gc);
    return true;
}

static bool ps_reject(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsPromise *p = js_promise_alloc(ctx);
    if (!p)
        return nthrow(ctx, r, "out of memory");
    js_promise_reject(ctx, p, ARG(0));
    *r = js_value_from_cell(&p->gc);
    return true;
}

/*
 * Promise.all / race / allSettled share a per-call state object bound into
 * each element's reaction: { results, remaining, promise, mode }.
 */
#define ALL_MODE_ALL 0
#define ALL_MODE_SETTLED 1

/* read/write state fields by ascii key */
static JsValue state_get(JsContext *ctx, JsObject *st, const char *k) {
    JsString *ks = js_ascii_cell(ctx->vm, k);
    if (!ks)
        return js_undefined();
    JsString *ik = js_intern_cell(ctx->vm, ks);
    if (!ik)
        return js_undefined();
    bool found;
    return js_map_get(&st->props, ik, &found);
}

static void state_set(JsContext *ctx, JsObject *st, const char *k, JsValue v) {
    js_object_set_ascii(ctx, st, k, v);
}

static bool all_element_resolve(JsContext *ctx, JsValue bound, JsValue tv,
                                const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    /* bound = [state, index] */
    JsObject *pair = js_value_object(bound);
    JsObject *state = js_value_object(pair->elems[0]);
    uint32_t idx = (uint32_t)js_get_number(pair->elems[1]);
    JsObject *results = js_value_object(state_get(ctx, state, "results"));
    int mode = (int)js_to_number_value(ctx, state_get(ctx, state, "mode"));
    if (mode == ALL_MODE_SETTLED) {
        JsValue ov = js_object_new(ctx);
        if (!js_is_object(ov))
            return nthrow(ctx, r, "out of memory");
        js_gc_protect(ctx->vm, &ov); /* root across the ascii/set allocations */
        JsString *status = js_ascii_cell(ctx->vm, "fulfilled");
        bool sok = status &&
                   js_object_set_ascii(ctx, js_value_object(ov), "status",
                                       js_value_from_cell(&status->gc)) &&
                   js_object_set_ascii(ctx, js_value_object(ov), "value", ARG(0));
        results->elems[idx] = ov;
        js_gc_unprotect(ctx->vm, &ov);
        if (!sok)
            return nthrow(ctx, r, "out of memory");
    } else {
        results->elems[idx] = ARG(0);
    }
    double rem = js_to_number_value(ctx, state_get(ctx, state, "remaining")) - 1;
    state_set(ctx, state, "remaining", js_number(rem));
    if (rem == 0) {
        JsPromise *rp = js_value_promise(state_get(ctx, state, "promise"));
        js_promise_fulfill(ctx, rp, js_value_from_cell(&results->gc));
    }
    *r = js_undefined();
    return true;
}

static bool all_element_reject(JsContext *ctx, JsValue bound, JsValue tv,
                               const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsObject *pair = js_value_object(bound);
    JsObject *state = js_value_object(pair->elems[0]);
    uint32_t idx = (uint32_t)js_get_number(pair->elems[1]);
    int mode = (int)js_to_number_value(ctx, state_get(ctx, state, "mode"));
    if (mode == ALL_MODE_SETTLED) {
        JsObject *results = js_value_object(state_get(ctx, state, "results"));
        JsValue ov = js_object_new(ctx);
        if (!js_is_object(ov))
            return nthrow(ctx, r, "out of memory");
        js_gc_protect(ctx->vm, &ov);
        JsString *status = js_ascii_cell(ctx->vm, "rejected");
        bool sok = status &&
                   js_object_set_ascii(ctx, js_value_object(ov), "status",
                                       js_value_from_cell(&status->gc)) &&
                   js_object_set_ascii(ctx, js_value_object(ov), "reason", ARG(0));
        results->elems[idx] = ov;
        js_gc_unprotect(ctx->vm, &ov);
        if (!sok)
            return nthrow(ctx, r, "out of memory");
        double rem = js_to_number_value(ctx, state_get(ctx, state, "remaining")) - 1;
        state_set(ctx, state, "remaining", js_number(rem));
        if (rem == 0) {
            JsPromise *rp = js_value_promise(state_get(ctx, state, "promise"));
            js_promise_fulfill(ctx, rp, js_value_from_cell(&results->gc));
        }
    } else {
        JsPromise *rp = js_value_promise(state_get(ctx, state, "promise"));
        js_promise_reject(ctx, rp, ARG(0));
    }
    *r = js_undefined();
    return true;
}

static bool all_impl(JsContext *ctx, const JsValue *args, int argc, JsValue *r, int mode) {
    JsValue iterable = ARG(0);
    if (!js_is_object(iterable) || js_value_object(iterable)->obj_kind != JS_OBJ_ARRAY)
        return nthrow(ctx, r, "TypeError: Promise.all expects an array");
    JsObject *input = js_value_object(iterable);
    uint32_t n = input->elem_count;

    JsValue rpv = js_promise_new(ctx);
    if (!js_is_promise(rpv))
        return nthrow(ctx, r, "out of memory");
    js_gc_protect(ctx->vm, &rpv);
    JsValue inputv = iterable;
    js_gc_protect(ctx->vm, &inputv);

    JsObject *results = js_array_new_cell(ctx, n);
    JsValue resultsv = results ? js_value_from_cell(&results->gc) : js_undefined();
    js_gc_protect(ctx->vm, &resultsv);
    JsValue statev = js_object_new(ctx);
    js_gc_protect(ctx->vm, &statev);
    bool ok = results && js_is_object(statev);
    if (ok) {
        for (uint32_t i = 0; i < n; i++)
            results->elems[i] = js_undefined();
        results->elem_count = n;
        JsObject *state = js_value_object(statev);
        state_set(ctx, state, "results", resultsv);
        state_set(ctx, state, "remaining", js_number(n == 0 ? 0 : n));
        state_set(ctx, state, "promise", rpv);
        state_set(ctx, state, "mode", js_number(mode));

        if (n == 0) {
            js_promise_fulfill(ctx, js_value_promise(rpv), resultsv);
        }
        for (uint32_t i = 0; i < n && ok; i++) {
            JsPromise *ep = coerce_promise(ctx, input->elems[i]);
            if (!ep) {
                ok = false;
                break;
            }
            JsValue epv = js_value_from_cell(&ep->gc);
            js_gc_protect(ctx->vm, &epv);
            /* bound pair [state, i] */
            JsObject *pair = js_array_new_cell(ctx, 2);
            if (pair) {
                pair->elems[0] = statev;
                pair->elems[1] = js_number(i);
                pair->elem_count = 2;
                JsValue pairv = js_value_from_cell(&pair->gc);
                js_gc_protect(ctx->vm, &pairv);
                JsValue onf = js_bound_native_new(ctx, all_element_resolve, pairv);
                js_gc_protect(ctx->vm, &onf);
                JsValue onr = js_bound_native_new(ctx, all_element_reject, pairv);
                js_gc_protect(ctx->vm, &onr);
                if (js_is_function(onf) && js_is_function(onr))
                    js_promise_then(ctx, js_value_promise(epv), onf, onr);
                else
                    ok = false;
                js_gc_unprotect(ctx->vm, &onr);
                js_gc_unprotect(ctx->vm, &onf);
                js_gc_unprotect(ctx->vm, &pairv);
            } else {
                ok = false;
            }
            js_gc_unprotect(ctx->vm, &epv);
        }
    }
    js_gc_unprotect(ctx->vm, &statev);
    js_gc_unprotect(ctx->vm, &resultsv);
    js_gc_unprotect(ctx->vm, &inputv);
    JsValue out = rpv;
    js_gc_unprotect(ctx->vm, &rpv);
    if (!ok)
        return nthrow(ctx, r, "out of memory");
    *r = out;
    return true;
}

static bool ps_all(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return all_impl(ctx, args, argc, r, ALL_MODE_ALL);
}

static bool ps_allSettled(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return all_impl(ctx, args, argc, r, ALL_MODE_SETTLED);
}

static bool race_resolve(JsContext *ctx, JsValue bound, JsValue tv,
                         const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    js_promise_resolve_with(ctx, js_value_promise(bound), ARG(0));
    *r = js_undefined();
    return true;
}

static bool race_reject(JsContext *ctx, JsValue bound, JsValue tv,
                        const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    js_promise_reject(ctx, js_value_promise(bound), ARG(0));
    *r = js_undefined();
    return true;
}

static bool ps_race(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsValue iterable = ARG(0);
    if (!js_is_object(iterable) || js_value_object(iterable)->obj_kind != JS_OBJ_ARRAY)
        return nthrow(ctx, r, "TypeError: Promise.race expects an array");
    JsObject *input = js_value_object(iterable);
    JsValue rpv = js_promise_new(ctx);
    if (!js_is_promise(rpv))
        return nthrow(ctx, r, "out of memory");
    js_gc_protect(ctx->vm, &rpv);
    JsValue inputv = iterable;
    js_gc_protect(ctx->vm, &inputv);
    bool ok = true;
    for (uint32_t i = 0; i < input->elem_count && ok; i++) {
        JsPromise *ep = coerce_promise(ctx, input->elems[i]);
        if (!ep) {
            ok = false;
            break;
        }
        JsValue epv = js_value_from_cell(&ep->gc);
        js_gc_protect(ctx->vm, &epv);
        JsValue onf = js_bound_native_new(ctx, race_resolve, rpv);
        js_gc_protect(ctx->vm, &onf);
        JsValue onr = js_bound_native_new(ctx, race_reject, rpv);
        js_gc_protect(ctx->vm, &onr);
        if (js_is_function(onf) && js_is_function(onr))
            js_promise_then(ctx, js_value_promise(epv), onf, onr);
        else
            ok = false;
        js_gc_unprotect(ctx->vm, &onr);
        js_gc_unprotect(ctx->vm, &onf);
        js_gc_unprotect(ctx->vm, &epv);
    }
    js_gc_unprotect(ctx->vm, &inputv);
    JsValue out = rpv;
    js_gc_unprotect(ctx->vm, &rpv);
    if (!ok)
        return nthrow(ctx, r, "out of memory");
    *r = out;
    return true;
}

/* Callable Promise(executor) */
static bool promise_ctor(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    if (!js_is_function(ARG(0)))
        return nthrow(ctx, r, "TypeError: Promise executor is not a function");
    JsValue pv = js_promise_new(ctx);
    if (!js_is_promise(pv))
        return nthrow(ctx, r, "out of memory");
    js_gc_protect(ctx->vm, &pv);
    JsValue executor = ARG(0);
    js_gc_protect(ctx->vm, &executor);
    JsValue resolve = js_bound_native_new(ctx, race_resolve, pv);
    js_gc_protect(ctx->vm, &resolve);
    JsValue reject = js_bound_native_new(ctx, race_reject, pv);
    js_gc_protect(ctx->vm, &reject);
    bool ok = js_is_function(resolve) && js_is_function(reject);
    if (ok) {
        JsValue ex_args[2] = {resolve, reject};
        JsValue ex_res;
        if (!js_call(ctx, executor, js_undefined(), ex_args, 2, &ex_res))
            js_promise_reject(ctx, js_value_promise(pv), ex_res); /* executor threw */
    }
    js_gc_unprotect(ctx->vm, &reject);
    js_gc_unprotect(ctx->vm, &resolve);
    js_gc_unprotect(ctx->vm, &executor);
    JsValue out = pv;
    js_gc_unprotect(ctx->vm, &pv);
    if (!ok)
        return nthrow(ctx, r, "out of memory");
    *r = out;
    return true;
}

/* ---- registration ---- */

static bool def_fn(JsContext *ctx, JsObject *o, const char *k, JsNativeFn fn) {
    JsValue nf = js_native_new(ctx, k, fn, NULL);
    return js_is_function(nf) && js_object_set_ascii(ctx, o, k, nf);
}

bool js_promise_builtins_init(JsContext *ctx) {
    JsVm *vm = ctx->vm;
    JsValue pm = js_object_new(ctx);
    if (!js_is_object(pm))
        return false;
    ctx->promise_methods = js_value_object(pm);
    if (!def_fn(ctx, ctx->promise_methods, "then", pm_then) ||
        !def_fn(ctx, ctx->promise_methods, "catch", pm_catch) ||
        !def_fn(ctx, ctx->promise_methods, "finally", pm_finally))
        return false;

    /* global callable Promise with statics */
    JsValue ctor = js_native_new(ctx, "Promise", promise_ctor, NULL);
    if (!js_is_function(ctor))
        return false;
    js_gc_protect(vm, &ctor);
    JsValue staticsv = js_object_new(ctx);
    if (!js_is_object(staticsv)) {
        js_gc_unprotect(vm, &ctor);
        return false;
    }
    JsObject *statics = js_value_object(staticsv);
    JsNative *native = (JsNative *)js_value_cell(ctor);
    native->statics = statics;
    bool ok = js_object_set_ascii(ctx, ctx->globals, "Promise", ctor);
    js_gc_unprotect(vm, &ctor);
    if (!ok)
        return false;
    JsObject *st = native->statics;
    if (!def_fn(ctx, st, "resolve", ps_resolve) || !def_fn(ctx, st, "reject", ps_reject) ||
        !def_fn(ctx, st, "all", ps_all) || !def_fn(ctx, st, "race", ps_race) ||
        !def_fn(ctx, st, "allSettled", ps_allSettled))
        return false;
    return true;
}
