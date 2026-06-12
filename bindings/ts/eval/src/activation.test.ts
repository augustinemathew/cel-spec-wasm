// Activation — the bound-variable bag's normalization contract.

import { describe, expect, it } from 'vitest';

import { normalizeActivation } from './activation.js';

describe('normalizeActivation', () => {
  it('returns an empty record for an omitted activation', () => {
    expect(normalizeActivation()).toEqual({});
  });

  it('returns an empty record for an explicit undefined', () => {
    expect(normalizeActivation(undefined)).toEqual({});
  });

  it('passes a concrete activation through unchanged', () => {
    const act = { x: 1n, name: 'Ada' };
    expect(normalizeActivation(act)).toBe(act);
  });

  it('preserves a null binding (a valid CelInput)', () => {
    const act = { x: null };
    const normalized = normalizeActivation(act);
    expect(Object.prototype.hasOwnProperty.call(normalized, 'x')).toBe(true);
    expect(normalized.x).toBeNull();
  });
});
