# C embedding API

The full public surface is `include/jsvm.h` — this page is a guided tour of
it, not a replacement. Everything hangs off an explicit `JsVm` or
`JsContext` handle (C's version of `this`); there is no static mutable
state anywhere in the core, so multiple engines can coexist in one process.

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
```

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

`heap_limit` and `js_context_set_fuel` (below) are the two knobs for
running genuinely untrusted scripts — see the security notes in the
project's `docs/plan.md` for how they combine with a WASM sandbox and a
regex step budget.

One `JsVm` can host multiple `JsContext`s (independent realms sharing one
heap/GC/atom table); a single context is enough for most embedders.

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
it (see `tools/jsvm_cli.c` for a small, dependency-free converter).

```c
JsValue js_string_new(JsVm *vm, const uint16_t *units, size_t len); // undefined on OOM
JsValue js_atom(JsVm *vm, const uint16_t *units, size_t len);       // interned; undefined on OOM
const uint16_t *js_string_units(JsValue str, size_t *len);          // NULL if not a string
size_t  js_string_length(JsValue str);
bool    js_string_equals(JsValue a, JsValue b);                     // content equality
```

## Objects

```c
JsValue js_object_new(JsVm *vm);                                          // undefined on OOM
JsValue js_object_get(JsVm *vm, JsValue obj, JsValue key);                // undefined if absent
bool    js_object_set(JsVm *vm, JsValue obj, JsValue key, JsValue value); // false on OOM/bad args
bool    js_object_delete(JsVm *vm, JsValue obj, JsValue key);             // true if removed
size_t  js_object_size(JsValue obj);
```

`js_object_new` is context-free by design and deliberately has **no
[[Prototype]]** — it predates contexts/realms, and a `JsVm` alone has no
single "the" `Object.prototype` to give it (a `JsVm` can host multiple
contexts, each with its own — see [Modules](#modules) for the same
context-vs-vm distinction on a bigger scale). This is *not* what a guest
`{}` literal produces: scripts get a real, script-visible
`Object.prototype` (`hasOwnProperty`/`toString`/`valueOf`, and every other
built-in prototype chains up to it too — see
[Built-ins → Object](/guide/builtins#object)), reachable via
`Object.getPrototypeOf`. There's currently no public function for a native
to construct an object with that same prototype; if you need guest code to
be unable to tell a host-constructed object from one it wrote itself,
that's a gap worth raising.

## Compiling & running

```c
JsValue js_compile_module(JsContext *ctx, const uint16_t *src, size_t len,
                          const char **err_msg, uint32_t *err_pos);
```

Compiles UTF-16 source as a strict-mode module body. Returns a function
value (root it per the GC contract), or `undefined` on error with
`*err_msg` (a static ASCII string) and `*err_pos` (a source offset) set.

```c
JsValue js_compile_module_repl(JsContext *ctx, const uint16_t *src, size_t len,
                               const char **err_msg, uint32_t *err_pos);
```

Same, but top-level `let`/`const`/`function` declarations become
persistent globals on the context — successive compiles in the same
context share state, i.e. a REPL session. This is what the npm package's
`eval` uses.

```c
bool js_run_module(JsContext *ctx, JsValue fn, JsValue *result);
```

Runs a compiled module function. Returns `true` with `*result` = the
completion value (the value of the last expression statement), or `false`
with `*result` = the thrown error value (`js_context_error_pos()` gives its
source offset).

```c
bool js_call(JsContext *ctx, JsValue fn, JsValue this_val, const JsValue *args,
             int argc, JsValue *result);
```

Calls a function value (a script closure or a native) on a fresh fiber.
Same result contract as `js_run_module`.

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

`fuel` is checked on interpreter loop back-edges and calls — the CPU-bound
half of running untrusted scripts (`heap_limit` in `JsVmConfig` is the
memory half).

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
typedef bool (*JsModuleResolver)(void *ud, const uint16_t *specifier, size_t spec_len,
                                 const uint16_t *referrer, size_t ref_len,
                                 const uint16_t **out_specifier, size_t *out_spec_len,
                                 const uint16_t **out_source, size_t *out_len);

void js_set_module_resolver(JsContext *ctx, JsModuleResolver fn, void *ud);
```

Given an import `specifier` and the importing module's `referrer` (empty
for the root), resolve and return the module's source. Write a canonical
specifier to `*out_specifier` (used as the cache/identity key — specifiers
are compared by content) and the source to `*out_source`/`*out_len`. The
returned buffers only need to stay valid until the call returns; the
engine copies what it needs.

