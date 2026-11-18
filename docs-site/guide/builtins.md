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

## Converting objects to primitives

An object, function or promise used where a primitive is needed is converted as V8 converts it
(ToPrimitive): its own or inherited `valueOf` and `toString` are called, `valueOf` first, or
`toString` first where a string is wanted, and the first to return a primitive wins. Neither
giving one is `TypeError: Cannot convert object to primitive value`, and what either throws
propagates. That covers every conversion: `+` and the other operators, comparisons, `==`,
templates, `String(x)` and `Number(x)`, a computed key (`o[k]`, `k in o`, `delete o[k]`, `{ [k]: v }`),
and every builtin argument that is converted (`'abc'.slice(x)`, `Math.max(x)`, `new Date(x)`,
`arr.join(sep)`, `new Error(message)`, …).

- The hint is V8's: string for `String(x)`, templates and keys; number for arithmetic and `<`;
  the default (as number) for `+` and `==`, except a `Date`, which takes the string hint there.
- A computed key converts on each use (`o[k] += 1` converts `k` twice, as V8), after the
  right-hand side, and not at all when the base is `null` or `undefined` (the TypeError comes
  first).
- `Array.prototype.join` (so `String(array)`) skips an array it is already joining further out:
  a cyclic array joins its cycle as `""`.
- Conversions nest on the C stack (an element's `toString` converting another array), so they share
  the limit on nested interpreter entries: past 200 levels they throw `RangeError: Maximum call
  stack size exceeded`, where V8 goes about ten times deeper.

## `Object`

`Object()` / `new Object()`: no argument (or `undefined`/`null`) creates a
new empty object; an object/array/function/promise argument is returned
as-is (identity, matching spec). A **primitive** argument is also returned
as-is — real JS would box it into a wrapper object, but this engine has no
boxed-primitive type at all (see `String`/`Number` below), so there's
nothing to box it into.

**Statics**: `keys`, `values`, `entries`, `assign`, `freeze`, `isFrozen`,
`seal`, `isSealed`, `preventExtensions`, `isExtensible`, `defineProperty`, `defineProperties`,
`getOwnPropertyDescriptor`, `getOwnPropertyDescriptors`, `fromEntries`, `hasOwn`,
`getOwnPropertyNames`, `getPrototypeOf`, `setPrototypeOf`, `create`, `is` (SameValue: `NaN` is `NaN`, `0` is not `-0`), `groupBy(items,
fn)` (a prototype-less object of arrays, keyed by what `fn(value, index)` returns as a property
key; `items` any iterable, closed if `fn` throws).

`Object.getOwnPropertyNames(o)` lists every own key, enumerable or not, in V8's order: a string's
indices then `length` (a number or boolean has none); an array's indices, `length`, then its named
properties; a function's `length`, `name`, `prototype` when it is a constructor, then a builtin's
statics and what a script wrote onto it; an error's `stack`, `message`, `cause`; a regexp's
`lastIndex`. `Object.hasOwn` and `hasOwnProperty` answer `true` for exactly those keys.
`Object.keys`/`values`/`entries` list a string's indices and characters, and they, `hasOwn` and
`getOwnPropertyNames` throw V8's `TypeError: Cannot convert undefined or null to object` for
`undefined` or `null`. On a module namespace, `getOwnPropertyNames` lists an export that has not
run yet, while `hasOwn` reads it and throws, as in V8.

`Object.setPrototypeOf(v, proto)` works on any object, function or promise, as in V8: a
function given another prototype inherits from it instead of `Function.prototype` (statics
through `Object.setPrototypeOf(B, A)`, or none with `null`), and a function or promise can be
another object's prototype (`Object.create(f)` reads `f`'s properties, `name` and statics
included). A chain may not loop (`TypeError: Cyclic __proto__ value`), and a sealed or frozen
value keeps its prototype. `instanceof` and `Object.getPrototypeOf` follow such chains. A
function from `bind` starts with its target's prototype. An object literal names its prototype
with `{ __proto__: proto }` ([Language](/guide/language)). `Object.prototype` keeps its `null`
prototype (`TypeError: Immutable prototype object 'Object.prototype' cannot have their prototype
set`).

`Object.prototype.__proto__` is V8's accessor. `v.__proto__` is `Object.getPrototypeOf(v)` for
any value but `null` and `undefined` (a primitive gives its type's prototype), and
`o.__proto__ = p` sets the prototype of an object, function or promise and makes no property:
`p` is an object, a function or `null`, any other value is ignored, and so is an assignment on
a primitive. It throws what `setPrototypeOf` throws (`Cyclic __proto__ value`,
`#<Object> is not extensible`). Every way of assigning goes through it (`o['__proto__'] = p`,
a computed key, `Object.assign`), so **copying untrusted keys onto an object with `t[k] = v`
can change `t`'s prototype**, exactly as in V8; what *defines* a property makes an own one named
`__proto__` instead: `JSON.parse`, `Object.fromEntries`, `Object.groupBy`, spread and rest, a
computed key in a literal, `Object.defineProperty`, and a regexp's named `groups` (which have
no prototype). An object without `Object.prototype` on its chain (`Object.create(null)`, or
after `o.__proto__ = null`) has no `__proto__`, and an own property of that name shadows it.
Its descriptor is `{ get, set, enumerable: false, configurable: true }` with the functions
`get __proto__` and `set __proto__`; `delete Object.prototype.__proto__` removes it, after
which the name is an ordinary property everywhere, and a frozen `Object.prototype` leaves it
working. At the top level a bare `__proto__` is the global object's, as any name missing from
the global scope is looked up on the global object's prototype chain (`typeof toString` is
`"function"`).

