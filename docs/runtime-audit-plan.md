# lamassu-js Runtime Audit & Remediation Plan

**Date:** 2026-07-22
**Scope:** Full C runtime (`src/*.c`, `include/lamassu.h`, `tools/lamassu.c`, `src/wasm_api.c`) — ~19.5k LOC.
**Method:** 14 subsystem finders + per-finding adversarial verification with ASan/UBSan repros against `build/lamassu_asan`. Headline crashes and the bytecode type-confusion were reproduced independently.
**Result:** 58 findings — **53 confirmed** (with repros), **5 plausible/latent**, **0 refuted**.

---

## Implementation status (2026-07-23)

**Done — all P0 and all P1, plus most of P2.** The full test suite (`make test`, plain + ASan/UBSan) is green, with new regression coverage added (see §5).

- **P0 (memory safety & code execution):** all 7 fixed — CTAG_NUMBER type confusion, `toFixed` stack overflow, module reason UAF (×2), `Array.sort` UAF, object-spread-of-string UAF, `Object.entries` UAF (plus a `js_array_append` rooting hardening that covers the whole class), compiler scratch-slot aliasing.
- **P1:** all fixed — WS-D (RET_SUB runtime boundary check via a retained instruction bitmap on loaded functions, forged root `n_upvals` reject), WS-E (`heap_limit` now enforced in `js_realloc_raw`, Map/Set backed by an open-addressed value-hash **index** in `src/js_valindex.h`, `verify_pass2` is now a worklist fixpoint), WS-C (`**`/nested-`new`/`JSON.parse` depth guards, VM-wide native-reentry counter + shared per-call-tree fuel), promise reactions now FIFO.
- **P2:** WS-A cast family (one `to_integer_clamped` helper + `js_to_int32`/strict-`<` bound in `js_to_uint32`, UBSan-clean); `Math.round` (half-to-+∞, −0 preserved, no `x+0.5` bug); `parseFloat('5e')`; strict JSON number grammar; `Promise.prototype.finally` thenable adoption; for-of/for-in `continue` per-iteration `CLOSE_UPVALS`.
- **P3 (selected):** Map/Set store normalized `+0`; `String.lastIndexOf` honors `fromIndex`.
- **Deferred (documented):** re-exports live bindings (needs a module-binding indirection refactor), `js_dtoa` extreme-magnitude precision (numeric rewrite, high risk), and the remaining P3 spec-lite/hardening tail — including HashDoS seeding, which conflicts with the engine's deterministic-RNG default and needs a design decision.

Items below are checked off as implemented.

> The individual bugs cluster into **five root causes**. Fixing each _class_ is cleaner and more durable than swatting sites, and it is how this plan is organised: strategic **workstreams** (the themes) drive the tactical **phases** (the execution order).

---

## 1. Status overview

| Severity | Count (deduped) | Reachable from |
| --- | --- | --- |
| Critical | 3 | plain script / tampered bytecode cache |
| High | ~13 | plain script, untrusted bytecode, sandbox escape |
| Medium | ~13 | mostly spec-correctness; two are DoS |
| Low | ~18 | spec-lite, hardening, maintainability |
| Plausible/latent | 5 | not currently reachable (documented for hardening) |

**One reassuring result:** the bytecode _loader's_ two-pass verifier (index bounds, control-flow-boundary proof, from-scratch `max_stack` recomputation) is excellent. Every serialize finding is a **runtime** trust gap the verifier cannot see — not a hole in the verifier.

---

## 2. Root-cause workstreams

Each workstream is a single, coherent fix that resolves multiple findings. Prefer these over per-site patches.

### WS-A — Guard `double`→integer casts (UB elimination)
Casting an out-of-range/`Infinity`/`NaN` double to `int`/`uint32_t` is C UB. Commit `104e9cc` started removing this pattern; several sites remain.
Introduce one helper (e.g. `to_integer_clamped()` / bounds-check-before-cast) and route every numeric-key / radix / count cast through it.
**Resolves:** delete-index UB, `parseInt` radix, `JSON.stringify` space, `Number.toString` radix, `js_to_uint32` 2^63 boundary.

### WS-B — Root unrooted temporaries across GC safe points
The GC is **non-moving** mark/sweep, so the only hazard is a value that is *live but unreachable* when an allocation collects it. Every one of these is an allocation (typically `js_number_to_string` / `js_ascii_cell`) firing while a freshly-allocated cell is held only in a C local.
Fix pattern: fill/store/root the new cell **before** the next allocation, or add a "pin last allocation" helper. Add a `gc_stress` regression test per site.
**Resolves:** `Array.sort` comparator UAF, object-spread-of-string UAF, `Object.entries` array UAF, module `graph_fail`/`eval_fail_state` reason UAF (×2).

