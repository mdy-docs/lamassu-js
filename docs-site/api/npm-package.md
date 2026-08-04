# npm package

```sh
npm install @mdy-docs/lamassu-js
```

```js
import { createLamassu } from "@mdy-docs/lamassu-js";

const engine = await createLamassu();
console.log(await engine.eval("1 + 2 * 3")); // "⇒ 7"
```

The package is the engine's REPL surface compiled to a WebAssembly ES
module, wrapped in a small friendly API. It works the same way in Node and
the browser; in native ESM the sibling `.wasm` is located automatically via
`import.meta.url`.

## `createLamassu(options?)`

```ts
function createLamassu(options?: {
  fuel?: number;
  heapLimit?: number;
  wasmUrl?: string;
  print?: (text: string) => void;
  natives?: Record<string, (...args: any[]) => any>;
  loadModule?: (specifier: string, referrer: string) => string | Promise<string>;
  canonicalizeModule?: (specifier: string, referrer: string) => string | undefined;
}): Promise<Lamassu>;
```

- **`fuel`** — bytecode instructions one `eval`/`evalModule` may run,
  counting the microtask drain it feeds. `0` (the default) is unlimited.
  **Set this before running untrusted code** — see [Sandbox
  limits](#sandbox-limits) below.
- **`heapLimit`** — bytes the guest heap may hold. `0` (the default) is
  unlimited.
- **`wasmUrl`** — explicit URL for `lamassu.wasm`. Only needed when a
  bundler (Vite, webpack, …) relocates the module — import it with
  `import wasmUrl from "@mdy-docs/lamassu-js/lamassu.wasm?url"` and pass it
  through. Omit for plain native ESM.
- **`print`** — sink for the engine's *internal* stdout. Rarely needed;
  a script's `print(...)` calls are captured and returned by `eval` anyway.
- **`natives`** — an object of host functions callable from guest code via
  `__hostcall(name, argsJson)`. Any of them may be `async` — see
  [Async & host calls](/guide/async#_2-hostcall-name-argsjson-—-synchronous-looking-asyncify-powered).
- **`loadModule`** — the ES-module loader behind `evalModule` and guest-side
  dynamic `import()`. Given a specifier and the importing module's referrer,
  return the module's source (or a promise for it); every module, the root
  included, arrives through it.
- **`canonicalizeModule`** — synchronous, and optional: maps a specifier to
  the identity it is cached under (a relative path to an absolute one, say).
  Must be deterministic for a given specifier/referrer pair.

Resolves to a `Lamassu` instance:

## `engine.eval(source)`

```ts
eval(source: string): Promise<string>;
```

Evaluates `source` in a **persistent REPL context** — top-level `let`,
`const`, and `function` declarations carry across calls, so a sequence of
`eval` calls behaves like a REPL session, not independent scripts.

The resolved string is the script's `print(...)` output, followed by:

- `"⇒ " + <completion value>` on success, or
- `"Uncaught " + <error>` if the script threw.

`eval` never rejects for a *guest* error (a thrown exception inside the
script) — it resolves with the `"Uncaught ..."` line either way. It's
`async` because a guest `__hostcall` may suspend WASM execution while a
native runs.

```js
await engine.eval("let x = 40;");
await engine.eval("print('hi'); x + 2;");
// => "hi\n⇒ 42"
```

## `engine.evalModule(specifier)`

```ts
evalModule(specifier: string): Promise<string>;
```

Loads, links, and evaluates the ES-module graph rooted at `specifier`,
pulling every module — the root included — through the installed
`loadModule`. Resolves to the same kind of string `eval` does. Modules stay
cached in the context, so the same canonical specifier evaluates once, until
`reset()`.

## Sandbox limits

```ts
setLimits(next?: { fuel?: number; heapLimit?: number }): { fuel: number; heapLimit: number };
getLimits(): { fuel: number; heapLimit: number };
```

`setLimits` changes either limit and returns the pair now in force;
`getLimits` just reports it. Omitted fields keep their current value.

The two behave differently when changed. `fuel` applies from the next
evaluation, because the budget is armed per turn. Changing `heapLimit`
**rebuilds the VM**, which discards REPL state — the cap belongs to the VM,
not the context, so there is nothing to change in place.

::: warning Both limits default to unlimited
WebAssembly on its own does not save you here. It keeps the guest out of your
memory; it will happily let a guest spin forever or grow linear memory until
the tab or the process dies.

```js
const lam = await createLamassu({
  fuel: 20_000_000,             // roughly 0.1s of guest CPU per evaluation
  heapLimit: 16 * 1024 * 1024,
});

await lam.eval("while (true) {}");
// ⇒ "Uncaught RangeError: execution budget exhausted"   (and it returns)
```

The budget is re-armed for every `eval`/`evalModule` and covers the microtask
drain that evaluation feeds, so re-queueing cannot extend it; the error it
throws re-arms itself, so guest `try`/`catch` cannot swallow the stop and keep
running. Fuel counts instructions rather than time — read [Running untrusted
code](/security) for what that does and doesn't bound.
:::

One thing to know: a guest that fills the heap leaves it full, because its
data is still reachable from the persistent REPL context. Call `reset()` to
drop it.

## `engine.setNatives(next)`

```ts
setNatives(next?: Record<string, (...args: any[]) => any>): void;
```

Replaces the natives table wholesale — useful for swapping in a different
set of host functions per request/task without recreating the engine.

## `engine.setModuleLoader(load, canonicalize)`

```ts
setModuleLoader(
  load?: (specifier: string, referrer: string) => string | Promise<string>,
  canonicalize?: (specifier: string, referrer: string) => string | undefined,
): void;
```

Replaces the module loader and canonicalizer installed at construction —
same contracts as the `loadModule` / `canonicalizeModule` options. Call it
with no arguments to uninstall.

## `engine.reset()`

```ts
reset(): void;
```

Discards all REPL state (every persistent global, every module) and starts
from a fresh VM + context. The natives table is untouched.

## `engine.module`

The underlying Emscripten module object (`M` from
`createLamassuModule(...)`), for advanced use — e.g. calling an exported C
function directly via `module.ccall`/`module.cwrap` that isn't wrapped by
this API yet.

## Errors vs. exceptions

A **syntax error** in `source` and a **runtime exception** thrown by the
guest script both surface the same way: as an `"Uncaught ..."` /
`"SyntaxError: ..."` line in the resolved string, never as a rejected
promise. The promise only rejects for a genuine host-side failure (e.g. the
WASM module failing to instantiate).
