/*
 * Sample programs for the playground, lifted verbatim from the standalone
 * demo. Kept in a plain module rather than inside the component because they
 * are dense with backticks and template literals, which read badly nested in
 * an SFC.
 */
export const SAMPLES = [
  ["Welcome",
`// Everything here runs in a C-based JS engine compiled to WASM.
// Try running this, then type  greet('you')  in the REPL →
function greet(who) { return \`Hello, \${who}, from lamassu!\`; }
const engine = { name: 'lamassu', lang: 'C', target: 'WebAssembly' };
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
`const data = { title: 'lamassu', features: ['closures', 'async', 'modules'], meta: { v: 1, wasm: true } };
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
