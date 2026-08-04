/*
 * A host for build/lamassu-reactor.wasm — the smallest thing that shows the
 * fleet shape working.
 *
 *   node tools/reactor-host.mjs build/lamassu-reactor.wasm page.jsbc [input...]
 *
 * Node rather than a real embedding on purpose: the reactor is a wasip1 core
 * module, so any WASI host can drive it, and every machine here already has
 * Node. A production host would embed wasmtime and add the things Node cannot
 * give you — AOT-compiled modules, the pooling allocator, copy-on-write memory,
 * epoch interruption — but the ABI it would call is exactly the one below.
 *
 * NO PREOPENS. The reactor is handed no directories at all, which is not a
 * precaution so much as an observation: its import list has no path_open in it.
 * Bytecode arrives through linear memory, from the host, and there is no code
 * in the guest that could read a file even if it wanted to.
 */
import fs from 'node:fs';
import process from 'node:process';

process.removeAllListeners('warning');
process.on('warning', (w) => {
  if (w.name !== 'ExperimentalWarning') console.error(w.stack || String(w));
});
const { WASI } = await import('node:wasi');

const [wasmPath, bcPath, ...inputs] = process.argv.slice(2);
if (!wasmPath || !bcPath) {
  console.error('usage: reactor-host.mjs <reactor.wasm> <file.jsbc> [input...]');
  process.exit(2);
}

const wasi = new WASI({ version: 'preview1', args: [], env: {}, preopens: {} });
const module = await WebAssembly.compile(fs.readFileSync(wasmPath));
const instance = await WebAssembly.instantiate(module, wasi.getImportObject());
wasi.initialize(instance); // reactor: _initialize, not _start

const x = instance.exports;
const mem = () => new Uint8Array(x.memory.buffer);

/* Copies bytes into guest memory and returns [ptr, len]; the guest owns it. */
function put(bytes) {
  const ptr = x.lam_alloc(bytes.length);
  if (!ptr) throw new Error('lam_alloc failed');
  mem().set(bytes, ptr);
  return [ptr, bytes.length];
}

function output() {
  const ptr = x.lam_out_ptr(), len = x.lam_out_len();
  return Buffer.from(mem().subarray(ptr, ptr + len)).toString('utf8');
}

// 1 MB heap cap, 200M fuel. A real host also bounds execution from outside.
x.lam_configure(200_000_000n, 1 << 20);

// --- init: load the bytecode ONCE. A Wizer snapshot would be taken here. ---
const [bcPtr, bcLen] = put(fs.readFileSync(bcPath));
if (x.lam_load(bcPtr, bcLen) !== 0) {
  console.error('load failed:', output());
  process.exit(1);
}
x.lam_free(bcPtr, bcLen);
console.log(`loaded ${bcPath} (${bcLen} bytes)`);

// --- per request: set input, run, read output. Same loaded function. ---
for (const input of inputs.length ? inputs : ['']) {
  const bytes = Buffer.from(input, 'utf8');
  if (bytes.length) {
    const [p, l] = put(bytes);
    x.lam_set_input(p, l);
    x.lam_free(p, l);
  } else {
    x.lam_set_input(0, 0);
  }
  const rc = x.lam_run();
  process.stdout.write(`--- run(${JSON.stringify(input)}) rc=${rc}\n${output()}`);
}
