# Deviations from real JS

lamassu-js aims to behave as V8 does over the subset it implements (the
reference is the V8 of Node 26), also where V8 is not the specification.
This page tracks every place that's known to differ, so you're not surprised
by one in production.

## Built-ins V8 has that lamassu leaves out

Checked against Node 26 by listing every own property of each global, constructor and
prototype in both engines (2026-11-15); what was missing then has since been added (2026-11-16),
so everything left is left out by design:

- The locale methods (`localeCompare`, `toLocaleString` on numbers, dates and arrays,
  `Object.prototype.toLocaleString`, `toLocaleDateString`, `toLocaleTimeString`,
  `toLocaleUpperCase`, `toLocaleLowerCase`): V8 answers them from ICU through `Intl`, a non-goal.
  `Object.prototype.toLocaleString` is left out with them: it would reach `toString` on a number
  (`"1234.5"`) where V8 formats it (`"1,234.5"`), a silent difference.
- Time zones: a `Date` is UTC (`getHours` is `getUTCHours`, `getTimezoneOffset` is `0`), as there
  is no time zone database; V8 uses the host's zone.
- `Object.getOwnPropertySymbols`, and everything else that takes or returns a symbol: there
  are no symbols.
- `__defineGetter__`, `__defineSetter__`, `__lookupGetter__`, `__lookupSetter__`: a script
  cannot write a getter or setter of its own (see Objects, below). The accessors of V8's
  builtins **are** there, on the prototypes and constructors where V8 has them
  (`Object.prototype.__proto__`, `Map.prototype.size`, `RegExp.prototype.source`, ...).
- `Function.prototype.constructor`: see Functions, below.
- `queueMicrotask` and `structuredClone` are the host's (HTML, Node), not V8's; an embedder adds
  them as natives if it wants them.
