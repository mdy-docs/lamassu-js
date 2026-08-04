<!--
  The REPL playground, as a page of the documentation site.

  It used to be a separate Vite app deployed at the site root with the docs
  nested beneath it, which meant two builds, two visual identities, and no way
  to get from a guide page to the thing the guide describes. It is one site now.

  Everything here runs client-side: the engine is a ~1 MB WebAssembly module,
  so it is imported dynamically in onMounted rather than at module scope. That
  keeps it out of VitePress's static render (which has no DOM, and would pay
  for the download on every page) and off the critical path of every other page
  on the site.
-->
<script setup>
import { onMounted, ref } from "vue";
import { SAMPLES } from "./samples.js";

const codeEl = ref(null);
const transcriptEl = ref(null);
const promptEl = ref(null);
const status = ref("");
const engineStatus = ref("Loading WebAssembly…");
const activeSample = ref(0);
const entries = ref([]);
const emptyNote = ref("Run a sample, or type an expression below — state carries over.");
const runKey = ref("Ctrl+↵");

let engine = null;
let ready = false;
const history = [];
let histIdx = 0;

/* Split one eval result into lines tagged for colouring. The engine returns
 * print() output followed by "⇒ value", or an error line. */
function renderOutput(text) {
  return (text || "").split("\n").map((line) => ({
    text: line,
    kind: line.indexOf("⇒") === 0 ? "result"
      : /^(Uncaught|SyntaxError|internal:)/.test(line) ? "error"
      : "print",
  }));
}

function appendEntry(source, label, output) {
  emptyNote.value = "";
  entries.value.push({
    id: entries.value.length,
    label,
    source: source.length > 200 ? source.slice(0, 200) + " …" : source,
    lines: renderOutput(output),
  });
  requestAnimationFrame(() => {
    const t = transcriptEl.value;
    if (t) t.scrollTop = t.scrollHeight;
  });
}

async function runSource(code, label) {
  if (!ready || !code.trim()) return;
  const t0 = performance.now();
  let out;
  try {
    out = await engine.eval(code); /* async: a __hostcall native may suspend */
  } catch (e) {
    out = "internal: " + (e && e.message ? e.message : e);
  }
  const ms = performance.now() - t0;
  appendEntry(code, label, out || "(no output)");
  status.value = (ms < 1 ? "<1" : ms.toFixed(1)) + " ms";
}

function loadSample(i) {
  activeSample.value = i;
  if (codeEl.value) codeEl.value.value = SAMPLES[i][1];
  if (ready) runSource(SAMPLES[i][1], "editor");
}

function runEditor() {
  if (codeEl.value) runSource(codeEl.value.value, "editor");
}

function resetVm() {
  if (engine) engine.reset();
  entries.value = [];
  emptyNote.value = "Session reset — a fresh VM. Run a sample or type below.";
  status.value = "VM reset";
}

function onEditorKey(e) {
  if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
    e.preventDefault();
    runEditor();
  }
  if (e.key === "Tab") {
    e.preventDefault();
    const el = codeEl.value;
    const s = el.selectionStart, en = el.selectionEnd;
    el.value = el.value.slice(0, s) + "  " + el.value.slice(en);
    el.selectionStart = el.selectionEnd = s + 2;
  }
}

function onPromptKey(e) {
  const el = promptEl.value;
  if (e.key === "Enter") {
    const code = el.value;
    if (!code.trim()) return;
    history.push(code);
    histIdx = history.length;
    runSource(code, null);
    el.value = "";
  } else if (e.key === "ArrowUp") {
    if (histIdx > 0) { histIdx--; el.value = history[histIdx]; e.preventDefault(); }
  } else if (e.key === "ArrowDown") {
    if (histIdx < history.length - 1) { histIdx++; el.value = history[histIdx]; }
    else { histIdx = history.length; el.value = ""; }
    e.preventDefault();
  }
}

onMounted(async () => {
  runKey.value = /Mac|iPhone|iPad/.test(navigator.platform) ? "⌘↵" : "Ctrl+↵";
  try {
    const [{ createLamassu }, { default: wasmUrl }] = await Promise.all([
      import("@mdy-docs/lamassu-js"),
      import("@mdy-docs/lamassu-js/lamassu.wasm?url"),
    ]);
    /*
     * A REPL open to whatever a visitor types, so the sandbox limits are on.
     * Without them `while (true) {}` — the first thing people try — locks the
     * tab up: WebAssembly keeps the guest out of the page's memory, it does
     * not stop it spinning. Generous enough for the samples and any ordinary
     * experiment, and re-armed for every evaluation. See /security.
     */
    engine = await createLamassu({
      wasmUrl,
      fuel: 200_000_000,            /* ~0.7s of guest CPU per evaluation */
      heapLimit: 128 * 1024 * 1024,
    });
    ready = true;
    engineStatus.value = `engine ready — ${SAMPLES.length} samples`;
    loadSample(0);
    if (promptEl.value) promptEl.value.focus();
  } catch (e) {
    engineStatus.value = "failed to load engine: " + (e && e.message ? e.message : e);
  }
});
</script>

