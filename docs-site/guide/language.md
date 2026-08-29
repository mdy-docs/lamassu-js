# Supported syntax

Every unit of source lamassu-js compiles is treated as an ES module and
executes under **strict-mode semantics** — there is no sloppy mode, no
mode switch, and no `"use strict"` directive to write (it's already on).

## Declarations

- `let` and `const` — yes.
- `var` — **no**. It's a reserved word that produces a targeted error
  (`'var' is not supported; use 'let' or 'const'`) rather than a generic
  parse failure, and it is never silently aliased to `let` (their hoisting
  and redeclaration semantics differ, and this engine treats a silent
  behavior difference from real JS as the worst possible failure mode for a
  templating language).
- Block-scoped `function` declarations are hoisted within their block, same
  as real JS.
- Assigning to an undeclared variable is a compile-time `ReferenceError`,
  not an implicit global (a strict-mode rule the engine applies
  unconditionally).

## Control flow

`if`/`else`, `for`, `while`, `do...while`, `for...of`, `switch` — all
supported. `for...in` is **not** (a targeted parse error points you at
`for...of`: `SyntaxError: for-in is not supported; use for-of`).

## Functions

- Regular functions, arrow functions, default parameters, rest parameters
  (`...args`), spread in call arguments.
- Closures and upvalues work as expected; the interpreter never uses the C
  stack for JS calls, so recursion depth is bounded and checked, not a
  segfault risk.
- `this` in a plain function is `undefined` (strict-mode semantics, not the
  global object). `arguments.callee`/`arguments.caller` are not supported.
- Duplicate parameter names are a compile error.

## Objects, arrays, and constructors

- Object and array literals, computed property names, shorthand properties,
  spread (`{...obj}`, `[...arr]`), destructuring (including defaults and
  nested patterns) in both declarations and assignments.
- `new` works, with real prototype chains: `Ctor.prototype` is a genuine,
  script-visible object, an instance's `[[Prototype]]` is set to it at
  construction, and property lookup walks the chain — the same mechanism
  built-in instance methods (Array, Date, Map, Set, RegExp) use to reach
  their own prototype's methods.
- `Object.getPrototypeOf` / `Object.setPrototypeOf` are available.
  `__proto__` as an accessor property is **not**.
- **Classes are not supported.** Use constructor functions and
  `Ctor.prototype` instead — see the example below.

```js
function Point(x, y) {
  this.x = x;
  this.y = y;
}
Point.prototype.length = function () {
  return Math.sqrt(this.x * this.x + this.y * this.y);
};
const p = new Point(3, 4);
p.length(); // 5
```

## Strings

Template literals (including tagged-less interpolation and multi-line
strings) are supported. Strings are UTF-16 internally, matching JS
`.length`/indexing semantics exactly.

## Error handling

`try`/`catch`/`finally` and `throw` are fully supported, including
`finally` blocks that run on every exit path (return, break, continue,
rethrow).

`Error` and the standard subclasses `TypeError`, `RangeError`,
`ReferenceError`, and `SyntaxError` are real constructors, and every error
the engine itself raises is an instance of the matching one — so a single
`catch` handles script and engine errors the same way:

```js
try {
  null.x;
} catch (e) {
  e.name;     // "TypeError"
  e.message;  // "cannot read properties of undefined or null (reading 'x')"
  String(e);  // "TypeError: cannot read properties of ..."
}
throw new RangeError('out of bounds', { cause: previous });
```

There is no `instanceof` (see [Deviations](./deviations.md)); discriminate
on `e.name` or `Object.getPrototypeOf(e) === RangeError.prototype`.

## Async

`async`/`await`, including top-level await in a module. See
[Async & host calls](/guide/async) for how this connects to native/host
functions.

## Modules

`import` / `export`, including named exports, `export default`,
`export * from`, `export { a as b } from`, and `export * as ns from`.
Cyclic and diamond import graphs are supported. See the
[Deviations](/guide/deviations) page for the edge cases (TDZ on cyclic
reads, snapshot vs. live re-exports).

## Regular expressions

Supported, ECMAScript-flavored, via a dedicated backtracking VM
([`baru-re`](https://github.com/mdy-docs/lamassu-js/tree/main/third_party/baru-re))
compiled into the engine. Catastrophic backtracking is bounded by an
engine-side step budget — a pathological pattern throws a catchable
`RangeError: regular expression step budget exhausted` instead of hanging.

## Explicit non-goals

These are permanent scoping decisions, not a "not yet implemented" list:

- **`eval` and the `Function` constructor.** Both would let guest code
  synthesize and run new source at runtime, which is exactly what a sandbox
  for untrusted scripts needs to prevent.
- **Sloppy (non-strict) mode.** Every unit of source is a module; there is
  no script goal to make sloppy.
- **`for...in`, labeled statements, `with`.**
- **Advanced built-ins**: `Reflect`, `Proxy`, `Symbol`, generators, typed
  arrays, `Intl`.
- **Full spec conformance.** Where the subset overlaps real JS, behavior is
  meant to match Node closely enough for differential testing — edge-case
  fidelity beyond that isn't a goal. See
  [Deviations from real JS](/guide/deviations) for the specifics.
- **Native code generation.** Bytecode interpretation is the execution
  model; it's also the only practical option running inside WebAssembly (no
  runtime codegen in linear memory).
