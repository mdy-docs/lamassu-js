# Running untrusted code

This is the threat model lamassu is built against, what the engine enforces on
its own, and — the part that actually decides whether an embedding is safe —
what the host still has to do.

Read the last section before shipping. Several of the guarantees below are
conditional on host configuration that is **not** the default.

## Threat model

Two distinct inputs can be hostile, and they are not equally well defended:

**Untrusted source.** The guest supplies JavaScript, the host compiles it with
`js_compile_module` and runs it. The guest controls the program but not the
bytecode: everything it runs came out of this compiler.

**Untrusted bytecode.** The guest (or a tampered cache, or a compromised build
step) supplies a `.jsbc` buffer directly to `js_bytecode_load` or a module
loader. This is the stronger attacker: it is not limited to programs the
compiler can emit, so every invariant the interpreter relies on has to be
*proven by the loader* rather than assumed from the compiler's output.

The frontend/runtime split exists to let a process opt out of the first case
entirely. A binary that links only `liblamassu_runtime.a` cannot turn text into
code, because the code that would do it is not in the binary — see
`include/lamassu_compile.h`. That is enforced by the link, and
`make check-runtime-only` proves it.

Out of scope: the host's own natives. Anything reachable through
`js_register_native` is as trustworthy as the host made it, and nothing here
constrains it.

## What the engine enforces

**Memory safety against hostile bytecode.** The loader verifies structure
before anything executes: opcode validity, operand ranges, constant *kinds*
(not just indices — a Number constant cannot be handed to an opcode expecting a
string or a function), instruction-boundary alignment of every jump target, and
a worklist fixpoint over all control-flow edges that proves the operand-stack
depth and the try-handler-stack depth at every reachable instruction.
`max_stack` is recomputed from that fixpoint; the value stored in the file is
never trusted.

Two runtime machines the bytecode can drive are checked rather than assumed:

- `TRY_POP` must have a matching `TRY_PUSH` on every path. Without that proof an
  unmatched pop underflows an unsigned counter, and the unwinder reads a
  handler record from far out of bounds and jumps to the address it finds
  there.
- `RET_SUB` pops its return address off the operand stack at runtime, so it is
  the one control transfer the verifier cannot follow statically. It is
  restricted to real `GOSUB` resume points, *at one of the operand depths the
  verifier proved for that offset* — landing anywhere else would run code the
  fixpoint never visited, and leave stale values below the new stack top that
  the collector has stopped tracing.

**Recursion bounds.** Parser nesting is capped (`JS_PARSE_MAX_DEPTH`), and so is
the compiler's own expression recursion (`JS_COMPILE_MAX_EXPR_DEPTH`) — the two
are separate because left-associative chains like `a+b+c+…` are parsed
iteratively but produce a deep left-leaning tree that the compiler walks
recursively. Call frames are capped at `JS_MAX_FRAMES`; native re-entry into
the interpreter is capped separately at `JS_MAX_REENTRY`, since each re-entry
spawns a fresh fiber the per-fiber frame limit cannot see.

**Execution budget.** `js_context_set_fuel` charges one unit per dispatched
bytecode instruction. The budget is shared, not per-fiber and not per-job:
nested calls inherit the remainder, and so does every microtask the queue runs,
so re-queueing cannot mint a fresh budget. Exhaustion throws a `RangeError` that
re-arms itself, so a `try`/`catch` around the loop cannot swallow it and keep
running — each subsequent instruction throws again until the fiber unwinds.

**Heap bound.** `heap_limit` is enforced in `js_realloc_raw`, so it covers bulk
allocations (property maps, array element buffers, fiber stacks, the job queue)
and not merely GC cell headers — and it is a cap, not a target: the check runs
before the allocation, so the accounted total never exceeds it. The collector's
mark stack is charged too, sized from the cell count before marking begins
rather than grown in the middle of it, where a refusal could not have been
reported. Individual operations have their own ceilings:
`JS_MAX_STRING_UNITS` for anything that builds a string, `JS_MAX_ARRAY_GAP` for
array growth per operation.

**Regex.** The matcher is a backtracking engine, so it is protected by an
explicit step budget proportional to subject length rather than by construction.
Catastrophic patterns like `/(a+)+$/` raise a catchable `RangeError` instead of
hanging. Bounded quantifiers compile to counter loops, so `/a{1000000}/` is a
few instructions, and pattern nesting depth, program size, and live compiled
patterns are all capped.

## What the host must do

**Set both limits.** `fuel` and `heap_limit` both default to unlimited. An
embedding that leaves them at the default has no bound on anything. `heap_limit`
in particular is what stops a single deeply recursive call from demanding a
fiber stack of `JS_MAX_FRAMES × n_locals` slots.

The bundled embeddings expose them, and also default to off, so pointing one at
someone else's code means passing them:

| embedding | how |
| --- | --- |
| CLI (`build/lamassu`, and the wasip2 build) | `--fuel N`, `--heap-limit N` (`K`/`M`/`G` suffixes accepted) |
| npm package (`@mdy-docs/lamassu-js`) | `createLamassu({ fuel, heapLimit })`, or `setLimits()` later |
| C API | `js_context_set_fuel`, `JsVmConfig.heap_limit` |

**Re-arm the fuel budget per turn.** `js_context_set_fuel` arms one turn — a
top-level run plus the microtask drain it feeds. A server that sets it once at
startup gives the whole process one budget, not one per request.

**Bound wall-clock.** Fuel counts instructions, not time, and a single
instruction can enter a builtin that runs long. `sort` is O(n²) in
element moves and charges nothing for them; string concatenation can copy and
rehash up to `JS_MAX_STRING_UNITS` per instruction; the regex step budget is per
call, so a loop of cheap-looking `re.test(s)` calls multiplies it. None of these
are memory-unsafe, and all are bounded per operation — but the aggregate is not
bounded in *time* by fuel alone. Arm a timer and call `js_vm_interrupt`, which
is safe from another thread or a signal handler and stops the run at the next
dispatch. Keep an outer kill as well if the deadline is hard rather than
best-effort: the interrupt is read between instructions, so a builtin already
running finishes its current step first. Under wasm that outer bound is the
runtime's epoch interruption (see the LIMITS note in `src/reactor.c`).

**Use a fresh context per tenant.** `Array.prototype`, `Object.prototype` and
the other object-kind prototypes are real, script-reachable, mutable objects, as
they are in any JS engine, so a context reused across mutually distrusting
guests carries one guest's prototype mutations into the next. Create a context
per untrusted execution; for the wasm reactor, prefer a fresh instance
(`src/reactor.c` explains why that is cheap enough to be the default).

`Object.freeze` is now enforced, so hardening the prototypes of a context you
do intend to reuse is a real defence rather than a no-op that reads like one.
Prefer a fresh context anyway: freezing covers the prototypes you remember to
freeze, while a new context covers everything.

**Prefer precompiled bytecode for fleet processes.** Link the runtime alone and
"this process cannot compile attacker-supplied source" stops being a policy
someone has to remember and becomes a property of the binary.

## Known gaps

None of the gaps this document originally listed remain open inside the engine.
The obligations in the previous section are still obligations — they are host
configuration, not engine defects — and two limits remain approximate by
construction rather than by omission:

- **Interrupt latency is bounded by the dispatch loop, not by wall-clock.** A
  builtin already running finishes its current step before the stop is seen.
  Every such step is individually bounded, so this is a latency question, not
  an unbounded one, but a hard deadline still wants an outer kill.
- **`heap_limit` bounds accounted bytes, not process RSS.** `bytes_live` counts
  what the engine *requested*, so the allocator's own metadata and size-class
  rounding are invisible to it, and a growing `realloc` may briefly hold the old
  block and the new one at once. A host that needs a true RSS ceiling should
  enforce it in the `JsReallocFn` it supplies, which is the only place with the
  real numbers.

Closed recently, listed because embeddings written against the old behaviour may
still be compensating for them:

- Regex memory is allocated through the VM, so it is counted in
  `js_vm_allocated_bytes` and capped by `heap_limit`. It used to come from libc
  directly and was invisible to both.
- `js_vm_interrupt` gives a host an asynchronous stop; there was previously no
  way to halt a running script from outside the engine.
- `js_gc_protect` can no longer fail, so the ~141 call sites that never checked
  its result are correct by construction rather than by luck.
- Out-of-memory always carries a reason, and a settled promise always notifies
  its reactions. Both used to degrade silently under memory pressure — the first
  to a bare `undefined` rejection, the second to an `await` that stayed pending
  forever.
- `Object.freeze` and `Object.seal` are enforced, on every path that can write:
  property assignment, array indexed writes, `length`, `delete`, the in-place
  array mutators that bypass the property path, and the host's `js_object_set`.
- Nothing pushes the accounted total past `heap_limit` any more, including the
  collector's own mark stack. That stack is now sized before marking rather than
  grown during it, so the one allocation the limit could not cover is gone; it
  used to overshoot by up to 20% of the cap on a heap of small cells.

## Testing

`make test` runs the suite under ASan and UBSan. Two parts of it exist
specifically for this threat model:

- `test_bytecode.c` sweeps every single-byte substitution (every offset, all 256
  values) through the loader and, for anything the loader accepts, through the
  interpreter. The invariant is not that mutants are rejected — most are
  harmless — but that whatever is accepted cannot crash. Three separate
  memory-safety bugs were reachable by a one-byte edit, which is why the sweep
  is exhaustive rather than sampled.
- `test_exec.c` exercises known use-after-free shapes under `gc_stress`, which
  collects at every safe point and so turns a missing root into a deterministic
  failure instead of a rare one.

When adding an opcode, add its row to the verifier's op-info table *and* check
whether it touches runtime state the verifier models. Operand-stack depth and
try-depth are modelled; anything else new is not, and the interpreter must not
be left trusting it.