`Object.create(proto, props)` makes a plain object whose prototype is `proto` (an object, function
or promise) or none (`null`); anything else is V8's `TypeError: Object prototype may only be an
Object or null`. `props` describes its properties, as `Object.defineProperties` takes them.

`Object.defineProperty(o, key, descriptor)` and `defineProperties` take data descriptors (`value`,
`writable`, `enumerable`, `configurable`, each looked up through the prototype chain, as V8 reads
them; a missing one is `false` on a new property and unchanged on an existing one) and validate
them as V8 does (`TypeError: Cannot redefine property: x` for a change a non-configurable property
refuses; `Cannot define property x, object is not extensible`). A `get` or `set` must be a builtin's accessor function (the `get` or `set` of a
descriptor `getOwnPropertyDescriptor` returned, such as `Map.prototype.size`'s): a function of the
script's is a `TypeError`, as a script cannot write a getter or setter (see
[Deviations](/guide/deviations)). `getOwnPropertyDescriptor(o, key)` returns `{ value, writable,
enumerable, configurable }` (`{ get, set, enumerable, configurable }` for an accessor) or
`undefined`, and `getOwnPropertyDescriptors(o)` one for each key
`getOwnPropertyNames` lists. Redefining a function's `name` or `length` works as in V8 (it is
configurable); `prototype` can be given a value or made read-only. What lamassu cannot store
(attributes on an array element, a read-only array `length`) is a
`TypeError`; see [Deviations](/guide/deviations).

`Object.assign` writes each property with `[[Set]]`, as V8: onto a read-only property, a frozen or
non-extensible object, or a module namespace it throws at the first write it cannot make, having
made the ones before it.

Own keys come out in the language's order everywhere — `keys`/`values`/
`entries`, spread, `assign` and `JSON.stringify`: array-index names (`"0"`,
`"12"`) first in ascending numeric order, then every other name in the order
it was first set, so `{ body, lid }` lists `body` then `lid`. A deleted and
re-added key moves to the end. An array lists its elements, then any named
properties.

`freeze` and `seal` are enforced, not advisory: writing to a frozen object,
or adding to or deleting from a sealed one, throws a `TypeError` (deleting a
property it does not have is `true`, as in V8), and so does
giving either a different prototype with `setPrototypeOf` (`#<Object> is not
extensible`). Sealing
fixes the shape while leaving existing properties writable, so
`reverse`/`fill`/`sort` still work on a sealed array while
`push`/`pop`/`shift`/`unshift` don't. `preventExtensions` only stops new properties (an array
can still `pop`, not `push`). All three return their argument and are no-ops on a primitive
(which `isFrozen`/`isSealed` report as frozen and sealed, and `isExtensible` as not extensible).
`isFrozen`/`isSealed` are V8's: an object made non-extensible whose properties are all
non-configurable (and read-only) is sealed (frozen) however it got there, so an empty sealed
object is frozen too. `Math.PI` and the other `Math` and `Number` constants, and the globals
`undefined`, `NaN` and `Infinity`, are read-only and not configurable, as in V8.

**`Object.prototype`**: `hasOwnProperty`, `isPrototypeOf` (`false` for a primitive argument),
`propertyIsEnumerable` (an own key that enumeration lists: an array's or string's index, not
`length`; not a builtin's methods or statics, a function's `name`/`length`/`prototype`, an error's
`stack`), `toString` (`"[object Object]"`, and V8's tag for
anything else: `Object.prototype.toString.call([])` is `"[object Array]"`, an error's
`"[object Error]"`, `null`'s `"[object Null]"`),
`valueOf` (returns the object itself; a `TypeError` on `null`/`undefined`). This is the root of *every*
`[[Prototype]]` chain in the engine — a plain object literal's own
`[[Prototype]]` is `Object.prototype`, and `Array.prototype`/
`Date.prototype`/`Map.prototype`/`Set.prototype`/`RegExp.prototype`, every
user-defined constructor's `.prototype`, and even `globalThis` itself all
chain up to it in turn (`Object.getPrototypeOf(Array.prototype) ===
Object.prototype`). `Object.prototype`'s own `[[Prototype]]` is `null`,
matching spec. `Object.getPrototypeOf` of a function is `Function.prototype` (of `TypeError` and
the other error constructors, `Error`; of an async function `AsyncFunction.prototype`, a plain
object that inherits from `Function.prototype`), of a number `Number.prototype`, of a string
`String.prototype` and of a promise `Promise.prototype`. `Function.prototype` is itself a function,
as in V8: callable with anything, returning `undefined`, not a constructor, with `length` 0 and
`name` `""`, its own properties `apply`, `bind`, `call` and `toString`, and its prototype
`Object.prototype`.

## `Array`

`Array(...)` / `new Array(...)`: one numeric argument creates an empty
array of that length; anything else behaves like `Array.of`.

- **Statics**: `isArray`, `of`, `from` (any iterable, else an array-like's `[0, length)`; with
  `mapFn(value, index)` and its `thisArg`), `fromAsync` (the same, as a promise: each value
  awaited, and each `mapFn` result; a rejection or a throwing `mapFn` closes the iterator).
- **`Array.prototype`**: `push`, `pop`, `shift`, `unshift`, `splice`, `at`, `with`,
  `indexOf`, `lastIndexOf`, `includes` (SameValueZero, so `[NaN].includes(NaN)`), `slice`, `join`,
  `reverse`, `toReversed`, `fill`, `copyWithin`, `concat`, `map`, `filter`, `forEach`, `some`,
  `every`, `find`, `findIndex`, `findLast`, `findLastIndex`, `reduce`, `reduceRight`, `sort`,
  `toSorted`, `toSpliced`, `flat`, `flatMap`, `toString` (generic, as V8's: `this.join()` when
  `join` is a function, else `Object.prototype.toString`), and `keys`, `values`, `entries`, which
  return an `Array Iterator` ([Iteration](#iteration)) that reads the array live (an element
  pushed before it reaches the end is visited) and works on any array-like, as V8's does.
  `sort` and `toSorted` are V8's TimSort (as in V8 14.6, Node 26): stable, O(n log n), and the
  comparator is called with the same pairs in the same order, so a comparator that logs, or one
  that is not a consistent ordering (`(a, b) => a > b`), gives what it gives in Node. `undefined`
  elements are never compared and go last. The elements are sorted aside and written back at the
  end, so a comparator that changes the array does not disturb the sort, and a frozen array is
  refused only after its comparator has run, as in V8.
- The callback methods read the length once, as V8's do: an element the callback appends is not
  visited, and one it removes is skipped (`find`, `findIndex`, `findLast` and `findLastIndex` see
  `undefined` there; `map` leaves `undefined` where V8 leaves a hole). `splice` and `copyWithin`
  move and delete as the spec does, so a sealed or frozen array refuses them at the same write
  V8 does, with V8's message, and keeps the writes made before it.
- **The methods are generic**, as V8's are. On a receiver that is not an array
  (`Array.prototype.push.call(obj, v)`, a method assigned to another object, an object that
  inherits from an array or from `Array.prototype`, a function, a string) they take the
  specification's steps: `length` through `ToLength` (so `'2'` is 2, a missing or negative one
  0, and it may be as large as 2<sup>53</sup> - 1), then the same reads, writes and deletes of
  the receiver's indices in the same order, with the arguments converted where V8 converts
  them. The errors are V8's, at the write V8 makes them: `Cannot add property 2, object is not
  extensible`, `Cannot assign to read only property 'length' of object '[object Object]'`, a
  length past 2<sup>53</sup> - 1, and for `undefined` or `null`
  `Array.prototype.push called on null or undefined`. A primitive receiver is read as V8 reads
  its wrapper object (a string has its characters and its `length`; a number, boolean or BigInt
  has what its prototype has, which is nothing unless a script put it there), and cannot be
  written (`Cannot assign to read only property '0' of object '[object String]'`). `concat`
  spreads only arrays, as there is no `Symbol.isConcatSpreadable`: any other receiver or
  argument is one element. See [Deviations](/guide/deviations#arrays) for the two places the
  result differs: the copy has no holes, and a primitive receiver stays a primitive.
- **The result of `map`, `filter`, `slice`, `splice`, `concat`, `flat` and `flatMap` is the
  receiver's `constructor`'s to make**, as in V8 (`ArraySpeciesCreate`), when the receiver is an
  array. The property is read as any other, on the array and then up its prototype chain, so
  `arr.constructor = C`, a prototype of the script's between the array and `Array.prototype`,
  and `Array.prototype.constructor = C` all count. `undefined`, `Array`, and any function or
  object that does not inherit from a builtin constructor give an ordinary array, without
  being called; a constructor that inherits from `Array` (`Object.setPrototypeOf(C, Array)`)
  is called as `new C(length)`, before the first callback, with the length V8 passes (`map`
  the receiver's, `slice` and `splice` the count of elements, the others 0), and whatever it
  answers has the elements defined on it as properties; `slice`, `splice` and `concat` then set
  its `length`, the others do not. A result that refuses them throws V8's errors
  (`Cannot define property 0, object is not extensible`, `Cannot redefine property: 0`,
  `Cannot assign to read only property 'length' of object '#<Object>'`), and a `constructor`
  that is `null` or a primitive is
  `TypeError: object.constructor[Symbol.species] is not a constructor`. A receiver that is not
  an array always gives an array, and `toSorted`, `toSpliced`, `toReversed` and `with` never
  ask. [Deviations](/guide/deviations#conversions) has what stands in for `Symbol.species`.
- **`Array.from` and `Array.of` build on `this`** when it is a constructor other than `Array`
  (`Array.from.call(C, items)`, or the function assigned to a constructor of the script's):
  `new C()` for an iterable and `new C(length)` for an array-like and for `of`, then each
  element defined as a property and `length` set, as in V8; a failure closes the iterator. Any
  other `this` (`const from = Array.from`, an arrow function, an object) gives an array.
- `flat` and `flatMap` go as deep as they are asked, whatever the nesting; an array that holds
  itself, flattened without a limit, is `RangeError: Maximum call stack size exceeded` as in V8
  (here after 2000 levels). `join` (and `toString`) of more than 2<sup>26</sup> code units, the
  longest string there is, is `RangeError: Invalid string length`.

Array elements are a flat `JsValue` vector (no holes/sparse storage), so
`arr.length = n` or `Array(n)` for a very large `n` throws
`RangeError` rather than allocating unboundedly — see
[Deviations](/guide/deviations).

## `String`

`String(x)` / `new String(x)` convert to a primitive string (there is no
boxed `String` object — this engine has no boxed-primitive type at all).

- **Statics**: `fromCharCode`, `fromCodePoint`, `raw` (``String.raw`a\n${x}` ``, or
  `String.raw({ raw: [...] }, ...subs)`).
- **`String.prototype`**: `charAt`, `charCodeAt`, `codePointAt`, `at`,
  `indexOf`, `lastIndexOf`, `includes`, `startsWith`, `endsWith`, `slice`,
  `substring`, `substr`, `toUpperCase`, `toLowerCase` (full Unicode case mapping, as V8's),
  `trim`, `trimStart`, `trimEnd` (and `trimLeft`/`trimRight`, the same two functions),
  `repeat`, `padStart`, `padEnd`, `isWellFormed`, `toWellFormed` (each lone surrogate
  U+FFFD), `normalize` (`NFC`, the default, `NFD`, `NFKC`, `NFKD`, on Unicode 17.0's data as
  V8's ICU has it; checked against V8 for every code point and against Unicode's
  `NormalizationTest.txt`), `replace`, `replaceAll`, `split`, `concat`, `toString`, `valueOf`, V8's legacy HTML
  methods (`anchor`, `big`, `blink`, `bold`, `fixed`, `fontcolor`, `fontsize`, `italics`,
  `link`, `small`, `strike`, `sub`, `sup`), and,
  when regex support is compiled in (the default): `match`, `matchAll` (a lazy
  `RegExp String Iterator`, one search per step, starting at the regexp's `lastIndex`), `search`.
- Called on `undefined` or `null` (`String.prototype.trim.call(null)`), each throws V8's
  `TypeError: String.prototype.trim called on null or undefined`.
- **`replace`/`replaceAll` with a string pattern** take a replacement string
  (`$$`, `$&`, `` $` `` and `$'` expand; `$1` and `$<name>` stay literal, as
  there are no groups) or a function, called as `fn(match, position, string)`
  for each match, whose result is converted to a string.

