// Property get/set on objects that all share one shape (same keys, same
// insertion order) — the case shapes (hidden classes) and per-site inline
// caches of (shape, slot) would target directly. Today every access is a
// hashed, pointer-compared lookup in JsObject's open-addressed table keyed
// by interned atom; this is the baseline that change should move.
import { run } from "./_util.js";

function Point(x, y) {
  this.x = x;
  this.y = y;
}

run("prop_monomorphic", 2000000, (iters) => {
  const p = new Point(1, 2);
  let sum = 0;
  for (let i = 0; i < iters; i++) {
    p.x = i;
    p.y = i + 1;
    sum += p.x + p.y;
  }
  return sum;
});