```c
JsValue js_eval_module(JsContext *ctx, const uint16_t *specifier, size_t spec_len,
                       const uint16_t *source, size_t source_len, bool *ok,
                       const char **err_msg, uint32_t *err_pos);

JsValue js_module_get_export(JsContext *ctx, JsValue ns, const uint16_t *name,
                             size_t name_len);
```

`js_eval_module` compiles, links, and evaluates a root module (pulling
dependencies through the resolver), returning its namespace (exports)
object on success. `js_module_get_export` is a host convenience for
reading a named export back out.

## Bytecode caching

Compile once, cache the bytecode, skip re-parsing on every subsequent run.
The loader treats its input as **hostile by default** — a tampered or
corrupted buffer is rejected structurally (bounds-checked constant/local/
upvalue indices, jump targets on instruction boundaries, a required
terminator, and an abstract stack-depth pass that recomputes `max_stack`
from scratch rather than trusting the stored value), so it never becomes
undefined behavior in the interpreter, only a clean load failure.

```c
bool js_bytecode_serialize(JsContext *ctx, JsValue fn, uint8_t **out, size_t *out_len);
void js_bytecode_free(JsContext *ctx, uint8_t *buf, size_t len);

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
bool js_bytecode_compile_module(JsContext *ctx, const uint16_t *specifier,
                                size_t spec_len, const uint16_t *source,
                                size_t source_len, uint8_t **out, size_t *out_len,
                                const char **err_msg, uint32_t *err_pos);

typedef bool (*JsBytecodeResolver)(void *ud, const uint16_t *specifier, size_t spec_len,
                                   const uint16_t *referrer, size_t ref_len,
                                   const uint16_t **out_specifier, size_t *out_spec_len,
                                   const uint8_t **out_bytecode, size_t *out_len);

void js_set_bytecode_resolver(JsContext *ctx, JsBytecodeResolver fn, void *ud);
int  js_bytecode_kind(const uint8_t *buf, size_t len); // 0 = script, 1 = module, <0 = invalid

JsValue js_eval_module_bytecode(JsContext *ctx, const uint16_t *specifier,
                                size_t spec_len, const uint8_t *bytecode, size_t len,
                                bool *ok, const char **err_msg, uint32_t *err_pos);
```

`js_bytecode_compile_module` compiles one module to bytecode *without*
resolving, linking, or evaluating it, so each module can be compiled and
cached independently. `js_eval_module_bytecode` loads, validates, links,
and evaluates a root module from bytecode, pulling dependencies through the
bytecode resolver — same result contract as `js_eval_module`. A script
buffer and a module buffer can't be confused: `js_bytecode_load` rejects a
module buffer and `js_eval_module_bytecode` rejects a script buffer.
`js_bytecode_kind` lets a host tell which one a `.jsbc` file is before
picking a path.

## GC

```c
void   js_gc_collect(JsVm *vm);
bool   js_gc_protect(JsVm *vm, JsValue *slot);   // register *slot as a GC root
void   js_gc_unprotect(JsVm *vm, JsValue *slot);
size_t js_gc_live_cells(const JsVm *vm);
```

Mark-and-sweep, precise, stop-the-world. `js_gc_protect`/`js_gc_unprotect`
are how you satisfy the [GC contract](#gc-contract) above for any `JsValue`
your native code holds across a safe point.

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
#include "jsvm.h"

JsVm *vm = js_vm_new(NULL);
JsContext *ctx = js_context_new(vm);

const uint16_t src[] = {'1', '+', '2'}; // "1+2"
const char *err_msg; uint32_t err_pos;
JsValue fn = js_compile_module(ctx, src, 3, &err_msg, &err_pos);
if (!js_is_function(fn)) { /* handle compile error */ }

js_gc_protect(vm, &fn);
JsValue result;
bool ok = js_run_module(ctx, fn, &result);
// ok == true, result == js_number(3)
js_gc_unprotect(vm, &fn);

js_vm_free(vm); // frees ctx too
```

For a fuller worked example — argument parsing, a module resolver, a
`print` native, bytecode caching — see `tools/jsvm_cli.c`, the native
command-line tool this same library backs.