## `Number`

`Number(x)` converts to a primitive number.

- **Statics**: `isInteger`, `isFinite`, `isNaN`, `isSafeInteger`,
  `parseInt`, `parseFloat`.
- **Constants**: `MAX_SAFE_INTEGER`, `MIN_SAFE_INTEGER`, `MAX_VALUE`,
  `MIN_VALUE`, `EPSILON`, `POSITIVE_INFINITY`, `NEGATIVE_INFINITY`, `NaN`.
- **`Number.prototype`**: `toFixed`, `toExponential`, `toPrecision`, `toString`, `valueOf`
  (`toFixed`, `toExponential` and `toPrecision` from the double's exact decimal value, rounded
  half up, as the spec and V8 have them: `(0.1).toFixed(20)` is `"0.10000000000000000555"`,
  `(1.005).toFixed(2)` is `"1.00"`; the same object `(5).constructor`
  and `Object.getPrototypeOf(5)` lead to; likewise `String.prototype`). Their receiver must be a
  number (`Number.prototype.toString requires that 'this' be a Number`), as `String.prototype`'s
  `toString`/`valueOf` need a string; the prototypes themselves stand for `0` and `""`, as V8's
  wrapper objects do. These tables, and functions' and promises', go on to `Object.prototype`
  (`(1).hasOwnProperty`, `f.valueOf`), and answer for any key the primitive does not have
  itself, an index included: `(5)[0]` and `'ab'[7]` are `undefined` unless
  `Number.prototype[0]` or `String.prototype[7]` has been set.

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
- `f.apply(thisArg, list)` does the same with the arguments in an array, or in any object with
  a `length`: its `[0, length)`, read as V8 reads them (`undefined` or `null` for none; a
  primitive is `TypeError: CreateListFromArrayLike called on non-object`).
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
replaced or deleted (then gone, as in V8) but are not listed by `Object.keys(Object)`, as in
other engines; `hasOwn` and `Object.getOwnPropertyNames(Object)` see them. `typeof` a function
is still `"function"`.

