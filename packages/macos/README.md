# LamassuRepl — example macOS app

A small SwiftUI JavaScript REPL running on the lamassu-js engine through the
`LamassuJS` Swift package (`packages/swift`).

```sh
swift run
```

(Or open this directory in Xcode and run the `LamassuRepl` scheme.)

What it shows:

- A scrolling transcript of inputs and results; completion values in teal,
  errors in red, `print(...)` output inline as it arrives.
- One persistent REPL context: top-level `let`/`const`/`function` carry
  across lines (`let x = 21`, then `x * 2`).
- Evaluation runs on a serial background queue so the UI never blocks, and
  the **Stop** button interrupts a runaway line (`while (true) {}`) via
  `js_vm_interrupt` — the one engine call that is safe from another thread.

Try:

```
let x = 21
x * 2
print('hi', { a: 1 }, [1, 2, 3])
'abc'.replace(/b/, 'B')
while (true) {}        // then press Stop
```
