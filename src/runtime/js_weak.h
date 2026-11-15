/*
 * WeakMap, WeakSet, WeakRef and FinalizationRegistry.
 *
 * Each is a JS_KIND_OBJECT cell (obj_kind JS_OBJ_WEAKMAP, _WEAKSET, _WEAKREF, _FINREG) on the
 * VM's weak list (JsVm.weak_list). The collector does not trace what they hold weakly: a weak
 * map's or set's keys, a weak ref's target, a registry's targets and unregister tokens. After
 * marking it runs js_weak_ephemerons, which marks a live weak map's value whose key is marked,
 * until nothing more is marked (a value stays alive as long as its key and the map do, and no
 * longer: an ephemeron). Before the sweep js_weak_prune drops the entries whose keys are about
 * to be freed, empties weak refs whose targets are, and moves a registry's dead targets' held
 * values to its pending list. Nothing allocates during collection: the cleanup callbacks run
 * later, from js_run_jobs once the microtask queue is empty (V8 runs them as a task of their
 * own), through js_weak_run_cleanups.
 *
 * A weak ref keeps its target alive until the end of the current job, as V8's WeakRef does
 * (KeepDuringJob): new WeakRef(t) and deref() add the target to JsContext.weak_kept, which
 * js_run_jobs clears when the queue is empty.
 */
#ifndef JS_WEAK_H
#define JS_WEAK_H

#include "lamassu_internal.h"

typedef struct JsWeakObj {
    JsObject obj;
    struct JsWeakObj *weak_next; /* JsVm.weak_list */
    /* WEAKMAP/WEAKSET: keys (values for a map), with a hash index. WEAKREF: its
     * target is `target`. FINREG: per registration, keys[i] the target,
     * values[i] the held value, tokens[i] the unregister token (undefined for none); pending
     * holds the held values of targets that have died, waiting for the callback. */
    JsValue *keys, *values, *tokens;
    uint32_t count, cap;
    int32_t *index;
    uint32_t index_cap;
    JsValue callback; /* FINREG: the cleanup callback */
    JsValue target;   /* WEAKREF: the target, undefined once collected */
    JsValue *pending, *pending_tokens;
    uint32_t pending_count, pending_cap;
} JsWeakObj;

void   js_weak_mark(JsVm *vm, JsObject *o);     /* what it holds strongly */
size_t js_weak_release(JsVm *vm, JsObject *o);
/* Marks the values of live weak maps whose keys are marked; true when it marked any (the
 * collector traces, then calls it again) */
bool   js_weak_ephemerons(JsVm *vm);
void   js_weak_prune(JsVm *vm);
/* Runs pending FinalizationRegistry callbacks. True when it ran any. */
bool   js_weak_run_cleanups(JsContext *ctx);
bool   js_weak_builtins_init(JsContext *ctx);
/* JsContext.weak_kept: marked strongly as a root; emptied when the job queue is */
void   js_weak_mark_kept(JsVm *vm, JsObject *kept);
void   js_weak_clear_kept(JsContext *ctx);

#endif /* JS_WEAK_H */