`prototype` is V8's. A constructor has it as its own property: a `function` declaration or
expression, a builtin constructor (`Map`, `Error`, `Number`, ...), a host's native. It starts as an
object whose `constructor` is the function, and `in`, `hasOwnProperty` and `Object.hasOwn` see it;
`Object.keys` does not, and `constructor` is not enumerable either (`Object.keys(F.prototype)` is
`[]`). A script may give it any value: after `F.prototype = 5`, `F.prototype` is 5 and `new F()`
makes an object whose prototype is `Object.prototype`, while `instanceof F` throws V8's
`TypeError: Function has non-object prototype '5' in instanceof check`. It cannot be deleted
(`TypeError: Cannot delete property 'prototype' of function F() {}`), and a builtin
constructor's, or a frozen function's, is read-only. Every other function — an arrow, an object
literal's method (`{ m() {} }`), an async function, a builtin function such as `Math.max`, a
function from `bind()` — is no constructor: it has no `prototype`, `new` on it throws
`TypeError: m is not a constructor`, and `prototype` is an ordinary property a script may add. A
function from `bind()` constructs its target. A constructor's return value replaces the new object
when it is an object (a function or a promise counts).

The properties the engine makes on the global object, the builtin prototypes, `Math` and `JSON`
are not enumerable (`Object.keys(Map.prototype)` and `Object.keys(globalThis)` are `[]`), as in
V8; ones a script adds are. Assigning one keeps it so (replacing `Map.prototype.get` leaves it
unlisted); deleting it and making it again gives an ordinary one.

