// Entry point for the cel-wasm browser demo (Vite + Monaco).
//
// The demo proves the architecture's thesis: type a CEL expression →
// compile it to a portable `.wasm` Program → download that artifact →
// run it right there in the browser.  Compile goes through the dev-server
// `/api/compile` endpoint (the browser can't subprocess the native CLI);
// eval is pure client-side TypeScript via `@cel-wasm/eval`.

import { attachController } from './internal/controller.js';
import { buildView } from './internal/view.js';

/** Bootstrap the demo into the given root element. */
export function mountDemo(root: HTMLElement): void {
  const view = buildView(root);
  attachController(view);
}
