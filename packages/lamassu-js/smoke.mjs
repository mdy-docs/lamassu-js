/*
 * Smoke test for the published package, run against the built artifact in
 * ./dist. `npm test` in this directory, or `node packages/lamassu-js/smoke.mjs`
 * from the repo root. Requires `make pkg` to have run.
 *
 * Two things are being checked, and the second is the one that matters:
 * that the API works at all, and that a hostile script cannot take the host
 * down with it. WebAssembly keeps the guest out of the host's memory; it does
 * nothing about a guest that loops forever or allocates until the process
 * dies, so an embedding that does not set limits is not sandboxed in any
 * useful sense. Each hostile case below hangs the process indefinitely if the
 * limits are not applied.
 */
import { createLamassu } from "./index.js";

let failures = 0;
const check = (label, actual, predicate, expectation) => {
  const ok = predicate(actual);
  if (!ok) failures++;
  console.log(`${ok ? "ok  " : "FAIL"} ${label.padEnd(28)} ${String(actual).slice(0, 64)}`);
  if (!ok) console.log(`     expected: ${expectation}`);
};
const has = (needle) => (v) => String(v).includes(needle);

const lam = await createLamassu({
  fuel: 20_000_000,
  heapLimit: 16 * 1024 * 1024,
  natives: {
    add: (a, b) => a + b,
    slow: async (k) => { await new Promise((r) => setTimeout(r, 5)); return `got-${k}`; },
  },
  loadModule: (spec) =>
    spec === "math.js" ? "export const double = (v) => v * 2;"
    : spec === "main.js" ? "import { double } from './math.js'; export default double(21);"
    : undefined,
  canonicalizeModule: (spec) => spec.replace("./", ""),
});

check("limits applied", JSON.stringify(lam.getLimits()), has('"fuel":20000000'), "fuel reported");

// --- the engine does what it says ---
check("eval", await lam.eval("[3,1,2].sort((a,b)=>a-b).join(',');"), has("1,2,3"), "1,2,3");
check("repl state persists", await lam.eval("const k = 7; k * 6;"), has("42"), "42");
check("...across calls", await lam.eval("k + 1;"), has("8"), "8");
check("closures", await lam.eval("function mk(){let n=0;return()=>++n;} const c=mk(); c(); c();"), has("2"), "2");
check("regex", await lam.eval("/(\\d+)-(\\d+)/.exec('10-20')[2];"), has("20"), "20");
check("JSON", await lam.eval("JSON.stringify({a:[1,2]});"), has('{"a":[1,2]}'), "json");
check("print()", await lam.eval("print('out'); 1;"), has("out"), "out");
check("sync native", await lam.eval("__hostcall('add', JSON.stringify([2,40]));"), has("42"), "42");
check("async native", await lam.eval("__hostcall('slow', JSON.stringify(['x']));"), has("got-x"), "got-x");
check("es modules", await lam.evalModule("main.js"), has("42"), "42");
check("guest error", await lam.eval("null.x;"), has("TypeError"), "TypeError");
check("syntax error", await lam.eval("let = ;"), has("SyntaxError"), "SyntaxError");

// --- and cannot run away with the host ---
check("infinite loop", await lam.eval("while (true) {}"), has("budget exhausted"), "budget error");
check("loop in try/catch", await lam.eval("try { while(true){} } catch (e) { 'swallowed'; }"),
      has("budget exhausted"), "uncatchable");
check("infinite recursion", await lam.eval("function f(){return f();} f();"), has("call stack"), "stack error");
check("microtask bomb", await lam.eval("function f(){Promise.resolve().then(f);} f(); 'queued';"),
      has("queued"), "returns, bounded");
check("still usable after", await lam.eval("1 + 1;"), has("2"), "2");
check("string bomb", await lam.eval("let s='x'; for(;;){ s = s + s; }"), has("too long"), "string cap");
check("ReDoS", await lam.eval("/(a+)+$/.test('a'.repeat(60)+'X');"), has("step budget"), "regex budget");
check("memory bomb", await lam.eval("let z=[]; for(;;) z.push({x:1});"), has("out of memory"), "heap cap");
// The memory bomb's array is a live REPL global, so the heap stays full until
// that state is dropped — which is what reset is for.
lam.reset();
check("recovers after reset", await lam.eval("2 + 2;"), has("4"), "4");

console.log(failures ? `\n${failures} check(s) FAILED` : "\nall smoke checks passed");
process.exit(failures ? 1 : 0);
