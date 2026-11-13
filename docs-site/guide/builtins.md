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

**Statics**: `keys`, `values`, `entries`, `assign`, `freeze`, `isFrozen`,
`seal`, `isSealed`, `fromEntries`, `hasOwn`, `getPrototypeOf`,
`setPrototypeOf`, `create`.

`Object.create(proto)` makes a plain object whose prototype is `proto` (an object) or none
(`null`); anything else is V8's `TypeError: Object prototype may only be an Object or null`. A
second argument (property descriptors) is a `TypeError`: there are no property attributes to give
the properties it describes.

Own keys come out in the language's order everywhere — `keys`/`values`/
`entries`, spread, `assign` and `JSON.stringify`: array-index names (`"0"`,
`"12"`) first in ascending numeric order, then every other name in the order
it was first set, so `{ body, lid }` lists `body` then `lid`. A deleted and
re-added key moves to the end. An array lists its elements, then any named
properties.

`freeze` and `seal` are enforced, not advisory: writing to a frozen object,
or adding to or deleting from a sealed one, throws a `TypeError`, and so does
giving either a different prototype with `setPrototypeOf` (`#<Object> is not
extensible`). Sealing
fixes the shape while leaving existing properties writable, so
`reverse`/`fill`/`sort` still work on a sealed array while
`push`/`pop`/`shift`/`unshift` don't. Both return their argument, and both
are no-ops on a primitive (which `isFrozen`/`isSealed` therefore report as
frozen and sealed). Integrity is whole-object — there are no per-property
attributes; see [Deviations](/guide/deviations).

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

