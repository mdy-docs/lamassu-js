# Deviations from real JS

lamassu-js aims to match Node closely enough for differential testing over
the shared subset — not full spec conformance. This page tracks every place
that's known to differ, so you're not surprised by one in production.

## Objects

- **`Object.prototype` only has three methods** — `hasOwnProperty`,
  `toString`, `valueOf`. Real JS also has `isPrototypeOf`,
  `propertyIsEnumerable`, `toLocaleString`, and the legacy `__proto__`
  accessor; none of those are implemented. `toString` always returns
  `"[object Object]"` — there's no per-kind `Symbol.toStringTag`-style
  internal class tag, though in practice this is only reachable for plain
  objects, since `Array`/`Map`/`Set`/`Date`/`RegExp` all define their own
  `toString` that shadows it.
- **`Object(primitiveValue)` returns the primitive unchanged** rather than
  boxing it into a wrapper object — this engine has no boxed-primitive type
  at all. `Object()`/`Object(null)`/`Object(undefined)` (a new empty
  object) and `Object(anObject)` (identity) both match spec.
- **Integrity is whole-object, not per-property.** `Object.freeze` and
  `Object.seal` are enforced — on property assignment, indexed array writes,
  `length`, `delete`, the in-place array mutators (`push`/`pop`/`shift`/
  `unshift`/`reverse`/`fill`/`sort`), and the host's `js_object_set` — and
  because the engine is strict-mode throughout, a refused write throws a
  `TypeError` rather than failing quietly. `Object.isFrozen` and
  `Object.isSealed` report the state. What's missing is anything *finer*:
  there are no per-property attributes here, so there is no
  `writable`/`configurable` distinction and no `Object.defineProperty` to set
  one. That covers everything guest code can observe of an ordinary frozen
  object; it only differs if you were reaching for a partially frozen one.

## Functions

- **There is no `Function` global.** The constructor is left out, as `eval` is, so that
  untrusted code cannot compile code at run time and a runtime-only build needs no compiler.
  `Function.prototype.toString.call(x)` is written `(() => {}).toString.call(x)`.
- **After `delete f.name` or `delete f.length`**, it reads `""` or `0` as in V8, but
  `f.hasOwnProperty(...)` still says `true`.

## Arrays

- **No holes.** Array storage is a flat vector, not a sparse structure —
  every index up to `.length` is a real, materialized slot. `Array(n)` and
  `arr.length = n` both throw `RangeError` past a fixed growth cap rather
  than eagerly allocating an attacker-chosen size; see
  [Built-ins → Array](/guide/builtins#array).
- **`Array.prototype.sort` is a stable O(n²) binary insertion sort** —
  fine for templating-sized arrays; revisit if you're sorting something
  large.
- **`Array.from` supports strings and arrays**, not arbitrary array-likes
  or iterators.

## Numbers

- **Transcendental `Math` functions are fdlibm-accurate (< 1 ulp), not
  correctly rounded.** The kernel is musl's fdlibm-derived code (`src/runtime/
  fdlibm/`), built without fused multiply-add so native and wasm give the same
  bits. V8 now uses LLVM libc's correctly-rounded functions, so about 2% of
  results differ from Node by one ulp; the rest are identical.
  `sqrt`/`floor`/`ceil`/`trunc`/`round` are exact (native ops).
- **Number-to-string formatting matches V8** (the shortest digits that read
  back as the same double, the nearest of those), and parsing is correctly
  rounded, on every host. Printing a number with a very large or very small
  exponent (beyond about 1e±22) is slower than in V8: each candidate is checked
  by parsing it back exactly.

## Strings

**Case mapping (`toUpperCase`/`toLowerCase`) is ASCII-only** — no full
Unicode case folding.

## Regular expressions

`String.prototype.matchAll` returns a plain array rather than a lazy
iterator (`for...of` over the result behaves the same either way).
`String.prototype.split` ignores its `limit` argument.

## Errors

- **No `instanceof`**, so `e instanceof TypeError` is a syntax error;
  test `e.name` or compare `Object.getPrototypeOf(e)` against the
  constructor's `.prototype`.
- **No `stack`**, and no `EvalError`/`URIError`/`AggregateError`.
- **`message` and `cause` are ordinary enumerable own properties** (the
  engine has no property attributes), so `JSON.stringify(new Error('x'))`
  is `{"message":"x"}` rather than `{}`, and `Object.keys` lists them.
- **`Object.prototype.toString` reports `[object Object]`** for errors.
- **Subclassing is not possible** (no classes); set `name` on an instance
  for a custom discriminator.
- **Messages follow V8's wording**, with these differences: an error about
  a feature lamassu leaves out says so (`'var' is not supported; use 'let' or
  'const'`, `classes are not supported`) where V8 would compile the script;
  a few of V8's own quirks are not copied (in a module V8 reports `let let`
  and `let 1x` as `Unexpected strict mode reserved word`, and spreading a
  non-iterable object into a call as `Spread syntax requires
  ...iterable[Symbol.iterator] to be a function`; lamassu says `let is
  disallowed as a lexically bound name`, `Invalid or unexpected token` and
  `o is not iterable`); a callee with no readable name shows its value
  (`undefined is not a function` for `(void 0)()`, where V8 prints the
  expression); and a duplicate label is not an error.
- **Out-of-memory errors may still be bare strings**: when the engine
  cannot allocate an Error object, it throws the message text instead so
  the failure is never lost.

## Promises & async

- **`Promise(executor)` is callable without `new`** — a deliberate
  accommodation, alongside real `new`/constructor-function support and
  prototype chains for everything else.
- **Unhandled promise rejections are silent.** The engine tracks a
  `handled` flag internally but doesn't report unhandled rejections; a host
  hook could be added by an embedder that needs one.
- **A pending top-level-await module** returns from the host's run call
  still pending — see [Async & host calls](/guide/async#top-level-await).

## Modules

- **Named and `export *` re-exports are snapshots**, taken once the source
  module finishes evaluating (dependency order guarantees this has already
  happened). A later mutation of a re-exported *mutable* binding isn't
  observed through the re-export. `export * as ns from` is live by
  reference instead (it aliases the source's namespace object). A module that
  *imports* a re-exported name reads the defining module's live binding, as
  in V8 (this includes `import { x } from 'm'; export { x }`); only the
  re-exporting module's own namespace object holds the snapshot, which is
  `undefined` for a binding whose module, in a cycle, had not run yet. For a
  templating workload this is exact in practice, since re-exports are
  almost always functions or constants.
- **Module evaluation drains the whole microtask queue** after each module
  body runs — there's no cross-module async evaluation ordering beyond
  plain dependency order.

## Not a deviation, but easy to trip on

`for...in`, `var`, classes, `eval`, the `Function` constructor, `Reflect`,
`Proxy`, `Symbol`, generators, typed arrays, and `Intl` aren't "different,"
they're **absent** — see [Supported syntax](/guide/language) for the full
list of non-goals.
