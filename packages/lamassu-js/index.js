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

  const evalRaw = M.cwrap("jsvm_eval", "string", ["string"], { async: true });
  const resetRaw = M.cwrap("jsvm_reset", null, []);

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

    /** Replace the natives table (e.g. per embedder task). */
    setNatives(next) {
      natives = { ...(next ?? {}) };
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