## `BigInt`

As in V8: a primitive of its own (`typeof 10n` is `"bigint"`), arbitrary precision (up to V8's
2^30 bits: `RangeError: Maximum BigInt size exceeded`). Literals in every base (`10n`, `0xFFn`,
`0b101n`, `1_000n`). Every operator: `+ - * / % **` (division truncates; `RangeError: Division by
zero`, `Exponent must be positive`), `& | ^ ~ << >>` (as infinite two's complement; `>>>` is
`TypeError: BigInts have no unsigned right shift, use >> instead`), unary `-`, `++`/`--`, while
unary `+` is `TypeError: Cannot convert a BigInt value to a number`, and mixing a BigInt with a
number is `TypeError: Cannot mix BigInt and other types, use explicit conversions` (`+` with a
string concatenates). `==` and `<` compare a BigInt with a number or a numeric string by value
(`1n == 1`, `2n > 1.5`); `===` does not (`1n === 1` is false); `0n` is falsy. As `Map`/`Set`
keys, in `includes` and `Object.is`, equal BigInts are the same key. As a property key a BigInt
is its decimal string, written as a literal or computed: `{ 5n: 1 }` has the key `"5"`, as does
the pattern `{ 5n: x }`, and `list[1n]` is `list[1]`.

- **`BigInt(value)`**: a number must be an integer (`RangeError: The number 1.5 cannot be
  converted to a BigInt because it is not an integer`); a string is parsed as V8 does (`0x`, `0o`,
  `0b`, surrounding white space; `SyntaxError: Cannot convert x to a BigInt`); `new BigInt` is a
  `TypeError`. **Statics**: `asIntN(bits, n)`, `asUintN(bits, n)`.
- **`BigInt.prototype`**: `toString(radix)`, `valueOf` (`toLocaleString` is left out with the
  locale methods).
- `Number(n)` converts, rounding to nearest; `JSON.stringify` refuses one (`TypeError: Do not know
  how to serialize a BigInt`) unless a `toJSON` says otherwise; `Math` functions refuse one.

## `Boolean`

`Boolean(x)` converts to a primitive boolean.

- **`Boolean.prototype`**: `toString`, `valueOf` (receiver a boolean, or the prototype itself,
  which stands for `false`); `Object.getPrototypeOf(true)` is it.

## `Math`

Constants: `PI`, `E`, `LN2`, `LN10`, `LOG2E`, `LOG10E`, `SQRT2`,
`SQRT1_2`. Functions: `abs`, `floor`, `ceil`, `trunc`, `sign`, `sqrt`,
`cbrt`, `exp`, `expm1`, `log`, `log1p`, `log2`, `log10`, `sin`, `cos`, `tan`, `asin`, `acos`,
`atan`, `atan2`, `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`, `pow`, `min`, `max`, `hypot`,
`fround`, `f16round`, `clz32`, `imul`, `random`.

## `JSON`

`parse`, `stringify`, `rawJSON`, `isRawJSON`, as V8 has them.

