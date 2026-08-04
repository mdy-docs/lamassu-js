# LamassuJS — Swift bindings for lamassu-js

A Swift package that embeds the lamassu-js C engine on Apple platforms.

Two targets:

- **CLamassu** — the engine itself, compiled straight from the monorepo's
  `src/` and `third_party/baru-re/` trees via symlinks (`Sources/CLamassu/
  engine`, `re-src`, `re-include`). There is one copy of the C code in the
  repo; this package just points at it. The build matches the Makefile's
  native flavour: frontend + runtime + regex, `LAMASSU_HAS_REGEX` defined.
- **LamassuJS** — the Swift API: `LamassuEngine`.

## Usage

```swift
import LamassuJS

let engine = LamassuEngine()                 // one VM, one persistent REPL context
engine.onPrint = { line in print("js:", line) }

try engine.evaluate("let x = 21")            // top-level let/const/function persist
let answer = try engine.evaluate("x * 2")    // "42" — stringified completion value
```

Failures throw `JSError`: `.syntax` (with 1-based `line`/`column`),
`.runtime` (the stringified thrown value), or `.interrupted`.

Running untrusted code? Arm the sandbox and keep a kill switch:

```swift
let engine = LamassuEngine(limits: .init(fuel: 300_000_000,   // ~1s of instructions
                                         heapLimit: 64 << 20))
// From any thread, while evaluate() runs:
engine.interrupt()
```

An engine is single-threaded: use it from one thread or serial queue.
`interrupt()` is the documented exception — it only sets a flag.

## Example app

`packages/macos` is a SwiftUI REPL built on this package — see its README.

## Tests

```sh
swift test
```
