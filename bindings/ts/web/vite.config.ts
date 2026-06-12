import { defineConfig } from 'vite';

// The app is fully static: compile runs `compiler.wasm` client-side
// (see `src/internal/compile-client.ts`) and eval is pure client-side TS.
// There is NO dev-server endpoint — the build is plain static files
// servable by any static host (GitHub Pages, an S3 bucket, …).

// The published site lives under the docs-site subpath on GitHub Pages
// (`https://augustinemathew.github.io/cel-spec-wasm/playground/`), so the
// production build must emit asset URLs rooted there.  `npm run dev` keeps
// serving from `/` for a friction-free local loop; only `vite build` (and
// `vite preview`) apply the Pages base.  Override with `VITE_BASE` if the
// site moves.
const PAGES_BASE = process.env.VITE_BASE ?? '/cel-spec-wasm/playground/';

// Both `compiler.wasm` and `cel_runtime.wasm` are wasi-threads reactors
// whose linear memory is declared `shared`, so the browser only
// instantiates them in a cross-origin-isolated context (a
// SharedArrayBuffer requires COOP + COEP).  Send the isolation headers
// from the dev/preview servers so the client-side compile + dynamic-link
// run work locally.  (GitHub Pages can't set response headers; the
// published site needs a coi-serviceworker shim for the same effect —
// tracked separately.)
const CROSS_ORIGIN_ISOLATION_HEADERS = {
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Embedder-Policy': 'require-corp',
};

export default defineConfig(({ command }) => ({
  base: command === 'build' ? PAGES_BASE : '/',
  server: { headers: CROSS_ORIGIN_ISOLATION_HEADERS },
  preview: { headers: CROSS_ORIGIN_ISOLATION_HEADERS },
  resolve: {
    alias: [
      {
        // Code imports the `monaco-editor` package root (so TypeScript
        // resolves its bundled types under nodenext), but the bundle only
        // needs the editor core — not every bundled language contribution.
        // An exact-match alias to `editor.api` keeps the build slim while
        // leaving deeper subpaths (e.g. the `editor.worker`) untouched.
        find: /^monaco-editor$/,
        replacement: 'monaco-editor/esm/vs/editor/editor.api',
      },
    ],
  },
  build: {
    target: 'es2022',
    // Under `dist/` so it shares the package's ignored build-output dir
    // (`dist/web/`) — the `tsc --build` tsbuildinfo lives at `dist/`, and
    // Vite only empties this subdir.
    outDir: 'dist/web',
    // Monaco's editor core is a single large chunk by design; the demo is
    // a showcase, not a size-budgeted production app, so raise the warning
    // ceiling above it rather than code-split the editor.
    chunkSizeWarningLimit: 3000,
  },
  // Monaco ships large worker bundles; this keeps Vite from warning on
  // them during the demo build.
  worker: {
    format: 'es',
  },
}));