- `JSON.stringify(value, replacer, space)` calls a value's `toJSON(key)` (a `Date` gives its ISO
  string), then a replacer function with the holder as `this` (`{"": value}` first) and
  `(key, value)`, or keeps only the keys a replacer array lists (its strings and numbers, once
  each). `space` indents by up to 10 spaces or the first 10 characters of a string. Values are
  read as they are reached, so a `toJSON` or replacer that changes what is still to come is seen,
  and keys are those the object had when it was reached. A lone surrogate is written as `\udXXX`.
  A cycle throws V8's `TypeError`, naming the path: `Converting circular structure to JSON
  --> starting at object with constructor 'Object' | property 'a' -> ... --- property 'c' closes
  the circle`.
- `JSON.parse(text, reviver)` calls the reviver for each member, innermost first, with the holder
  as `this` and `(key, value, context)`; what it returns replaces the member, and `undefined`
  deletes it. `context.source` is a primitive's source text (`"1.50"`, `"\"x\""`) while the
  value is still the one parsed there, as V8 gives it; for an object, or a value a reviver has
  changed, `context` is `{}`.
- `JSON.rawJSON(text)` makes a frozen, prototype-less object with `rawJSON: text`, which
  `JSON.stringify` writes as it is (`JSON.stringify({ n: JSON.rawJSON('1e1000') })` is
  `{"n":1e1000}`); `text` must be one JSON primitive with nothing around it. `JSON.isRawJSON`
  tells such an object.

## `Date`

`new Date()`, `new Date(ms)`, `new Date(isoString)`,
`new Date(year, month, day, hours, minutes, seconds, ms)` (the ES5 form —
not the full parser for arbitrary date strings). `Date()` without `new` is the time now as a
string, whatever it is given.

- **Statics**: `now`, `UTC`, `parse`.
- **`Date.prototype`**: `getTime`/`setTime`, `valueOf`, the full
  `get`/`set` pair for `FullYear`, `Month`, `Date`, `Day` (get only),
  `Hours`, `Minutes`, `Seconds`, `Milliseconds` (each with a `UTC`-prefixed
  alias — this engine doesn't model timezones, so local and UTC accessors
  return the same thing), `getYear`/`setYear` (V8's legacy two-digit years),
  `getTimezoneOffset` (`0`, or `NaN` for an invalid date), `toISOString`,
  `toJSON`, `toString`, `toDateString`, `toTimeString`, `toUTCString` /
  `toGMTString`. `toJSON` takes any receiver, as the specification has it: `null` when the
  receiver converts to a number that is not finite, and otherwise whatever the `toISOString` it
  finds on the receiver returns, so a replaced `Date.prototype.toISOString` or `valueOf` is what
  `JSON.stringify` of a date goes through.

## `Error`

`new Error(message?, { cause }?)` (callable without `new` too), plus
`TypeError`, `RangeError`, `ReferenceError`, `SyntaxError`, `EvalError` and `URIError` with the
same signature and `Error.prototype` in their chain, and
`new AggregateError(errors, message?, { cause }?)`, whose own `errors` is a new array of what
`errors` iterates (see [Iteration](#iteration); anything else is V8's `TypeError`).
Each subclass constructor's `[[Prototype]]` is `Error`, so `TypeError.captureStackTrace` is
`Error.captureStackTrace`. `Error.isError(v)` is whether `v` was made by an error constructor
(an object that merely inherits from `Error.prototype` is not). Each error prototype has its own
`name` and `message` (`""`).

- **Instances**: own `message` (ToString of the argument; absent when the
  argument is `undefined`, inheriting `""`), own `cause` when the options
  object has one, and an `AggregateError`'s own `errors`. As in V8 none is enumerated: `JSON.stringify(new Error('x'))` is `{}`, and
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

Both need `new` (`Map()` is `TypeError: Constructor Map requires 'new'`, as in V8). Both accept
any iterable to seed from — `new Map([["a", 1]])`, `new Map(otherMap)`, `new Set("abc")` — each
value going through the collection's own `set` or `add`, as V8 does (a Map's entries can be any
object with `0` and `1`). `size` is the accessor `Map.prototype.size` (`Set.prototype.size`), as
in V8: not an own property of the collection, and a `TypeError` on a receiver that is not one
(`Method get Map.prototype.size called on incompatible receiver #<Set>`).

- **`Map`**: `Map.groupBy(items, fn)` (a Map of arrays, keyed by what `fn(value, index)`
  returns). **`Map.prototype`**: `set`, `get`, `has`, `delete`, `clear`, `forEach`,
  `keys`, `values`, `entries`, `getOrInsert(key, value)`, `getOrInsertComputed(key, fn)`.
- **`Set.prototype`**: `add`, `has`, `delete`, `clear`, `forEach`,
  `values`, `keys` (the same function as `values`, per spec), `entries`, and the set methods
  `union`, `intersection`, `difference`, `symmetricDifference`, `isSubsetOf`, `isSupersetOf`,
  `isDisjointFrom`. Their argument is anything with a numeric `size` and callable `has` and
  `keys` (a `Set`, a `Map`, or an object of a script's), used as V8 uses it: `has` per entry or
  `keys()` iterated depending on which set is smaller, and the iterator closed when the answer
  comes early.
- `keys`, `values` and `entries` return iterators (`Map Iterator`, `Set Iterator`) with `next()`,
  and `forEach` and `for...of` walk the collection the same way. They are live, as V8's are: an
  entry added before the iterator reaches the end is visited, one deleted before it is reached is
  not, and `clear()` sends it back to the start of whatever is added next. A done iterator stays
  done.

## `WeakMap` / `WeakSet` / `WeakRef` / `FinalizationRegistry`

As in V8. Each needs `new`. A key, a target or an unregister token must be an object (a function
or a promise counts): anything else is V8's `TypeError` (`Invalid value used as weak map key`,
`Invalid value used in weak set`, `WeakRef: invalid target`, `Invalid unregisterToken ('1')`).

