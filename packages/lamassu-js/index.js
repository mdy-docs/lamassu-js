/*
 * @mdy-docs/lamassu-js — friendly ESM wrapper over the Emscripten build.
 *
 * The heavy lifting lives in ./dist/lamassu.mjs (the engine compiled to a
 * WebAssembly ES module by Emscripten). This wrapper turns its low-level
 * cwrap surface into a small typed API and handles locating the .wasm in both
 * native ESM (import.meta.url) and bundled (explicit wasmUrl) setups.
 */
import createLamassuModule from "./dist/lamassu.mjs";

/**
 * Instantiate the engine. Resolves to a Lamassu instance.
 *
 * @param {object} [options]
 * @param {string} [options.wasmUrl]  Explicit URL for lamassu.wasm. Required
 *   when a bundler (Vite/webpack/…) relocates the module — import it with
 *   `import wasmUrl from "@mdy-docs/lamassu-js/lamassu.wasm?url"`. Omit for
 *   native ESM, where the sibling .wasm is found via import.meta.url.
 * @param {(text: string) => void} [options.print]  Sink for the engine's
 *   internal stdout (rarely needed; script output is returned by `eval`).
 * @param {Record<string, (...args: any[]) => any>} [options.natives]  Host
 *   functions callable from guest code (may be async — see below). Can be
 *   swapped later with `setNatives`.
 * @param {(specifier: string, referrer: string) => string | Promise<string>}
 *   [options.loadModule]  ES-module loader: given an import specifier and the
 *   importing module's specifier ("" for the root), return that module's
 *   source text (may be async — same Asyncify suspension as natives). Powers
 *   `evalModule` and guest-side dynamic `import()`. Can be swapped later with
 *   `setModuleLoader`.
 * @param {(specifier: string, referrer: string) => string | undefined}
 *   [options.canonicalizeModule]  Optional synchronous specifier
 *   canonicalization, run before dedupe/load: the returned string becomes the
 *   module's registry identity (so "./a.js" imported from two different
 *   referrers can be two different modules). Return a non-string to keep the
 *   raw specifier as the identity.
 */
export async function createLamassu(options = {}) {
  const moduleArg = {};
  if (options.wasmUrl) {
    moduleArg.locateFile = (path) =>
      path.endsWith(".wasm") ? options.wasmUrl : path;
  }
  if (typeof options.print === "function") {
    moduleArg.print = options.print;
    moduleArg.printErr = options.print;
  }

  const M = await createLamassuModule(moduleArg);

  /*
   * Host calls: guest code calls `__hostcall(name, argsJson)`, which
   * suspends the entire wasm execution (Asyncify) until the named native
   * settles — so a native may be async (a database query, a fetch, …) while
   * the guest sees an ordinary synchronous call. Arguments arrive as a JSON
   * string; the native's resolved value is JSON-encoded and returned to the
   * guest as a string (guest-side wrappers JSON.parse it). A native throwing
   * rethrows inside the guest at the call site.
   *
   * CAUTION: while an eval is suspended in a host call this instance must
   * not be re-entered (Asyncify is not reentrant). Embedders that nest evals
   * (an eval whose native triggers another eval) use one instance per level.
   */
  let natives = { ...(options.natives ?? {}) };
  M.lamassuHostCall = async (name, argsJson) => {
    const fn = natives[name];
    if (typeof fn !== "function") throw new Error(`unknown native "${name}"`);
    const args = argsJson === "" ? [] : JSON.parse(argsJson);
    const value = await fn(...(Array.isArray(args) ? args : [args]));
    return JSON.stringify(value === undefined ? null : value);
  };

  /*
   * Module loading rides the same Asyncify suspension as host calls: a guest
   * `import` suspends the wasm execution while `loadModule` fetches source.
   * The same non-reentrancy caution applies — a loader must not re-enter
   * this instance. `canonicalizeModule` is synchronous by contract (the
   * engine resolves registry identity inline, before dedupe).
   */
  let loadModule = options.loadModule ?? null;
  let canonicalizeModule = options.canonicalizeModule ?? null;
  M.lamassuLoadModule = (specifier, referrer) => {
    if (typeof loadModule !== "function") {
      throw new Error("no module loader installed (createLamassu loadModule option / setModuleLoader)");
    }
    return loadModule(specifier, referrer);
  };
  M.lamassuCanonicalizeModule = (specifier, referrer) =>
    typeof canonicalizeModule === "function"
      ? canonicalizeModule(specifier, referrer)
      : undefined;

  const evalRaw = M.cwrap("lamassu_eval", "string", ["string"], { async: true });
  const evalModuleRaw = M.cwrap("lamassu_eval_module", "string", ["string"], { async: true });
  const resetRaw = M.cwrap("lamassu_reset", null, []);

  return {
    /**
     * Evaluate a chunk of source in the persistent REPL context. Top-level
     * `let`/`const`/`function` declarations carry across calls. Resolves to
     * the combined `print()` output followed by the completion value
     * (prefixed with "⇒ ") or an error line — never rejects for guest
     * errors. Async because a guest `__hostcall` may suspend execution while
     * a native runs.
     * @param {string} source
     * @returns {Promise<string>}
     */
    eval(source) {
      return Promise.resolve(evalRaw(String(source ?? "")));
    },

    /**
     * Load, link, and evaluate the ES-module graph rooted at `specifier`.
     * Every module in the graph — the root included — is fetched through
     * the installed module loader (`loadModule` option / `setModuleLoader`).
     * Resolves like `eval`: any `print()` output followed by "⇒ " + the
     * root module's default export (its namespace object when there is no
     * default), or an error line. The module registry persists across calls
     * — the same canonical specifier evaluates once — until `reset()`.
     * @param {string} specifier
     * @returns {Promise<string>}
     */
    evalModule(specifier) {
      return Promise.resolve(evalModuleRaw(String(specifier ?? "")));
    },

    /** Replace the natives table (e.g. per embedder task). */
    setNatives(next) {
      natives = { ...(next ?? {}) };
    },

    /**
     * Install or replace the module loader (and, optionally, the specifier
     * canonicalizer) — same contracts as the `loadModule` /
     * `canonicalizeModule` options. Pass no arguments to uninstall.
     */
    setModuleLoader(load, canonicalize) {
      loadModule = load ?? null;
      canonicalizeModule = canonicalize ?? null;
    },

    /** Discard all REPL state and start from a fresh VM + context. */
    reset() {
      resetRaw();
    },

    /** The underlying Emscripten module, for advanced/low-level use. */
    module: M,
  };
}

export default createLamassu;