<template>
  <div class="pg">
    <div class="pg-main">
      <section class="col">
        <div class="bar">
          <span class="label">Editor</span>
          <button class="run" @click="runEditor">Run ▶</button>
          <button title="Discard all session state and start a fresh VM" @click="resetVm">Reset</button>
          <span class="hint">or <kbd>{{ runKey }}</kbd></span>
        </div>
        <div class="samples">
          <button
            v-for="(s, i) in SAMPLES"
            :key="s[0]"
            class="chip"
            :class="{ active: activeSample === i }"
            @click="loadSample(i)"
          >{{ s[0] }}</button>
        </div>
        <textarea
          ref="codeEl"
          spellcheck="false"
          autocomplete="off"
          autocapitalize="off"
          @keydown="onEditorKey"
        ></textarea>
      </section>

      <section class="col">
        <div class="bar">
          <span class="label">REPL</span>
          <span class="hint">shares the VM with the editor — top-level <code>let</code>/<code>const</code>/<code>function</code> persist</span>
        </div>
        <div ref="transcriptEl" class="transcript">
          <div v-if="emptyNote" class="empty">{{ emptyNote }}</div>
          <div v-for="entry in entries" :key="entry.id" class="entry">
            <div class="src">
              <span v-if="entry.label" class="badge">{{ entry.label }}</span>
              <span v-else class="caret">›</span>
              <span>{{ entry.source }}</span>
            </div>
            <div class="out"><span
              v-for="(l, i) in entry.lines"
              :key="i"
              :class="l.kind"
            >{{ l.text }}{{ i < entry.lines.length - 1 ? "\n" : "" }}</span></div>
          </div>
        </div>
        <div class="prompt-row">
          <span class="prompt-caret">›</span>
          <input
            ref="promptEl"
            class="prompt"
            spellcheck="false"
            autocomplete="off"
            placeholder="type an expression and press Enter  ·  ↑ / ↓ for history"
            @keydown="onPromptKey"
          />
        </div>
      </section>
    </div>

    <footer class="pg-foot">
      <span>{{ engineStatus }}</span>
      <a href="https://github.com/mdy-docs/lamassu-js" target="_blank" rel="noopener">source on GitHub</a>
      <span class="status">{{ status }}</span>
    </footer>
  </div>
</template>

<style scoped>
/*
 * The standalone demo defined its own palette on :root and flipped it with
 * prefers-color-scheme. Inside VitePress that fights the site's own theme
 * toggle, which sets `.dark` on <html> and can disagree with the OS. The
 * variables are scoped to .pg and keyed off that class instead, so the
 * playground follows the switch in the navbar like every other page.
 */
.pg {
  --pg-bg: #f4f6fb; --pg-panel: #ffffff; --pg-panel-2: #eef2fa; --pg-edge: #d7deee;
  --pg-fg: #1a2233; --pg-muted: #5a6784; --pg-accent: #2f6fed; --pg-accent-2: #159a75;
  --pg-err: #c0392b; --pg-ok: #159a75; --pg-dim: #8794ac; --pg-run-fg: #fff;
  --pg-mono: ui-monospace, SFMono-Regular, "SF Mono", Menlo, Consolas, monospace;
  border: 1px solid var(--pg-edge);
  border-radius: 10px;
  overflow: hidden;
  margin-top: 24px;
}
/* Not :global(html.dark) .pg — Vue collapses that to `html.dark` alone, which
 * loses to the light values set directly on .pg. Written this way the scoped
 * transform yields `html.dark .pg[data-v-…]`, which outranks them. */
html.dark .pg {
  --pg-bg: #0f1420; --pg-panel: #161c2b; --pg-panel-2: #1c2436; --pg-edge: #2a3550;
  --pg-fg: #e6ebf5; --pg-muted: #93a1bd; --pg-accent: #6ea8fe; --pg-accent-2: #7ee0c0;
  --pg-err: #ff8a8a; --pg-ok: #7ee0c0; --pg-dim: #6b7a99; --pg-run-fg: #08101f;
}

