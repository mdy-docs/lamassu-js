# jsvm — Implementation Plan

A simplified JavaScript virtual machine written in C, compiled to a WASM module,
for safely executing untrusted user scripts inside a web-framework templating
language.

## Goals

- Run scripts written in a practical subset of modern JavaScript.
- Compile source to bytecode once; execute compiled functions repeatedly with no
  re-parse/re-compile cost.
- Support async/await for calling out to native (host) functions that may take time.
- Support ECMAScript modules (`import` / `export`).
- Be safe against hostile scripts: bounded CPU (fuel), bounded memory, bounded
  stack depth.
- Clean object-oriented C design: no static globals; all state hangs off
  explicit handles (`JsVm`, `JsContext`, …) passed as the first argument.
- A native command-line tool (`jsvm`) that compiles and runs a JavaScript
  file — both a developer convenience and the harness the script-level and
  differential test suites drive.

## Non-goals

- Classes, `eval`, the `Function` constructor.
- Sloppy (non-strict) mode. Every unit of source is an ES module and executes
  under strict semantics; there is no script goal and no mode switch.
- Advanced built-ins (Reflect, Proxy, Symbol, generators, typed arrays, Intl, …).
- Full spec conformance. Where the subset overlaps real JS, behavior should
  match Node closely enough for differential testing, but edge-case fidelity is
  not a goal.
- Native code generation. Bytecode interpretation is the execution model;
  it satisfies the "compile once, run repeatedly" requirement and is the only
  practical option inside WASM anyway (no runtime codegen in linear memory).

## Decisions (locked in)

| Decision | Choice | Notes |
|---|---|---|
| Host environment | Multiple / undecided | Core must be host-agnostic; per-target glue layers. |
| Syntax scope | Basics + template literals, try/catch/throw, destructuring & spread, regex | Regex is feature-flagged and integrated last. |
| Regex engine | Reuse [`third_party/regex-engine`](../third_party/regex-engine) (git submodule) | ECMAScript-flavored compiler + backtracking VM extracted from jsvm2; we write the binding layer, not the engine. |
| String encoding | UTF-16 everywhere internally | Never raw NUL-terminated C strings; all string data is `uint16_t*` + code-unit length. Matches JS `.length`/indexing semantics and the regex engine's native format. |
| Language mode | Strict mode only | All source compiles as an ES module (modules are strict by spec). No sloppy mode exists anywhere in the engine. |
| Declarations | `let`/`const` only — no `var` | `var` stays a reserved word with a targeted error: `'var' is not supported; use 'let' or 'const'`. Never aliased to `let` (semantics differ). |
| Property storage | One unified table per object; interned-atom pointer lookup | All property kinds share one entry layout (kind = attribute bits). Shapes + inline caches as a later optimization pass. See "Property storage". |
| Bytecode caching | Serializable | Versioned binary format; validating loader. |
| GC | Mark-and-sweep tracing | Precise, stop-the-world; handles closure cycles; ~1–2k lines. |

"Basics" = `let`/`const` (no `var` — see below), `if`/`else`,
`for`/`while`/`do`, `for-of`, `switch`, functions, arrow functions,
object/array literals, property access, all the usual operators,
`async`/`await`, `import`/`export`.

Strict-mode-only simplifies the engine meaningfully: there is exactly one
compilation goal (module, strict by spec), so no `with`, no sloppy `this`
coercion (`this` in a plain function is `undefined`, not the global object),
assignment to an undeclared variable is a compile-visible `ReferenceError`
rather than an implicit global, no `arguments.callee`/`caller`, no legacy
octal literals, and duplicate parameter names are rejected. The parser and
compiler implement strict semantics unconditionally — there is no mode flag
to thread through.

Dropping `var` goes one step further: it removes the only construct whose
scoping isn't shaped like the block structure of the code. `var` hoists to
function scope from arbitrary nesting depth, which would require a
whole-function declaration pre-pass and the spec's dual
VariableEnvironment/LexicalEnvironment bookkeeping, plus all the
`var`/`let` interaction rules (collision errors, silent redeclaration,
different `for`-head closure behavior). With `let`/`const` only, scoping is
exactly one rule — a binding belongs to its enclosing block — and scope
slots are allocated and released strictly block-by-block. The costs `let`
already imposes (temporal dead zone, fresh per-iteration loop bindings for
closures) are paid regardless, so this is pure subtraction. The only
remaining hoisting is block-scoped function declarations, a small uniform
per-block scan. `var` remains a reserved word so the parser can reject it
with a targeted message instead of a confusing generic parse error; it is
deliberately **not** aliased to `let`, since hoisting/redeclaration
semantics differ and silently diverging from real JS is the worst failure
mode for a templating language. Differential testing against Node is
unaffected — the shared subset simply never contains `var`.

## Size & timeline estimate

Total scope is roughly **20–30k lines of C** — a medium-sized engine.
Calibration points: MuJS (ES5 interpreter, no async/modules) ≈ 15k lines;
QuickJS (full ES2023) ≈ 85k lines.

Working in AI-assisted sessions:

- **Runnable core** (control flow, closures, objects, template literals,
  try/catch): ~1 week of sessions.