`toString()`/number-to-string prints what V8 prints: the shortest digits that
read back as the same double, and of those the nearest to its value (ties to
even), across the whole range including subnormals. Parsing a literal or a
string (`Number('…')`) is correctly rounded however many digits it has, by
musl's decimal scanner (`src/runtime/fdlibm/floatscan.c`) rather than the C
library's `strtod`, so every host reads the same digits as the same double.
Transcendental `Math` functions are fdlibm-accurate
(< 1 ulp, musl's fdlibm-derived kernel in `src/runtime/fdlibm/`, no libm) rather
than correctly rounded — `sqrt`/`floor`/`ceil`/`trunc`/`round` are exact native ops.

## Functions

Every function value, a script closure or a native, has `call`, `apply`, `bind` and `toString`:

- `f.call(thisArg, ...args)` calls `f` with `this` set to `thisArg`.
- `f.apply(thisArg, array)` does the same with the arguments in an array (`undefined` or `null`
  for none; any other non-array is a `TypeError`, as there are no array-like objects here).
- `f.bind(thisArg, ...args)` returns a function that calls `f` with that `this` and those arguments
  first; calling it with another `this` (or through `call`) does not change it. `new` on a bound
  function constructs `f` with the preset arguments and ignores the bound `this`.

Modules are strict, so `undefined` and `null` are passed through as they are rather than replaced by
the global object, and arrow functions keep their own `this`.

Every function has V8's `name`, a read-only own property: a declaration's or a named expression's
own name; for an anonymous function or arrow, the name of what it is defined as (`const f = () =>
{}`, `f = function () {}`, `{ f: () => {} }` and `{ f() {} }`, a computed key's string, a default
`f = () => {}`, `export default () => {}` as `default`); a builtin's (`Math.max.name` is `max`);
`bound f` for `f.bind(...)`; otherwise `""`. Assigning it throws `TypeError: Cannot assign to read
only property 'name' of function 'function f() {}'` (a message shows a function as its source
text, as V8's do); `delete f.name` leaves `""`. A named function expression
sees its own name inside its body, as a constant holding the function.

`length` is V8's too, and read-only: a script function's parameters before the first default or
rest (`(a, b = 1, c) => 0` has 1); a builtin's (`Math.max.length` is 2, `Date.UTC.length` 7); for
`f.bind(t, ...args)`, `f.length` less the arguments bound, never below 0. `delete f.length` leaves
0. A native the host registers (`js_register_native`) has length 0.

`f.toString()`, and `String(f)`, a template or a concatenation, give V8's text: a script function's
own source exactly as written, from `function` (or `async`), its first parameter, or its method key
to its last token (`"(a) => a + 1"`, `"m(x) { return x }"`, `"['k']() {}"`); for a builtin or a
host native, `"function max() { [native code] }"` with the name it was made with; for a bound
function or a promise's `resolve`/`reject`, `"function () { [native code] }"`. Calling it on
anything else is V8's `TypeError: Function.prototype.toString requires that 'this' be a Function`.
The engine keeps one copy of each module's source (two bytes a character) for as long as a function
compiled from it is alive, and the bytecode cache carries it too.

A function can hold properties like any object, which is how a constructor gets statics with
classes out of the language: `F.from = (v) => new F(v)`, `F.ORIGIN = new F(0)`, a cache on a helper.
`in`, `delete`, `hasOwnProperty`, `Object.keys`/`values`/`entries`/`assign`/`hasOwn` and
`freeze`/`seal` see them. A builtin constructor's own statics (`Object.keys`, `Array.isArray`) can be
replaced but are not listed by `Object.keys(Object)`, as in other engines. `typeof` a function
is still `"function"`, and `prototype` keeps its special meaning.

## `Boolean`

`Boolean(x)` converts to a primitive boolean. No instance methods table
(booleans have no prototype-method chain in this engine, unlike strings and
numbers).

## `Math`

Constants: `PI`, `E`, `LN2`, `LN10`, `LOG2E`, `LOG10E`, `SQRT2`,
`SQRT1_2`. Functions: `abs`, `floor`, `ceil`, `trunc`, `sign`, `sqrt`,
`cbrt`, `exp`, `log`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`,
`pow`, `min`, `max`, `hypot`, `fround`, `random`.

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

## `Error`

`new Error(message?, { cause }?)` (callable without `new` too), plus
`TypeError`, `RangeError`, `ReferenceError`, and `SyntaxError` with the same
signature and `Error.prototype` in their chain.

- **Instances**: own `message` (ToString of the argument; absent when the
  argument is `undefined`, inheriting `""`), own `cause` when the options
  object has one. As in V8 neither is enumerated: `JSON.stringify(new Error('x'))` is `{}`, and
  `Object.keys`, spread and `Object.assign` leave them out (one deleted and made again, or
  assigned to an error that had none, is ordinary).
- **`Error.prototype`**: `name` (`"Error"`; each subclass prototype
  overrides it), `message` (`""`), `toString` → `"name: message"`, dropping
  either half when it is empty.
- **`stack`**, as V8 gives it: `"TypeError: x\n    at f (main.js:3:9)\n    at main.js:7:1"`.
  The frames are taken when the error is made and the text is built the first time `stack` is
  read, so the first line shows the name and message the error has by then. A frame reads
  `at name (module:line:column)`, or just the location for an anonymous function or a module
  body, with V8's details: `Object.m` / `F.m` for a method (the receiver's type; an inferred
  `x.y` for `x.y = function () {}`), ` [as k]` when it was called under another key, `new C`,
  `async f` for an async function waiting on the one above it, and builtins as
  `at Array.map (<anonymous>)` (no frames for `call`, `apply` or a bound function, nor for a
  host's natives). The column is where V8 puts it: a call at its callee's name (or its `(`), a
  property read at the property, `new` and `await` at the keyword. A function compiled outside
  the module pipeline is in `<anonymous>`. `stack` is an own property that is not enumerated
  (`Object.keys`, `JSON.stringify` and spread do not see it); it can be assigned, even on a
  frozen error, and deleted.
- **`Error.stackTraceLimit`** (10): the frames kept. Anything but a number gives a `stack` that
  reads `undefined`.
- **`Error.captureStackTrace(object, fn?)`** gives `object` a `stack` of the frames running now,
  leaving out `fn`'s call and those above it.
- String conversion (`String(e)`, `'' + e`, template literals) uses the
  same `"name: message"` form, so an error renders identically whether it
  came from `throw new TypeError('x')` or from the engine.
- **Engine-raised errors** (`null.x`, an undefined variable, a bad `Map`
  iterable, the regex step budget, the execution budget, …) are instances of
  the matching subclass, not strings.

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
registered by the CLI (`tools/lamassu.c`) and the npm package's WASM glue
(`src/wasm_api.c`) for convenience, not part of `liblamassu` itself. An
embedder that doesn't register it won't have it.
