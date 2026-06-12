import { describe, it } from 'vitest';

// `mountDemo` builds the static view (covered by `view.test.ts`) and then
// attaches the Monaco-backed controller.  Monaco-editor's entry pulls in
// browser-only globals (it cannot be `import`ed under node or jsdom — its
// main module touches `document`/`window` worker APIs at load), so the
// full mount can only be exercised in a real browser.  The compile → run
// wiring beneath the editor IS exercised headlessly in `run.test.ts`
// (compile via the dev-server handler → eval via `@cel-wasm/eval`); the
// view structure in `view.test.ts`; the variables / render / compile-client
// logic in their own colocated suites.  The client-side compile→run path
// (the SPA's only moving parts) is exercised end-to-end in `run.test.ts`
// against the committed `compiler.wasm`.  The Monaco integration itself is
// verified by `vite build` (a clean static bundle) plus the manual browser
// steps in `README.md`.
describe('@cel-wasm/web mountDemo', () => {
  it.skip('mounts the Monaco editor + controller into a root element', () => {
    // Blocked on a browser environment: `import 'monaco-editor'` fails
    // under node/jsdom. Un-skip and assert the editor + buttons render
    // once the suite runs in a headless browser (e.g. Playwright).
  });
});
