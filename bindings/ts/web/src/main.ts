// The Vite browser entry — mount the demo into `#app` once the DOM is
// ready.  Separated from `index.ts` (the reusable `mountDemo` API) so the
// public entry stays free of bootstrap side effects.

import './styles.css';

import EditorWorker from 'monaco-editor/esm/vs/editor/editor.worker?worker';

import { mountDemo } from './index.js';

// Monaco resolves its language/editor web workers through this hook.  The
// demo registers only a Monarch tokenizer (no language service), so the
// base editor worker is the only one needed; pointing every worker label
// at it keeps Monaco from trying to fetch language-specific bundles.
self.MonacoEnvironment = {
  getWorker(_workerId: string, _label: string): Worker {
    return new EditorWorker();
  },
};

const root = document.getElementById('app');
if (root === null) {
  throw new Error('demo bootstrap: #app element not found');
}
mountDemo(root);
