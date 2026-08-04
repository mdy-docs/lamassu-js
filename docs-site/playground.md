---
layout: page
title: Playground
description: Run JavaScript in the lamassu engine, live in your browser.
---

<div class="pg-page">

# Playground

The editor and the REPL below share **one engine instance**: load a sample, run
it, then poke at its functions and variables from the REPL. Everything executes
in the C VM compiled to WebAssembly — not in your browser's JavaScript engine.

<ClientOnly>
  <Playground />
</ClientOnly>

## What you are actually running

A bytecode compiler, a mark-and-sweep collector, closures with real upvalue
capture, promises and `async`/`await`, ES modules, and a regex engine — all
written in C and compiled to a ~1 MB WebAssembly module. The
[language guide](/guide/language) covers what the subset does and does not
include, and [deviations](/guide/deviations) is the honest list of where it
parts company with a full engine.

This page runs with the sandbox limits switched on — a 200 million instruction
budget per evaluation and a 128 MB heap cap — because it is a REPL open to
whatever a visitor types. Try `while (true) {}`: it returns a `RangeError`
rather than locking the tab up. [Running untrusted code](/security) explains
what those limits do, and what they do not.

::: tip Reset
**Reset** discards the whole session and starts a fresh VM. Worth reaching for
after an experiment that fills the heap: the data a script leaves behind is
still reachable from the persistent REPL context, so the memory stays used
until the context goes.
:::

</div>

<style>
/* `layout: page` gives a bare canvas with no sidebar; this keeps the prose
   around the playground aligned with the rest of the site rather than running
   the full width of a large monitor. */
.pg-page {
  max-width: 1152px;
  margin: 0 auto;
  padding: 32px 24px 64px;
}
.pg-page h1 { font-size: 2rem; font-weight: 600; margin-bottom: 12px; }
.pg-page h2 {
  font-size: 1.35rem; font-weight: 600;
  margin: 40px 0 12px; padding-top: 20px;
  border-top: 1px solid var(--vp-c-divider);
}
.pg-page p { line-height: 1.7; margin: 12px 0; }
</style>
