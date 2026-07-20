# Built-ins

This is the exact set of globals and methods lamassu-js registers — not an
aspirational list. If something below isn't listed, it isn't implemented;
see [Deviations from real JS](/guide/deviations) for behavioral differences
in the things that *are*.

::: tip Every constructor is callable
`Object`, `Array`, `String`, `Number`, `Boolean`, `Date`, `Map`, `Set`,
`RegExp`, and `Promise` are all callable — `Ctor(...)` and `new Ctor(...)`
both work, and (except for the primitive wrappers) behave identically
either way; see the [C embedding API](/api/c-embedding) note on native
constructors.
:::

## `Object`

`Object()` / `new Object()`: no argument (or `undefined`/`null`) creates a
new empty object; an object/array/function/promise argument is returned
as-is (identity, matching spec). A **primitive** argument is also returned
as-is — real JS would box it into a wrapper object, but this engine has no
boxed-primitive type at all (see `String`/`Number` below), so there's
nothing to box it into.

**Statics**: `keys`, `values`, `entries`, `assign`, `freeze` (returns the
object but does not enforce immutability — see
[Deviations](/guide/deviations)), `fromEntries`, `hasOwn`,
`getPrototypeOf`, `setPrototypeOf`.

**`Object.prototype`**: `hasOwnProperty`, `toString` (`"[object Object]"`),
`valueOf` (returns the object itself). This is the root of *every*
`[[Prototype]]` chain in the engine — a plain object literal's own
`[[Prototype]]` is `Object.prototype`, and `Array.prototype`/
`Date.prototype`/`Map.prototype`/`Set.prototype`/`RegExp.prototype`, every
user-defined constructor's `.prototype`, and even `globalThis` itself all
chain up to it in turn (`Object.getPrototypeOf(Array.prototype) ===
Object.prototype`). `Object.prototype`'s own `[[Prototype]]` is `null`,
matching spec.

## `Array`

`Array(...)` / `new Array(...)`: one numeric argument creates an empty
array of that length; anything else behaves like `Array.of`.

- **Statics**: `isArray`, `of`, `from` (strings and arrays only — not
  arbitrary array-likes or iterators).
- **`Array.prototype`**: `push`, `pop`, `shift`, `unshift`, `at`,
  `indexOf`, `lastIndexOf`, `includes`, `slice`, `join`, `reverse`, `fill`,
  `concat`, `map`, `filter`, `forEach`, `some`, `every`, `find`,
  `findIndex`, `reduce`, `sort`, `flat`, `toString`.

Array elements are a flat `JsValue` vector (no holes/sparse storage), so
`arr.length = n` or `Array(n)` for a very large `n` throws
`RangeError` rather than allocating unboundedly — see
[Deviations](/guide/deviations).

## `String`

`String(x)` / `new String(x)` convert to a primitive string (there is no
boxed `String` object — this engine has no boxed-primitive type at all).

- **Statics**: `fromCharCode`, `fromCodePoint`.
- **`String.prototype`**: `charAt`, `charCodeAt`, `codePointAt`, `at`,
  `indexOf`, `lastIndexOf`, `includes`, `startsWith`, `endsWith`, `slice`,
  `substring`, `substr`, `toUpperCase`, `toLowerCase` (**ASCII-only** case
  mapping), `trim`, `trimStart`, `trimEnd`, `repeat`, `padStart`, `padEnd`,
  `replace`, `replaceAll`, `split`, `concat`, `toString`, `valueOf`, and,
  when regex support is compiled in (the default): `match`, `matchAll`
  (returns an array, not a lazy iterator), `search`.

## `Number`

`Number(x)` converts to a primitive number.

- **Statics**: `isInteger`, `isFinite`, `isNaN`, `isSafeInteger`,
  `parseInt`, `parseFloat`.
- **Constants**: `MAX_SAFE_INTEGER`, `MIN_SAFE_INTEGER`, `MAX_VALUE`,
  `MIN_VALUE`, `EPSILON`, `POSITIVE_INFINITY`, `NEGATIVE_INFINITY`, `NaN`.
