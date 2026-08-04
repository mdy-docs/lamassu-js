---
layout: home

hero:
  name: lamassu-js
  text: A JavaScript subset, sandboxed
  tagline: A strict, safe JavaScript-subset engine written in C, compiled to WebAssembly — for running untrusted scripts inside a web-framework templating language.
  actions:
    - theme: brand
      text: Read the Guide
      link: /guide/
    - theme: alt
      text: API Reference
      link: /api/
    - theme: alt
      text: Running untrusted code
      link: /security
    - theme: alt
      text: Try it live
      link: /playground

features:
  - title: Built for untrusted input
    details: A per-instruction CPU budget, a hard heap cap, an interrupt safe to call from a signal handler, bounded stack depth, and a bytecode loader that treats its input as hostile. The limits are opt-in — set them, and a guest that never stops is a caught error rather than a hung process.
    link: /security
    linkText: How to set the limits
  - title: Compile once, run repeatedly
    details: Source compiles to a versioned, validated bytecode format. Cache it, ship it, run it — the validating loader treats bytecode as hostile input, never as trusted state.
  - title: Real async, two ways
    details: Guest-level await of a native-returned promise for ordinary host calls, and a synchronous-looking __hostcall for embedders who'd rather not thread await through guest code.
  - title: Small, deliberate subset
    details: let/const only, strict mode only, no classes/eval/Function constructor. Every omission is a design decision, documented — not a missing feature.
---
