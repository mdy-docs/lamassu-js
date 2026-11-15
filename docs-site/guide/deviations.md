# Deviations from real JS

lamassu-js aims to match Node closely enough for differential testing over
the shared subset — not full spec conformance. This page tracks every place
that's known to differ, so you're not surprised by one in production.

## Built-ins V8 has that lamassu leaves out

Checked against Node 26 by listing every own property of each global, constructor and
prototype in both engines (2026-11-15).

**Not yet implemented** (nothing stands in the way; each is queued):

- `Object.defineProperty`, `defineProperties`, `getOwnPropertyDescriptor`,
  `getOwnPropertyDescriptors` for data properties (`value`, `writable`, `enumerable`,
  `configurable`; accessor descriptors are out, below), and with them `Object.preventExtensions`
  and `Object.isExtensible`: all need per-property `writable`/`configurable`, which integrity
  (whole-object `freeze`/`seal`, below) does not have yet.
- `WeakMap`, `WeakSet`, `WeakRef`, `FinalizationRegistry`.
- `String.prototype.normalize` (needs the Unicode normalization tables).
- `Array.fromAsync`.
- `RegExp.prototype.compile` and the legacy `RegExp.$1`–`$9`, `lastMatch`, `input` (and the
  rest) statics, V8's Annex B leftovers.
- `BigInt` (a primitive type of its own, with `10n` literals; see the syntax below).

**Left out by design:**

- The locale methods (`localeCompare`, `toLocaleString` on numbers, dates and arrays,
  `Object.prototype.toLocaleString`, `toLocaleDateString`, `toLocaleTimeString`,
  `toLocaleUpperCase`, `toLocaleLowerCase`): V8 answers them from ICU through `Intl`, a non-goal.
  `Object.prototype.toLocaleString` is left out with them: it would reach `toString` on a number
  (`"1234.5"`) where V8 formats it (`"1,234.5"`), a silent difference.
- Time zones: a `Date` is UTC (`getHours` is `getUTCHours`, `getTimezoneOffset` is `0`), as there
  is no time zone database; V8 uses the host's zone.
- `Object.getOwnPropertySymbols`, and everything else that takes or returns a symbol: there
  are no symbols.
- `__proto__`, `__defineGetter__`, `__defineSetter__`, `__lookupGetter__`, `__lookupSetter__`:
  there are no accessors. For the same reason `size` (`Map`, `Set`) and `source`, `flags`,
  `global` and the other flags (`RegExp`) are read from the instance rather than through an
  accessor on the prototype, so they work as in V8 but
  `Object.getOwnPropertyNames(Map.prototype)` does not list `size`.
- `Function.prototype.constructor`, `arguments` and `caller`: see Functions, below.
- `Error.prepareStackTrace`: see Errors, below.
- `queueMicrotask` and `structuredClone` are the host's (HTML, Node), not V8's; an embedder adds
  them as natives if it wants them.
