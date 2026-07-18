/*
 * jsvm playground — Vite entry. Loads the engine from the npm package
 * (@mdy-docs/lamassu-js) and drives the editor + REPL UI. The wasm URL is
 * imported through Vite's `?url` asset handling and handed to the factory, so
 * the binary resolves correctly in dev and in the hashed production build.
 */
import { createLamassu } from "@mdy-docs/lamassu-js";
import wasmUrl from "@mdy-docs/lamassu-js/lamassu.wasm?url";

const SAMPLES = [
  ["Welcome",
`// Everything here runs in a C-based JS engine compiled to WASM.
// Try running this, then type  greet('you')  in the REPL →
function greet(who) { return \`Hello, \${who}, from jsvm!\`; }
const engine = { name: 'jsvm', lang: 'C', target: 'WebAssembly' };
for (const [k, v] of Object.entries(engine)) print(\`\${k}: \${v}\`);
greet('world');`],

  ["Closures",
`// Each counter keeps its own private state (real upvalue capture).
function makeCounter(start = 0) {
  let n = start;
  return { inc: () => ++n, dec: () => --n, value: () => n };
}
// makeCounter persists — call it again from the REPL: makeCounter(100)
const c = makeCounter(10);
c.inc(); c.inc(); c.dec();
c.value();`],

  ["Arrays",
`const nums = [5, 2, 8, 1, 9, 3, 7];
print('sorted :', [...nums].sort((a, b) => a - b).join(', '));
print('evens  :', nums.filter(n => n % 2 === 0).join(', '));
print('squares:', nums.map(n => n * n).join(', '));
print('sum    :', nums.reduce((a, b) => a + b, 0));
nums.filter(n => n > 4).map(n => n * 10);`],

  ["Destructuring",
`const user = { name: 'Ada', roles: ['admin', 'dev'], age: 36 };
const { name, roles: [primary, ...rest], age = 0 } = user;
print(\`\${name} (\${age}) — \${primary}, also \${rest.join('/')}\`);
const [first, , third, ...tail] = [10, 20, 30, 40, 50];
[first, third, tail];`],

  ["async / await",
`// The suspendable-fiber design makes await nearly free.
async function fetchUser(id) { return await Promise.resolve({ id, name: 'user' + id }); }
async function main() {
  const users = await Promise.all([fetchUser(1), fetchUser(2), fetchUser(3)]);
  return users.map(u => u.name).join(', ');
}
print('awaiting…');
await main();`],

  ["Promises",
`let log = 'start';
Promise.resolve().then(() => log += ' → t1').then(() => log += ' → t2');
Promise.reject('oops').catch(e => print('caught:', e));
const settled = await Promise.allSettled([Promise.resolve('ok'), Promise.reject('bad')]);
print('statuses:', settled.map(s => s.status).join(', '));
await Promise.resolve();
log;`],

  ["Regex",
`// ECMAScript RegExp, backed by a backtracking engine with a step budget.
const log = '2026-07-18 GET /home 200 · 2026-07-18 POST /login 403';
const re = /(\\d{4}-\\d{2}-\\d{2}) (\\w+) (\\S+) (\\d{3})/g;
for (const m of log.matchAll(re)) print(m[4], m[2].padEnd(4), m[3]);
'emails: ' + 'a@b.com, c@d.org'.match(/\\w+@\\w+\\.\\w+/g).join(' | ');`],

  ["JSON",
`const data = { title: 'jsvm', features: ['closures', 'async', 'modules'], meta: { v: 1, wasm: true } };
const text = JSON.stringify(data, null, 2);
print(text);
const parsed = JSON.parse(text);
\`round-trip: \${parsed.features.length} features, wasm=\${parsed.meta.wasm}\`;`],

  ["Errors",
`function risky(n) {
  if (n < 0) throw { message: 'negative not allowed' };
  return Math.sqrt(n);
}
let log = '';
for (const n of [16, -4, 9]) {
  try { log += \`√\${n}=\${risky(n)} \`; }
  catch (e) { log += \`[\${e.message}] \`; }
  finally { log += '· '; }
}
log.trim();`],

  ["Recursion",
`// Deep call stacks live on the fiber's heap, not the C stack.
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); }
const seq = [];
for (let i = 0; i < 12; i++) seq.push(fib(i));
print('fibonacci:', seq.join(', '));
\`10! = \${fact(10)}\`;`],

  ["Text",
`const s = '  The Quick Brown Fox  ';
print('trimmed :', JSON.stringify(s.trim()));
print('upper   :', s.trim().toUpperCase());
print('words   :', s.trim().split(' ').length);
print('slug    :', s.trim().toLowerCase().split(' ').join('-'));
s.trim().split('').reverse().join('');`],

  ["Math",
`print('π       =', Math.PI.toFixed(6));
print('√2      =', Math.sqrt(2).toFixed(6));
print('sin(π/6)=', Math.sin(Math.PI / 6).toFixed(4));
print('2^0.5   =', Math.pow(2, 0.5).toFixed(6));
print('(255)₁₆ =', (255).toString(16));
print('parseInt=', parseInt('0xFF'), parseInt('101', 2));
Math.hypot(3, 4);`],
];

