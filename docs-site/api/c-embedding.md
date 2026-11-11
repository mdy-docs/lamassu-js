# C embedding API

The public surface is two headers — this page is a guided tour of them, not a
replacement. Everything hangs off an explicit `JsVm` or `JsContext` handle
(C's version of `this`); there is no static mutable state anywhere in the
core, so multiple engines can coexist in one process.

## Two headers, two archives

`include/lamassu.h` is the **runtime**: values, objects, strings, promises,
modules, the GC, and loading precompiled bytecode. `include/lamassu_compile.h`
is the **frontend**: everything that turns source text into code — the two
`js_compile_module` entry points, bytecode production, and
`js_enable_source_modules`.

They ship as separate archives (`liblamassu_runtime.a`,
`liblamassu_frontend.a`), and the split is the point rather than a side
effect. A process that links only the runtime *cannot* compile
attacker-supplied source, because the code that would do it is not in the
binary — a property of the link rather than a policy someone has to remember.
`make check-runtime-only` links `tools/run_bc.c` against the runtime alone to
prove the split holds; that file is also the skeleton of what such a process
looks like. See [Running untrusted code](/security) for when this is worth
doing.

Link order is always frontend-then-runtime: the frontend calls into the
runtime, never the reverse.

## GC contract

Read this before anything else: creating a string/object/context, or
calling `js_object_set`, is a **GC safe point**. Every `JsValue` you hold
across a safe point — including arguments passed in — must be reachable
from a slot registered with `js_gc_protect`, or from something that already
is (e.g. a protected object it's stored into). Build with
`JsVmConfig.gc_stress = true` to shake out violations during development;
it collects at every safe point instead of only when a threshold is hit.

## Core types

```c
typedef struct JsVm       JsVm;       // owns heap, GC, atom table; one per engine instance
typedef struct JsContext  JsContext;  // a realm: globals, module cache, microtask queue
typedef uint64_t          JsValue;    // NaN-boxed; doubles direct, pointers in the NaN payload
typedef struct JsObject   JsObject;   // property map; kind = plain | array | error | ...
typedef struct JsFunction JsFunction; // compiled bytecode chunk + upvalue descriptors
typedef struct JsClosure  JsClosure;  // JsFunction* + captured upvalues
typedef struct JsModule   JsModule;   // exports, dependencies, link/eval status
typedef struct JsFiber    JsFiber;    // heap-allocated call frames + value stack; suspendable
```

`JsFiber` is why `await` is cheap: the interpreter never uses the C stack
for JS calls, so suspending a script mid-`await` is just "stop running this
fiber," not a C-stack save/restore.

## VM lifecycle

```c
JsVm *js_vm_new(const JsVmConfig *cfg); // cfg may be NULL for defaults
void  js_vm_free(JsVm *vm);
size_t js_vm_allocated_bytes(const JsVm *vm);

JsContext *js_context_new(JsVm *vm);
void       js_context_free(JsContext *ctx);
JsValue    js_context_globals(JsContext *ctx);
JsVm      *js_context_vm(JsContext *ctx);
```

A `JsNativeFn` is handed only the context, but most of the value and GC API is
VM-scoped — `js_context_vm` is how a native gets from one to the other without
threading a `JsVm *` through its own userdata.

```c
typedef struct JsVmConfig {
    JsReallocFn realloc_fn; // NULL: use libc realloc
    void *alloc_ud;
    size_t gc_threshold;    // live bytes before first auto-collect; 0 = default
    size_t heap_limit;      // hard cap on live bytes; 0 = unlimited
    uint64_t rng_seed;      // Math.random seed; 0 = fixed default (deterministic)
    bool gc_stress;         // collect at every safe point (for tests)
} JsVmConfig;
```

`heap_limit` and `js_context_set_fuel` (below) are the two knobs for running
genuinely untrusted scripts. Both default to off, so an embedding that leaves
them alone has no bound on anything — see [Running untrusted
code](/security) for how to set them and what they don't cover.

`heap_limit` is checked before each allocation, so the accounted total never
exceeds it, and it covers bulk allocations (property maps, array element
buffers, fiber stacks, the job queue) rather than only GC cell headers. It
bounds *accounted* bytes, though, not process RSS: allocator metadata and
size-class rounding are invisible to it. A host that needs a true RSS ceiling
should enforce it in the `JsReallocFn` it supplies, which is the only place
with the real numbers.

One `JsVm` can host multiple `JsContext`s (independent realms sharing one
heap/GC/atom table); a single context is enough for most embedders.

### Stopping a run from outside

```c
void js_vm_interrupt(JsVm *vm);
void js_vm_clear_interrupt(JsVm *vm);
bool js_vm_interrupted(const JsVm *vm);
```

`js_vm_interrupt` is the one function here that may be called **while the VM
is running**, including from another thread or a signal handler: it sets a
flag, touches nothing else, and needs no lock. It exists because fuel counts
instructions and a host usually wants to bound *time* — arm a timer, call
this, and the run stops.

The stop surfaces as a thrown `Error: execution interrupted` that re-arms
itself, so guest code cannot catch it and carry on; the run unwinds and the
host gets control back. The flag is **not** cleared automatically — call
`js_vm_clear_interrupt` before using the VM again, or every subsequent
instruction keeps throwing.

::: warning Latency is bounded by the dispatch loop, not by wall-clock
The flag is read between instructions, so a builtin already running (a large
sort, a big string build, one regex match) finishes its current step first.
Each of those steps is individually bounded, so this is a latency question
rather than an unbounded one — but if the deadline is hard rather than
best-effort, keep an outer kill as well. Under wasm that is the runtime's
epoch interruption.
:::

```c
bool js_vm_out_of_memory(const JsVm *vm);
```

True once the VM has hit an allocation failure it could not report cleanly —
specifically one that forced the collector off (see [GC](#gc) below). The VM
stays memory-safe and keeps refusing work with out-of-memory errors, but it
can no longer reclaim anything, so a host should finish the current call and
discard it rather than keep serving from it.

## Values

`JsValue` is a NaN-boxed 64-bit value — doubles stored as their own bit
pattern, pointers/tags in the NaN payload (WASM32 pointers fit comfortably).
Construct and inspect primitives directly, no allocation involved:

```c
JsValue js_undefined(void);
JsValue js_null(void);
JsValue js_bool(bool b);
JsValue js_number(double d);
double  js_get_number(JsValue v);

bool js_is_number(JsValue v);
bool js_is_undefined(JsValue v);
bool js_is_null(JsValue v);
bool js_is_bool(JsValue v);
bool js_get_bool(JsValue v);
bool js_is_string(JsValue v);
bool js_is_object(JsValue v);
bool js_is_function(JsValue v);
bool js_is_promise(JsValue v);

// identity (same bits), not === — two equal-content heap strings differ
bool js_same_value(JsValue a, JsValue b);
```

## Strings

Strings are **UTF-16 code units**, always carried as a `uint16_t*` +
code-unit length — never NUL-terminated C strings, in the core or at the
embedding boundary. This matches JS `.length`/indexing/slice semantics
exactly. Convert to/from UTF-8 at your own outer edge if your host needs
it (see `tools/lamassu.c` for a small, dependency-free converter).

```c
JsValue js_string_new(JsVm *vm, const uint16_t *units, size_t len); // undefined on OOM
JsValue js_atom(JsVm *vm, const uint16_t *units, size_t len);       // interned; undefined on OOM
const uint16_t *js_string_units(JsValue str, size_t *len);          // NULL if not a string
size_t  js_string_length(JsValue str);
bool    js_string_equals(JsValue a, JsValue b);                     // content equality
```

## Objects

```c
JsValue js_object_new(JsContext *ctx);                                    // undefined on OOM
JsValue js_object_get(JsVm *vm, JsValue obj, JsValue key);                // undefined if absent
bool    js_object_set(JsVm *vm, JsValue obj, JsValue key, JsValue value); // false on OOM/bad args
bool    js_object_delete(JsVm *vm, JsValue obj, JsValue key);             // true if removed
size_t  js_object_size(JsValue obj);
```

`js_object_new` takes the context, not just the VM, because
`[[Prototype]]` is a realm concept: the new object's `[[Prototype]]` is
`ctx->object_proto`, the same real, script-visible `Object.prototype`
(`hasOwnProperty`/`toString`/`valueOf`, and every other built-in prototype
chains up to it too — see [Built-ins → Object](/guide/builtins#object)) a
guest `{}` literal gets, reachable via `Object.getPrototypeOf`. A native
constructing an object this way is indistinguishable from guest code doing
it. `get`/`set`/`delete`/`size` stay VM-scoped — they only ever touch own
properties (no `[[Prototype]]` walk), so they need no realm context.

## Compiling & running

Compilation lives in `lamassu_compile.h`; running lives in `lamassu.h`.

```c
JsValue js_compile_module(JsContext *ctx, const uint16_t *src, size_t len,
                          const char **err_msg, uint32_t *err_pos);
```

Compiles UTF-16 source as a strict-mode module body. Returns a function
value (root it per the GC contract), or `undefined` on error with
`*err_msg` and `*err_pos` (a source offset) set. The message is worded as V8
words it (`Unexpected token ';'`, `Identifier 'x' has already been declared`),
without the `SyntaxError: ` prefix; it is UTF-8, since a name in it may be any
text, and it stays valid until the next compile on the same VM, so copy it to
keep it. The same holds for every compile entry point.
Source that uses top-level `import`/`export` has to go through the module
pipeline instead, and says so by failing with `*err_msg` equal to
`JS_ERR_NEEDS_MODULE_LOADER` — compare against that constant rather than
searching the message for a substring.

```c
JsValue js_compile_module_repl(JsContext *ctx, const uint16_t *src, size_t len,
                               const char **err_msg, uint32_t *err_pos);
```

Same, but top-level `let`/`const`/`function` declarations become
persistent globals on the context — successive compiles in the same
context share state, i.e. a REPL session. This is what the npm package's
`eval` uses.

```c
bool js_check_module(JsContext *ctx, const uint16_t *source, size_t source_len, bool entry,
                     const char **err_msg, uint32_t *err_pos);
```

A syntax check for an editor: compiles `source` exactly as the module loader
would (top-level `import`/`export` allowed, and top-level `return` too when
`entry`, as for the root of `js_eval_entry`) and throws the result away.
Nothing is registered, resolved, linked or run, and imports are not loaded, so
it is cheap enough to run on every pause in typing. Returns `true` when the
source compiles, otherwise `false` with `*err_msg` and `*err_pos` set as for
`js_compile_module`; `js_source_line_col` turns the offset into a line and
column.

```c
JsValue js_run_module(JsContext *ctx, JsValue fn);
```

Runs a compiled module function and returns its **completion promise**:
fulfilled with the completion value (the value of the last expression
statement), rejected with the thrown error (`js_context_error_pos()` gives its
source offset), or still pending if top-level `await` suspended on a promise
the host hasn't settled yet. In that last case, protect the returned promise,
settle the host promises it is waiting on, call `js_run_jobs()`, and observe
the outcome with `js_promise_state` / `js_promise_result`.
Engine-raised errors are `Error` objects — read `name` / `message` with
`js_object_get`, or `js_to_string` for `"name: message"`; only an allocation
failure degrades the value to a bare string.

```c
typedef enum JsErrorCause {
    JS_CAUSE_GUEST = 0,     // a guest throw, or an ordinary engine TypeError etc.
    JS_CAUSE_BUDGET = 1,    // the js_context_set_fuel budget ran out
    JS_CAUSE_INTERRUPT = 2, // js_vm_interrupt stopped the run
    JS_CAUSE_OOM = 3        // heap_limit or the allocator refused
} JsErrorCause;
JsErrorCause js_error_cause(JsContext *ctx, JsValue error);
```

The three stops the engine imposes carry a structured cause, so a host can
route them (retry, 503, discard the VM) without parsing message text. It
works on whatever a failed `js_call`, a rejected promise, or a `catch`
handler holds — including the bare `"out of memory"` string the engine
falls back to when it cannot allocate an `Error`. A guest-built error that
merely says the same words is `JS_CAUSE_GUEST`.

```c
bool js_call(JsContext *ctx, JsValue fn, JsValue this_val, const JsValue *args,
             int argc, JsValue *result);
```

Calls a function value (a script closure or a native) on a fresh fiber.
Returns `true` with `*result` = the return value, or `false` with `*result` =
the error value.

```c
bool js_register_native(JsContext *ctx, const uint16_t *name, size_t name_len,
                        JsNativeFn fn, void *userdata);
```

Defines a global native function.

```c
typedef bool (*JsNativeFn)(JsContext *ctx, JsValue this_val, const JsValue *args,
                           int argc, JsValue *result);
```

`args` points into the fiber's value stack — valid only for the duration of
the call; copy anything you need to keep. Return `true` with `*result` set,
or `false` with `*result` = the value to throw.

```c
uint32_t js_context_error_pos(const JsContext *ctx);
void     js_context_set_fuel(JsContext *ctx, uint64_t fuel); // 0 = unlimited
```

### Native function values and errors

```c
JsValue js_native_new(JsContext *ctx, const char *name, JsNativeFn fn, void *userdata);
void   *js_native_userdata(JsContext *ctx);

typedef enum JsErrorKind { JS_ERROR_PLAIN, JS_ERROR_TYPE, JS_ERROR_RANGE, JS_ERROR_REFERENCE, JS_ERROR_SYNTAX } JsErrorKind;
JsValue js_error_new_ascii(JsContext *ctx, JsErrorKind kind, const char *message);
JsValue js_error_new_utf16(JsContext *ctx, JsErrorKind kind, const uint16_t *message, size_t len);
```

`js_register_native` defines a global. `js_native_new` gives the native as a *value*, to put
wherever a function may go: a property of a namespace object, a method on a prototype, an array
element, a callback. Inside the native, `js_native_userdata` returns the `userdata` that was passed
when it was created (nested calls through `js_call` see their own and the outer value is restored),
so one C function can serve many natives.

A native that fails returns `false` with `*result` set to the value to throw. The constructors make
a real `TypeError`, `RangeError`, … with `name` and `message` set, catchable by guest code and
reported by `js_error_cause` as `JS_CAUSE_GUEST`. Messages may be any length.

### Host objects and finalizers

```c
typedef struct JsHostClass {
    const char *name;
    void (*finalize)(void *ptr, void *userdata);
    void *userdata;
} JsHostClass;

JsValue js_host_object_new(JsContext *ctx, const JsHostClass *cls, void *ptr, size_t external_bytes);
void   *js_host_object_ptr(JsValue v, const JsHostClass *cls);
bool    js_host_object_set_external(JsVm *vm, JsValue v, const JsHostClass *cls, size_t external_bytes);
size_t  js_gc_external_bytes(const JsVm *vm);
```

A host object gives guest code something the host owns: a mesh, a file, a native handle. To the
script it is an ordinary object (`typeof` is `"object"`, it has `Object.prototype` and can take
properties); the host gets its pointer back with `js_host_object_ptr`, which checks the class by
address, so a script cannot pass one kind of resource, or a plain object, where another is expected.
When the collector frees the object, or the VM is freed, the class's `finalize` runs once with the
pointer: the resource lives exactly as long as the script can reach it.

`finalize` runs inside a collection, so it must not call back into the engine: no allocation, no
`js_*` calls, no rooting. Releasing the resource is all it is for. If `js_host_object_new` fails it
returns `undefined` and the caller still owns the pointer.

`external_bytes` is the host memory the object keeps alive. It does not count against the heap
limit, but it counts toward the collection threshold, so a script that makes many objects small to
the engine and large to the host collects as often as that memory warrants. A host that only learns
the real size later (a lazily computed result) updates it with `js_host_object_set_external`.

### Evicting modules

```c
size_t  js_context_module_count(const JsContext *ctx);
JsValue js_context_module_specifier(const JsContext *ctx, size_t index);
bool    js_context_evict_module(JsContext *ctx, const uint16_t *specifier, size_t spec_len);
```

Loaded modules stay in the context's registry, so a second `js_eval_module` of the same
specifier returns the cached graph. A host whose sources change between runs (an editor
rebuilding the user's project) evicts the changed module first; every module that imports it goes
with it, transitively, because their bindings point into it, and the next evaluation loads them
afresh while unrelated modules (a library prelude) stay compiled. Modules still loading, linking or
evaluating cannot be evicted. Closures from the old graph keep working — a function keeps its
module alive — so evicting is safe at any quiet point.

### Stack traces

```c
typedef struct JsStackFrame {
    JsValue  specifier;   // the module's canonical specifier, or undefined outside the module pipeline
    JsValue  function;    // the function's name, or undefined (anonymous, module body)
    uint32_t pos;         // source offset of the instruction in flight in that frame
} JsStackFrame;
#define JS_TRACE_MAX 32

size_t js_context_stack(JsContext *ctx, JsStackFrame *out, size_t max);
size_t js_context_error_stack(const JsContext *ctx, JsStackFrame *out, size_t max);
void   js_source_line_col(const uint16_t *src, size_t len, uint32_t pos, uint32_t *line, uint32_t *col);
```

`js_context_error_pos` is an offset with no module attached, and a throw inside
library code points at the library. The trace fixes both. It is captured the
moment an exception starts unwinding, before any frame is popped, and includes
the fibers a re-entering native was called from, so a host can walk outward to
the first frame in the user's own file:

```c
JsStackFrame fr[JS_TRACE_MAX];
size_t n = js_context_error_stack(ctx, fr, JS_TRACE_MAX);
for (size_t i = 0; i < n; i++) {
    if (!is_user_file(fr[i].specifier)) continue;   // skip the prelude / library frames
    uint32_t line, col;
    js_source_line_col(source_of(fr[i].specifier), source_len, fr[i].pos, &line, &col);
    report(fr[i].specifier, line, col);
    break;
}
```

`js_context_stack` reads the frames of the fiber currently running, so a
native can learn its call site (frame 0 is the caller) — the way to give a
`log()` or `warn()` the script line it was called from. A compile error in a
module graph produces a one-frame trace naming the module and the offset; a
loader rejection has no frames. The strings in a frame are engine values and
stay valid while the module and function live; copy them to keep them.

`js_context_set_fuel` arms an execution budget, charged **one unit per
dispatched bytecode instruction** — the CPU-bound half of running untrusted
scripts (`heap_limit` in `JsVmConfig` is the memory half). Three properties of
it matter more than the count:

- **It covers a turn, not a call.** One top-level run (`js_run_module`,
  `js_call`, `js_eval_module`) *plus* the microtask drain it feeds, including
  every job `js_run_jobs` runs.
- **It is shared, not per-fiber and not per-job.** Nested calls inherit the
  remainder, and so does every queued microtask, so no amount of re-queueing
  mints a fresh budget.
- **Exhaustion is not catchable.** It throws a `RangeError` that re-arms
  itself: a `try`/`catch` around the loop swallows one throw and the next
  instruction throws again, until the fiber unwinds.

Because it arms *one* turn, it has to be re-armed per turn. A server that
calls it once at startup has given the whole process a single budget, not one
per request.

::: warning Fuel counts instructions, not time
A single instruction can enter a builtin that runs a while — `sort` is O(n²)
in element moves and charges nothing for them, string concatenation can copy
and rehash a large string, and the regex step budget is per call, so a loop of
cheap-looking `re.test(s)` calls multiplies it. All of those are individually
bounded and none are memory-unsafe, but the aggregate is not bounded in *time*
by fuel alone. To bound wall-clock, arm a timer and call
[`js_vm_interrupt`](#stopping-a-run-from-outside).
:::

## Promises / async

```c
JsValue js_promise_new(JsContext *ctx);                  // undefined on OOM
bool    js_resolve(JsContext *ctx, JsValue promise, JsValue value);
bool    js_reject(JsContext *ctx, JsValue promise, JsValue reason);

void js_run_jobs(JsContext *ctx);          // drains queued microtasks to quiescence
bool js_has_pending_jobs(const JsContext *ctx);
```

A native that needs real time returns a pending promise from
`js_promise_new`; the host settles it later with `js_resolve`/`js_reject`
and then drains the queue with `js_run_jobs`. **You must keep the returned
promise GC-rooted** (`js_gc_protect`) until you settle it, or the collector
may reclaim an in-flight promise. `js_resolve` adopts a promise/thenable
argument (chains, matching real JS) — to fulfill with an object value
verbatim, that object simply must not itself be a promise.

See [Async & host calls](/guide/async) for the full picture, including how
this differs from the npm package's Asyncify-based `__hostcall`.

## Modules

```c
typedef JsValue (*JsModuleLoader)(void *ud, JsContext *ctx,
                                  const uint16_t *specifier, size_t spec_len,
                                  const uint16_t *referrer, size_t ref_len);

typedef bool (*JsModuleCanonicalizer)(void *ud, const uint16_t *specifier, size_t spec_len,
                                      const uint16_t *referrer, size_t ref_len,
                                      const uint16_t **out_specifier, size_t *out_spec_len);

void js_set_module_loader(JsContext *ctx, JsModuleLoader load,
                          JsModuleCanonicalizer canon, void *ud);
```

The loader is **promise-returning**, which is what lets module loading do real
I/O. Given a canonical `specifier` and the importing module's `referrer`
(empty for the root, or for a dynamic `import()` from a plain script), return a
promise — already settled, or pending and settled later with
`js_resolve`/`js_reject` followed by `js_run_jobs()`. Fulfil it with:

- a **JS string** — ES module source, compiled here (its own imports are
  loaded through this same loader);
- a **bytecode value** from `js_bytecode_value` — precompiled module bytecode;
- **any other object** — adopted directly as the module's exports, i.e. a
  synthetic leaf module with no parse and no dependencies. This is how you
  expose a host-provided module.

Rejecting fails the load, and the reason propagates to every dependent.

Canonicalization is separate and synchronous: it runs *before* dedupe and
fetch, and maps a raw specifier plus referrer to the identity the module is
cached under (a relative path to an absolute URL, say). It must be
deterministic for a given pair. A `NULL` canonicalizer means the raw specifier
is the identity.

::: tip Source modules are a frontend capability
Compiling a loader's string result needs the frontend. A context has to be
granted it explicitly with `js_enable_source_modules(ctx)` from
`lamassu_compile.h` — and there is no way to call that in a runtime-only
build, because the symbol isn't in the archive. Without it a string specifier
fails with "source modules unavailable in this build (precompile to
bytecode)", while loaders fulfilling with `js_bytecode_value` keep working.
It's per-context, so a host embedding both trusted tooling and untrusted
execution can hand out contexts that differ in exactly this capability.
:::

```c
JsValue js_eval_module(JsContext *ctx, const uint16_t *specifier, size_t spec_len);
JsValue js_eval_entry(JsContext *ctx, const uint16_t *specifier, size_t spec_len);

JsValue js_module_get_export(JsContext *ctx, JsValue ns, const uint16_t *name,
                             size_t name_len);
```

`js_eval_module` loads, links, and evaluates the module graph rooted at
`specifier` — everything, the root included, arrives through the loader. It
always returns a live promise: protect it, drive outstanding loads with
`js_resolve`/`js_reject` plus `js_run_jobs()`, then read the outcome with
`js_promise_state` / `js_promise_result`. It fulfils with the module's
namespace (exports) object and rejects with the load/compile/link/evaluate
error. `js_module_get_export` is a host convenience for reading a named export
back out.

`js_eval_entry` is the same for a program's entry point, where the host wants a
result rather than exports. The root module — only the root — may `return` at
its top level, anywhere a function body could (inside a block, a loop or a
`try` with its `finally`); the return ends its evaluation, and the promise
fulfils with the returned value, or `undefined` if the body runs off its end.
Its imports load and run first as usual, and a top-level `return` in any of them
is still a `SyntaxError`, as it is under `js_eval_module`. The exports are still
there: a following `js_eval_module` of the same specifier hands back the
namespace from the cached graph. A script host that used to wrap the entry in a
function to get a return value (and pay a line offset in every error location)
can hand over the source as written.

```c
static const uint16_t main_js[] = {'m', 'a', 'i', 'n', '.', 'j', 's'};
JsValue p = js_eval_entry(ctx, main_js, 7);   /* main.js: `...; return { body, lid };` */
js_gc_protect(vm, &p);
/* settle loads, js_run_jobs, then: */
if (js_promise_state(p) == 1) use(js_promise_result(p));
```

## Bytecode caching

Compile once, cache the bytecode, skip re-parsing on every subsequent run.
The loader treats its input as **hostile by default** — a tampered or
corrupted buffer is rejected structurally (opcode validity, bounds-checked
constant/local/upvalue indices, constant *kinds* as well as indices, jump
targets on instruction boundaries, a required terminator, and a worklist
fixpoint over every control-flow edge that proves the operand-stack and
try-handler depths at each reachable instruction, recomputing `max_stack`
rather than trusting the stored value), so it never becomes undefined
behavior in the interpreter, only a clean load failure. [Running untrusted
code](/security#untrusted-bytecode) has the detail, including why this is a
stronger attacker than untrusted source.

Production is in `lamassu_compile.h`, loading in `lamassu.h` — the whole
point of the split:

```c
// frontend
bool js_bytecode_serialize(JsContext *ctx, JsValue fn, uint8_t **out, size_t *out_len);
void js_bytecode_free(JsContext *ctx, uint8_t *buf, size_t len);

// runtime
JsValue js_bytecode_load(JsContext *ctx, const uint8_t *buf, size_t len,
                         const char **err_msg);
```

`js_bytecode_serialize` takes a function from `js_compile_module` (not a
module body — import/export bytecode isn't portable through this path; see
below). The format is versioned, little-endian, and carries no source —
runtime error positions survive via an embedded line table, but mapping
them to line:col needs the original source. `js_bytecode_load` returns a
runnable function (root it, then `js_run_module` it like a freshly
compiled one) or `undefined` with `*err_msg` set on any structural problem.

Modules have their own bytecode path, since a module's link metadata
(imports, star re-exports, dependency specifiers) isn't part of a plain
script:

```c
// frontend
bool js_bytecode_compile_module(JsContext *ctx, const uint16_t *specifier,
                                size_t spec_len, const uint16_t *source,
                                size_t source_len, uint8_t **out, size_t *out_len,
                                const char **err_msg, uint32_t *err_pos);

// runtime
JsValue js_bytecode_value(JsContext *ctx, const uint8_t *buf, size_t len);
int     js_bytecode_kind(const uint8_t *buf, size_t len); // 0 = script, 1 = module, <0 = invalid
```

`js_bytecode_compile_module` compiles one module to bytecode *without*
resolving, linking, or evaluating it, so each module can be compiled and
cached independently. The buffer carries the module body plus its link
metadata (import descriptors, star re-exports, dependency specifiers);
resolved dependencies and live exports are runtime state, rebuilt at load.
`specifier` becomes the module's identity.

There is no separate bytecode resolver: running a precompiled graph uses the
ordinary [module loader](#modules), with each fulfilment wrapped in
`js_bytecode_value` instead of being a source string. The bytes are copied
into a GC-managed value, and every loaded module buffer is fully validated —
including the import-index bounds the interpreter would otherwise trust — so a
tampered cache cannot become undefined behavior. `js_eval_module` then drives
the graph exactly as it does for source.

A script buffer and a module buffer can't be confused: `js_bytecode_load`
rejects a module buffer and the module loader rejects a script buffer.
`js_bytecode_kind` lets a host tell which one a `.jsbc` file is before picking
a path, without loading it.

## GC

```c
void   js_gc_collect(JsVm *vm);
void   js_gc_protect(JsVm *vm, JsValue *slot);   // register *slot as a GC root
void   js_gc_unprotect(JsVm *vm, JsValue *slot);
size_t js_gc_live_cells(const JsVm *vm);
```

Mark-and-sweep, precise, stop-the-world. `js_gc_protect`/`js_gc_unprotect`
are how you satisfy the [GC contract](#gc-contract) above for any `JsValue`
your native code holds across a safe point.

### Why rooting cannot fail

`js_gc_protect` returns `void`, and that is a design decision rather than an
optimistic one. It used to return `false` when the root array could not grow.
Of its ~149 call sites, 8 checked. The other 141 carried on with a value they
believed was rooted — and because the array grew through the heap-limited
allocator, **a guest could choose the moment that happened** by driving the
heap to its cap. "Unrooted value, then collect, then use" became a condition
an attacker could schedule, which is the worst shape a use-after-free can
have.

Rather than ask 149 sites to handle a failure correctly, the failure mode was
removed. The root array is allocated outside the heap limit, so a guest cannot
make rooting fail. If the *real* allocator refuses — the process is genuinely
out of memory, nothing to do with the guest — collection is suspended for the
VM's lifetime instead. An unrooted slot is only dangerous because something
can collect it; with the collector off, the value survives, the operation
unwinds through the ordinary out-of-memory path, and `heap_limit` still
refuses further growth, so the VM fails closed rather than corrupting.
[`js_vm_out_of_memory`](#stopping-a-run-from-outside) reports that state, and
a host that sees it should finish the current call and discard the VM.

There is therefore no status to check, and a caller who checked one could not
have done anything safe with it anyway.

## Native constructors and `new`

Built-in constructors (`Array`, `Date`, `Map`, `Set`, `RegExp`, …) are
plain `JsNativeFn` factory functions that already return a suitable
object — `new Ctor(...)` on a native constructor just calls it the same way
`Ctor(...)` would; there's no special `this`-binding step the way there is
for a user-defined (closure) constructor. If you register your own native
and want it usable with `new`, write it to return the object you want
constructed, the same way `Array`/`Date`/etc. do — see `src/js_date.c`'s
`g_Date` for a worked example of a native constructor with a real,
script-visible `.prototype`.

## Minimal example

```c
#include "lamassu.h"
#include "lamassu_compile.h"

JsVmConfig cfg = {0};
cfg.heap_limit = 16 * 1024 * 1024;   // set both limits for untrusted input
JsVm *vm = js_vm_new(&cfg);
JsContext *ctx = js_context_new(vm);
js_context_set_fuel(ctx, 20000000); // and re-arm this per turn

const uint16_t src[] = {'1', '+', '2'}; // "1+2"
const char *err_msg; uint32_t err_pos;
JsValue fn = js_compile_module(ctx, src, 3, &err_msg, &err_pos);
if (!js_is_function(fn)) { /* handle compile error */ }

js_gc_protect(vm, &fn);
JsValue done = js_run_module(ctx, fn);
// js_promise_state(done) == 1 (fulfilled), js_promise_result(done) == js_number(3)
js_gc_unprotect(vm, &fn);

js_vm_free(vm); // frees ctx too
```

For a fuller worked example — argument parsing, a module loader, a `print`
native, bytecode caching, `--fuel`/`--heap-limit` — see `tools/lamassu.c`, the
native command-line tool this same library backs. For the other shape, a
process that runs only precompiled bytecode and links the runtime alone, see
`tools/run_bc.c`.