.pg-main {
  display: grid; grid-template-columns: 1fr 1fr; gap: 1px;
  background: var(--pg-edge); min-height: 0;
}
@media (max-width: 960px) { .pg-main { grid-template-columns: 1fr; } }
.col {
  background: var(--pg-bg); display: flex; flex-direction: column;
  min-height: 0; min-width: 0; height: 460px;
}
.bar {
  display: flex; align-items: center; gap: 8px; flex-wrap: wrap;
  padding: 10px 14px; background: var(--pg-panel);
  border-bottom: 1px solid var(--pg-edge);
}
.bar .label {
  font: 600 11px/1 var(--pg-mono); text-transform: uppercase;
  letter-spacing: .8px; color: var(--pg-muted); margin-right: 2px;
}
.pg button {
  font: inherit; font-size: 13px; color: var(--pg-fg); background: var(--pg-panel-2);
  border: 1px solid var(--pg-edge); border-radius: 8px; padding: 6px 12px;
  cursor: pointer; transition: background .12s, border-color .12s, transform .05s;
}
.pg button:hover { border-color: var(--pg-accent); }
.pg button:active { transform: translateY(1px); }
.pg button.run {
  background: var(--pg-accent); color: var(--pg-run-fg);
  border-color: transparent; font-weight: 600;
}
.pg button.run:hover { filter: brightness(1.08); }
.samples {
  display: flex; gap: 6px; flex-wrap: wrap; padding: 10px 14px;
  background: var(--pg-panel); border-bottom: 1px solid var(--pg-edge);
}
.pg button.chip {
  font-size: 12.5px; padding: 4px 10px; border-radius: 999px;
  background: transparent; border: 1px solid var(--pg-edge); color: var(--pg-muted);
}
.pg button.chip:hover { color: var(--pg-fg); border-color: var(--pg-accent); }
.pg button.chip.active {
  color: var(--pg-bg); background: var(--pg-accent);
  border-color: transparent; font-weight: 600;
}
textarea {
  flex: 1; width: 100%; resize: none; border: 0; outline: 0; padding: 16px;
  background: var(--pg-bg); color: var(--pg-fg); font-family: var(--pg-mono);
  font-size: 13.5px; line-height: 1.55; tab-size: 2; min-height: 0;
}
.transcript { flex: 1; overflow: auto; min-height: 0; padding: 8px 0; }
.entry {
  padding: 6px 16px;
  border-bottom: 1px solid color-mix(in srgb, var(--pg-edge) 50%, transparent);
}
.entry .src {
  font-family: var(--pg-mono); font-size: 13px; white-space: pre-wrap;
  word-break: break-word; color: var(--pg-fg); display: flex; gap: 8px;
}
.entry .src .caret { color: var(--pg-accent); font-weight: 700; user-select: none; }
.entry .badge {
  font: 600 10px/1 var(--pg-mono); text-transform: uppercase; letter-spacing: .6px;
  color: var(--pg-muted); border: 1px solid var(--pg-edge); border-radius: 5px;
  padding: 3px 6px; user-select: none; align-self: flex-start;
}
.entry .out {
  font-family: var(--pg-mono); font-size: 13px; white-space: pre-wrap;
  word-break: break-word; margin-top: 4px; color: var(--pg-fg);
}
.entry .out .result { color: var(--pg-ok); }
.entry .out .error { color: var(--pg-err); }
.entry .out .print { color: var(--pg-fg); }
.prompt-row {
  display: flex; align-items: center; gap: 8px; padding: 10px 14px;
  background: var(--pg-panel); border-top: 1px solid var(--pg-edge);
}
.prompt-caret { color: var(--pg-accent); font: 700 15px/1 var(--pg-mono); }
input.prompt {
  flex: 1; border: 0; outline: 0; background: transparent; color: var(--pg-fg);
  font-family: var(--pg-mono); font-size: 13.5px; padding: 6px 0;
}
input.prompt::placeholder { color: var(--pg-dim); }
.hint { color: var(--pg-muted); font-size: 12px; }
.hint code { font-family: var(--pg-mono); font-size: 12px; }
.empty { color: var(--pg-dim); font-size: 13px; padding: 16px; font-style: italic; }
kbd {
  font: 600 11px/1 var(--pg-mono); background: var(--pg-panel-2);
  border: 1px solid var(--pg-edge); border-bottom-width: 2px;
  border-radius: 5px; padding: 3px 5px;
}
.pg-foot {
  padding: 10px 16px; border-top: 1px solid var(--pg-edge); color: var(--pg-muted);
  font-size: 12.5px; display: flex; gap: 16px; flex-wrap: wrap; align-items: center;
  background: var(--pg-panel);
}
.pg-foot a { color: var(--pg-accent); text-decoration: none; font-weight: 500; }
.pg-foot a:hover { text-decoration: underline; }
.pg-foot .status { color: var(--pg-muted); margin-left: auto; }
</style>
