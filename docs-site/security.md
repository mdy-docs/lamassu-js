# Running untrusted code

Running scripts you didn't write is what this engine is for, so this page is
not a disclaimer — it's the operating manual. It covers the threat model the
engine is built against, what it enforces on its own, and the part that
actually decides whether your embedding is safe: what your host still has to
do.

::: warning The limits are off by default
`fuel` and `heap_limit` are both unlimited unless you set them, in every
embedding. That is the right default for running your own code and the wrong
one for running anyone else's. If you read nothing else here, read [Set both
limits](#set-both-limits).
:::

## Threat model

Two distinct inputs can be hostile, and they are not equally well defended.

### Untrusted source

The guest supplies JavaScript, you compile it with `js_compile_module` (or
`eval` in the npm package), and run it. The guest controls the *program* but
not the *bytecode*: everything that executes came out of this compiler, so the
interpreter can rely on the invariants the compiler establishes.

### Untrusted bytecode

The guest — or a tampered cache, or a compromised build step — supplies a
`.jsbc` buffer directly to `js_bytecode_load` or a module loader. This is the
stronger attacker. It is not limited to programs the compiler can emit, so
every invariant the interpreter relies on has to be *proven by the loader*
rather than assumed.

The [frontend/runtime split](/api/c-embedding#two-headers-two-archives) exists
so a process can opt out of the first case entirely. A binary that links only
`liblamassu_runtime.a` cannot turn text into code, because the code that would
do it is not in the binary. That is enforced by the link, and
`make check-runtime-only` proves it stays that way.

Out of scope: your own natives. Anything reachable through
`js_register_native` (or the npm package's `natives`) is exactly as
trustworthy as you made it, and nothing here constrains it.

## What the engine enforces

**Memory safety against hostile bytecode.** The loader verifies structure
before anything executes: opcode validity, operand ranges, constant *kinds*
(not merely indices — a Number constant cannot be handed to an opcode
expecting a string or a function), instruction-boundary alignment of every
jump target, and a worklist fixpoint over all control-flow edges that proves
the operand-stack depth and the try-handler depth at every reachable
instruction. `max_stack` is recomputed from that fixpoint; the value stored in
the file is never trusted. A rejected buffer never yields an executable
function, so a tampered cache becomes a clean load failure rather than
undefined behavior.

**Recursion bounds.** Parser nesting is capped, and so is the compiler's own
expression recursion — separately, because a left-associative chain like
`a+b+c+…` parses iteratively but produces a deep tree the compiler walks
recursively. Call frames are capped, and native re-entry into the interpreter
is capped on top of that, since each re-entry spawns a fresh fiber the
per-fiber frame limit cannot see.

**An execution budget.** One unit per dispatched bytecode instruction. It
covers a *turn*, it is shared rather than per-fiber or per-job, and exhaustion
throws an error the guest cannot catch — see [Set both
limits](#set-both-limits).

**A heap bound.** Checked before each allocation, so the accounted total never
exceeds it, and covering bulk allocations (property maps, array element
buffers, fiber stacks, the job queue) rather than only GC cell headers.
Individual operations have their own ceilings on top: a maximum for anything
that builds a string, and a maximum array growth per operation.

**Bounded regex.** The matcher backtracks, so it is protected by an explicit
step budget proportional to subject length rather than by construction.
Catastrophic patterns like `/(a+)+$/` raise a catchable `RangeError` instead
of hanging. Bounded quantifiers compile to counter loops, so `/a{1000000}/` is
a few instructions, and pattern nesting depth, program size, and live compiled
patterns are all capped. Regex memory is allocated through the VM, so it
counts against `heap_limit` too.

**Enforced integrity.** `Object.freeze` and `Object.seal` are real, on every
path that can write: property assignment, indexed array writes, `length`,
`delete`, the in-place array mutators that bypass the property path, and the
host's own `js_object_set`. The engine is strict-mode throughout, so a refused
write throws a `TypeError`. This matters below, where hardening a reused
context's prototypes is discussed.

**No ambient authority.** There is no filesystem, no network, no `eval`, no
`Function` constructor — and under wasm, linear memory is a hard boundary
besides. The guest reaches nothing you did not hand it.

## What the host must do

### Set both limits

`fuel` and `heap_limit` both default to unlimited. An embedding that leaves
them there has no bound on anything — WebAssembly included, which keeps the
guest out of your memory but will happily let it spin forever. `heap_limit` in
particular is what stops a single deeply recursive call from demanding a fiber
stack of (max frames × locals) slots.

::: code-group

```c [C]
JsVmConfig cfg = {0};
cfg.heap_limit = 16 * 1024 * 1024;   // hard cap on live bytes
JsVm *vm = js_vm_new(&cfg);
JsContext *ctx = js_context_new(vm);

// Arm the budget for THIS turn, then run.
js_context_set_fuel(ctx, 20000000);
JsValue done = js_run_module(ctx, fn);
```

```js [npm]
import { createLamassu } from "@mdy-docs/lamassu-js";

const lam = await createLamassu({
  fuel: 20_000_000,               // ≈0.1s of guest CPU per evaluation
  heapLimit: 16 * 1024 * 1024,
});

await lam.eval("while (true) {}");
// ⇒ "Uncaught RangeError: execution budget exhausted"   (and it returns)
```

```sh [CLI]
lamassu --fuel 20000000 --heap-limit 64M untrusted.js
```

:::

The details are on the [C embedding](/api/c-embedding#compiling-running) and
[npm package](/api/npm-package#sandbox-limits) pages; the CLI accepts a
`K`/`M`/`G` suffix on `--heap-limit`, and `lamassu --help` says the same
things this page does, briefly.

Three properties of the budget are worth knowing, because they are what stop
the obvious evasions:

- **It covers a turn** — one top-level run *plus* the microtask drain that run
  feeds.
- **It is shared**, not per-fiber and not per-job: nested calls inherit the
  remainder and so does every queued microtask, so re-queueing cannot mint a
  fresh budget.
- **Exhaustion re-arms itself.** A `try`/`catch` around the loop swallows one
  throw and the next instruction throws again, until the fiber unwinds.

### Re-arm the budget per turn

Because it arms exactly one turn, it must be armed per turn. A server that
calls `js_context_set_fuel` once at startup has given the whole process a
single budget, not one per request — the second request inherits whatever the
first left. The npm package and the CLI already re-arm at each evaluation
point, for the same reason: a REPL where the budget were per session would be
killed permanently by its first infinite loop, instead of surviving it and
carrying on.

### Bound wall-clock separately

::: warning Fuel counts instructions, not time
A single instruction can enter a builtin that runs long. `sort` is O(n²) in
element moves and charges nothing for them; string concatenation can copy and
rehash a large string in one step; the regex step budget is per call, so a
loop of cheap-looking `re.test(s)` calls multiplies it. None of these are
memory-unsafe and all are bounded per operation — but the aggregate is not
bounded in *time* by fuel alone.
:::

Arm a timer and call
[`js_vm_interrupt`](/api/c-embedding#stopping-a-run-from-outside), which is
safe from another thread or a signal handler and stops the run at the next
dispatch. Clear the flag with `js_vm_clear_interrupt` before reusing the VM.

Keep an outer kill as well if your deadline is hard rather than best-effort:
the interrupt is read between instructions, so a builtin already running
finishes its current step first. Under wasm, that outer bound is the runtime's
epoch interruption; in Node, it is a worker you can terminate.

### Use a fresh context per tenant

`Array.prototype`, `Object.prototype` and the other object-kind prototypes are
real, script-reachable, mutable objects here, exactly as they are in any JS
engine. A context reused across mutually distrusting guests therefore carries
one guest's prototype mutations into the next. Create a context per untrusted
execution — `reset()` in the npm package, a new `JsContext` in C.

::: tip Freezing is now a real defence
`Object.freeze` used to return its argument and enforce nothing, which made
hardening a reused context's prototypes a no-op that read like a mitigation.
It is enforced now, so it works. Prefer a fresh context anyway: freezing covers
the prototypes you remembered to freeze, while a new context covers
everything.
:::

### Prefer precompiled bytecode for fleet processes

If a process only ever runs code your build pipeline produced, link the
runtime alone. "This process cannot compile attacker-supplied source" stops
being a policy someone has to remember and becomes a property of the binary.
`tools/run_bc.c` is the skeleton, and it is what `make check-runtime-only`
links.

### Watch for the unrecoverable VM

`js_vm_out_of_memory` reports that the VM hit an allocation failure it could
not report cleanly and had to switch the collector off. It stays memory-safe
and keeps refusing work, but it can no longer reclaim anything. Finish the
current call and discard it rather than keep serving from it.

## Where the limits are approximate

Two bounds are approximate by construction rather than by omission, and it is
better to plan around them than to be surprised:

- **Interrupt latency is bounded by the dispatch loop, not by wall-clock.** A
  builtin already running finishes its current step before the stop is seen.
  Every such step is individually bounded, so this is a latency question
  rather than an unbounded one — but a hard deadline still wants an outer
  kill.
- **`heap_limit` bounds accounted bytes, not process RSS.** The counter tracks
  what the engine *requested*, so the allocator's own metadata and size-class
  rounding are invisible to it, and a growing `realloc` may briefly hold the
  old block and the new one at once. A host needing a true RSS ceiling should
  enforce it in the `JsReallocFn` it supplies, which is the only place with
  the real numbers.

## Going deeper

[`docs/security.md`](https://github.com/mdy-docs/lamassu-js/blob/main/docs/security.md)
in the repository is the authority on the internals — which specific bytecode
invariants the verifier proves and why each one matters, how the two runtime
machines the bytecode can drive (`TRY_POP`, `RET_SUB`) are checked rather than
assumed, and the testing that backs it: an exhaustive single-byte mutation
sweep over the loader, and known use-after-free shapes exercised under
`gc_stress`. Read it if you are extending the engine, auditing it, or adding
an opcode.
