# Async & host calls

`async`/`await` and the `Promise` built-in work as you'd expect inside a
script. What's specific to lamassu-js is how a **native/host** function —
one written in C (or JS, via the npm package) — hands time-consuming work
back to a guest `await`. There are two distinct, complementary mechanisms.

## 1. Guest-level `await` of a native-returned promise

A native creates a pending promise and returns it immediately; guest code
`await`s it exactly like any other promise. This suspends the *JS fiber*,
not the C (or WASM) call stack — call frames live on the heap in a
suspendable `JsFiber`, so suspension is just "the fiber stops being run."
No C stack involved is also why `await` is cheap in this engine generally.

The host settles the promise later — from wherever real time actually
happens (a database callback, a `setTimeout`, an HTTP response handler) —
and drains the microtask queue to resume anything waiting on it.

```c
// native side (C)
JsValue p = js_promise_new(ctx);
// ... stash p somewhere reachable, keep it GC-rooted ...
*result = p;
return true;
```

```c
// later, when the real work finishes:
js_resolve(ctx, p, value);   // or js_reject(ctx, p, reason)
js_run_jobs(ctx);            // resumes anything awaiting p
```

```js
// guest side
const value = await nativeThing();
```

This is the mechanism `js_promise_new`/`js_resolve`/`js_reject`/
`js_run_jobs` expose in the [C embedding API](/api/c-embedding). The npm
package's WASM build exercises it with a small test-only
`__nativeDefer`/`lamassu_settle_deferred` pair in its CI smoke test, settled
from a real JS timer callback — the same pattern a browser or Node host
would use with its own event loop.

## 2. `__hostcall(name, argsJson)` — synchronous-looking, Asyncify-powered

The npm package additionally exposes `__hostcall`, a *synchronous-looking*
guest call: no guest-level `await` needed at all. Guest code calls it like
an ordinary function; under the hood it suspends the **entire WebAssembly
call stack** via Emscripten Asyncify while the embedder's JS handler runs
(which may itself be `async`), then resumes with the result.

```js
const engine = await createLamassu({
  natives: {
    // may be async — the guest still sees a synchronous call
    twice: async (n) => {
      await someRealAsyncWork();
      return n * 2;
    },
  },
});

await engine.eval("__hostcall('twice', JSON.stringify([21]))");
// => "42" (JSON-encoded)
```

Arguments arrive at the native as a JSON string; the resolved value is
JSON-encoded on the way back. A handler that throws rethrows inside the
guest at the call site.

::: warning Not reentrant
While an eval is suspended in a host call, that WASM instance must not be
re-entered — Asyncify is not reentrant. If a native itself needs to trigger
a *nested* eval, use one engine instance per nesting level.
:::

## Which one do I want?

- Writing a **C embedder**? You have direct access to `js_promise_new` /
  `js_resolve` / `js_run_jobs` — mechanism 1 is the natural fit, and it's
  also what `async`/`await` in guest code compiles down to internally.
- Using the **npm package**? `__hostcall` (mechanism 2) is what's wired up
  today — pass an object of `natives` to `createLamassu`, and any of them
  can be `async`. The guest-level mechanism exists in the engine but isn't
  exposed as a general-purpose guest API on the npm side yet (only as the
  test-only `__nativeDefer` hook used to exercise it in CI).

## Top-level await

A module's `await` at the top level works, including across
[modules](/guide/language#modules) with async dependencies. One subtlety:
if a top-level-await module is still pending when the host's run call
returns, the host is responsible for settling whatever it's waiting on and
draining jobs — the module's completion becomes observable once that
happens, driven by the microtask queue rather than a return value.

A module that is waiting this way is not done for anything else either: a
graph imported meanwhile that depends on it (a second `js_eval_module`, or an
`import()`) waits for it too, and is rejected with its error if it fails.
While it waits it cannot be evicted.