- **`WeakMap.prototype`**: `get`, `set`, `has`, `delete`, `getOrInsert`, `getOrInsertComputed`;
  `new WeakMap(iterable)` takes entries as `Map` does. A value lives while its key does and no
  longer: a value that refers to its own key does not keep it (an ephemeron).
- **`WeakSet.prototype`**: `add`, `has`, `delete`.
- **`WeakRef.prototype.deref()`**: the target, or `undefined` once it has been collected. A target
  stays alive to the end of the job that made the `WeakRef` or called `deref` (as V8's does), so
  `deref()` twice in one job gives the same answer.
- **`FinalizationRegistry(callback)`**: `register(target, held, token)` (`held` may not be the
  target), `unregister(token)` (true when it removed any). After a target is collected,
  `callback(held)` runs once the job queue is empty; see [Deviations](/guide/deviations).

None is enumerable or has a size: there is no way to list what a weak collection holds.

## Iteration

There is no `Symbol`, so what is iterable is the engine's own: strings (by code point), arrays,
`Map`s (their entries), `Set`s (their values), every iterator object (each is its own iterable,
so `[...map.keys()]` works and a part-consumed iterator spreads what is left), and **any object
that inherits from `Iterator.prototype`**, which is how a script makes its own iterator: in V8
`Iterator.prototype[Symbol.iterator]` returns the object itself, so its `next` is what steps it.
An object that inherits from `Array.prototype` without being an array (`Object.create([1, 2])`)
is iterated as V8 iterates it, by `Array.prototype.values` over its `length` and indices; an
array as a prototype passes on its elements and `length` like any other property.

```js
const countdown = Object.create(Iterator.prototype);
let n = 3;
countdown.next = () => (n > 0 ? { value: n--, done: false } : { value: undefined, done: true });
[...countdown];                        // [3, 2, 1]
Iterator.from({ next() { /* ... */ } }) // any object with next(), as an Iterator
```

`for...of`, spread, array destructuring, `Array.from`, `Object.fromEntries`, the `Map`/`Set`
constructors, `Promise.all`/`race`/`allSettled`/`any`, `AggregateError` and `Iterator.concat` all
accept exactly these, and follow V8's iterator protocol: `next` is read once when iteration starts,
each result must be an object (`Iterator result 1 is not an object`), `done` then `value` are read
from it, and an iterator that stops early is **closed** — its `return()` is called when `for...of`
is left by `break`, `return`, a labelled `continue` or a throw, when destructuring takes fewer values
than the iterator has (or a default or a target throws part way), and when a native's callback
throws (`Array.from`'s map function, a Map's `set`). A throw from `next()` itself closes nothing.
Destructuring steps the iterator only as far as the pattern needs, and an assignment pattern's
target (`[o.x] = it`) is evaluated before the iterator steps, as in V8.

An array and a string follow the same protocol, through the `Array Iterator` and the
`String Iterator` (their prototypes are `Object.getPrototypeOf([].values())` and
`Object.getPrototypeOf(Iterator.from(''))`). A `next` a script puts on one of them is what steps
`for...of`, spread, destructuring and the natives above over every array or string from then on
(a loop already running keeps the `next` it started with), and a `return` put there, on
`Iterator.prototype` or on `Object.prototype` is called, on an iterator standing where the
iteration stopped, whenever one stops early. As in V8, `Object.groupBy` and `Map.groupBy` are the
exception: over an array that still has `Array.prototype` and the built-in `next`, a callback
that throws closes nothing. While the two prototypes are as the engine made them, no iterator
object is made for an array or a string.

The iterators' prototypes (`Map Iterator`, `Set Iterator`, `Array Iterator`, `String Iterator`,
`RegExp String Iterator`, `Iterator Helper`) chain to `Iterator.prototype`, which has V8's
**Iterator helpers**:

- **lazy**, returning an `Iterator Helper` (with `next` and `return`): `map(fn)`, `filter(fn)`,
  `take(n)`, `drop(n)`, `flatMap(fn)` (each result an iterable or an iterator, not a string);
- **eager**: `reduce(fn[, initial])`, `toArray()`, `forEach(fn)`, `some(fn)`, `every(fn)`,
  `find(fn)`;
- **statics**: `Iterator.from(value)` (an iterable's iterator, a string's code points, or an object
  with `next` wrapped so that it has the helpers) and `Iterator.concat(...iterables)`.

Callbacks get `(value, index)` (`reduce` gets the accumulator first). The helpers work on any
object with a `next` method (`Iterator.prototype.toArray.call({ next() { ... } })`), close the
underlying iterator when they stop early (`take`, `some`, `every`, `find`, a callback that throws,
`return()` on the helper), and throw V8's errors (`Generator is already running` for a helper
re-entered from its own callback, `-1 must be positive` for `take(-1)`). `Iterator` itself is
abstract: `new Iterator()` throws, as it does in V8 without a subclass (and there are no classes).

