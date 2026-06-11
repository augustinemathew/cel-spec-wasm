// Entry point for the cel-wasm browser demo (Vite + Monaco).
//
// The demo proves the architecture's thesis: type a CEL expression →
// compile it to a portable `.wasm` Program → download that artifact →
// run it right there in the browser.  Both compile (via `compiler.wasm`,
// run client-side) and eval (via `@cel-wasm/eval`) are pure client-side
// TypeScript — the whole app is static, with no server at any step.

import { attachController } from './internal/controller.js';
import { buildView } from './internal/view.js';

/** Bootstrap the demo into the given root element. */
export function mountDemo(root: HTMLElement): void {
  const view = buildView(root);
  attachController(view);
}
