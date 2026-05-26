import { describe, it, expect } from 'vitest';
import { ExternrefTable } from '../src/externref.js';

describe('ExternrefTable', () => {
  it('interns monotonically starting at slot 1 (0 is the sentinel)', () => {
    const t = new ExternrefTable<string>();
    expect(t.internMessage('a')).toBe(1);
    expect(t.internMessage('b')).toBe(2);
    expect(t.lookupMessage(1)).toBe('a');
    expect(t.lookupMessage(2)).toBe('b');
    expect(t.lookupMessage(0)).toBeUndefined();
  });

  it('returns undefined for an out-of-range slot', () => {
    const t = new ExternrefTable<string>();
    t.internMessage('a');
    expect(t.lookupMessage(99)).toBeUndefined();
  });

  it('keeps the three namespaces independent', () => {
    const t = new ExternrefTable<string, string, string>();
    const m = t.internMessage('msg');
    const mp = t.internMap('map');
    const l = t.internList('list');
    // All three got slot 1 — but each namespace resolves only its own.
    expect([m, mp, l]).toEqual([1, 1, 1]);
    expect(t.lookupMessage(1)).toBe('msg');
    expect(t.lookupMap(1)).toBe('map');
    expect(t.lookupList(1)).toBe('list');
    // A slot from one namespace does not resolve in another... here all
    // are populated; prove cross-namespace isolation by interning into
    // only one and querying the others.
    const t2 = new ExternrefTable<string, string, string>();
    const only = t2.internList('x');
    expect(t2.lookupList(only)).toBe('x');
    expect(t2.lookupMessage(only)).toBeUndefined();
    expect(t2.lookupMap(only)).toBeUndefined();
  });

  it('reset() clears every namespace and restarts numbering at 1', () => {
    const t = new ExternrefTable<string, string, string>();
    t.internMessage('a');
    t.internMap('b');
    t.internList('c');
    t.reset();
    expect(t.lookupMessage(1)).toBeUndefined();
    expect(t.lookupMap(1)).toBeUndefined();
    expect(t.lookupList(1)).toBeUndefined();
    // numbering restarts at 1 (sentinel 0 preserved).
    expect(t.internMessage('fresh')).toBe(1);
    expect(t.lookupMessage(1)).toBe('fresh');
  });
});
