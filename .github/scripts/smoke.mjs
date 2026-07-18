/*
 * CI smoke test: load the freshly built package artifact and confirm the
 * engine actually instantiates and evaluates before we publish it. Run from
 * the repo root after `make pkg` (Node ESM locates the sibling .wasm via
 * import.meta.url — no bundler needed).
 */
import { createLamassu } from "../../packages/lamassu-js/index.js";

const engine = await createLamassu({
  natives: {
    // async on purpose: proves a host call can take time while the guest
    // sees a synchronous __hostcall.
    twice: async (n) => {
      await new Promise((r) => setTimeout(r, 20));
      return n * 2;
    },
  },
});

function check(label, actual, expected) {
  if (actual !== expected) {
    console.error(`FAIL ${label}\n  expected: ${JSON.stringify(expected)}\n  actual:   ${JSON.stringify(actual)}`);
    process.exitCode = 1;
  } else {
    console.log(`ok  ${label}`);
  }
}

check("arithmetic", await engine.eval("const x = 40; x + 2;"), "⇒ 42");
check("persistent state", await engine.eval("print('hi'); x * 10;"), "hi\n⇒ 400");
check("regex", await engine.eval("'a1b2'.match(/\\d/g).join(',');"), "⇒ 1,2");
check("closures", await engine.eval("const f = (() => { let n = 0; return () => ++n; })(); f(); f(); f();"), "⇒ 3");
check(
  "async host call (__hostcall)",
  await engine.eval("JSON.parse(__hostcall('twice', JSON.stringify([21])));"),
  "⇒ 42",
);
check(
  "unknown native throws in guest",
  await engine.eval("try { __hostcall('nope', '[]') } catch (e) { 'caught: ' + e }"),
  "⇒ caught: unknown native \"nope\"",
);
check(
  "ReDoS step budget",
  await engine.eval("/(a+)+$/.test('a'.repeat(200) + 'b');"),
  "Uncaught RangeError: regular expression step budget exhausted",
);
engine.reset();
check("reset clears state", await engine.eval("typeof x;"), "⇒ undefined");

if (process.exitCode) {
  console.error("\nsmoke test FAILED");
} else {
  console.log("\nall smoke checks passed");
}
