import { describe, expect, it } from 'vitest';

import { mountDemo } from './index.js';

// Smoke test for the web demo scaffold.  The Monaco integration lands in
// WI-4.1; here we only assert the bootstrap entry point exists and is a
// loud stub (no real DOM is needed for that).
describe('@cel-wasm/web scaffold', () => {
  it('exposes mountDemo() as a stub that throws', () => {
    const fakeRoot = {} as unknown as HTMLElement;
    expect(() => {
      mountDemo(fakeRoot);
    }).toThrow('WI-4.1');
  });
});
