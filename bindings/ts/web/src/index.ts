// Entry point for the cel-wasm browser demo (Vite + Monaco).
//
// The demo proves the architecture's thesis: type a CEL expression →
// compile it to a portable `.wasm` Program → download that artifact →
// run it right there in the browser.  The Monaco integration and the
// compile/download/run loop land in WI-4.1; this stub fixes the public
// bootstrap entry point.

/** Bootstrap the demo into the given root element.  Implemented in WI-4.1. */
export function mountDemo(_root: HTMLElement): void {
  throw new Error('mountDemo() is a stub until WI-4.1 (browser demo)');
}
