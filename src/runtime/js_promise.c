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
#include "js_weak.h"
#include "js_bytecode.h"
#include "js_error.h"
#include "js_iter.h"
#include "lamassu_internal.h"

#define RX_THEN 0
#define RX_AWAIT 1
#define RX_THENABLE 2 /* NewPromiseResolveThenableJob: value.then(resolve, reject) for result */

/* ---- allocation ---- */

JsPromise *js_promise_alloc(JsContext *ctx) {
    JsGcCell *c = js_gc_new_cell(ctx->vm, JS_KIND_PROMISE, sizeof(JsPromise));
    if (!c)
        return NULL;
    JsPromise *p = (JsPromise *)c;
    p->state = JS_PROMISE_PENDING;
    p->value = js_undefined();
    p->reactions = NULL;
    p->props = NULL;
    p->own_proto = js_undefined();
    p->proto_set = false;
    p->handled = false;
    p->trace = NULL;
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
    job->trace = NULL;
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

/*
 * Recycled job nodes kept for reuse. The free list used to be unbounded, which
 * was invisible until heap_limit started being enforced against it: a guest
 * that queues a microtask per turn (`function f(){ Promise.resolve().then(f) }`)
 * runs its budget out, every job is abandoned, and the nodes it churned through
 * stay parked for the VM's lifetime — the guest's heap ends up full of memory
 * the collector cannot see and nothing will ever reuse. Trimming after a drain
 * keeps the fast path (a warm pool for the common case of a few pending
 * reactions) without letting one burst pin the heap.
 */
#define JS_JOBS_FREE_MAX 64

static void jobs_free_trim(JsVm *vm) {
    size_t kept = 0;
    JsJob **link = &vm->jobs_free;
    while (*link) {
        if (kept < JS_JOBS_FREE_MAX) {
            kept++;
            link = &(*link)->next;
            continue;
        }
        JsJob *dead = *link;
        *link = dead->next;
        js_realloc_raw(vm, dead, sizeof *dead, 0);
    }
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
        if (j->trace)
            js_gc_mark_cell(vm, &j->trace->gc);
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
        if (j->trace)
            js_gc_mark_cell(vm, &j->trace->gc);
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

/* Schedules a job from a reaction + settlement of P. Consumes nothing. */
static void schedule_reaction(JsContext *ctx, JsReaction *rx, const JsPromise *p) {
    JsJob *job = job_alloc(ctx->vm);
    if (!job)
        return; /* unreachable: reaction_alloc reserved this node (job_reserve) */
    bool fulfilled = p->state == JS_PROMISE_FULFILLED;
    job->kind = rx->kind;
    job->fulfilled = fulfilled;
    job->value = p->value;
    job->trace = fulfilled ? NULL : p->trace;
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
    if (state == JS_PROMISE_REJECTED) {
        /* rejected by the error on its way out of a fiber: the trace of that raise is this
         * rejection's to keep (js_promise_error_stack). P and VALUE may have no root but each
         * other, and making the trace does not collect. */
        if (!p->trace && ctx->raise_fresh && js_same_value(value, ctx->error_trace_value))
            p->trace = js_raise_trace(ctx);
        /* HostPromiseRejectionTracker(p, "reject"): after the trace, so the host can ask where
         * the rejection came from there and then */
        if (!p->handled && ctx->rejection_fn)
            ctx->rejection_fn(ctx, js_value_from_cell(&p->gc), false, ctx->rejection_ud);
    }
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
        schedule_reaction(ctx, rx, p); /* GC-safe: rx still on p */
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
    if (!p->handled) {
        p->handled = true;
        /* HostPromiseRejectionTracker(p, "handle") */
        if (p->state == JS_PROMISE_REJECTED && ctx->rejection_fn)
            ctx->rejection_fn(ctx, js_value_from_cell(&p->gc), true, ctx->rejection_ud);
    }
    if (p->state == JS_PROMISE_PENDING) {
        rx->next = p->reactions;
        p->reactions = rx;
        return;
    }
    schedule_reaction(ctx, rx, p);
    js_realloc_raw(ctx->vm, rx, sizeof *rx, 0);
}

/*
 * Puts one spare job node on the free list. Every reaction reserves the node it
 * will need when the promise settles, because settlement is a point with no way
 * to report failure: a dropped job leaves an `await` pending forever and a
 * `.then` that never runs, which a host cannot tell apart from work that is
 * legitimately still outstanding. Registration CAN fail visibly, so the
 * allocation belongs there. job_alloc has this one consumer, and job_recycle
 * returns nodes here, so the reserve is never raided by anything else.
 */
static bool job_reserve(JsVm *vm) {
    JsJob *job = js_realloc_raw(vm, NULL, 0, sizeof(JsJob));
    if (!job)
        return false;
    job->next = vm->jobs_free;
    vm->jobs_free = job;
    return true;
}

static JsReaction *reaction_alloc(JsContext *ctx) {
    if (!job_reserve(ctx->vm))
        return NULL;
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

/* Promise Resolve Functions, steps 8-16, for an object VALUE that is not a promise: its `then`
 * read now (a throw rejects P); callable, P follows it through a job that calls
 * value.then(resolve, reject) (NewPromiseResolveThenableJob), else P fulfills with VALUE. */
static void resolve_thenable(JsContext *ctx, JsPromise *p, JsValue value) {
    JsValue pv = js_value_from_cell(&p->gc), then = js_undefined();
    js_gc_protect(ctx->vm, &pv);
    js_gc_protect(ctx->vm, &value);
    static const uint16_t kthen[] = {'t', 'h', 'e', 'n'};
    JsValue key = js_atom(ctx->vm, kthen, 4);
    if (!js_is_string(key) || !js_get_property(ctx, value, key, &then)) {
        JsValue e = js_is_undefined(ctx->pending_error) ? js_oom_value(ctx->vm) : js_take_error(ctx);
        js_promise_reject(ctx, p, e);
    } else if (!js_is_function(then)) {
        js_promise_fulfill(ctx, p, value);
    } else {
        js_gc_protect(ctx->vm, &then);
        JsJob *job = job_alloc(ctx->vm);
        if (!job) {
            js_promise_reject(ctx, p, js_oom_value(ctx->vm));
        } else {
            job->kind = RX_THENABLE;
            job->value = value;
            job->on_fulfilled = then;
            job->result = p;
            job_enqueue(ctx->vm, job);
        }
        js_gc_unprotect(ctx->vm, &then);
    }
    js_gc_unprotect(ctx->vm, &value);
    js_gc_unprotect(ctx->vm, &pv);
}

/*
 * Resolution procedure: if value is a promise, p adopts its eventual state;
 * otherwise p fulfills with value.
 */
void js_promise_resolve_with(JsContext *ctx, JsPromise *p, JsValue value) {
    if (!js_is_promise(value)) {
        /* an object a script can see (a host's bytecode buffer carries the object tag too) */
        if ((js_is_object(value) && js_value_cell(value)->kind == JS_KIND_OBJECT) || js_is_function(value)) {
            resolve_thenable(ctx, p, value);
            return;
        }
        js_promise_fulfill(ctx, p, value);
        return;
    }
    JsPromise *q = js_value_promise(value);
    if (q == p) { /* resolving with itself: reject per spec */
        JsValue e = js_error_fmt(ctx, "TypeError: Chaining cycle detected for promise #<Promise>");
        js_promise_reject(ctx, p, e);
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

/*
 * A rejection handed on: the raise it came from is on its way out again, through whoever was
 * waiting. Its trace T is put back as the context's, so a fiber it leaves adds its frames to it
 * and a promise it rejects takes it, whatever was raised in between. With no trace, nothing is
 * on its way.
 */
static void raise_again(JsContext *ctx, JsTrace *t, JsValue value) {
    ctx->raise_fresh = t != NULL;
    if (!t)
        return;
    memcpy(ctx->error_trace, t->frames, t->count * sizeof(JsStackFrame));
    ctx->error_trace_count = t->count;
    ctx->error_trace_value = value;
    ctx->raise_trace = t;
}

static void run_thenable_job(JsContext *ctx, JsJob *job);

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
    /* Stop dequeuing once interrupted. Each job would only enter the
     * interpreter and throw on its first instruction, but a chain that
     * re-queues itself would still spin here allocating a fiber per turn.
     * Anything left queued stays queued, and js_has_pending_jobs reports it. */
again:
    while (vm->jobs_head && !vm->interrupt) {
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
        raise_again(ctx, job->trace, job->value);
        if (job->kind == RX_AWAIT)
            js_resume_fiber(ctx, job->fiber, job->value, job->is_throw);
        else if (job->kind == RX_THENABLE)
            run_thenable_job(ctx, job);
        else
            run_then_job(ctx, job);
        ctx->raise_fresh = false;
        vm->running_job = job->next;
        job_recycle(vm, job);
    }
    if (!vm->running_job && !vm->interrupt) {
        /* the queue is empty: the job is over. WeakRef targets kept for it may go now
         * (ClearKeptObjects), and FinalizationRegistry callbacks run, as a task of their own
         * would in V8, then whatever jobs they queued. */
        js_weak_clear_kept(ctx);
        if (vm->weak_cleanup_pending && js_weak_run_cleanups(ctx))
            goto again;
    }
    /* Only at quiescence: trimming mid-drain would free nodes the very next
     * reaction wants back. */
    if (!vm->running_job)
        jobs_free_trim(vm);
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

size_t js_promise_error_stack(JsContext *ctx, JsValue promise, JsStackFrame *out, size_t max) {
    if (!out || !js_is_promise(promise))
        return 0;
    const JsPromise *p = js_value_promise(promise);
    if (p->state != JS_PROMISE_REJECTED)
        return 0;
    size_t n = js_error_stack(ctx, p->value, out, max);
    if (n || !p->trace)
        return n;
    n = p->trace->count < max ? p->trace->count : max;
    memcpy(out, p->trace->frames, n * sizeof *out);
    return n;
}

/* ===================================================================
 * JS-level Promise library
 * =================================================================== */

#define ARG(i) ((i) < argc ? args[i] : js_undefined())

static bool nthrow(JsContext *ctx, JsValue *r, const char *msg) {
    *r = js_error_from_ascii(ctx, msg); /* Error object; string on OOM */
    return false;
}

/* Coerces a value to a promise: passes promises through, wraps others in a
 * fulfilled promise. NULL on OOM. */
static JsPromise *coerce_promise(JsContext *ctx, JsValue v) {
    if (js_is_promise(v))
        return js_value_promise(v);
    js_gc_protect(ctx->vm, &v);
    JsPromise *p = js_promise_alloc(ctx);
    js_gc_unprotect(ctx->vm, &v);
    if (!p)
        return NULL;
    JsValue pv = js_value_from_cell(&p->gc);
    js_gc_protect(ctx->vm, &pv);
    js_promise_resolve_with(ctx, p, v); /* a thenable is adopted, as PromiseResolve does */
    js_gc_unprotect(ctx->vm, &pv);
    return p;
}

/* ---- prototype methods (this = promise) ---- */

static bool pm_then(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (!js_is_promise(tv))
        return (*r = js_error_receiver(ctx, "Promise", tv)), false;
    JsPromise *res = js_promise_then(ctx, js_value_promise(tv), ARG(0), ARG(1));
    if (!res)
        return nthrow(ctx, r, "out of memory");
    *r = js_value_from_cell(&res->gc);
    return true;
}

static bool pm_catch(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (!js_is_promise(tv))
        return (*r = js_error_receiver(ctx, "Promise", tv)), false;
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
 * rejection reason: BOUND is a rejected promise nothing else sees, which keeps the reason
 * together with the trace of its raise. */
static bool finally_reason_rethrow(JsContext *ctx, JsValue bound, JsValue tv,
                                   const JsValue *args, int argc, JsValue *r) {
    (void)tv; (void)args; (void)argc;
    JsPromise *kept = js_value_promise(bound);
    *r = kept->value; /* the original rejection reason */
    raise_again(ctx, kept->trace, kept->value);
    return false;
}

/* Runs onFinally (bound). Returns true if the caller should fall through to
 * passing the original settlement along; false if *chain has been set to a
 * derived promise the caller must adopt, or if onFinally threw (in which case
 * *threw is set and *r holds the thrown value). `passthrough` re-emits the
 * original settlement once onFinally's returned thenable settles. A rejection (REJECTED) goes
 * on with the trace of its raise, which onFinally, running in between, would otherwise end. */
static bool finally_run(JsContext *ctx, JsValue bound, JsValue orig, bool rejected,
                        JsBoundFn passthrough, JsValue *chain,
                        bool *threw, JsValue *r) {
    JsVm *vm = ctx->vm;
    *threw = false;
    *chain = js_undefined();
    if (!js_is_function(bound))
        return true; /* no onFinally: pass the original settlement straight through */
    JsTrace *trace = rejected && ctx->raise_fresh ? js_raise_trace(ctx) : NULL;
    JsValue tracev = trace ? js_value_from_cell(&trace->gc) : js_undefined(); /* only to root it */
    js_gc_protect(vm, &tracev);
    JsValue result;
    if (!js_call(ctx, bound, js_undefined(), NULL, 0, &result)) {
        js_gc_unprotect(vm, &tracev);
        *threw = true;
        *r = result; /* onFinally threw — that rejection wins */
        return false;
    }
    if (!js_is_promise(result)) {
        js_gc_unprotect(vm, &tracev);
        if (rejected)
            raise_again(ctx, trace, orig);
        return true; /* non-thenable return is ignored; pass original through */
    }
    /* onFinally returned a thenable: wait for it, then re-emit `orig`. */
    js_gc_protect(vm, &result);
    js_gc_protect(vm, &orig);
    JsValue kept = orig;
    if (rejected) {
        /* the reason and its trace, as a rejected promise of the machinery's own */
        JsPromise *k = js_promise_alloc(ctx);
        if (k) {
            k->handled = true;
            k->trace = trace;
            ctx->raise_fresh = false; /* nothing is being raised: the trace is the one given */
            js_promise_reject(ctx, k, orig);
            kept = js_value_from_cell(&k->gc);
        } else {
            kept = js_undefined();
        }
    }
    js_gc_protect(vm, &kept);
    JsValue passer = js_is_undefined(kept) && rejected ? js_undefined()
                                                       : js_bound_native_new(ctx, passthrough, kept);
    JsPromise *chained = js_is_function(passer)
        ? js_promise_then(ctx, js_value_promise(result), passer, js_undefined())
        : NULL;
    js_gc_unprotect(vm, &kept);
    js_gc_unprotect(vm, &orig);
    js_gc_unprotect(vm, &result);
    js_gc_unprotect(vm, &tracev);
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
    if (finally_run(ctx, bound, ARG(0), false, finally_value_passthrough, &chain, &threw, r)) {
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
    if (finally_run(ctx, bound, ARG(0), true, finally_reason_rethrow, &chain, &threw, r)) {
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
        return (*r = js_error_receiver(ctx, "Promise", tv)), false;
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
 * Promise.all / allSettled / any share a per-call state object bound into
 * each element's reaction: { results, remaining, promise, mode }. For any,
 * `results` collects the rejection reasons.
 */
#define ALL_MODE_ALL 0
#define ALL_MODE_SETTLED 1
#define ALL_MODE_ANY 2

static bool race_resolve(JsContext *ctx, JsValue bound, JsValue tv, const JsValue *args, int argc, JsValue *r);

/* Promise.any's rejection when every input rejected (or there were none) */
static void reject_all_rejected(JsContext *ctx, JsPromise *rp, JsObject *reasons) {
    JsValue rpv = js_value_from_cell(&rp->gc);
    js_gc_protect(ctx->vm, &rpv);
    JsValue e = js_aggregate_error_new(ctx, reasons, "All promises were rejected");
    js_promise_reject(ctx, js_value_promise(rpv), js_is_object(e) ? e : js_oom_value(ctx->vm));
    js_gc_unprotect(ctx->vm, &rpv);
}

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
    int mode = (int)js_primitive_to_number(state_get(ctx, state, "mode"));
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
    double rem = js_primitive_to_number(state_get(ctx, state, "remaining")) - 1;
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
    int mode = (int)js_primitive_to_number(state_get(ctx, state, "mode"));
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
        double rem = js_primitive_to_number(state_get(ctx, state, "remaining")) - 1;
        state_set(ctx, state, "remaining", js_number(rem));
        if (rem == 0) {
            JsPromise *rp = js_value_promise(state_get(ctx, state, "promise"));
            js_promise_fulfill(ctx, rp, js_value_from_cell(&results->gc));
        }
    } else if (mode == ALL_MODE_ANY) {
        JsObject *reasons = js_value_object(state_get(ctx, state, "results"));
        reasons->elems[idx] = ARG(0);
        double rem = js_primitive_to_number(state_get(ctx, state, "remaining")) - 1;
        state_set(ctx, state, "remaining", js_number(rem));
        if (rem == 0)
            reject_all_rejected(ctx, js_value_promise(state_get(ctx, state, "promise")), reasons);
    } else {
        JsPromise *rp = js_value_promise(state_get(ctx, state, "promise"));
        js_promise_reject(ctx, rp, ARG(0));
    }
    *r = js_undefined();
    return true;
}

/* A combinator's answer when its argument cannot be listed: a promise rejected with ERR, as V8
 * gives (it does not throw) */
static bool reject_with(JsContext *ctx, JsValue err, JsValue *r) {
    if (err.bits == js_oom_value(ctx->vm).bits) /* out of memory still stops the script */
        return (*r = err), false;
    js_gc_protect(ctx->vm, &err);
    JsValue pv = js_promise_new(ctx);
    if (js_is_promise(pv))
        js_promise_reject(ctx, js_value_promise(pv), err);
    js_gc_unprotect(ctx->vm, &err);
    if (!js_is_promise(pv))
        return nthrow(ctx, r, "out of memory");
    *r = pv;
    return true;
}

static bool all_impl(JsContext *ctx, const JsValue *args, int argc, JsValue *r, int mode) {
    JsObject *input = js_iterable_to_array(ctx, ARG(0), r);
    if (!input)
        return reject_with(ctx, *r, r);
    JsValue iterable = js_value_from_cell(&input->gc); /* a new array: rooted below */
    uint32_t n = input->elem_count;

    js_gc_protect(ctx->vm, &iterable);
    JsValue rpv = js_promise_new(ctx);
    js_gc_unprotect(ctx->vm, &iterable);
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

        if (n == 0 && mode == ALL_MODE_ANY)
            reject_all_rejected(ctx, js_value_promise(rpv), results);
        else if (n == 0)
            js_promise_fulfill(ctx, js_value_promise(rpv), resultsv);
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
                JsValue onf = mode == ALL_MODE_ANY ? js_bound_native_new(ctx, race_resolve, rpv)
                                                   : js_bound_native_new(ctx, all_element_resolve, pairv);
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

static bool ps_any(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return all_impl(ctx, args, argc, r, ALL_MODE_ANY);
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
    JsObject *input = js_iterable_to_array(ctx, ARG(0), r);
    if (!input)
        return reject_with(ctx, *r, r);
    JsValue iterable = js_value_from_cell(&input->gc);
    js_gc_protect(ctx->vm, &iterable);
    JsValue rpv = js_promise_new(ctx);
    js_gc_unprotect(ctx->vm, &iterable);
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

/* CreateResolvingFunctions: resolve and reject for PV, sharing [[AlreadyResolved]]: both are bound
 * to one array that holds the promise until either is called. False on OOM. RESOLVE and REJECT
 * are rooted by the caller. */
static bool rf_settle(JsContext *ctx, JsValue bound, const JsValue *args, int argc, bool reject) {
    JsObject *a = js_value_object(bound);
    JsValue pv = a->elems[0];
    if (!js_is_promise(pv))
        return true; /* already resolved: a later call does nothing */
    a->elems[0] = js_undefined();
    js_gc_protect(ctx->vm, &pv);
    if (reject)
        js_promise_reject(ctx, js_value_promise(pv), ARG(0));
    else
        js_promise_resolve_with(ctx, js_value_promise(pv), ARG(0));
    js_gc_unprotect(ctx->vm, &pv);
    return true;
}

static bool rf_resolve(JsContext *ctx, JsValue bound, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    return rf_settle(ctx, bound, args, argc, false);
}

static bool rf_reject(JsContext *ctx, JsValue bound, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    *r = js_undefined();
    return rf_settle(ctx, bound, args, argc, true);
}

static bool resolving_functions(JsContext *ctx, JsValue pv, JsValue *resolve, JsValue *reject) {
    *resolve = *reject = js_undefined();
    js_gc_protect(ctx->vm, &pv);
    JsObject *a = js_array_new_cell(ctx, 1);
    JsValue av = a ? js_value_from_cell(&a->gc) : js_undefined();
    if (a) {
        a->elems[0] = pv;
        a->elem_count = 1;
    }
    js_gc_protect(ctx->vm, &av);
    if (a)
        *resolve = js_bound_native_new(ctx, rf_resolve, av);
    if (js_is_function(*resolve)) {
        js_gc_protect(ctx->vm, resolve);
        *reject = js_bound_native_new(ctx, rf_reject, av);
        js_gc_unprotect(ctx->vm, resolve);
    }
    js_gc_unprotect(ctx->vm, &av);
    js_gc_unprotect(ctx->vm, &pv);
    if (!js_is_function(*reject))
        return false;
    /* they take one argument, as V8's say */
    ((JsNative *)js_value_cell(*resolve))->length = 1;
    ((JsNative *)js_value_cell(*reject))->length = 1;
    return true;
}

/* NewPromiseResolveThenableJob's work: thenable.then(resolve, reject) for the job's promise; a
 * throw rejects it unless it was resolved first */
static void run_thenable_job(JsContext *ctx, JsJob *job) {
    JsValue pv = js_value_from_cell(&job->result->gc), resolve, reject, res;
    if (!resolving_functions(ctx, pv, &resolve, &reject)) {
        js_promise_reject(ctx, job->result, js_oom_value(ctx->vm));
        return;
    }
    js_gc_protect(ctx->vm, &resolve);
    js_gc_protect(ctx->vm, &reject);
    JsValue args[2] = {resolve, reject};
    if (!js_call(ctx, job->on_fulfilled, job->value, args, 2, &res)) {
        js_gc_protect(ctx->vm, &res);
        JsValue dummy;
        rf_reject(ctx, ((JsNative *)js_value_cell(reject))->bound, js_undefined(), &res, 1, &dummy);
        js_gc_unprotect(ctx->vm, &res);
    }
    js_gc_unprotect(ctx->vm, &reject);
    js_gc_unprotect(ctx->vm, &resolve);
}

/* Callable Promise(executor) */
static bool promise_ctor(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    if (!js_native_is_construct_call(ctx)) /* before the executor is looked at, as in V8 */
        return nthrow(ctx, r, "TypeError: Promise constructor cannot be invoked without 'new'");
    if (!js_is_function(ARG(0)))
        return (*r = js_error_fmt(ctx, "TypeError: Promise resolver %v is not a function", ARG(0))), false;
    JsValue pv = js_promise_new(ctx);
    if (!js_is_promise(pv))
        return nthrow(ctx, r, "out of memory");
    js_gc_protect(ctx->vm, &pv);
    JsValue executor = ARG(0);
    js_gc_protect(ctx->vm, &executor);
    JsValue resolve, reject;
    bool ok = resolving_functions(ctx, pv, &resolve, &reject);
    js_gc_protect(ctx->vm, &resolve);
    js_gc_protect(ctx->vm, &reject);
    if (ok) {
        JsValue ex_args[2] = {resolve, reject};
        JsValue ex_res, dummy;
        if (!js_call(ctx, executor, js_undefined(), ex_args, 2, &ex_res)) {
            js_gc_protect(ctx->vm, &ex_res); /* executor threw: rejects, unless resolved first */
            rf_reject(ctx, ((JsNative *)js_value_cell(reject))->bound, js_undefined(), &ex_res, 1, &dummy);
            js_gc_unprotect(ctx->vm, &ex_res);
        }
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

/* Promise.withResolvers(): { promise, resolve, reject }, the resolving functions those an
 * executor would get */
static bool ps_withResolvers(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    (void)args;
    (void)argc;
    JsValue pv = js_promise_new(ctx);
    if (!js_is_promise(pv))
        return nthrow(ctx, r, "out of memory");
    js_gc_protect(ctx->vm, &pv);
    JsValue resolve, reject;
    bool made = resolving_functions(ctx, pv, &resolve, &reject);
    js_gc_protect(ctx->vm, &resolve);
    js_gc_protect(ctx->vm, &reject);
    JsValue o = made ? js_object_new(ctx) : js_undefined();
    js_gc_protect(ctx->vm, &o); /* the property writes allocate */
    bool ok = js_is_object(o);
    if (ok) {
        JsObject *obj = js_value_object(o);
        ok = js_object_set_ascii(ctx, obj, "promise", pv) && js_object_set_ascii(ctx, obj, "resolve", resolve) &&
             js_object_set_ascii(ctx, obj, "reject", reject);
    }
    js_gc_unprotect(ctx->vm, &o);
    js_gc_unprotect(ctx->vm, &reject);
    js_gc_unprotect(ctx->vm, &resolve);
    js_gc_unprotect(ctx->vm, &pv);
    if (!ok)
        return nthrow(ctx, r, "out of memory");
    *r = o;
    return true;
}

/* Promise.try(f, ...args): a promise resolved with f(...args), or rejected with what it threw
 * (a non-function f is a rejection too, as V8) */
static bool ps_try(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsPromise *p = js_promise_alloc(ctx);
    if (!p)
        return nthrow(ctx, r, "out of memory");
    JsValue pv = js_value_from_cell(&p->gc), res;
    js_gc_protect(ctx->vm, &pv);
    if (!js_is_function(ARG(0)))
        js_promise_reject(ctx, p, js_error_fmt(ctx, "TypeError: %t is not a function", ARG(0)));
    else if (!js_call(ctx, ARG(0), js_undefined(), argc > 1 ? args + 1 : NULL, argc > 1 ? argc - 1 : 0, &res))
        js_promise_reject(ctx, p, res);
    else
        js_promise_resolve_with(ctx, p, res);
    js_gc_unprotect(ctx->vm, &pv);
    *r = pv;
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

    /* the global Promise (a constructor only: a plain call throws) with its statics */
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
    js_native_builtin_ctor(ctor, ctx->promise_methods); /* Promise.prototype: then, catch, finally */
    bool ok = js_object_set_ascii(ctx, ctx->promise_methods, "constructor", ctor) &&
              js_object_set_ascii(ctx, ctx->globals, "Promise", ctor);
    js_gc_unprotect(vm, &ctor);
    if (!ok)
        return false;
    JsObject *st = native->statics;
    if (!def_fn(ctx, st, "resolve", ps_resolve) || !def_fn(ctx, st, "reject", ps_reject) ||
        !def_fn(ctx, st, "all", ps_all) || !def_fn(ctx, st, "race", ps_race) ||
        !def_fn(ctx, st, "allSettled", ps_allSettled) || !def_fn(ctx, st, "any", ps_any) ||
        !def_fn(ctx, st, "try", ps_try) || !def_fn(ctx, st, "withResolvers", ps_withResolvers))
        return false;
    return true;
}

/* ===================================================================
 * Array.fromAsync
 * ===================================================================
 *
 * A sync iterable's values (or an array-like's elements) awaited in turn, each mapped (and the
 * mapping awaited) when a map function is given, into a new array the returned promise fulfills
 * with. There are no async iterables (no symbols, no async generators), so every source is
 * read as V8 reads a sync one, through an async-from-sync iterator: a value that rejects closes
 * the iterator, as does a map function that throws or rejects. The steps run as reactions on the
 * awaited promises, carrying their state in an array. */

enum { FA_SRC, FA_I, FA_A, FA_MAP, FA_THIS, FA_P, FA_K, FA_LEN, FA_COUNT };

static JsObject *fa_state(JsValue st) {
    return js_value_object(st);
}

static void fa_step(JsContext *ctx, JsValue st);

/* Rejects the result with ERR, closing the iterator first when CLOSE (ERR rooted by the caller) */
static void fa_fail(JsContext *ctx, JsValue st, JsValue err, bool close) {
    JsObject *s = fa_state(st);
    if (close && js_get_number(s->elems[FA_LEN]) < 0) {
        JsIterCursor cur = {s->elems[FA_SRC], (uint32_t)js_get_number(s->elems[FA_I])};
        JsValue ignored;
        js_iter_close(ctx, &cur, true, false, &ignored); /* a throw: what return() does is ignored */
        s->elems[FA_I] = js_number(JS_ITER_DONE_INDEX);
    }
    js_promise_reject(ctx, js_value_promise(s->elems[FA_P]), err);
}

/* Await(V), then ON_F(value) or ON_R(reason) with the state */
static void fa_await(JsContext *ctx, JsValue st, JsValue v, JsBoundFn on_f, JsBoundFn on_r) {
    js_gc_protect(ctx->vm, &st);
    js_gc_protect(ctx->vm, &v);
    JsPromise *p = NULL;
    if (js_is_promise(v)) {
        p = js_value_promise(v);
    } else if ((p = js_promise_alloc(ctx)) != NULL) {
        JsValue pv = js_value_from_cell(&p->gc);
        js_gc_protect(ctx->vm, &pv);
        js_promise_resolve_with(ctx, p, v); /* a thenable is adopted */
        js_gc_unprotect(ctx->vm, &pv);
    }
    JsValue pv = p ? js_value_from_cell(&p->gc) : js_undefined();
    js_gc_protect(ctx->vm, &pv);
    JsValue f = p ? js_bound_native_new(ctx, on_f, st) : js_undefined();
    js_gc_protect(ctx->vm, &f);
    JsValue rj = js_is_function(f) ? js_bound_native_new(ctx, on_r, st) : js_undefined();
    js_gc_protect(ctx->vm, &rj);
    if (!js_is_function(rj) || !js_promise_then(ctx, p, f, rj))
        fa_fail(ctx, st, js_error_from_ascii(ctx, "out of memory"), true);
    js_gc_unprotect(ctx->vm, &rj);
    js_gc_unprotect(ctx->vm, &f);
    js_gc_unprotect(ctx->vm, &pv);
    js_gc_unprotect(ctx->vm, &v);
    js_gc_unprotect(ctx->vm, &st);
}

static bool fa_on_mapped(JsContext *ctx, JsValue st, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsObject *s = fa_state(st);
    uint32_t k = (uint32_t)js_get_number(s->elems[FA_K]);
    if (!js_array_set_index(ctx->vm, js_value_object(s->elems[FA_A]), k, argc ? args[0] : js_undefined())) {
        fa_fail(ctx, st, js_error_from_ascii(ctx, "out of memory"), true);
    } else {
        s->elems[FA_K] = js_number((double)k + 1);
        fa_step(ctx, st);
    }
    *r = js_undefined();
    return true;
}

static bool fa_on_rejected_close(JsContext *ctx, JsValue st, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    fa_fail(ctx, st, argc ? args[0] : js_undefined(), true);
    *r = js_undefined();
    return true;
}

static bool fa_on_value(JsContext *ctx, JsValue st, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsObject *s = fa_state(st);
    JsValue v = argc ? args[0] : js_undefined();
    *r = js_undefined();
    if (js_is_undefined(s->elems[FA_MAP]))
        return fa_on_mapped(ctx, st, tv, &v, 1, r);
    JsValue call_args[2] = {v, s->elems[FA_K]}, res;
    js_gc_protect(ctx->vm, &st);
    if (!js_call(ctx, s->elems[FA_MAP], s->elems[FA_THIS], call_args, 2, &res)) {
        js_gc_protect(ctx->vm, &res);
        fa_fail(ctx, st, res, true);
        js_gc_unprotect(ctx->vm, &res);
    } else {
        fa_await(ctx, st, res, fa_on_mapped, fa_on_rejected_close);
    }
    js_gc_unprotect(ctx->vm, &st);
    return true;
}

static void fa_step(JsContext *ctx, JsValue st) {
    JsObject *s = fa_state(st);
    JsValue v;
    js_gc_protect(ctx->vm, &st);
    double len = js_get_number(s->elems[FA_LEN]);
    int got;
    if (len < 0) { /* an iterable */
        JsIterCursor cur = {s->elems[FA_SRC], (uint32_t)js_get_number(s->elems[FA_I])};
        js_gc_protect(ctx->vm, &cur.src);
        got = js_iter_next(ctx, &cur, &v);
        js_gc_unprotect(ctx->vm, &cur.src);
        s->elems[FA_SRC] = cur.src;
        s->elems[FA_I] = js_number(cur.i);
    } else { /* an array-like */
        double k = js_get_number(s->elems[FA_K]);
        got = k >= len ? 0 : 1;
        if (got) {
            JsValue key = js_number(k);
            JsString *ks = js_op_to_string(ctx, key);
            got = ks && js_get_property(ctx, s->elems[FA_SRC], js_value_from_cell(&ks->gc), &v) ? 1 : -1;
            if (got < 0)
                v = js_take_error(ctx);
        }
    }
    js_gc_protect(ctx->vm, &v);
    if (got == 0) {
        JsValue a = s->elems[FA_A];
        js_promise_resolve_with(ctx, js_value_promise(s->elems[FA_P]), a);
    } else if (got < 0) {
        js_promise_reject(ctx, js_value_promise(s->elems[FA_P]), v); /* next threw: nothing to close */
    } else {
        fa_await(ctx, st, v, fa_on_value, fa_on_rejected_close); /* an array-like has nothing to close */
    }
    js_gc_unprotect(ctx->vm, &v);
    js_gc_unprotect(ctx->vm, &st);
}

bool js_array_from_async(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsPromise *p = js_promise_alloc(ctx);
    if (!p)
        return nthrow(ctx, r, "out of memory");
    JsValue pv = js_value_from_cell(&p->gc);
    js_gc_protect(ctx->vm, &pv);
    JsValue src = ARG(0), mapfn = ARG(1);
    if (!js_is_undefined(mapfn) && !js_is_function(mapfn)) {
        js_promise_reject(ctx, p, js_error_fmt(ctx, "TypeError: %v is not a function", mapfn)); /* V8 words this one without the type */
    } else if (js_is_undefined(src) || js_is_null(src)) {
        js_promise_reject(ctx, p, js_error_fmt(ctx, "TypeError: Cannot read properties of %v (reading 'Symbol(Symbol.asyncIterator)')", src));
    } else {
        JsObject *s = js_array_new_cell(ctx, FA_COUNT);
        JsValue st = s ? js_value_from_cell(&s->gc) : js_undefined();
        js_gc_protect(ctx->vm, &st);
        if (s) { /* every slot set, so the collector may trace it from here on */
            for (int i = 0; i < FA_COUNT; i++)
                s->elems[i] = js_undefined();
            s->elem_count = FA_COUNT;
        }
        JsObject *a = s ? js_array_new_cell(ctx, 0) : NULL;
        if (a)
            s->elems[FA_A] = js_value_from_cell(&a->gc); /* rooted through the state */
        JsIterCursor cur = {js_undefined(), 0};
        int open = a ? js_iter_open(ctx, src, &cur) : -1;
        double len = -1;
        bool ok = open >= 0;
        if (ok && open == 0) { /* an array-like: ToObject, then its length */
            JsValue lv = js_undefined();
            JsString *lk = js_ascii_cell(ctx->vm, "length");
            ok = lk && js_get_property(ctx, src, js_value_from_cell(&lk->gc), &lv);
            double d = 0;
            if (ok && !js_is_undefined(lv))
                ok = js_op_to_number(ctx, lv, &d);
            d = d != d || d <= 0 ? 0 : d > 9007199254740991.0 ? 9007199254740991.0 : __builtin_floor(d);
            len = d;
            cur.src = src;
        }
        if (ok) {
            s->elems[FA_SRC] = cur.src;
            s->elems[FA_I] = js_number(cur.i);
            s->elems[FA_MAP] = mapfn;
            s->elems[FA_THIS] = ARG(2);
            s->elems[FA_P] = pv;
            s->elems[FA_K] = js_number(0);
            s->elems[FA_LEN] = js_number(len);
            fa_step(ctx, st);
        } else {
            JsValue e = js_is_undefined(ctx->pending_error) ? js_error_from_ascii(ctx, "out of memory")
                                                             : js_take_error(ctx);
            js_promise_reject(ctx, p, e);
        }
        js_gc_unprotect(ctx->vm, &st);
    }
    js_gc_unprotect(ctx->vm, &pv);
    *r = pv;
    return true;
}