### WS-C — Bound all C-stack recursion
The parser header claims "hostile input cannot exhaust the C stack," but three paths bypass the depth guard, and the interpreter re-enters itself through native callbacks with no global bound.
Fix: make `parse_binary` (`**`), `parse_new_expr`/`parse_new_callee`, and `JSON.parse` respect a depth limit; add a VM-wide native-reentry depth counter; stop resetting `fb->fuel` per fiber.
**Resolves:** `**` recursion, nested `new` recursion, `JSON.parse` recursion, native-mediated interpreter recursion + fuel-reset sandbox defeat.

### WS-D — Close runtime trust gaps for untrusted bytecode
The loader's contract (`lamassu.h:305-317`: "a tampered cache cannot become undefined behavior") is **currently false**. Three runtime sites trust unvalidated data.
Fix: reject non-number `CTAG_NUMBER` bit patterns; validate `RET_SUB` targets at runtime (retain a boundary check); force root-function `n_upvals == 0`.
**Resolves:** CTAG_NUMBER type confusion, `RET_SUB` boundary bypass, forged root `n_upvals` NULL-deref.

### WS-E — Make the resource sandbox real
Fix: enforce `heap_limit` at `js_realloc_raw` (the single byte-accounting choke point, guarding GC reentrancy during sweep); replace Map/Set linear scan with the existing open-addressed hash map; make `verify_pass2` a worklist fixpoint.
**Resolves:** `heap_limit` bypass, Map/Set O(n²), `verify_pass2` O(n²).

---

## 3. Prioritised execution plan

Priority = severity × reachability × blast radius. Effort: **S** ≤ half-day, **M** ≈ 1–2 days, **L** ≈ multi-day. Each item lists the confirming location, trigger, and fix.

### P0 — Memory safety & code execution — *do before shipping to any untrusted input*

- [x] **CTAG_NUMBER type confusion → arbitrary pointer deref** · `src/js_serialize.c:528` · WS-D
  Trigger: tampered `.jsbc` with a boxed-tag (≥`0xFFF9`) constant → `js_value_cell()->kind` on attacker pointer (reproduced: SEGV in `verify_fn:961`).
  Fix: after decoding, reject any pattern where `!js_is_number(v)`. **S**
- [x] **`Number.prototype.toFixed` stack-buffer-overflow** · `src/js_builtins.c:1204-1215` · —
  Trigger: `(0).toFixed(100)` — range-checked to `[0,100]` but buffer is `char digs[64]`; pad loop writes to `digs[100]` (reproduced: ASan stack-buffer-overflow).
  Fix: size buffer for ≥100 digits (or cap digits at 20 per real engines); fix the `nd < 63` extraction cap too. **S**
- [x] **Module load/eval failure UAF on the error reason** · `src/js_module.c:417` and `:667` · WS-B
  Trigger: canonicalizer rejects a specifier (or OOM) → unrooted `reason` freed by `mstate_get`'s allocation before `js_promise_reject` stores it (reproduced deterministically under `gc_stress`).
  Fix: protect `reason`, or fetch the promise/module before allocating the reason. **S**
- [x] **`Array.prototype.sort` hands an unrooted element to the comparator** · `src/js_builtins.c:1070` · WS-B
  Trigger: comparator does `arr.length = k` then allocates → freed element read from the pre-shrink range (reproduced: ASan UAF).
  Fix: root elements / re-validate bounds against mutated `elem_count` each step. **M**
- [x] **Object-spread-of-string UAF** · `src/js_interp.c:640` · WS-B
  Trigger: `{...'abcdef'}` — single-unit string freed by the next `js_number_to_string` (reproduced under stress).
  Fix: root or store the unit string before the index-string allocation. **S**
- [x] **`Object.entries` on an array UAF** · `src/js_builtins.c:1849` · WS-B
  Trigger: `Object.entries([...])` — `pair` array freed by `js_number_to_string` before its slots are written (reproduced under stress).
  Fix: fill/root `pair` before the key allocation. **S**
- [x] **Compiler scratch-slot aliases a sibling block's live local** · `src/js_compiler.c:360` · —
  Trigger: `{let a=99; o.p++;} {let c=42; o.p++; print(c);}` prints `1` not `42` — `fs->scratch_slot` cached function-wide, never reset by `scope_leave` (reproduced).
  Fix: reset `scratch_slot` on `scope_leave`, or allocate the temp in the current scope. **S**

### P1 — Sandbox integrity, DoS resistance, untrusted-bytecode robustness