const codeEl = document.getElementById("code");
const runBtn = document.getElementById("run");
const resetBtn = document.getElementById("reset");
const samplesEl = document.getElementById("samples");
const transcriptEl = document.getElementById("transcript");
let emptyEl = document.getElementById("empty");
const promptEl = document.getElementById("prompt");
const engineEl = document.getElementById("engine");
const statusEl = document.getElementById("status");
const kbdEl = document.getElementById("kbd");

const isMac = /Mac|iPhone|iPad/.test(navigator.platform);
kbdEl.textContent = isMac ? "⌘↵" : "Ctrl+↵";

let engine = null, ready = false, activeChip = null;
const history = [];
let histIdx = 0;

SAMPLES.forEach((s, i) => {
  const b = document.createElement("button");
  b.className = "chip";
  b.textContent = s[0];
  b.onclick = () => loadSample(i, b);
  samplesEl.appendChild(b);
});

function loadSample(i, chip) {
  codeEl.value = SAMPLES[i][1];
  if (activeChip) activeChip.classList.remove("active");
  activeChip = chip;
  chip.classList.add("active");
  if (ready) runSource(codeEl.value, "editor");
}

// Build the colored output lines for one eval result.
function renderOutput(text) {
  const out = document.createElement("div");
  out.className = "out";
  (text || "").split("\n").forEach((line, idx, arr) => {
    const span = document.createElement("span");
    if (line.indexOf("⇒") === 0) span.className = "result";
    else if (/^(Uncaught|SyntaxError|internal:)/.test(line)) span.className = "error";
    else span.className = "print";
    span.textContent = line + (idx < arr.length - 1 ? "\n" : "");
    out.appendChild(span);
  });
  return out;
}

function appendEntry(source, label, output) {
  if (emptyEl) { emptyEl.remove(); emptyEl = null; }
  const entry = document.createElement("div");
  entry.className = "entry";
  const src = document.createElement("div");
  src.className = "src";
  if (label) {
    const badge = document.createElement("span");
    badge.className = "badge";
    badge.textContent = label;
    src.appendChild(badge);
    const pre = document.createElement("span");
    pre.textContent = source.length > 200 ? source.slice(0, 200) + " …" : source;
    src.appendChild(pre);
  } else {
    const caret = document.createElement("span");
    caret.className = "caret";
    caret.textContent = "›";
    src.appendChild(caret);
    const expr = document.createElement("span");
    expr.textContent = source;
    src.appendChild(expr);
  }
  entry.appendChild(src);
  entry.appendChild(renderOutput(output));
  transcriptEl.appendChild(entry);
  transcriptEl.scrollTop = transcriptEl.scrollHeight;
}

async function runSource(code, label) {
  if (!ready || !code.trim()) return;
  const t0 = performance.now();
  let out;
  try {
    out = await engine.eval(code); // async: a __hostcall native may suspend
  } catch (e) {
    out = "internal: " + (e && e.message ? e.message : e);
  }
  const ms = performance.now() - t0;
  appendEntry(code, label, out || "(no output)");
  statusEl.textContent = (ms < 1 ? "<1" : ms.toFixed(1)) + " ms";
}

runBtn.onclick = () => runSource(codeEl.value, "editor");
resetBtn.onclick = () => {
  if (engine) engine.reset();
  transcriptEl.innerHTML = "";
  emptyEl = document.createElement("div");
  emptyEl.className = "empty";
  emptyEl.textContent = "Session reset — a fresh VM. Run a sample or type below.";
  transcriptEl.appendChild(emptyEl);
  statusEl.textContent = "VM reset";
};

codeEl.addEventListener("keydown", (e) => {
  if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
    e.preventDefault();
    runSource(codeEl.value, "editor");
  }
  if (e.key === "Tab") {
    e.preventDefault();
    const s = codeEl.selectionStart, en = codeEl.selectionEnd;
    codeEl.value = codeEl.value.slice(0, s) + "  " + codeEl.value.slice(en);
    codeEl.selectionStart = codeEl.selectionEnd = s + 2;
  }
});

promptEl.addEventListener("keydown", (e) => {
  if (e.key === "Enter") {
    const code = promptEl.value;
    if (!code.trim()) return;
    history.push(code);
    histIdx = history.length;
    runSource(code, null);
    promptEl.value = "";
  } else if (e.key === "ArrowUp") {
    if (histIdx > 0) { histIdx--; promptEl.value = history[histIdx]; e.preventDefault(); }
  } else if (e.key === "ArrowDown") {
    if (histIdx < history.length - 1) { histIdx++; promptEl.value = history[histIdx]; }
    else { histIdx = history.length; promptEl.value = ""; }
    e.preventDefault();
  }
});

createLamassu({ wasmUrl })
  .then((e) => {
    engine = e;
    ready = true;
    engineEl.textContent = "jsvm engine ready — " + SAMPLES.length + " samples";
    loadSample(0, samplesEl.firstChild);
    promptEl.focus();
  })
  .catch((e) => {
    engineEl.textContent = "failed to load engine: " + e;
  });
