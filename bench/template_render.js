// Composite, closer to the workload lamassu was first built for (scripts in a
// web-framework templating language): iterate an array of
// same-shape row objects, read several properties per row, build a string
// via a template literal. Property access dominates; string building is
// along for the ride since that's what a template does with it.
import { run } from "./_util.js";

function Row(id, name, qty) {
  this.id = id;
  this.name = name;
  this.qty = qty;
}

const N = 2000;
const rows = [];
for (let i = 0; i < N; i++) rows.push(new Row(i, `item-${i}`, i % 50));

run("template_render", 300, (passes) => {
  let totalLen = 0;
  for (let pass = 0; pass < passes; pass++) {
    for (let i = 0; i < N; i++) {
      const row = rows[i];
      const line = `<tr><td>${row.id}</td><td>${row.name}</td><td>${row.qty}</td></tr>`;
      totalLen += line.length;
    }
  }
  return totalLen;
});
