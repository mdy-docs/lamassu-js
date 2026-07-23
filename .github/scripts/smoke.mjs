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

// Guest-level await of a native-returned promise (js_promise_new /
// js_resolve — the OTHER async mechanism, distinct from __hostcall's
// Asyncify suspension): __nativeDefer(id) returns a pending promise;
// jsvm_settle_deferred settles it from a real JS timer callback, mirroring
// how a browser/Node host would resolve a promise from its own event loop.
{
  const settleDeferred = engine.module.cwrap("lamassu_settle_deferred", null, ["number", "string"]);
  await engine.eval("let OUT;");
  // Module suspends on the await; __nativeDefer's promise never auto-settles,
  // so this returns with the guest fiber still pending.
  await engine.eval("OUT = await __nativeDefer(1);");
  // Settle from a real event-loop callback, like a browser/Node host would.
  await new Promise((r) => setTimeout(() => (settleDeferred(1, "43"), r()), 20));
  check("guest await of native promise (__nativeDefer)", await engine.eval("OUT"), "⇒ 43");
}
check(
  "ReDoS step budget",
  await engine.eval("/(a+)+$/.test('a'.repeat(200) + 'b');"),
  "Uncaught RangeError: regular expression step budget exhausted",
);

// ES modules through the host loader: every module (root included) is
// fetched via loadModule/setModuleLoader; the canonicalizer resolves
// "./util.js" against its referrer so relative imports get distinct
// registry identities per directory. Loading is async on purpose — same
// Asyncify suspension as __hostcall.
{
  const modules = {
    "/main.js": "import { double } from './util.js'; print('loading main'); export default double(21);",
    "/util.js": "export const double = (n) => n * 2;",
  };
  engine.setModuleLoader(
    async (specifier) => {
      await new Promise((r) => setTimeout(r, 10));
      return modules[specifier];
    },
    (specifier, referrer) =>
      specifier.startsWith("./")
        ? referrer.slice(0, referrer.lastIndexOf("/") + 1) + specifier.slice(2)
        : specifier,
  );
  check("ES module graph (evalModule)", await engine.evalModule("/main.js"), "loading main\n⇒ 42");
  check(
    "module registry persists (no re-evaluation)",
    await engine.evalModule("/main.js"),
    "⇒ 42",
  );
  check(
    "dynamic import() from plain eval",
    await engine.eval("const m = await import('/util.js'); m.double(5);"),
    "⇒ 10",
  );
  check(
    "missing module rejects",
    await engine.evalModule("/nope.js"),
    'Uncaught module not found: "/nope.js"',
  );
}

engine.reset();
check("reset clears state", await engine.eval("typeof x;"), "⇒ undefined");

if (process.exitCode) {
  console.error("\nsmoke test FAILED");
} else {
  console.log("\nall smoke checks passed");
}
