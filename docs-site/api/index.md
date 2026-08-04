# API Reference

lamassu-js has two embedding surfaces, plus a command-line tool:

- **[npm package](/api/npm-package)** — `@mdy-docs/lamassu-js`, the engine
  compiled to a WebAssembly ES module with a small JS wrapper. Use this from
  Node or the browser.
- **[C embedding API](/api/c-embedding)** — the native `liblamassu` C library
  (`include/lamassu.h` and `include/lamassu_compile.h`), for embedders working
  directly in C, or building their own bindings for another host environment.
- **The `lamassu` CLI** — see below.

All three sit on top of the same core: a portable C11 interpreter with no
static globals — every function takes a `JsVm` or `JsContext` handle
explicitly, so multiple independent engine instances can coexist in one
process. The npm package is a thin Emscripten-compiled wrapper around exactly
this C API; if you're embedding from C directly, the [C embedding
API](/api/c-embedding) page is the ground truth.

See [Async & host calls](/guide/async) for how the two embedding surfaces
relate on the async side: the npm package's `__hostcall` is a
synchronous-looking guest call built on Emscripten Asyncify (it suspends the
whole WASM call stack), while the C API's
`js_promise_new`/`js_resolve`/`js_run_jobs` is the lower-level, host-neutral
mechanism behind ordinary guest-level `await` of a native-returned promise —
two complementary mechanisms, not one built on the other.

Whichever you pick, the sandbox limits are off until you set them. [Running
untrusted code](/security) covers all three.

## The `lamassu` CLI

`make cli` builds `build/lamassu`, which runs a source file, a bytecode
cache, or an interactive REPL — the same library, driven from a shell. The
wasip2 build shares the file, so it takes the same options.

```sh
lamassu [options] [file.js]             # run a source file, or the REPL
lamassu [options] --run-bytecode FILE   # run a bytecode cache
lamassu --emit-bytecode SRC OUT         # compile SRC to a bytecode cache
```

| option | meaning |
| --- | --- |
| `--fuel N` | Bytecode-instruction budget per evaluation (`0` = unlimited). Roughly 3e8 per second. |
| `--heap-limit N` | Cap on the guest heap, with an optional `K`/`M`/`G` suffix (e.g. `64M`). |
| `-h`, `--help` | The same summary, from the tool itself. |

Options come before the subcommand or filename. Both limits are off by
default, which is what you want for your own code and not what you want for
anyone else's; the budget is armed per evaluation, so a REPL survives
`while (true) {}` and keeps going. Neither bounds wall-clock on its own —
see [Running untrusted code](/security).