- `Reflect`, `Proxy`, `Symbol`, typed arrays, `ArrayBuffer`, `SharedArrayBuffer`,
  `DataView`, `Atomics`, `Intl`: see [Supported syntax](/guide/language#explicit-non-goals).

## Syntax V8 accepts that lamassu refuses

Each is a compile error that names the feature (`SyntaxError: classes are not supported`,
`generators are not supported`, `getter/setter properties are not supported`), never
a silent difference. What was not yet implemented has been (2026-11-16); this is what is left out
by design (see [Supported syntax](/guide/language#explicit-non-goals)): `var`,
`for...in`, `with`, classes (and with them private names, `#x`), `super` in the methods of an
object literal (`'super' is not supported`), getters and setters in object literals, generators,
sloppy mode, `eval` and the `Function` constructor.

String literals as module export names are left out too (2026-11-17): V8, as ES2022 has it,
takes `export { a as "a b" }`, `import { "a b" as a } from 'm'` and `export * as "a b" from
'm'`; in lamassu a name in an import or export clause is always a word, and a string there is
`SyntaxError: Unexpected string` (this one in V8's general wording, not naming the feature). A
string V8 refuses too gets that message where V8 has another: `import { "a" } from 'm'` is
`Unexpected reserved word` in V8.

## Objects

- **`Object.prototype` has five methods and `__proto__`** — `hasOwnProperty`, `isPrototypeOf`,
  `propertyIsEnumerable`, `toString`, `valueOf`. V8 also has `toLocaleString` (left out with the
  locale methods) and the legacy `__defineGetter__`, `__defineSetter__`, `__lookupGetter__` and
  `__lookupSetter__` (a script cannot make an accessor). `toString` gives V8's tags
  (`Object.prototype.toString.call(x)` is `[object Array]`, `[object Error]`,
  `[object Null]`, `[object Math]`, `[object AsyncFunction]`, …), and what
  inherits from `Math`, `JSON`, `Promise.prototype`, `AsyncFunction.prototype`, `Map.prototype`,
  `Set.prototype` or the weak collections' prototypes reports that tag, as V8's own
  `Symbol.toStringTag` makes it; there is no
  symbol to set another (there are no symbols).
- **The builtins' accessors are V8's; a script cannot write one of its own.** Where V8 has an
  accessor property lamassu has one, on the same object, found by the walk up the prototype
  chain: `Object.prototype.__proto__`, `size` on `Map.prototype` and `Set.prototype`, `source`,
  `flags`, `global` and the other flags on `RegExp.prototype`, `RegExp`'s legacy statics
  (`RegExp.$1`, `lastMatch`, `input`, ...), an error's own `stack`, `Function.prototype`'s
  `arguments` and `caller` (which only throw) and `Iterator.prototype.constructor`. Each has
  V8's descriptor (`get size`, no `set`, not enumerable, configurable) and V8's errors
  (`Method get Map.prototype.size called on incompatible receiver #<Set>`, `Cannot set property
  size of #<Map> which has only a getter`), is inherited by what has the prototype in its chain
  (`Object.create(new Map()).size` throws as in V8), and can be shadowed, deleted, redefined as
  a data property, or defined on any object, function or promise under any name, alone or as a
  pair (`Object.defineProperty(o, 'count', Object.getOwnPropertyDescriptor(Map.prototype,
  'size'))`). What V8 takes and lamassu refuses: a descriptor whose `get` or `set` is a
  function of the script's (`TypeError: A script function as a getter or setter is not
  supported`; there is no getter or setter syntax either), and an accessor as an array element
  (`Cannot redefine property: 0 (an array element cannot be an accessor in lamassu)`).
- **`Object(primitiveValue)` returns the primitive unchanged** rather than
  boxing it into a wrapper object — this engine has no boxed-primitive type
  at all. `Object()`/`Object(null)`/`Object(undefined)` (a new empty
  object) and `Object(anObject)` (identity) both match spec.
- **Property attributes are V8's, with three exceptions.** Every own property has V8's
  `writable`, `enumerable` and `configurable`: `Object.defineProperty`,
  `defineProperties` and `Object.create`'s second argument set them, `getOwnPropertyDescriptor(s)`
  reports them (a function's `name` and `length`, a builtin's statics, `Math.PI` and the other
  constants, `undefined`/`NaN`/`Infinity`, a string's characters, a regexp's `lastIndex`), and
  `preventExtensions`, `freeze` and `seal` work with them as in V8: a read-only property refuses
  assignment (also when it is inherited, as V8 refuses it in strict code), a non-configurable one
  refuses `delete` and redefinition, and `isFrozen`/`isSealed` look at each property. The engine
  is strict-mode throughout, so a refused write throws a `TypeError`. The exceptions, each a
  `TypeError: Cannot redefine property` that says so: an **array element** cannot be made
  read-only, non-enumerable or non-configurable (elements are a flat vector with no attribute
  storage; a frozen or sealed array's elements are), nor can an **array's `length`** be made
  read-only (outside `freeze`).
- **`Object.getOwnPropertyNames` of a builtin gives V8's names in V8's order**, but for what is
  left out (see the list at the top): `Math` starts `abs, acos, acosh`, `Array.prototype`
  `length, constructor, at`, as in Node. What V8 makes non-enumerable (a builtin prototype's
  methods, a function's `prototype.constructor`, an error's `message`) is non-enumerable here
  too: left out of `Object.keys`, JSON, spread and `Object.assign`, and listed by
  `Object.getOwnPropertyNames`. A host's own globals come after V8's. `arguments` (an array
  here) has no `callee`.

## Functions

- **There is no `Function` global.** The constructor is left out, as `eval` is, so that
  untrusted code cannot compile code at run time and a runtime-only build needs no compiler.
  `Function.prototype.toString.call(x)` is written `(() => {}).toString.call(x)`, and
  `Object.getPrototypeOf(() => {})` is `Function.prototype`. For the same reason
  `Function.prototype` has no `constructor`, and `AsyncFunction.prototype`
  (`Object.getPrototypeOf(async () => {})`) none either, so `f.constructor` is `Object` here
  and `Function` in V8.

## Conversions

- **Conversions nest at most 200 deep** (the limit on nested interpreter entries, which guards
  the C stack): `String` of an array nested more than about 200 levels, or a `toString` that
  converts another object that does the same, throws `RangeError: Maximum call stack size
  exceeded`. V8 goes about ten times deeper.
- **A stack trace through an object key's `toString`** (`o[k]`, `k in o`, `delete o[k]`) shows the
  calling frame at its previous call rather than at the access. Keeping that position exact costs
  the hot property path about a quarter of its speed; operators and templates are exact.
- **There is no `Symbol.match`, `matchAll`, `replace`, `search` or `split`** (no symbols), so
  `String.prototype`'s methods of those names hand the work to the regexp side when their argument
  has `RegExp.prototype` in its prototype chain, which is where V8 finds those methods; a script
  cannot replace them, or give them to an object that does not inherit from `RegExp.prototype`.
  Everything they read from the regexp is a script's to redefine, as in V8: `exec`, `flags` (and
  through it `global`, `unicode` and the other flags), `constructor` (whose species `split` and
  `matchAll` copy the regexp with, see the next point) and `lastIndex`. See
  [`RegExp`](/guide/builtins#regexp).
- **There is no `Symbol.species`**, so a script cannot give a constructor one. What V8's
  builtins have is here: `Array`, `Map`, `Set`, `Promise` and `RegExp` have the getter that
  answers `this`, so a constructor has a species, itself, when it is one of those or inherits
  from one (`Object.setPrototypeOf(C, Array)`, which is what `class C extends Array` does), and
  any other function or object has none. Where V8 reads `constructor[Symbol.species]` the answer
  is therefore the same as V8's for everything a script here can build: the array methods that
  make an array (`map`, `filter`, `slice`, `splice`, `concat`, `flat`, `flatMap`), the regexp
  ones and a promise's `then`, `catch` and `finally` call `new constructor(...)` for a
  constructor with a species, make the default for one without, and refuse a `constructor` that
  is not an object (`TypeError: object.constructor[Symbol.species] is not a constructor`, and of
  a promise `TypeError: The .constructor property is not an object`).
- **`Object.prototype.valueOf.call(5)`** gives `5`, not a `Number` object (no wrapper objects).

## Strings

- **The longest string is 2<sup>26</sup> code units (64M), where V8's is 2<sup>29</sup>−24**, on
  purpose: lamassu runs untrusted code, and a native builds a string without spending fuel, so
  the limit bounds both the memory one call can take (128 MiB, as every string is UTF-16 here,
  where V8 keeps Latin-1 text in a byte a character) and the time it can spend filling it. Every
  operation that makes a string checks it, and throws `RangeError: Invalid string length` as V8
  does at its own limit, so a script sees V8's error, just sooner. Two places V8 cannot reach
  are held to it too: an engine error whose message would pass it (a function's source text in
  `TypeError: … is not a function`, a name of nearly that length in a `ReferenceError`) is that
  `RangeError` instead, and a script whose source is longer does not compile (`Invalid string
  length`), as its text becomes a string. A module's source, which the host hands over as a
  string, cannot be longer either.

## Arrays

- **No holes.** Array storage is a flat vector, not a sparse structure —
  every index up to `.length` is a real, materialized slot. `Array(n)` and
  `arr.length = n` both throw `RangeError` past a fixed growth cap rather
  than eagerly allocating an attacker-chosen size; see
  [Built-ins → Array](/guide/builtins#array).
- **An array method on another receiver gives V8's result, but for the holes.** The methods
  are generic as V8's are (`Array.prototype.map.call({ length: 3, 0: 'a' }, f)`), and where V8's
  result is an array with a hole for each index the receiver did not have (`map`, `slice`,
  `splice`'s removed elements, `concat`), the result here has `undefined` there: `1 in result`
  is `true` and `Object.keys` lists the index. A run of more than 4096 such indices is
  `RangeError: Invalid array length`, the error writing that far past an array's end is. An
  object receiver itself is left exactly as V8 leaves it, missing indices included.
- **`flat` goes 2000 arrays deep**, the depth calls go here: an array nested further (or one
  that holds itself), flattened all the way, is `RangeError: Maximum call stack size exceeded`.
  V8 throws the same where its own stack ends, about five times deeper.
- **A primitive receiver stays a primitive.** `Array.prototype.fill.call('abc', 1, 3)` returns
  an object in V8 (`Object('abc')`) and `'abc'` here, and a callback's third argument is the
  string, not its wrapper (no wrapper objects). What is read and what is refused are the same.
- **`Array.of.call(Object, 1)` throws.** `Array.from` and `Array.of` build on `new this(...)`
  when `this` is a constructor, as in V8, and `new Object(1)` is the number 1 here, not a
  wrapper object, so the element cannot be defined on it:
  `TypeError: Cannot create property '0' on number '1'`. The same goes for `Number`, `String`,
  `Boolean` and `BigInt` as `this`. V8 answers a wrapper object with the elements on it.

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

- **There is no `Symbol.iterator`**, so a script cannot give an object an iterator method of its
  own: an object iterates by the `[Symbol.iterator]` V8 would find on its prototype chain, which
  is the one of the nearest of `Iterator.prototype` (returns the object itself),
  `Array.prototype` (`values`: `[...Object.create([1, 2])]` is `[1, 2]`, read through `length`
  and the indices), `String.prototype`, `Map.prototype` and `Set.prototype`, called on the
  object as V8 calls it. An iterable that is not its own iterator (one whose
  `[Symbol.iterator]()` makes a fresh iterator each time) cannot otherwise be written.
  `Iterator.from({ next() { ... } })` makes any object with `next` iterable. A value stops
  being iterable when its prototype chain no longer reaches one of those five
  (`Object.setPrototypeOf([1], null)`, a `Map` whose prototype was replaced), as in V8. See
  [Built-ins → Iteration](/guide/builtins#iteration).

## Errors

- **Nothing raises `EvalError`**: there is no `eval`. The constructor is
  there, as in V8.
- **`stack` is V8's**, with these differences: the first line is made as Node makes it, not as V8's own formatter does (the one a browser
  runs): it has the `message` the error has when `stack` is first read, where V8's own keeps
  the message the error was constructed with (`e.message = 'b'` before the first read shows
  `b` here and in Node, the original in Chrome), and a `name` or `message` whose conversion
  throws makes reading `stack` throw that, as in Node, where V8's own writes
  `<error: RangeError: x>` in its place (and so does Node for a `stack` read from inside that
  conversion, which here throws too);
  an anonymous function's inferred name comes only from a member assignment
  (`x.y = function () {}` is `x.y`, `F.prototype.m = ...` is `F.m`), where V8 also infers from
  other places (`const a = [function () {}]` is `a` there); a host's natives have no frame.
- **A `for`–`of` declaration's pattern is not named after V8's hidden variable**: V8 reads
  `for (const [x] of [1])` as a declaration of `.for`, and says `.for is not iterable` at the
  pattern's `]` (and `Cannot destructure property 'p' of '.for'`). Here the error is placed as
  for any nested pattern and words the value by its type
  (`number 1 is not iterable (cannot read property Symbol(Symbol.iterator))`).
- **Subclassing is not possible** (no classes); set `name` on an instance
  for a custom discriminator.
- **Messages follow V8's wording**, with these differences: an error about
  a feature lamassu leaves out says so (`'var' is not supported; use 'let' or
  'const'`, `classes are not supported`) where V8 would compile the script
  (where V8 would not, as for `import * as class from 'm'`, `const var = 1`, `let [var] = []`
  or `(class) => 1`, the message is V8's, `Unexpected token 'var'`, at the token V8 stops at).
  What decides it is the start of the construct, not the whole of it: `var` and then a name or
  a pattern is a declaration, `class` as far as the `{` of its body is a class (its name and
  its `extends` clause are read as V8 reads them, so `x = class;` is `Unexpected token ';'`
  and `class {}` is `Unexpected token '{'`), and `super.x` or `super[x]` in a method of an
  object literal is a reference to `super` (anywhere else it is V8's `'super' keyword
  unexpected here`). A generator is `function*`, or `*` before the key of a method, as far as
  the `(` of its parameters, and an accessor is `get` or `set` before a key as far as the same
  `(`: `function* g() {}` is `generators are not supported`, at the `*` (also where its name
  clashes with another declaration, `let g; function* g() {}`, which V8 reports as the clash),
  and `function* ;` is `Unexpected token ';'`; `({ get x() {} })` is `getter/setter properties are not supported`
  and `({ get x })` is `Unexpected token '}'`. Past that point lamassu has stopped reading,
  so an error V8 would find further on is not the one reported: `var x y;` is `'var' is not
  supported` here and `Unexpected identifier 'y'` in V8, and `const [class {}] = []` is
  `classes are not supported` here and `Invalid destructuring assignment target` in V8;
  one of V8's own quirks is not copied (a destructured
  parameter given `null` is `'object null'` in V8's message, `'null'` here, and array
  destructuring names its source only when V8 does: V8 prints `number 5 is not iterable (cannot
  read property Symbol(Symbol.iterator))` for `const [a] = o.x` with `o.x` 5, where lamassu
  prints `o.x is not iterable`, because V8's printer finds a name or a literal there but not a
  property read or a call); an expression named in a message is printed as V8's call printer
  prints it, literals folded as V8's parser folds them (`(x + y) is not a function`,
  `(1 % 2)()` is `1 is not a function`, `x + "a" + "b"` shows as `(x + "ab")`, `o["f"]` as `o.f`,
  `(a?.b)()` as `(intermediate value)`), except that a text longer than 120 characters or
  nested more than 32 deep shows the value instead; an object named in a message is named
  as V8 names it: by its constructor when its `toString` is `Object.prototype.toString` and its
  `constructor` a named function (`#<Object>`, `#<Set>`, `#<Foo>`), as `Name: message` when it is
  an error or has `Error.prototype.toString`, and by its tag otherwise (`[object Array]`,
  `[object Date]`, `[object Module]`, `[object Map Iterator]`, and `[object Object]` for an object
  with a `toString` of its own, with no prototype, or inheriting from `Iterator.prototype`).
- **A `SyntaxError` is V8's, message and position**, including which error is reported when a
  source has several (the earliest, as V8 chooses it: `({a = 1}); 1 +` is `Invalid shorthand
  property initializer`) and where strict mode's octal errors land (at the end of the function
  around them). The differences:
  - A call as the target of an assignment, an update or a `for...of` (`f() = 1`, `f()++`,
    `for (f() of xs)`) is the early `SyntaxError` the specification asks for (`Invalid left-hand
    side in assignment` and its relatives). V8 compiles these, for old web pages, and throws a
    `ReferenceError` when the statement runs.
  - `for (a.async of xs)` is accepted, as the specification has it. V8 refuses it (`The left-hand
    side of a for-of loop may not be 'async'`), meaning to refuse only `for (async of xs)`,
    which lamassu refuses too.
  - A catch block that redeclares more than one name its parameter's pattern binds (`catch ([a,
    b]) { let b; let a; }`) reports the first of them in the source. V8 reports the one its hash
    table gives first, which changes from one run of node to the next.
  - `([f(a.b = 1)]) => 1` is `Invalid destructuring assignment target`; V8 says `Illegal
    property in declaration context`.
- **Out-of-memory errors may still be bare strings**: when the engine
  cannot allocate an Error object, it throws the message text instead so
  the failure is never lost.

## JSON

- **`JSON.stringify` and `JSON.parse` nest at most 200 deep** (their C
  recursion is bounded like the interpreter's nested entries): past that,
  both throw `RangeError: Maximum call stack size exceeded`. V8 goes several
  thousand levels deeper.

## Promises & async

- **An unhandled promise rejection is the host's to report.** The engine has no process to
  end. It tells the host which rejected promises nothing reacts to, and which of those are
  reacted to later (`js_set_rejection_tracker`, the specification's
  HostPromiseRejectionTracker); a host that sets no tracker hears nothing. The `lamassu`
  command line tool reports the first one still unhandled when the run is over and exits with
  status 1, as Node does. There is no `unhandledrejection` event for a script to listen to.
- **A settling function that throws inside a job is reported as an unhandled rejection.** When
  `then` made its result with another constructor (see
  [`Promise`](/guide/builtins#promise)) and the `resolve` or `reject` that constructor handed
  over throws while a reaction is settling the result, there is no promise left to reject. V8
  reports an uncaught exception; lamassu gives the error to the host's rejection tracker as a
  rejected promise nothing holds. Under Node and under the `lamassu` command line tool the
  outcome is the same: the error is printed and the exit status is 1.
- **A pending top-level-await module** returns from the host's run call
  still pending — see [Async & host calls](/guide/async#top-level-await).
- **`Array.fromAsync` closes an iterator once.** When a value it awaits rejects, V8 calls the
  iterator's `return` twice; the spec's async-from-sync iterator closes it once, and so does
  lamassu. (There are no async iterables to give it: no symbols, no async generators.)

## Weak references

- **`FinalizationRegistry` callbacks run when the job queue empties.** The collector notes the
  dead targets; their callbacks run from `js_run_jobs` once the microtask queue is empty (and any
  jobs they queue run in the same drain), where V8 runs them as a task of their own. A callback
  that throws is ignored and the others still run (V8 reports the error to the host). A host that
  never drains the queue never sees them. When anything is collected is the collector's business,
  in lamassu as in V8: a weak reference only says it *may* be.

## Modules

- **Linking is V8's, also where V8 is not the specification.** Re-exports
  are live bindings, and a graph links or fails to link as it does in V8:
  the same error, naming the same module and export when several would
  fail. Three places where V8, and so lamassu, differs from the
  specification or from Node's loader:
  - `import * as w from 'm'; export { w }` and `export * as w from 'm'`
    export one and the same binding, the namespace of `m`. Two modules that
    pass it on under one name do not conflict under `export *` (by the
    specification the first form is a local binding of its module, and they
    do).
  - A name that the `export *` of a module `s` leave ambiguous is not
    ambiguous one level up: a module with `export * from 's'` and `export *
    from 't'` lists the name in its namespace, with `t`'s binding. Importing
    it by name from that module is still `SyntaxError: ... contains
    conflicting star exports`, unless the module's namespace was made
    before.
  - A module that failed to link is linked again by the next import of it
    and fails the same way. Node keeps the first failure of a dynamic
    `import()` for that URL; lamassu, like V8 alone, asks again, so an
    import that could not link earlier may link later (see the case above).
- **The words of an import or export clause are checked as V8 checks them**, by the parser,
  before anything is compiled: a name an import binds must be one a declaration could bind
  (`import { if } from 'm'` and `import { a as let } from 'm'` are `Unexpected reserved word`,
  at the word), and so must every local name of an `export { ... }` with no `from` (`export {
  if }`; with `from` the names are the other module's, and any word will do). As in V8, the
  first such word of an export list is what is reported whatever else is wrong after it, and it
  comes before `Export 'x' is not defined in module`. The one difference is the string names
  lamassu leaves out (see Syntax, above).
- **A module's declarations and exports are checked as V8 checks them**, by the parser, so an
  error about them comes in the same order relative to other syntax errors as in V8. An import
  binding clashes with any other declaration of its name (`import { a } from 'm'; let a;`; V8
  points a default or namespace import's clash at `import`). Once the module is read, a name
  exported twice is `Duplicate export of 'x'` (counting `export default`, `export * as x` and
  re-exports), and only then is an export of an undeclared name `Export 'x' is not defined in
  module`; of several, the one V8 picks, by its order of local names. Two `export default`
  declarations are V8's `Identifier '.default' has already been declared`, its name for the
  hidden binding of a default export. `export { x } from ""` is a re-export from the module
  `""`.
- **A module specifier may not contain U+0000** (`SyntaxError`, or a rejected `import()`): a
  module request carries its import attributes after one.
- **A graph may be 3000 modules deep along its imports.** No walk over a module graph is on
  the C stack, so how deep a graph may be does not depend on the stack of the thread: a chain of
  imports 3000 modules long links and runs everywhere, and a longer one is rejected with V8's
  `RangeError: Maximum call stack size exceeded` and left as it was (importing it again fails
  the same way, and a part of it that is short enough works). V8 throws that error when its own
  stack runs out: with the stack Node gives it, from a chain of about 3100 modules, and Node's
  loader from one of several hundred. A chain of re-exports (`export * from`,
  `export { a } from`) is followed at any length; V8 follows one of 10 000 modules and crashes
  on one of 20 000.
- **A cycle whose first module failed after a top-level `await`, imported again at another of
  its modules, is rejected with that module's error**, as ECMA-262 has it. V8 stops the process
  there (a failed check in `SourceTextModule::Evaluate`; Node 26.3 aborts on it). Everywhere else
  the evaluation of a graph is V8's, step for step: the order the bodies run in, no job between
  two of them, the modules that wait for a top-level `await` and the order they go on in, and
  the error a failed module gives every later graph, after the modules before it have run.
- **What `import()` settles after the evaluation is done is the host's.** lamassu takes the one
  job the specification gives it; Node's loader takes several more, so a reaction to an
  `import()` can come before a module body that Node would run first, when both follow the
  same top-level `await`. The order of the module bodies themselves is the same.

## Not a deviation, but easy to trip on

`for...in`, `var`, classes, `eval`, the `Function` constructor, `Reflect`,
`Proxy`, `Symbol`, generators, typed arrays, and `Intl` aren't "different,"
they're **absent** — see [Supported syntax](/guide/language) for the full
list of non-goals.