- **Feature-complete** (+ async/await, modules, destructuring, bytecode
  serialization): ~2–3 weeks.
- **Regex**: 1–2 sessions to integrate `third_party/regex-engine` (binding layer +
  step-budget hardening), deliberately last, behind a compile flag.
- **Hardening tail** (fuzzing, sanitizers, differential testing): a few weeks
  of mostly background machine time.

Hand-written solo without assistance this would be a 2–4 month full-time project.

## Architecture

### Layering

```
┌───────────────────────────────────────────┐
│ per-host glue (thin, one per target)      │  wasm_api.c — flat ABI: uint32 handles,
│ browser / wasmtime / node …               │  ptr+len strings, exported functions
│ native CLI (tools/jsvm) — file I/O and    │
│ UTF-8→UTF-16 conversion live here, not    │
│ in the core                               │
├───────────────────────────────────────────┤
│ libjsvm core — pure freestanding C11      │  no globals, no I/O, no OS calls;
│ lexer → parser → compiler → interpreter   │  allocator + host callbacks injected
│ GC, values, builtins, modules, promises   │  via JsVmConfig
└───────────────────────────────────────────┘
```

### Core types

Everything is instance-based; every API takes `vm` or `ctx` as its first
argument (C's version of `this`). No static mutable state anywhere in the core.

```c
typedef struct JsVm       JsVm;       // owns heap, GC, atom table; one per wasm instance
typedef struct JsContext  JsContext;  // realm: globals, module cache, microtask queue
typedef uint64_t          JsValue;    // NaN-boxed (wasm32 pointers fit easily)
typedef struct JsObject   JsObject;   // property map; kind = plain | array | error | …
typedef struct JsFunction JsFunction; // compiled bytecode chunk + upvalue descriptors
typedef struct JsClosure  JsClosure;  // JsFunction* + captured upvalues
typedef struct JsModule   JsModule;   // exports, dependencies, link/eval status
typedef struct JsFiber    JsFiber;    // heap-allocated call frames + value stack; suspendable
```

Representative API shape:

```c
JsVm      *js_vm_new(const JsVmConfig *cfg);
JsContext *js_context_new(JsVm *vm);
int        js_compile(JsContext *ctx, const uint16_t *src, size_t len, JsModule **out);
int        js_call(JsContext *ctx, JsValue fn, const JsValue *argv, int argc, JsValue *result);
void       js_register_native(JsContext *ctx, const uint16_t *name, size_t name_len,
                              JsNativeFn fn, void *userdata);
void       js_resolve(JsContext *ctx, JsValue promise, JsValue value);
void       js_run_jobs(JsContext *ctx);
```

### Key structural decisions

1. **The interpreter never uses the C stack for JS calls.** Call frames live on
   the heap in a suspendable `JsFiber`. This makes `await` nearly free
   (suspension = the fiber simply stops being run), enforces safe stack-depth
   limits against hostile recursion, and is very hard to retrofit later — so it
   is built in from phase 3.
2. **Host-neutral async contract.** A native function that needs time returns a
   pending promise; the fiber suspends. When the host finishes, it calls
   `js_resolve(ctx, promise, value)` then `js_run_jobs(ctx)`. A browser does
   that from a JS promise callback, wasmtime from its own scheduler — the core
   never knows the difference.
3. **NaN-boxed values.** 64-bit `JsValue` with doubles stored directly and
   pointers/tags in the NaN payload; wasm32's 32-bit pointers fit comfortably.
4. **Interned atoms for property keys**; immutable UTF-16 strings, always
   carried as `uint16_t*` + code-unit length — never NUL-terminated C
   strings, in the core or at the embedding boundary. This matches JS
   `.length`/indexing/slice semantics exactly and feeds the regex engine
   with zero conversion. (One boundary quirk: the regex engine's
   `regex_compile` wants a NUL-terminated *UTF-16* pattern, so the binding
   layer appends a `0x0000` unit when handing patterns over.) Host glue
   converts to/from UTF-8 only at its own outer edge if a target needs it.
5. **Fuel metering** in the dispatch loop, checked on loop back-edges and
   calls — plus a step budget around the regex engine (see security notes).
6. **Modules** follow the real compile → link → evaluate pipeline with a host
   resolver callback and cyclic-import handling.

### Property storage

Named properties are uniform and lookup is pointer-fast; the design has three
tiers, the first of which is already built (phase 1).

1. **Interned keys, one unified table (built).** Every property key is an
   interned atom — one canonical `JsString` per distinct content, hash
   computed once at creation and cached on the string. Lookup masks the
   cached hash into a per-object open-addressed table and compares
   *pointers*: no character comparison, no re-hashing, typically one probe.

   Properties are never separated by kind: a property is always one entry in
   one table — `{ key, attributes, value }` — and the kind lives in the
   attributes byte, not the storage layout. Data properties (the common
   case) have attributes 0 and pay no branch on the fast path; accessors, if
   the subset keeps them, are the same slot with a flag bit and a
   getter/setter pair cell in the value slot; function-valued properties
   ("methods") are plain data properties holding a closure. No parallel
   tables — no data map + accessor list + special builtin path where every
   `get` pays a "which table?" question and every feature adds another.

2. **Compile-time interning (phase 3).** In `obj.name` the key is known
   statically, so the compiler interns it once into the function's constant
   pool. The interpreter hands the map an already-canonical atom; the hash
   is computed once per *program*, not per access.

3. **Shapes + inline caches (post-phase-7 optimization pass).** Hash-free is
   the ceiling for a hash map; the step beyond is shapes (hidden classes):
   objects created with the same property insertion order share a shape
   mapping key → slot index, values live in a flat array, and each
   property-access bytecode site carries an inline cache of `(shape, slot)`
   — a hit is one pointer compare plus an array index. Still unified:
   attribute bits live in the shape entry and every property kind occupies
   the same slot array. Because the API and semantics encode no separation,
   swapping the storage engine underneath is a contained change. Do this
   only once the interpreter is stable and benchmarks exist.

The one principled exception is **array elements**: `arr[0]` is spec-wise a
named property (`"0"`), but dense integer-indexed storage as a flat vector
is too large a win to forgo. That split is keyed on key *shape* (canonical
index vs. name), is invisible semantically, and string-named properties on
arrays still use the same unified table.

## Security considerations

Users are untrusted; this shapes several choices:

- **WASM is the primary sandbox.** An interpreter bug that corrupts memory
  corrupts the module's own linear memory, not the host. The host runtime can
  additionally enforce CPU and memory limits externally (e.g. wasmtime epoch
  interruption + memory caps). In-VM limits are defense-in-depth and exist to
  produce graceful errors rather than killed instances.
- **Regex is the ReDoS hole.** Bytecode fuel metering does not cover a
  backtracking regex engine running in a native C loop — `/(a+)+$/` would hang
  the VM from inside. `third_party/regex-engine` is a backtracking VM with
  **no internal step limit** (verified: no fuel/step/timeout in `regexp.c`),
  so our integration must add one — either a step counter patched into
  `vm_execute_internal` (upstreamable) or, minimally, reliance on host-level
  interruption (wasmtime epochs) with the caveat that a hung regex kills the
  whole instance rather than throwing a catchable error.
- **Regex memory footprint.** A compiled `Program` is a large fixed-size
  struct (multi-megabyte; `MAX_OPCODES`/`MAX_CLASSES`/`MAX_GROUPS` in
  `regexp.h` are generous fixed bounds). Untrusted scripts compiling many
  distinct patterns is a memory-exhaustion vector: cap live compiled patterns
  per context, and consider shrinking the `MAX_*` constants for our build.
- **The bytecode loader validates.** Serialized bytecode means the interpreter
  can receive bytes the compiler didn't just produce. Structural validation on
  load (jump targets in range, constant/register indices bounded, arity sane)
  keeps a corrupted or tampered cache from becoming undefined behavior.
- **Stack depth and heap size** are hard-limited per context; allocation goes
  through the injected allocator so the host can meter it.

## Phased roadmap

Each phase lands with its tests passing; the project is shippable at any point
past phase 7.

| Phase | Contents | Sessions |
|---|---|---|
| 1 | Repo scaffolding; build (native + `clang --target=wasm32`); NaN-boxed `JsValue`; mark-sweep GC; interned strings/atoms; hash maps | 2–3 |
| 2 | Lexer + parser → AST, incl. template literals, destructuring patterns, ASI | 3–4 |
| 3 | Bytecode format, compiler, interpreter core: expressions, control flow, scoping; minimal `jsvm` CLI (compile + run a file, print result/errors) | 2–3 |
| 4 | Functions, closures/upvalues, heap-frame fibers, try/catch/throw | 2 |
| 5 | Object/Array/String/Math/JSON builtins, `for-of` | 2 |
| 6 | Promises, microtask queue, async/await (suspend/resume on fibers) | 1–2 |
| 7 | ES modules: compile → link → evaluate, host resolver hook, cyclic imports | 1–2 |
| 8 | Bytecode serializer + validating loader; CLI grows `--emit-bytecode`/`--run-bytecode` to round-trip the format | 1 |
| 9 | WASM export surface + example host glue; fuel/memory/stack limits | 1–2 |
| 10 | Regex: integrate `third_party/regex-engine` — binding layer (RegExp object, match results, named groups), step-budget patch, pattern-count cap; behind a compile flag | 1–2 |
| opt | Shapes + inline caches for property access (see "Property storage") — only once benchmarks exist | 2–3 |
| — | Fuzzing (parser + interpreter under ASan/UBSan), differential testing vs Node | background |

## Testing & hardening strategy

- **Unit tests from phase 1** — GC correctness (collection actually reclaims,
  roots survive) proven before anything is built on top.
- **Script-level tests**: source files with expected output/errors, driven
  through the `jsvm` CLI; the same suite runs against the wasm build.
- **Differential testing**: run the shared subset against Node and compare
  results, catching semantic drift.
- **Fuzzing**: libFuzzer/AFL targets for the lexer/parser, the interpreter
  (fuzzed bytecode through the validating loader), and later the regex engine;
  CI runs under ASan/UBSan.