- [x] **`RET_SUB` jumps to an unchecked offset** · `src/js_interp.c:1847` · WS-D
  Trigger: crafted `CONST <mid-insn-offset>; RET_SUB` resumes mid-instruction → OOB stack access, bypassing all loader validation (reproduced).
  Fix: retain a boundary check (bitmap or recompute) and validate `ret` at runtime. **M**
- [x] **Forged root-function `n_upvals` → NULL deref** · `src/js_serialize.c:410` · WS-D
  Trigger: root header `n_upvals=1` + `GET_UPVAL 0` passes verification (root is never instantiated by `CLOSURE`) → NULL upvalue deref (reproduced).
  Fix: force `n_upvals == 0` for the depth-0 function. **S**
- [x] **`heap_limit` bypass — cap only guards cell headers** · `src/js_gc.c:305` (real fix in `src/js_vm.c:18`) · WS-E
  Trigger: `for(i<1e6) a[i]=i` overran a 200 KB cap to **8.4 MB (~41×)** — bulk allocs skip the check; unboxed numbers allocate zero cells (measured).
  Fix: enforce in `js_realloc_raw` when `new_size>old_size`, collect-then-recheck, guard sweep reentrancy; drop the bespoke `js_gc_new_cell` check. **M**
- [x] **Native-mediated / async calls recurse on the C stack; per-fiber fuel reset defeats the budget** · `src/js_interp.c:2027` (reset at `:2003`) · WS-C
  Trigger: `function f(){ [1].forEach(f); } f()` → SIGSEGV, while `f(){return f()}` correctly throws `RangeError`.
  Fix: VM-wide native-reentry depth counter; do not reset `fb->fuel` per fiber (shared budget). **M**
- [x] **Parser `**` right-assoc recursion bypasses depth guard** · `src/js_parser.c:1405` · WS-C
  Trigger: `a**1**1…` ×100k → stack-overflow crash (reproduced). Fix: `enter()/leave()` on the `**` recursion. **S**
- [x] **Parser nested-`new` recursion bypasses depth guard** · `src/js_parser.c:1089` · WS-C
  Trigger: `new new … F()` ×100k → stack-overflow crash (reproduced). Fix: depth-account each `new`. **S**