- `Reflect`, `Proxy`, `Symbol`, typed arrays, `ArrayBuffer`, `SharedArrayBuffer`,
  `DataView`, `Atomics`, `Intl`: see [Supported syntax](/guide/language#explicit-non-goals).

## Syntax V8 accepts that lamassu refuses

Each is a compile error that names the feature (`SyntaxError: tagged templates are not
supported`), never a silent difference.

**Not yet implemented:** object rest in destructuring (`const { a, ...rest } = o`), tagged
templates (`` tag`x${y}` ``), numeric separators (`1_000`), `new.target`, `debugger` (a no-op
without a debugger attached), `import.meta`, import attributes (`with { type: "json" }`),
unicode escapes in identifiers (`\u0061`), `delete a?.b`, and `BigInt` literals.

**Left out by design** (see [Supported syntax](/guide/language#explicit-non-goals)): `var`,
`for...in`, `with`, classes (and with them private names, `#x`), getters and setters in object
literals, generators, sloppy mode, `eval` and the `Function` constructor.

## Objects

- **`Object.prototype` has five methods** — `hasOwnProperty`, `isPrototypeOf`,
  `propertyIsEnumerable`, `toString`, `valueOf`. V8 also has `toLocaleString` (left out with the
  locale methods) and the legacy `__proto__`, `__defineGetter__` (and the rest) accessors (there
  are no accessors). `toString` gives V8's tags
  (`Object.prototype.toString.call(x)` is `[object Array]`, `[object Error]`,
  `[object Null]`, `[object Math]`, `[object AsyncFunction]`, …), and what
  inherits from `Math`, `JSON`, `Promise.prototype` or `AsyncFunction.prototype`
  reports that tag, as V8's own `Symbol.toStringTag` makes it; there is no
  symbol to set another (there are no symbols). `Array.prototype` is a plain object here, not an
  array, so its tag is `[object Object]` (V8: `[object Array]`).
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
  Enumerability is per property, though: what V8 makes non-enumerable (a builtin
  prototype's methods, a function's `prototype.constructor`, an error's `message`)
  is left out of `Object.keys`, JSON, spread and `Object.assign` here too, and
  `Object.getOwnPropertyNames` lists it. Since only part of each builtin is
  implemented, `Object.getOwnPropertyNames(Math)` and the like list fewer names
  than V8's. `Array.prototype` has no `length`, being a plain object, and
  `arguments` (an array here) has no `callee`.

## Functions

- **There is no `Function` global.** The constructor is left out, as `eval` is, so that
  untrusted code cannot compile code at run time and a runtime-only build needs no compiler.
  `Function.prototype.toString.call(x)` is written `(() => {}).toString.call(x)`, and
  `Object.getPrototypeOf(() => {})` is `Function.prototype`. For the same reason
  `Function.prototype` has no `constructor`, and `AsyncFunction.prototype`
  (`Object.getPrototypeOf(async () => {})`) none either. `Function.prototype` has no
  `arguments` or `caller` (in V8, accessors that only throw).
- **After `delete f.name` or `delete f.length`**, it reads `""` or `0` as in V8, but
  `f.hasOwnProperty(...)` still says `true` and `Object.getOwnPropertyNames(f)` still
  lists it.

## Conversions

- **Conversions nest at most 200 deep** (the limit on nested interpreter entries, which guards
  the C stack): `String` of an array nested more than about 200 levels, or a `toString` that
  converts another object that does the same, throws `RangeError: Maximum call stack size
  exceeded`. V8 goes about ten times deeper.
- **A stack trace through an object key's `toString`** (`o[k]`, `k in o`, `delete o[k]`) shows the
  calling frame at its previous call rather than at the access. Keeping that position exact costs
  the hot property path about a quarter of its speed; operators and templates are exact.
- **`regexp.lastIndex = v` converts `v` to a number when it is set**; V8 keeps the value and
  converts it at the next match.
- **`Object.prototype.valueOf.call(5)`** gives `5`, not a `Number` object (no wrapper objects).

## Arrays

- **No holes.** Array storage is a flat vector, not a sparse structure —
  every index up to `.length` is a real, materialized slot. `Array(n)` and
  `arr.length = n` both throw `RangeError` past a fixed growth cap rather
  than eagerly allocating an attacker-chosen size; see
  [Built-ins → Array](/guide/builtins#array).
- **`Array.prototype.sort` is a stable O(n²) binary insertion sort** —
  fine for templating-sized arrays; revisit if you're sorting something
  large.

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

## Iteration

- **There is no `Symbol.iterator`**, so an object of a script's iterates only if it inherits from
  `Iterator.prototype` (whose `[Symbol.iterator]` returns the object itself in V8): an iterable
  that is not its own iterator (one whose `[Symbol.iterator]()` makes a fresh iterator each time)
  cannot be written. `Iterator.from({ next() { ... } })` makes any object with `next` iterable. See
  [Built-ins → Iteration](/guide/builtins#iteration).
- **`for...of`, spread and destructuring of an array or a string do not consult the
  `Array Iterator` or `String Iterator` prototype**: a script that replaces their `next` (or adds a
  `return`) changes what `[].values().next()` does, but not how `for...of` walks an array. (They do
  read `next` from a `Map`, `Set` or other iterator object, as V8 does, so replacing it there takes
  effect.)
- **`Iterator.prototype.constructor` is a plain property**: in V8 it is an accessor whose setter
  refuses an assignment to `Iterator.prototype.constructor` itself (there are no accessors here),
  so that assignment succeeds here.

## Errors

- **Nothing raises `EvalError`**: there is no `eval`. The constructor is
  there, as in V8.
- **`stack` is V8's**, with these differences: no `Error.prepareStackTrace` hook;
  `Error.captureStackTrace` on an object that is not an error makes an ordinary (enumerable)
  `stack` property; an anonymous function's inferred name comes only from a member assignment
  (`x.y = function () {}` is `x.y`, `F.prototype.m = ...` is `F.m`), where V8 also infers from
  other places (`const a = [function () {}]` is `a` there); a host's natives have no frame.
- **Subclassing is not possible** (no classes); set `name` on an instance
  for a custom discriminator.
- **Messages follow V8's wording**, with these differences: an error about
  a feature lamassu leaves out says so (`'var' is not supported; use 'let' or
  'const'`, `classes are not supported`) where V8 would compile the script;
  a few of V8's own quirks are not copied (in a module V8 reports `let let`
  and `let 1x` as `Unexpected strict mode reserved word`; lamassu says `let is
  disallowed as a lexically bound name` and `Invalid or unexpected token`); an
  operand with no readable name shows its value (`undefined is not a function` for `(void 0)()`
  and `#<Object> is not iterable` for `[...{}]`, where V8 prints the expression,
  `{(intermediate value)}` for an object literal); an object named in a message is `#<Object>`
  whatever its kind (V8 says `#<Map>`, `[object Date]`), except an array, a module namespace, an
  iterator (`[object Map Iterator]`) and what inherits from `Iterator.prototype`
  (`[object Object]`), as V8 names them; and a duplicate label is not an error.
- **Out-of-memory errors may still be bare strings**: when the engine
  cannot allocate an Error object, it throws the message text instead so
  the failure is never lost.

## JSON

- **`JSON.stringify` and `JSON.parse` nest at most 200 deep** (their C
  recursion is bounded like the interpreter's nested entries): past that,
  both throw `RangeError: Maximum call stack size exceeded`. V8 goes several
  thousand levels deeper.

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
