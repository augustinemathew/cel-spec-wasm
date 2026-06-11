import { describe, expect, it } from 'vitest';

import {
  ARENA_LIST_ELEMENT_STRIDE,
  ARENA_MAP_ENTRY_STRIDE,
  CEL_VALUE_KIND_OFFSET,
  CEL_VALUE_PAYLOAD_OFFSET,
  CEL_VALUE_SIZE,
  CelErrorCode,
  CelKind,
} from './types.js';

// Smoke + wire-format pin.  These assertions mirror the frozen layout in
// `runtime/cel_data.h`; if the C++ side ever renumbers a kind or moves a
// payload offset, this test fails loudly rather than the codec silently
// misreading bytes.
describe('wire-format constants', () => {
  it('pins the CelValue layout (cel_data.h:31-200)', () => {
    expect(CEL_VALUE_SIZE).toBe(24);
    expect(CEL_VALUE_KIND_OFFSET).toBe(0);
    expect(CEL_VALUE_PAYLOAD_OFFSET).toBe(8);
    expect(ARENA_LIST_ELEMENT_STRIDE).toBe(24);
    expect(ARENA_MAP_ENTRY_STRIDE).toBe(48);
  });

  it('pins the CelKind discriminants', () => {
    expect(CelKind.NULL).toBe(0);
    expect(CelKind.INT).toBe(2);
    expect(CelKind.MESSAGE).toBe(10);
    expect(CelKind.ERROR).toBe(16);
    expect(CelKind.CIDR).toBe(19);
  });

  it('pins the CelErrorCode values', () => {
    expect(CelErrorCode.OVERFLOW).toBe(10);
    expect(CelErrorCode.NO_SUCH_KEY).toBe(15);
    expect(CelErrorCode.FIELD_NOT_FOUND).toBe(20);
  });
});
