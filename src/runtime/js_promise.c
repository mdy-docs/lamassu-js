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
 * allocation belongs there. job_alloc takes from the reserve, and job_recycle
 * returns nodes there; a job queued without a reaction (job_new) adds the node
 * it takes, so the reserve is never raided.
 */
static bool job_reserve(JsVm *vm) {
    JsJob *job = js_realloc_raw(vm, NULL, 0, sizeof(JsJob));
    if (!job)
        return false;
    job->next = vm->jobs_free;
    vm->jobs_free = job;
    return true;
}

/* A job to queue now, for no reaction: NULL on OOM */
static JsJob *job_new(JsVm *vm) {
    return job_reserve(vm) ? job_alloc(vm) : NULL;
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

/* %Promise%: the constructor the engine made, whatever the global Promise holds now */
static bool is_promise_ctor(const JsContext *ctx, JsValue c) {
    const JsNative *n = ctx->species_ctors[JS_SPECIES_PROMISE];
    return n && js_is_function(c) && js_value_cell(c) == &n->gc;
}

static JsValue promise_ctor_value(JsContext *ctx) {
    return js_value_from_cell(&ctx->species_ctors[JS_SPECIES_PROMISE]->gc);
}

/* A promise a script has changed nothing about: no property of its own, the prototype it was made
 * with, and Promise.prototype's `then` and `constructor` the ones the engine put there. What the
 * specification reads from such a promise (its `then`, its `constructor`) is known without
 * looking, so the paths below take those reads as done and go straight to the reaction lists. */
static bool promise_plain(JsContext *ctx, const JsPromise *p) {
    return !p->props && !p->proto_set && js_intact(ctx, JS_INTACT_PROMISE_THEN) &&
           js_intact(ctx, JS_INTACT_PROMISE_CONSTRUCTOR);
}

static JsValue key_then(JsContext *ctx) {
    static const uint16_t kthen[] = {'t', 'h', 'e', 'n'};
    return js_atom(ctx->vm, kthen, 4);
}

/*
 * Promise Resolve Functions (27.2.1.3.2), steps 7-16: P is resolved with VALUE. A value that is
 * no object fulfills it. An object's `then` is read now (a throw rejects P); when that is not
 * callable P fulfills with the object, and otherwise P follows it through a job that calls
 * value.then(resolve, reject) (NewPromiseResolveThenableJob). A promise is such an object: P
 * settles two jobs after it, one for the call of `then` and one for the reaction that call
 * registers, as in V8. A plain promise's `then` is known (promise_plain), so the job is queued
 * with no function (undefined) and run_thenable_job registers the reaction directly.
 */
void js_promise_resolve_with(JsContext *ctx, JsPromise *p, JsValue value) {
    JsVm *vm = ctx->vm;
    bool is_p = js_is_promise(value);
    /* an object a script can see (a host's bytecode buffer carries the object tag too) */
    if (!is_p && !(js_is_object(value) && js_value_cell(value)->kind == JS_KIND_OBJECT) && !js_is_function(value)) {
        js_promise_fulfill(ctx, p, value);
        return;
    }
    if (is_p && js_value_promise(value) == p) { /* resolving with itself: reject per spec */
        JsValue e = js_error_fmt(ctx, "TypeError: Chaining cycle detected for promise #<Promise>");
        js_promise_reject(ctx, p, e);
        return;
    }
    JsValue pv = js_value_from_cell(&p->gc), then = js_undefined();
    js_gc_protect(vm, &pv);
    js_gc_protect(vm, &value);
    js_gc_protect(vm, &then);
    bool queue = is_p && promise_plain(ctx, js_value_promise(value));
    if (!queue) {
        JsValue key = key_then(ctx);
        if (!js_is_string(key) || !js_get_property(ctx, value, key, &then)) {
            JsValue e = js_is_undefined(ctx->pending_error) ? js_oom_value(vm) : js_take_error(ctx);
            js_promise_reject(ctx, p, e);
        } else if (!js_is_function(then)) {
            js_promise_fulfill(ctx, p, value);
        } else {
            queue = true;
        }
    }
    if (queue) {
        JsJob *job = job_new(vm);
        if (!job) {
            js_promise_reject(ctx, p, js_oom_value(vm));
        } else {
            job->kind = RX_THENABLE;
            job->value = value;
            job->on_fulfilled = then;
            job->result = p;
            job_enqueue(vm, job);
        }
    }
    js_gc_unprotect(vm, &then);
    js_gc_unprotect(vm, &value);
    js_gc_unprotect(vm, &pv);
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
    if (!r) {
        /* a reaction whose promise another constructor made (cap_reaction): the resolve or
         * reject it gave threw, and no promise is left to take that. V8 reports an uncaught
         * exception; here the host hears of it as a rejection nothing handles. */
        if (!ok) {
            js_gc_protect(ctx->vm, &res);
            JsPromise *lost = js_promise_alloc(ctx);
            if (lost)
                js_promise_reject(ctx, lost, res);
            js_gc_unprotect(ctx->vm, &res);
        }
        return;
    }
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

/* An object as the specification counts one (a promise and a function are objects) */
static bool is_objectish(JsValue v) {
    return js_is_promise(v) || js_is_function(v) || (js_is_object(v) && js_value_cell(v)->kind == JS_KIND_OBJECT);
}

/* ---- resolving functions and capabilities ---- */

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

/* A PromiseCapability Record: a promise with the functions that settle it */
typedef struct PromiseCap {
    JsValue promise, resolve, reject;
} PromiseCap;

static void cap_protect(JsVm *vm, PromiseCap *cap) {
    js_gc_protect(vm, &cap->promise);
    js_gc_protect(vm, &cap->resolve);
    js_gc_protect(vm, &cap->reject);
}

static void cap_unprotect(JsVm *vm, PromiseCap *cap) {
    js_gc_unprotect(vm, &cap->reject);
    js_gc_unprotect(vm, &cap->resolve);
    js_gc_unprotect(vm, &cap->promise);
}

/* The executor NewPromiseCapability hands a constructor (27.2.1.5.1), bound to the two slots the
 * constructor's resolve and reject land in */
static bool cap_executor(JsContext *ctx, JsValue bound, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    JsObject *s = js_value_object(bound);
    if (!js_is_undefined(s->elems[0]) || !js_is_undefined(s->elems[1]))
        return nthrow(ctx, r, "TypeError: Promise executor has already been invoked with non-undefined arguments");
    s->elems[0] = ARG(0);
    s->elems[1] = ARG(1);
    *r = js_undefined();
    return true;
}

/*
 * NewPromiseCapability(C) (27.2.1.5). %Promise% gives a promise of the engine's with its resolving
 * functions; any other constructor is called with an executor and must hand it two functions.
 * False thrown, *err the error. The three values come back unrooted: the caller protects them
 * (cap_protect) before it allocates.
 */
static bool new_capability(JsContext *ctx, JsValue c, PromiseCap *cap, JsValue *err) {
    JsVm *vm = ctx->vm;
    cap->promise = cap->resolve = cap->reject = js_undefined();
    if (is_promise_ctor(ctx, c)) {
        JsValue pv = js_promise_new(ctx);
        if (!js_is_promise(pv) || !resolving_functions(ctx, pv, &cap->resolve, &cap->reject))
            return (*err = js_error_from_ascii(ctx, "out of memory")), false;
        cap->promise = pv; /* held by the functions' shared array until here */
        return true;
    }
    js_gc_protect(vm, &c);
    JsObject *s = js_array_new_cell(ctx, 2);
    JsValue sv = s ? js_value_from_cell(&s->gc) : js_undefined();
    if (s) {
        s->elems[0] = s->elems[1] = js_undefined();
        s->elem_count = 2;
    }
    js_gc_protect(vm, &sv);
    JsValue ex = s ? js_bound_native_new(ctx, cap_executor, sv) : js_undefined();
    js_gc_protect(vm, &ex);
    JsValue made = js_undefined();
    bool ok = false;
    if (!js_is_function(ex)) {
        *err = js_error_from_ascii(ctx, "out of memory");
    } else {
        ((JsNative *)js_value_cell(ex))->length = 2;
        if (!js_construct(ctx, c, &ex, 1, &made)) {
            *err = made;
        } else if (!js_is_function(s->elems[0]) || !js_is_function(s->elems[1])) {
            *err = js_error_from_ascii(ctx, "TypeError: Promise resolve or reject function is not callable");
        } else {
            cap->promise = made;
            cap->resolve = s->elems[0];
            cap->reject = s->elems[1];
            ok = true;
        }
    }
    js_gc_unprotect(vm, &ex);
    js_gc_unprotect(vm, &sv);
    js_gc_unprotect(vm, &c);
    return ok;
}

/*
 * PromiseResolve(C, X) (27.2.4.7.1): X itself when it is a promise whose `constructor` is C, else
 * a promise made by C and resolved with X. False thrown, *out the error. *out may be a slot the
 * caller has rooted.
 */
static bool promise_resolve(JsContext *ctx, JsValue c, JsValue x, JsValue *out) {
    JsVm *vm = ctx->vm;
    bool native = is_promise_ctor(ctx, c);
    if (js_is_promise(x)) {
        if (native && promise_plain(ctx, js_value_promise(x)))
            return (*out = x), true;
        js_gc_protect(vm, &c);
        js_gc_protect(vm, &x);
        JsString *k = js_ascii_cell(vm, "constructor");
        JsValue xc = js_undefined();
        bool got = k && js_get_property(ctx, x, js_value_from_cell(&k->gc), &xc);
        js_gc_unprotect(vm, &x);
        js_gc_unprotect(vm, &c);
        if (!got)
            return (*out = k ? js_take_error(ctx) : js_error_from_ascii(ctx, "out of memory")), false;
        if (js_same_value(xc, c))
            return (*out = x), true;
    }
    if (native) {
        js_gc_protect(vm, &x);
        JsValue pv = js_promise_new(ctx);
        js_gc_protect(vm, &pv);
        if (js_is_promise(pv))
            js_promise_resolve_with(ctx, js_value_promise(pv), x);
        js_gc_unprotect(vm, &pv);
        js_gc_unprotect(vm, &x);
        if (!js_is_promise(pv))
            return (*out = js_error_from_ascii(ctx, "out of memory")), false;
        return (*out = pv), true;
    }
    PromiseCap cap;
    js_gc_protect(vm, &x);
    bool ok = new_capability(ctx, c, &cap, out);
    if (ok) {
        cap_protect(vm, &cap);
        JsValue ignored;
        ok = js_call(ctx, cap.resolve, js_undefined(), &x, 1, &ignored);
        cap_unprotect(vm, &cap);
        *out = ok ? cap.promise : ignored;
    }
    js_gc_unprotect(vm, &x);
    return ok;
}

/* Await's PromiseResolve(%Promise%, V) and PerformPromiseThen: FIBER resumes with what V settles
 * to. V itself is waited on when it is a promise whose `constructor` is Promise (its `then` is
 * never looked at); anything else is resolved into a new promise first. 1 waiting, 0 thrown
 * (reading `constructor` threw) with the error in FIBER->error, -1 out of memory. V must be
 * rooted. */
int js_promise_await_value(JsContext *ctx, JsValue v, JsFiber *fiber) {
    JsValue pv = js_undefined();
    if (js_is_promise(v)) {
        if (promise_plain(ctx, js_value_promise(v)))
            return js_promise_await(ctx, js_value_promise(v), fiber) ? 1 : -1;
    } else if (!is_objectish(v)) { /* nothing to look at: a promise already fulfilled with it */
        JsPromise *p = js_promise_alloc(ctx);
        if (!p)
            return -1;
        js_promise_fulfill(ctx, p, v);
        return js_promise_await(ctx, p, fiber) ? 1 : -1;
    }
    js_gc_protect(ctx->vm, &pv);
    int st = 1;
    if (!promise_resolve(ctx, promise_ctor_value(ctx), v, &pv)) {
        fiber->error = pv;
        st = 0;
    } else if (!js_promise_await(ctx, js_value_promise(pv), fiber)) {
        st = -1;
    }
    js_gc_unprotect(ctx->vm, &pv);
    return st;
}

/* Invoke(V, "then", ARGS) (7.3.12): V's `then`, read as a property and called on V. A plain
 * promise has Promise.prototype's, which makes a promise of the engine's and adds the reaction. */
static bool invoke_then(JsContext *ctx, JsValue v, const JsValue *args, int argc, JsValue *out) {
    if (js_is_promise(v) && promise_plain(ctx, js_value_promise(v))) {
        JsPromise *res = js_promise_then(ctx, js_value_promise(v), ARG(0), ARG(1));
        if (!res)
            return nthrow(ctx, out, "out of memory");
        *out = js_value_from_cell(&res->gc);
        return true;
    }
    JsValue key = key_then(ctx), then = js_undefined();
    if (!js_is_string(key))
        return nthrow(ctx, out, "out of memory");
    if (js_is_undefined(v) || js_is_null(v))
        return (*out = js_error_fmt(ctx, "TypeError: Cannot read properties of %v (reading 'then')", v)), false;
    if (!js_get_property(ctx, v, key, &then))
        return (*out = js_take_error(ctx)), false;
    if (!js_is_function(then))
        return (*out = js_error_fmt(ctx, "TypeError: %t is not a function", then)), false;
    return js_call(ctx, then, v, args, argc, out);
}

/* ---- prototype methods ---- */

/* The handlers of a reaction whose promise another constructor made: the capability's resolve and
 * reject are functions of a script's, so the reaction has no promise of its own and these call
 * them with what the handler gave (NewPromiseReactionJob, 27.2.2.1, steps d-h). */
enum { CR_ON_FULFILLED, CR_ON_REJECTED, CR_RESOLVE, CR_REJECT, CR_COUNT };

static bool cap_reaction(JsContext *ctx, JsValue bound, const JsValue *args, int argc, JsValue *r, bool rejected) {
    JsVm *vm = ctx->vm;
    js_gc_protect(vm, &bound);
    JsObject *s = js_value_object(bound);
    JsValue handler = s->elems[rejected ? CR_ON_REJECTED : CR_ON_FULFILLED], v = ARG(0), res = v, ignored;
    bool fulfilled = !rejected;
    if (js_is_function(handler))
        fulfilled = js_call(ctx, handler, js_undefined(), &v, 1, &res);
    js_gc_protect(vm, &res);
    bool ok = js_call(ctx, s->elems[fulfilled ? CR_RESOLVE : CR_REJECT], js_undefined(), &res, 1, &ignored);
    js_gc_unprotect(vm, &res);
    js_gc_unprotect(vm, &bound);
    *r = ok ? js_undefined() : ignored;
    return ok;
}

static bool cap_on_fulfilled(JsContext *ctx, JsValue bound, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return cap_reaction(ctx, bound, args, argc, r, false);
}

static bool cap_on_rejected(JsContext *ctx, JsValue bound, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return cap_reaction(ctx, bound, args, argc, r, true);
}

/*
 * Promise.prototype.then (27.2.5.4) for the promise TV: the result is made by the species
 * constructor (TV.constructor when that is Promise or inherits from it, js_species), which for a
 * plain promise is %Promise% without looking. TV, ON_F and ON_R must be rooted by the caller.
 */
static bool promise_then_method(JsContext *ctx, JsValue tv, JsValue on_f, JsValue on_r, JsValue *r) {
    JsVm *vm = ctx->vm;
    JsValue c = js_undefined();
    int sp = promise_plain(ctx, js_value_promise(tv))
                 ? 0 : js_species(ctx, tv, ctx->species_ctors[JS_SPECIES_PROMISE], &c);
    if (sp < 0)
        return (*r = c), false;
    if (sp == 0) {
        JsPromise *res = js_promise_then(ctx, js_value_promise(tv), on_f, on_r);
        if (!res)
            return nthrow(ctx, r, "out of memory");
        *r = js_value_from_cell(&res->gc);
        return true;
    }
    PromiseCap cap;
    if (!new_capability(ctx, c, &cap, r))
        return false;
    cap_protect(vm, &cap);
    JsObject *s = js_array_new_cell(ctx, CR_COUNT);
    JsValue sv = s ? js_value_from_cell(&s->gc) : js_undefined();
    if (s) {
        s->elems[CR_ON_FULFILLED] = on_f;
        s->elems[CR_ON_REJECTED] = on_r;
        s->elems[CR_RESOLVE] = cap.resolve;
        s->elems[CR_REJECT] = cap.reject;
        s->elem_count = CR_COUNT;
    }
    js_gc_protect(vm, &sv);
    JsValue f = s ? js_bound_native_new(ctx, cap_on_fulfilled, sv) : js_undefined();
    js_gc_protect(vm, &f);
    JsValue rj = js_is_function(f) ? js_bound_native_new(ctx, cap_on_rejected, sv) : js_undefined();
    js_gc_protect(vm, &rj);
    JsReaction *rx = js_is_function(rj) ? reaction_alloc(ctx) : NULL;
    if (rx) {
        rx->kind = RX_THEN;
        rx->on_fulfilled = f;
        rx->on_rejected = rj;
        add_reaction(ctx, js_value_promise(tv), rx);
    }
    js_gc_unprotect(vm, &rj);
    js_gc_unprotect(vm, &f);
    js_gc_unprotect(vm, &sv);
    cap_unprotect(vm, &cap);
    if (!rx)
        return nthrow(ctx, r, "out of memory");
    *r = cap.promise;
    return true;
}

static bool pm_then(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (!js_is_promise(tv))
        return (*r = js_error_receiver(ctx, "Promise", tv)), false;
    return promise_then_method(ctx, tv, ARG(0), ARG(1), r);
}

/* Promise.prototype.catch (27.2.5.1): this.then(undefined, onRejected), whatever `this` is */
static bool pm_catch(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsValue a[2] = {js_undefined(), ARG(0)};
    return invoke_then(ctx, tv, a, 2, r);
}

/*
 * Promise.prototype.finally (27.2.5.3): this.then(thenFinally, catchFinally), where each calls
 * onFinally, resolves what it returned into a promise of the species constructor C
 * (PromiseResolve) and returns that promise's then(thunk), the thunk giving back the value or
 * throwing the reason. So the settlement goes on after whatever onFinally returned settles, and a
 * throw or a rejection there takes its place. A non-callable onFinally is passed to `then` as
 * both handlers.
 */

/* the value thunk: bound to the original fulfillment value */
static bool finally_value_passthrough(JsContext *ctx, JsValue bound, JsValue tv,
                                      const JsValue *args, int argc, JsValue *r) {
    (void)ctx; (void)tv; (void)args; (void)argc;
    *r = bound;
    return true;
}

/* the thrower: BOUND is a rejected promise nothing else sees, which keeps the original reason
 * together with the trace of its raise */
static bool finally_reason_rethrow(JsContext *ctx, JsValue bound, JsValue tv,
                                   const JsValue *args, int argc, JsValue *r) {
    (void)tv; (void)args; (void)argc;
    JsPromise *kept = js_value_promise(bound);
    *r = kept->value; /* the original rejection reason */
    raise_again(ctx, kept->trace, kept->value);
    return false;
}

/* thenFinally and catchFinally (27.2.5.3.1-2), bound to [onFinally, C]. A rejection (REJECTED)
 * goes on with the trace of its raise, which onFinally, running in between, would otherwise end. */
static bool finally_step(JsContext *ctx, JsValue bound, JsValue orig, bool rejected, JsValue *r) {
    JsVm *vm = ctx->vm;
    JsTrace *trace = rejected && ctx->raise_fresh ? js_raise_trace(ctx) : NULL;
    JsValue tracev = trace ? js_value_from_cell(&trace->gc) : js_undefined(); /* only to root it */
    JsValue result = js_undefined(), p = js_undefined(), kept = orig, thunk = js_undefined();
    JsValue *roots[] = {&bound, &orig, &tracev, &result, &p, &kept, &thunk};
    const int nroots = (int)(sizeof roots / sizeof roots[0]);
    for (int i = 0; i < nroots; i++)
        js_gc_protect(vm, roots[i]);
    JsObject *pair = js_value_object(bound);
    bool ok = js_call(ctx, pair->elems[0], js_undefined(), NULL, 0, &result);
    if (!ok) {
        *r = result; /* onFinally threw: that takes the settlement's place */
    } else if (!is_objectish(result) && is_promise_ctor(ctx, pair->elems[1]) &&
               js_intact(ctx, JS_INTACT_PROMISE_THEN) && js_intact(ctx, JS_INTACT_PROMISE_CONSTRUCTOR)) {
        /* Nothing of a script's is in the steps below: a new promise fulfilled with RESULT, and
         * its then(thunk), a promise the thunk's job settles. That promise and that job are
         * made here without the rest. */
        JsPromise *np = js_promise_alloc(ctx);
        if (np)
            p = js_value_from_cell(&np->gc);
        JsJob *job = np ? job_new(vm) : NULL;
        if (!job) {
            ok = nthrow(ctx, r, "out of memory");
        } else {
            job->kind = RX_THEN; /* with no handler: the settlement passes to its promise */
            job->fulfilled = !rejected;
            job->value = orig;
            job->trace = trace;
            job->result = np;
            job_enqueue(vm, job);
            *r = p;
        }
    } else if (!(ok = promise_resolve(ctx, pair->elems[1], result, &p))) {
        *r = p;
    } else {
        bool have = true;
        if (rejected) {
            /* the reason and its trace, as a rejected promise of the machinery's own */
            JsPromise *k = js_promise_alloc(ctx);
            have = k != NULL;
            if (k) {
                k->handled = true;
                k->trace = trace;
                ctx->raise_fresh = false; /* nothing is being raised: the trace is the one given */
                js_promise_reject(ctx, k, orig);
                kept = js_value_from_cell(&k->gc);
            }
        }
        if (have)
            thunk = js_bound_native_new(ctx, rejected ? finally_reason_rethrow : finally_value_passthrough, kept);
        if (!js_is_function(thunk))
            ok = nthrow(ctx, r, "out of memory");
        else
            ok = invoke_then(ctx, p, &thunk, 1, r);
    }
    for (int i = nroots; i-- > 0;)
        js_gc_unprotect(vm, roots[i]);
    return ok;
}

static bool finally_on_fulfill(JsContext *ctx, JsValue bound, JsValue tv,
                               const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return finally_step(ctx, bound, ARG(0), false, r);
}

static bool finally_on_reject(JsContext *ctx, JsValue bound, JsValue tv,
                              const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return finally_step(ctx, bound, ARG(0), true, r);
}

static bool pm_finally(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    JsVm *vm = ctx->vm;
    if (!is_objectish(tv))
        return nthrow(ctx, r, "TypeError: Promise.prototype.finally called on non-object");
    JsValue c = promise_ctor_value(ctx);
    if (!js_is_promise(tv) || !promise_plain(ctx, js_value_promise(tv))) {
        JsValue sc = js_undefined();
        int sp = js_species(ctx, tv, ctx->species_ctors[JS_SPECIES_PROMISE], &sc);
        if (sp < 0)
            return (*r = sc), false;
        if (sp > 0)
            c = sc;
    }
    JsValue onf = ARG(0), a[2] = {onf, onf};
    if (!js_is_function(onf))
        return invoke_then(ctx, tv, a, 2, r);
    js_gc_protect(vm, &c);
    JsObject *pair = js_array_new_cell(ctx, 2);
    JsValue pairv = pair ? js_value_from_cell(&pair->gc) : js_undefined();
    if (pair) {
        pair->elems[0] = onf;
        pair->elems[1] = c;
        pair->elem_count = 2;
    }
    js_gc_protect(vm, &pairv);
    a[0] = pair ? js_bound_native_new(ctx, finally_on_fulfill, pairv) : js_undefined();
    js_gc_protect(vm, &a[0]);
    a[1] = js_is_function(a[0]) ? js_bound_native_new(ctx, finally_on_reject, pairv) : js_undefined();
    js_gc_protect(vm, &a[1]);
    bool ok;
    if (!js_is_function(a[1])) {
        ok = nthrow(ctx, r, "out of memory");
    } else {
        ((JsNative *)js_value_cell(a[0]))->length = 1;
        ((JsNative *)js_value_cell(a[1]))->length = 1;
        ok = invoke_then(ctx, tv, a, 2, r);
    }
    js_gc_unprotect(vm, &a[1]);
    js_gc_unprotect(vm, &a[0]);
    js_gc_unprotect(vm, &pairv);
    js_gc_unprotect(vm, &c);
    return ok;
}

/* ---- statics ---- */

/* Each takes its constructor from `this` (Promise.resolve.call(C, v)): the promise it answers
 * with is one C made, through NewPromiseCapability. */

static bool ps_resolve(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (!is_objectish(tv))
        return nthrow(ctx, r, "TypeError: PromiseResolve called on non-object");
    return promise_resolve(ctx, tv, ARG(0), r);
}

static bool ps_reject(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (!is_objectish(tv))
        return nthrow(ctx, r, "TypeError: PromiseReject called on non-object");
    if (is_promise_ctor(ctx, tv)) {
        JsPromise *p = js_promise_alloc(ctx);
        if (!p)
            return nthrow(ctx, r, "out of memory");
        js_promise_reject(ctx, p, ARG(0));
        *r = js_value_from_cell(&p->gc);
        return true;
    }
    PromiseCap cap;
    if (!new_capability(ctx, tv, &cap, r))
        return false;
    cap_protect(ctx->vm, &cap);
    JsValue reason = ARG(0), ignored;
    bool ok = js_call(ctx, cap.reject, js_undefined(), &reason, 1, &ignored);
    cap_unprotect(ctx->vm, &cap);
    *r = ok ? cap.promise : ignored;
    return ok;
}

/*
 * Promise.all, allSettled, any and race (27.2.4.1-3, 27.2.4.5). The constructor's `resolve` is
 * read once and called on each value the iterable gives, and `then` is called on what it
 * returned, one value at a time: next, resolve, then, next, ... A throw from any of them closes
 * the iterator and rejects the promise that is returned.
 *
 * All but race keep [values, remaining, resolve, reject] for the call (CS_*), and give each
 * element's `then` functions bound to [state, index, called] (EL_*) that run once.
 */
enum { CM_ALL, CM_SETTLED, CM_ANY, CM_RACE };
enum { CS_VALUES, CS_REMAINING, CS_RESOLVE, CS_REJECT, CS_COUNT };
enum { EL_STATE, EL_INDEX, EL_CALLED, EL_COUNT };
enum { EK_VALUE, EK_FULFILLED, EK_REJECTED, EK_REASON };

/* One fewer to wait for; at none the capability's resolve gets the values (Promise.any: its
 * reject an AggregateError of the reasons), and what that returns or throws is the answer */
static bool combinator_done(JsContext *ctx, JsValue statev, bool any, JsValue *r) {
    JsObject *st = js_value_object(statev);
    double rem = js_get_number(st->elems[CS_REMAINING]) - 1;
    st->elems[CS_REMAINING] = js_number(rem);
    *r = js_undefined();
    if (rem != 0)
        return true;
    JsValue v = st->elems[CS_VALUES];
    js_gc_protect(ctx->vm, &statev);
    js_gc_protect(ctx->vm, &v);
    bool ok = true;
    if (any) {
        v = js_aggregate_error_new(ctx, js_value_object(v), "All promises were rejected");
        if (!js_is_object(v))
            ok = nthrow(ctx, r, "out of memory");
    }
    if (ok)
        ok = js_call(ctx, js_value_object(statev)->elems[any ? CS_REJECT : CS_RESOLVE], js_undefined(), &v, 1, r);
    js_gc_unprotect(ctx->vm, &v);
    js_gc_unprotect(ctx->vm, &statev);
    return ok;
}

static bool el_settle(JsContext *ctx, JsValue bound, const JsValue *args, int argc, JsValue *r, int kind) {
    JsObject *el = js_value_object(bound);
    *r = js_undefined();
    if (!js_is_undefined(el->elems[EL_CALLED]))
        return true; /* [[AlreadyCalled]] */
    el->elems[EL_CALLED] = js_number(1);
    JsValue statev = el->elems[EL_STATE], v = ARG(0);
    uint32_t idx = (uint32_t)js_get_number(el->elems[EL_INDEX]);
    js_gc_protect(ctx->vm, &statev);
    js_gc_protect(ctx->vm, &v);
    bool ok = true;
    if (kind == EK_FULFILLED || kind == EK_REJECTED) {
        /* allSettled's { status, value } or { status, reason } */
        JsValue ov = js_object_new(ctx);
        js_gc_protect(ctx->vm, &ov);
        JsString *status = js_is_object(ov) ? js_ascii_cell(ctx->vm, kind == EK_FULFILLED ? "fulfilled" : "rejected") : NULL;
        ok = status && js_object_set_ascii(ctx, js_value_object(ov), "status", js_value_from_cell(&status->gc)) &&
             js_object_set_ascii(ctx, js_value_object(ov), kind == EK_FULFILLED ? "value" : "reason", v);
        js_gc_unprotect(ctx->vm, &ov);
        v = ov;
    }
    if (ok) {
        js_value_object(js_value_object(statev)->elems[CS_VALUES])->elems[idx] = v;
        ok = combinator_done(ctx, statev, kind == EK_REASON, r);
    } else {
        nthrow(ctx, r, "out of memory");
    }
    js_gc_unprotect(ctx->vm, &v);
    js_gc_unprotect(ctx->vm, &statev);
    return ok;
}

static bool el_value(JsContext *ctx, JsValue bound, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return el_settle(ctx, bound, args, argc, r, EK_VALUE);
}

static bool el_fulfilled(JsContext *ctx, JsValue bound, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return el_settle(ctx, bound, args, argc, r, EK_FULFILLED);
}

static bool el_rejected(JsContext *ctx, JsValue bound, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return el_settle(ctx, bound, args, argc, r, EK_REJECTED);
}

static bool el_reason(JsContext *ctx, JsValue bound, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    (void)tv;
    return el_settle(ctx, bound, args, argc, r, EK_REASON);
}

static JsValue el_function(JsContext *ctx, JsBoundFn fn, JsValue elv) {
    JsValue f = js_bound_native_new(ctx, fn, elv);
    if (js_is_function(f))
        ((JsNative *)js_value_cell(f))->length = 1;
    return f;
}

static bool combinator(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r, int mode) {
    static const char *const non_object[] = {
        "TypeError: Promise.all called on non-object", "TypeError: Promise.allSettled called on non-object",
        "TypeError: Promise.any called on non-object", "TypeError: Promise.race called on non-object"};
    JsVm *vm = ctx->vm;
    if (!is_objectish(tv))
        return nthrow(ctx, r, non_object[mode]);
    PromiseCap cap;
    if (!new_capability(ctx, tv, &cap, r))
        return false;
    cap_protect(vm, &cap);
    JsValue resolve = js_undefined(), statev = js_undefined(), next = js_undefined(), np = js_undefined(),
            elv = js_undefined(), on_f = js_undefined(), on_r = js_undefined(), err = js_undefined(), ignored;
    JsIterCursor cur = {js_undefined(), JS_ITER_DONE_INDEX};
    JsValue *roots[] = {&resolve, &statev, &next, &np, &elv, &on_f, &on_r, &err, &cur.src};
    const int nroots = (int)(sizeof roots / sizeof roots[0]);
    for (int i = 0; i < nroots; i++)
        js_gc_protect(vm, roots[i]);
    bool failed = false, oom = false, direct = false;

    /* GetPromiseResolve(C) */
    JsString *rk = js_ascii_cell(vm, "resolve");
    if (!rk) {
        oom = true;
    } else if (!js_get_property(ctx, tv, js_value_from_cell(&rk->gc), &resolve)) {
        err = js_take_error(ctx);
        failed = true;
    } else if (!js_is_function(resolve)) {
        err = js_error_fmt(ctx, "TypeError: resolve is not a function"); /* V8 names the property */
        failed = true;
    } else {
        const JsNative *n = (const JsNative *)js_value_cell(resolve);
        direct = n->gc.kind == JS_KIND_NATIVE && !n->is_bound && n->fn == ps_resolve;
    }

    if (!failed && !oom) {
        int open = js_iter_open(ctx, ARG(0), &cur);
        if (open <= 0) {
            cur.src = js_undefined();
            cur.i = JS_ITER_DONE_INDEX;
            if (open < 0) {
                oom = true;
            } else {
                err = js_iter_not_iterable(ctx, ARG(0));
                failed = true;
            }
        }
    }

    JsObject *values = NULL;
    if (!failed && !oom && mode != CM_RACE) {
        JsObject *st = js_array_new_cell(ctx, CS_COUNT);
        if (st) {
            for (int i = 0; i < CS_COUNT; i++)
                st->elems[i] = js_undefined();
            st->elem_count = CS_COUNT;
            st->elems[CS_REMAINING] = js_number(1);
            st->elems[CS_RESOLVE] = cap.resolve;
            st->elems[CS_REJECT] = cap.reject;
            statev = js_value_from_cell(&st->gc);
            values = js_array_new_cell(ctx, 0);
            if (values)
                js_value_object(statev)->elems[CS_VALUES] = js_value_from_cell(&values->gc);
        }
        oom = !values;
    }

    for (uint32_t index = 0; !failed && !oom; index++) {
        int rc = js_iter_next(ctx, &cur, &next);
        if (rc == 0)
            break;
        if (rc < 0) {
            err = next; /* the step threw: the iterator is not closed */
            failed = true;
            break;
        }
        if (values && !js_array_append(vm, values, js_undefined())) {
            oom = true;
            break;
        }
        if (!(direct ? promise_resolve(ctx, tv, next, &np) : js_call(ctx, resolve, tv, &next, 1, &np))) {
            err = np;
            failed = true;
            break;
        }
        on_f = cap.resolve;
        on_r = cap.reject;
        if (mode != CM_RACE) {
            JsObject *el = js_array_new_cell(ctx, EL_COUNT);
            if (el) {
                el->elems[EL_STATE] = statev;
                el->elems[EL_INDEX] = js_number(index);
                el->elems[EL_CALLED] = js_undefined();
                el->elem_count = EL_COUNT;
                elv = js_value_from_cell(&el->gc);
                if (mode == CM_ANY) {
                    on_r = el_function(ctx, el_reason, elv);
                } else {
                    on_f = el_function(ctx, mode == CM_ALL ? el_value : el_fulfilled, elv);
                    if (mode == CM_SETTLED && js_is_function(on_f))
                        on_r = el_function(ctx, el_rejected, elv);
                }
            }
            if (!el || !js_is_function(on_f) || !js_is_function(on_r)) {
                oom = true;
                break;
            }
            JsObject *st = js_value_object(statev);
            st->elems[CS_REMAINING] = js_number(js_get_number(st->elems[CS_REMAINING]) + 1);
        }
        JsValue a[2] = {on_f, on_r};
        if (!invoke_then(ctx, np, a, 2, &ignored)) {
            err = ignored;
            failed = true;
        }
    }

    /* the iterable is listed: the count taken for the listing itself goes */
    if (!failed && !oom && mode != CM_RACE) {
        JsObject *st = js_value_object(statev);
        double rem = js_get_number(st->elems[CS_REMAINING]) - 1;
        st->elems[CS_REMAINING] = js_number(rem);
        if (rem == 0) {
            JsValue v = st->elems[CS_VALUES];
            if (mode == CM_ANY) {
                err = js_aggregate_error_new(ctx, js_value_object(v), "All promises were rejected");
                failed = js_is_object(err);
                oom = !failed;
            } else if (!js_call(ctx, cap.resolve, js_undefined(), &v, 1, &ignored)) {
                err = ignored;
                failed = true;
            }
        }
    }

    /* IfAbruptRejectPromise, after IteratorClose when the iterator was left before its end.
     * Out of memory still stops the script. */
    bool ok = true;
    if (failed && err.bits == js_oom_value(vm).bits)
        oom = true;
    if (oom) {
        ok = nthrow(ctx, r, "out of memory");
    } else if (failed) {
        js_iter_close(ctx, &cur, true, false, &ignored); /* a throw: what return() does is ignored */
        ok = js_call(ctx, cap.reject, js_undefined(), &err, 1, &ignored);
        if (!ok)
            *r = ignored;
    }
    for (int i = nroots; i-- > 0;)
        js_gc_unprotect(vm, roots[i]);
    cap_unprotect(vm, &cap);
    if (ok)
        *r = cap.promise;
    return ok;
}

static bool ps_all(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    return combinator(ctx, tv, args, argc, r, CM_ALL);
}

static bool ps_allSettled(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    return combinator(ctx, tv, args, argc, r, CM_SETTLED);
}

static bool ps_any(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    return combinator(ctx, tv, args, argc, r, CM_ANY);
}

static bool ps_race(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    return combinator(ctx, tv, args, argc, r, CM_RACE);
}

/*
 * NewPromiseResolveThenableJob's work (27.2.2.2): thenable.then(resolve, reject) for the job's
 * promise; a throw rejects it unless it was resolved first. A promise that was plain when it
 * was given (no `then` to read: the job's is undefined) and still is has Promise.prototype's
 * then, whose result nothing sees: the job's promise takes the settlement as a reaction.
 */
static void run_thenable_job(JsContext *ctx, JsJob *job) {
    bool native_then = js_is_undefined(job->on_fulfilled);
    if (native_then && promise_plain(ctx, js_value_promise(job->value))) {
        JsReaction *rx = reaction_alloc(ctx);
        if (!rx) {
            js_promise_reject(ctx, job->result, js_oom_value(ctx->vm));
            return;
        }
        rx->kind = RX_THEN;
        rx->on_fulfilled = rx->on_rejected = js_undefined();
        rx->result = job->result;
        add_reaction(ctx, js_value_promise(job->value), rx);
        return;
    }
    JsValue pv = js_value_from_cell(&job->result->gc), resolve, reject, res;
    if (!resolving_functions(ctx, pv, &resolve, &reject)) {
        js_promise_reject(ctx, job->result, js_oom_value(ctx->vm));
        return;
    }
    js_gc_protect(ctx->vm, &resolve);
    js_gc_protect(ctx->vm, &reject);
    JsValue args[2] = {resolve, reject};
    bool ok = native_then ? promise_then_method(ctx, job->value, resolve, reject, &res)
                          : js_call(ctx, job->on_fulfilled, job->value, args, 2, &res);
    if (!ok) {
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
    (void)args;
    (void)argc;
    if (!is_objectish(tv))
        return nthrow(ctx, r, "TypeError: Promise.withResolvers called on non-object");
    PromiseCap cap;
    if (!new_capability(ctx, tv, &cap, r))
        return false;
    cap_protect(ctx->vm, &cap);
    JsValue o = js_object_new(ctx);
    js_gc_protect(ctx->vm, &o); /* the property writes allocate */
    bool ok = js_is_object(o);
    if (ok) {
        JsObject *obj = js_value_object(o);
        ok = js_object_set_ascii(ctx, obj, "promise", cap.promise) &&
             js_object_set_ascii(ctx, obj, "resolve", cap.resolve) && js_object_set_ascii(ctx, obj, "reject", cap.reject);
    }
    js_gc_unprotect(ctx->vm, &o);
    cap_unprotect(ctx->vm, &cap);
    if (!ok)
        return nthrow(ctx, r, "out of memory");
    *r = o;
    return true;
}

/* Promise.try(f, ...args): a promise resolved with f(...args), or rejected with what it threw
 * (a non-function f is a rejection too, as V8) */
static bool ps_try(JsContext *ctx, JsValue tv, const JsValue *args, int argc, JsValue *r) {
    if (!is_objectish(tv))
        return nthrow(ctx, r, "TypeError: Promise.try called on non-object");
    const JsValue *fargs = argc > 1 ? args + 1 : NULL;
    int fargc = argc > 1 ? argc - 1 : 0;
    JsValue res;
    if (is_promise_ctor(ctx, tv)) {
        /* settled here, not through the resolving functions: a rejection takes the trace of
         * the raise that is still fresh */
        JsPromise *p = js_promise_alloc(ctx);
        if (!p)
            return nthrow(ctx, r, "out of memory");
        JsValue pv = js_value_from_cell(&p->gc);
        js_gc_protect(ctx->vm, &pv);
        if (!js_is_function(ARG(0)))
            js_promise_reject(ctx, p, js_error_fmt(ctx, "TypeError: %t is not a function", ARG(0)));
        else if (!js_call(ctx, ARG(0), js_undefined(), fargs, fargc, &res))
            js_promise_reject(ctx, p, res);
        else
            js_promise_resolve_with(ctx, p, res);
        js_gc_unprotect(ctx->vm, &pv);
        *r = pv;
        return true;
    }
    PromiseCap cap;
    if (!new_capability(ctx, tv, &cap, r))
        return false;
    cap_protect(ctx->vm, &cap);
    bool called = js_is_function(ARG(0));
    if (!called)
        res = js_error_fmt(ctx, "TypeError: %t is not a function", ARG(0));
    else
        called = js_call(ctx, ARG(0), js_undefined(), fargs, fargc, &res);
    js_gc_protect(ctx->vm, &res);
    JsValue ignored;
    bool ok = js_call(ctx, called ? cap.resolve : cap.reject, js_undefined(), &res, 1, &ignored);
    js_gc_unprotect(ctx->vm, &res);
    cap_unprotect(ctx->vm, &cap);
    *r = ok ? cap.promise : ignored;
    return ok;
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
    /* while both are the engine's, a promise with nothing of its own is settled and chained
     * without looking either up (promise_plain) */
    if (!js_intact_watch(ctx, JS_INTACT_PROMISE_THEN, ctx->promise_methods, "then") ||
        !js_intact_watch(ctx, JS_INTACT_PROMISE_CONSTRUCTOR, ctx->promise_methods, "constructor"))
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
    JsValue pv = js_undefined();
    js_gc_protect(ctx->vm, &pv);
    /* PromiseResolve(%Promise%, V): a thenable is adopted, a promise waited on as it is */
    if (!promise_resolve(ctx, promise_ctor_value(ctx), v, &pv)) {
        fa_fail(ctx, st, pv, true);
    } else {
        JsValue f = js_bound_native_new(ctx, on_f, st);
        js_gc_protect(ctx->vm, &f);
        JsValue rj = js_is_function(f) ? js_bound_native_new(ctx, on_r, st) : js_undefined();
        js_gc_protect(ctx->vm, &rj);
        if (!js_is_function(rj) || !js_promise_then(ctx, js_value_promise(pv), f, rj))
            fa_fail(ctx, st, js_error_from_ascii(ctx, "out of memory"), true);
        js_gc_unprotect(ctx->vm, &rj);
        js_gc_unprotect(ctx->vm, &f);
    }
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
