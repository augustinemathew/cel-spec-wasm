import { describe, expect, it } from 'vitest';

import { compile } from './index.js';

// Smoke test for the compiler package scaffold.  The real compile path
// lands in WI-2.3; here we only assert the stub exists and surfaces a
// rejection rather than a silent no-op.
describe('@cel-wasm/compiler scaffold', () => {
  it('exposes compile() as a stub that rejects', async () => {
    await expect(compile('1 + 2')).rejects.toThrow('WI-2.3');
  });
});
