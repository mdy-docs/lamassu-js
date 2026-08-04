# @mdy-docs/lamassu-js

A strict, safe **JavaScript-subset engine written in C and compiled to
WebAssembly**. It runs untrusted scripts in a sandbox with bounded CPU (fuel),
bounded memory, and no ambient host access — designed for evaluating
user-provided templates. The package ships the compiled `.wasm` plus a small
typed ESM wrapper.

> Strict-mode-only, ES-module-aware subset: `let`/`const` (no `var`),
> arrow/async functions, template literals, destructuring, classes-free objects,
> `try/catch`, promises, `import`/`export`, and ECMAScript `RegExp`. See the
> [project README](https://github.com/mdy-docs/lamassu-js) for the full language
> scope and the safety model.

## Install

```sh
npm install @mdy-docs/lamassu-js
```

## Usage

### With a bundler (Vite, webpack, …)

Import the wasm as a URL asset and hand it to the factory:

```js
import { createLamassu } from "@mdy-docs/lamassu-js";
import wasmUrl from "@mdy-docs/lamassu-js/lamassu.wasm?url"; // Vite

const engine = await createLamassu({ wasmUrl });

console.log(await engine.eval("const x = 40; x + 2;")); // "⇒ 42"
console.log(await engine.eval("print('hi'); x * 10;")); // "hi\n⇒ 420"  (state persists)
engine.reset();                                          // fresh VM
```

### Native ESM (Node, Deno, `<script type="module">`)

No bundler: the sibling `.wasm` is located automatically via `import.meta.url`.

```js
import { createLamassu } from "@mdy-docs/lamassu-js";

const engine = await createLamassu();
console.log(await engine.eval("[1,2,3].map(n => n*n).join(',');")); // "⇒ 1,4,9"
```

## API

### `createLamassu(options?) → Promise<Lamassu>`

| option                | type                       | notes                                                            |
| --------------------- | -------------------------- | ---------------------------------------------------------------- |
| `fuel`                | `number`                   | Bytecode instructions one `eval`/`evalModule` may run, counting the microtask drain it feeds. `0` (default) = unlimited. **Set this for untrusted code** — see Safety. |
| `heapLimit`           | `number`                   | Bytes the guest heap may hold. `0` (default) = unlimited.        |
| `wasmUrl`             | `string`                   | Explicit `lamassu.wasm` URL. Required under a bundler.            |
| `print`               | `(text: string) => void`   | Sink for the engine's internal stdout (script output is returned by `eval`). |
| `natives`             | `Record<string, Function>` | Host functions callable from guest code via `__hostcall` (see Host calls). |
| `loadModule`          | `(specifier, referrer) => string \| Promise<string>` | ES-module loader; powers `evalModule` and guest `import()`. |
| `canonicalizeModule`  | `(specifier, referrer) => string \| undefined` | Maps a specifier to the identity it is cached under. |

### `Lamassu`

- **`eval(source: string): Promise<string>`** — evaluate source in the
  persistent REPL context. Top-level `let`/`const`/`function` declarations
  carry across calls. Resolves to the combined `print()` output followed by
  the completion value (prefixed with `⇒ `) or an error line — never rejects
  for guest errors. Async because a guest host call may suspend execution
  while a native runs.
- **`evalModule(specifier: string): Promise<string>`** — load, link and
  evaluate the ES-module graph rooted at `specifier`; every module, the root
  included, arrives through `loadModule`.
- **`setLimits({ fuel, heapLimit }): {fuel, heapLimit}`** — change the sandbox
  limits. `fuel` applies from the next evaluation; changing `heapLimit`
  rebuilds the VM, which discards REPL state.
- **`getLimits(): {fuel, heapLimit}`** — the limits currently in force.
- **`setNatives(natives): void`** — replace the natives table.
- **`setModuleLoader(load, canonicalize): void`** — replace the module loader.
- **`reset(): void`** — discard all REPL state; start from a fresh VM + context.
- **`module`** — the underlying Emscripten module, for advanced use.

## Host calls (async natives)

Guest code can call out to the host — and the host function **may be
async**: the whole VM execution suspends (Emscripten Asyncify) until it
settles, so the guest sees an ordinary synchronous call whose answer may
take time (a database query, a fetch, …).

```js
const engine = await createLamassu({
  natives: { find: async (query) => db.collection.find(query).toArray() },
});
await engine.eval(`
  const rows = JSON.parse(__hostcall("find", JSON.stringify([{ role: "member" }])));
  rows.length;
`);
```

Guest-side contract: `__hostcall(name, argsJson)` — arguments as a JSON
array string; returns the native's resolved value JSON-encoded as a string
(`JSON.parse` it). A native throwing rethrows inside the guest at the call
site, catchable with `try/catch`.

**Reentrancy:** while an eval is suspended in a host call, the instance must
not be re-entered (Asyncify is not reentrant). Embedders whose natives
trigger further evals (nested rendering, …) should use one instance per
nesting level.

## Safety

Each engine instance is an isolated VM: no filesystem, network, `eval`, or
`Function` constructor, and no access to the host beyond what you expose.

**Set `fuel` and `heapLimit` before running untrusted code.** Both default to
unlimited, and WebAssembly on its own does not save you: it keeps the guest out
of your memory, but it will happily let a guest spin forever or grow linear
memory until the tab or the process dies. With the limits set:

```js
const lam = await createLamassu({
  fuel: 20_000_000,               // ≈0.1s of guest CPU per evaluation
  heapLimit: 16 * 1024 * 1024,
});

await lam.eval("while (true) {}");
// ⇒ "Uncaught RangeError: execution budget exhausted"   (and it returns)
```

The budget is re-armed for every `eval`/`evalModule`, covers the microtask
drain that evaluation feeds — so re-queueing cannot extend it — and the error
it throws re-arms itself, so guest `try`/`catch` cannot swallow the stop and
keep running. Catastrophic regex backtracking is separately bounded by a step
budget, and deep recursion by a call-stack cap.

One thing to know: a guest that fills the heap leaves it full, because its data
is still reachable from the persistent REPL context. Call `reset()` to drop it.

`node smoke.mjs` in this package exercises all of the above, including the
hostile cases — each of which hangs the process if the limits are not applied.
The engine's threat model, and what a host is still responsible for, is written
up in [docs/security.md](https://github.com/mdy-docs/lamassu-js/blob/main/docs/security.md).

## License

MIT © Daniel Walton