## `Promise`

`new Promise(executor)` — and, as a deliberate accommodation for this
engine's subset, callable **without** `new` too.

- **Statics**: `resolve`, `reject`, `all`, `race`, `allSettled`, `any`, `try(fn, ...args)` (a
  promise of `fn(...args)`, rejected with what it throws), `withResolvers()` (`{ promise,
  resolve, reject }`). The four combinators take
  any iterable ([Iteration](#iteration)); given something else they return a
  promise rejected with V8's `TypeError`, as V8 does, rather than throwing. `Promise.any` rejects
  with an `AggregateError` (`All promises were rejected`) whose `errors` are the reasons in
  input order.
- **`Promise.prototype`**: `then`, `catch`, `finally`.
- Resolving with a thenable (any object or function with a callable `then`) adopts it, as V8
  does: `Promise.resolve`, an executor's `resolve`, `await`, an async function's return value and
  the combinators' inputs call its `then` in a job of its own. An executor's `resolve` and
  `reject` act once between them: the first call wins, even while a thenable it was given is
  still pending.
- A promise holds properties as any object does (`p.label = 'load'`): reads, `in`, `delete`,
  `Object.keys`, spread, `Object.assign`, `JSON.stringify` and `freeze`/`seal` see them, and one
  can shadow `then`.

See [Async & host calls](/guide/async) for how promises connect to
native/host functions.

## `RegExp`

ECMAScript-flavored, via the bundled
[`baru-re`](https://github.com/mdy-docs/lamassu-js/tree/main/third_party/baru-re)
engine. Regex literals and `new RegExp(pattern, flags)`, the `lastIndex`
protocol, named capture groups, and `/d` (indices) all work.

- **`RegExp.prototype`**: `exec`, `test`, `toString`, and the accessors `source`, `flags`,
  `global`, `ignoreCase` and the other flags. **`RegExp.escape(s)`** escapes what a
  pattern could read as syntax, as V8 does (`RegExp.escape("a.b")` is `"\\x61\\.b"`).
- **`RegExp.prototype.compile(pattern, flags)`** (Annex B) recompiles the regexp in place and sets
  its `lastIndex` to 0.
- **The legacy statics**, as V8 has them: `RegExp.input` (`$_`), `lastMatch` (`$&`), `lastParen`
  (`$+`), `leftContext` (`` $` ``), `rightContext` (`$'`) and `$1`–`$9` describe the last
  successful match of any regexp (`exec`, `test`, and the string methods), and stay as they were
  after one that fails. `input` can be set (to a string); writing the others does nothing, as
  V8's setters do. Each is an accessor property of `RegExp`, as in V8, and can be deleted or redefined.
- Consumed by `String.prototype.match`/`matchAll`/`search`/`replace`/
  `replaceAll`/`split` when given a `RegExp` argument, or any object with `RegExp.prototype` in
  its prototype chain. `includes`, `startsWith` and `endsWith` refuse one (`TypeError: First
  argument to String.prototype.includes must not be a regular expression`).
- **What those methods and `test` read from the regexp, they read as properties**, up the
  prototype chain, as V8 does, so a script's own are the ones used: `exec` (called for every
  match; it must return an object or `null`), `flags`, `constructor` (which makes the copy
  `split` and `matchAll` work on) and `lastIndex`. `RegExp.prototype.test.call({ exec })` and
  `RegExp.prototype.toString.call({ source, flags })` work on any object. A regexp nothing has
  been redefined for takes a path that does the same without the lookups.
- **`lastIndex`** holds whatever value is written to it, and a match converts it (`ToLength`):
  `re.lastIndex = '3'` reads back as `'3'` and the next `exec` starts at 3. It is writable, not
  enumerable and not configurable; `Object.defineProperty(re, 'lastIndex', { writable: false })`
  or `Object.freeze(re)` makes it read-only, after which whatever would write it throws
  (`TypeError: Cannot assign to read only property 'lastIndex' of object '[object RegExp]'`:
  `exec` and `test` on a global or sticky regexp, `match` and `replace` on a global one,
  `compile`). `RegExp(re)` without `new` and without flags returns `re` itself when
  `re.constructor` is `RegExp`.
- Catastrophic backtracking is bounded by an engine-side step budget — see
  [Supported syntax](/guide/language#regular-expressions).

## Global functions

`parseInt`, `parseFloat`, `isNaN`, `isFinite`, `escape`, `unescape` (V8's legacy
percent-encoding), and `encodeURI`, `encodeURIComponent`,
`decodeURI`, `decodeURIComponent` as V8 has them: UTF-8 percent-escapes, `decodeURI` leaving an
escaped reserved character (`%2F`, `%23`, …) escaped, and a lone surrogate or a malformed or
non-UTF-8 escape a `URIError: URI malformed`.

`print(...)` is **not** a language built-in — it's a host-provided native,
registered by the CLI (`tools/lamassu.c`) and the npm package's WASM glue
(`src/wasm_api.c`) for convenience, not part of `liblamassu` itself. An
embedder that doesn't register it won't have it.