- [x] **`JSON.parse` has no nesting depth limit** · `src/js_builtins.c:1753` · WS-C
  Trigger: `JSON.parse('['.repeat(200000))` → SIGSEGV `try/catch` cannot recover (reproduced). Fix: depth cap (mirror `JSON.stringify`'s 200). **S**
- [x] **Promise reactions run in reverse (LIFO) registration order** · `src/js_promise.c:160` · —
  Trigger: handlers attached while pending fire in reverse of attachment — violates spec FIFO for all real async flows.
  Fix: append reactions, or reverse on drain. **S**
- [x] **Map/Set are O(n) linear scans → O(n²) bulk build (algorithmic DoS)** · `src/js_mapobj.c:61`, `src/js_setobj.c:50` · WS-E
  Fix: back Map/Set with the existing open-addressed hash map. **L**
- [x] **`verify_pass2` fixpoint is O(n²) time + 32× transient memory (untrusted-load DoS)** · `src/js_serialize.c:824` · WS-E
  Fix: worklist-driven fixpoint instead of full re-sweeps. **M**

### P2 — Correctness / spec conformance

- [x] **`double`→int cast family (UB)** · WS-A · `delete arr[idx]` `src/js_interp.c:1572`, `parseInt` radix `:2246`, `JSON.stringify` space `src/js_builtins.c:1512`, `Number.toString` radix `:1237`, `js_to_uint32` 2^63 `src/js_number.c:51`. One helper fixes all. **S–M**
- [x] **`for-of`/`for-in` `continue` skips per-iteration `CLOSE_UPVALS`** · `src/js_compiler.c:1712` — continued iterations share a stale loop binding. **M**
- [x] **`Math.round` uses `floor(x+0.5)`** · `src/js_builtins.c:2461` — wrong at `0.49999999999999994`, loses `-0`. **S**
- [x] **`parseFloat('5e')` → `NaN`** (should be `5`) · `src/js_builtins.c:2314` — dangling exponent consumed. **S**
- [x] **`JSON.parse` accepts malformed numbers** (`1e`, `--5`, `1.2.3`, `01`, `1.`) · `src/js_builtins.c:1574` — should throw `SyntaxError`. **M**
- [x] **`Promise.prototype.finally` ignores `onFinally` return** · `src/js_promise.c:394` — no thenable adoption / rejection propagation. **M**
- [ ] **Re-exports snapshot-copy values, breaking ES live bindings** · `src/js_module.c:646` — direct imports are live via `GET_IMPORT`; re-exports are not. **M**
- [ ] **`js_dtoa` inexact for extreme-magnitude doubles** · `src/js_number.c:203` — breaks `Number(x.toString())===x`. **L**

### P3 — Hardening, spec-lite, maintainability

- [ ] Spec-lite correctness: `in` misses Map/Set/RegExp synth props `src/js_interp.c:594`; `**` accepts unary left operand `src/js_parser.c:1405`; `String.lastIndexOf` ignores position `src/js_builtins.c:262`; `Math.hypot` no Infinity/NaN/scaling `:1335`; `Number.toString(radix)` truncates at 79 digits `:1259`; `JSON.stringify` no cycle detection `:1417`; Map/Set store `-0` unnormalized `src/js_mapobj.c:79`; `Date` ISO parse no range validation `src/js_date.c:228`; math kernels drift ~1e-12 `src/js_mathkernel.c:41`.
- [ ] Security hardening: **seed the FNV-1a hash** (atoms + object props) to defeat HashDoS `src/js_string.c`; UTF-8 decoders should substitute U+FFFD, not accept overlong/surrogate forms `tools/lamassu.c:42`, `src/wasm_api.c`.
- [ ] Maintainability: remove dead `JsPromise.handled` or implement unhandled-rejection tracking `src/js_promise.c:28`; index the module registry (currently linear `src/js_module.c:105`); stop routing the CLI on `strstr(err_msg,"module loader")` `tools/lamassu.c:363`; fix `.js` suffix test (uses `strstr`) `:177`; `emit_bytecode` passes source, not path, as specifier `:371`; de-duplicate UTF-16↔UTF-8 conversion (5×) `src/wasm_api.c:116`; `json_str` throwaway per-property probe buffer `src/js_builtins.c:1477`.

### Backlog — plausible/latent (hardening; not currently reachable)

- [ ] `CALL_VARARGS`/`NEW_VARARGS` `uint32` overflow + `(int)argc` truncation · `src/js_interp.c:1694-1696` (needs ~34 GB array).
- [ ] `js_arena_alloc` size-rounding/chunk overflow · `src/js_arena.c:17` (unreachable on 64-bit).
- [ ] Regexp `ub_reserve`/`sb_reserve` `uint32` overflow · `src/js_regexp.c:870` (~4–8 GiB).
- [ ] `lamassu_reset()` from inside a suspended `__hostcall` handler → fiber UAF on resume · `src/wasm_api.c:319`.

---

## 4. Suggested sequencing

1. **Sprint 1 (P0):** WS-B (root the UAFs) + the two standalone P0s (toFixed, scratch-slot). All small, all reproducible — add a `gc_stress` regression test per UAF site.
2. **Sprint 2 (P1, bytecode + sandbox):** WS-D (three trust gaps close the loader contract) and WS-E's `heap_limit` fix + WS-C's recursion bounds. These convert "untrusted input" from unsafe to merely rate-limited.
3. **Sprint 3 (P1 tail + P2):** promise ordering, Map/Set hash backing, `verify_pass2` worklist, then WS-A (the cast helper sweeps several P2s at once).
4. **Ongoing (P3/backlog):** spec-lite conformance, HashDoS seeding, maintainability cleanups.

## 5. Regression coverage to add

- A `gc_stress` test harness that runs each WS-B repro (`sort` truncation, `{...'str'}`, `Object.entries([...])`, module canon-reject) and asserts no ASan report.
- A **hostile-bytecode corpus**: the CTAG_NUMBER, `RET_SUB`, and root-`n_upvals` crafted buffers must load-reject cleanly (WS-D).
- A **sandbox test**: `heap_limit` and `ctx->fuel` must actually bound `for(i<1e6) a[i]=i` and `forEach`-recursion respectively (WS-C/WS-E).
- UBSan-clean run over the numeric-cast corpus (`toFixed(100)`, `parseInt(x,Infinity)`, `JSON.stringify(x,null,Infinity)`, `delete a[1e30]`) (WS-A).

## 6. Verification notes

All P0/P1 crash findings were reproduced against `build/lamassu_asan` (ASan+UBSan). The bytecode type-confusion, `RET_SUB`, and forged-`n_upvals` findings were reproduced with crafted `.jsbc` buffers. `heap_limit` was measured with a peak-tracking `realloc_fn`. The UAF findings that require `gc_stress` (spread, `Object.entries`, module reason) were confirmed with a stress-enabled build; the CLI (`build/lamassu`, `build/lamassu_asan`) runs with `gc_stress` off, so those need the stress build or the regression harness above to surface.