- **`Number.prototype`**: `toFixed`, `toString`, `valueOf`.

`toString()`/number-to-string formatting is exact for safe integers and for
magnitudes within ~1e±22; transcendental `Math` functions are ~1e-12
accurate rather than correctly rounded (a custom freestanding kernel, no
libm) — `sqrt`/`floor`/`ceil`/`trunc`/`round` are exact native ops.

## `Boolean`

`Boolean(x)` converts to a primitive boolean. No instance methods table
(booleans have no prototype-method chain in this engine, unlike strings and
numbers).

## `Math`

Constants: `PI`, `E`, `LN2`, `LN10`, `LOG2E`, `LOG10E`, `SQRT2`,
`SQRT1_2`. Functions: `abs`, `floor`, `ceil`, `trunc`, `sign`, `sqrt`,
`cbrt`, `exp`, `log`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`,
`pow`, `min`, `max`, `hypot`, `random`.

## `JSON`

`stringify`, `parse`.

## `Date`

`new Date()`, `new Date(ms)`, `new Date(isoString)`,
`new Date(year, month, day, hours, minutes, seconds, ms)` (the ES5 form —
not the full parser for arbitrary date strings).

- **Statics**: `now`, `UTC`, `parse`.
- **`Date.prototype`**: `getTime`/`setTime`, `valueOf`, the full
  `get`/`set` pair for `FullYear`, `Month`, `Date`, `Day` (get only),
  `Hours`, `Minutes`, `Seconds`, `Milliseconds` (each with a `UTC`-prefixed
  alias — this engine doesn't model timezones, so local and UTC accessors
  return the same thing), `getTimezoneOffset` (`0`, or `NaN` for an invalid
  date), `toISOString`,
  `toJSON`, `toString`, `toDateString`, `toTimeString`, `toUTCString` /
  `toGMTString`.

## `Map` / `Set`

Both accept an iterable (array) to seed from — `new Map([["a", 1]])`,
`new Set([1, 2, 3])` — and expose a synthesized `.size` property (not a
regular own property; it shadows a same-named data property the way
`RegExp.lastIndex` does).

- **`Map.prototype`**: `set`, `get`, `has`, `delete`, `clear`, `forEach`,
  `keys`, `values`, `entries`.
- **`Set.prototype`**: `add`, `has`, `delete`, `clear`, `forEach`,
  `values`, `keys` (alias for `values`, per spec), `entries`.

## `Promise`

`new Promise(executor)` — and, as a deliberate accommodation for this
engine's subset, callable **without** `new` too.

- **Statics**: `resolve`, `reject`, `all`, `race`, `allSettled`.
  `Promise.any` is not implemented.
- **`Promise.prototype`**: `then`, `catch`, `finally`.

See [Async & host calls](/guide/async) for how promises connect to
native/host functions.

## `RegExp`

ECMAScript-flavored, via the bundled
[`baru-re`](https://github.com/mdy-docs/lamassu-js/tree/main/third_party/baru-re)
engine. Regex literals and `new RegExp(pattern, flags)`, the `lastIndex`
protocol, named capture groups, and `/d` (indices) all work.

- **`RegExp.prototype`**: `exec`, `test`, `toString`.
- Consumed by `String.prototype.match`/`matchAll`/`search`/`replace`/
  `replaceAll`/`split` when given a `RegExp` argument. `split` ignores its
  `limit` argument (as the plain string `split` already did).
- Catastrophic backtracking is bounded by an engine-side step budget — see
  [Supported syntax](/guide/language#regular-expressions).

## Global functions

`parseInt`, `parseFloat`, `isNaN`, `isFinite`.

`print(...)` is **not** a language built-in — it's a host-provided native,
registered by the CLI (`tools/jsvm_cli.c`) and the npm package's WASM glue
(`src/wasm_api.c`) for convenience, not part of `libjsvm` itself. An
embedder that doesn't register it won't have it.
