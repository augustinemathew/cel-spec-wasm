import { describe, expect, it } from 'vitest';

import { ExternrefTable, Namespace } from './externref.js';

// The externref table is the JS side of the host-handle ABI (§A.4.5).
// Its load-bearing invariants — slot 0 = null sentinel, slots start at 1,
// the three namespaces are independent, reset clears everything back to
// empty — mirror the C++ contract in `eval/internal/cel_host.h:390-423`.

describe('Namespace — single handle namespace', () => {
  it('interns then looks up a round-trip', () => {
    const ns = new Namespace<string>();
    const slot = ns.intern('alpha');
    expect(ns.lookup(slot)).toBe('alpha');
  });

  it('hands out the first slot as 1 (slot 0 is the null sentinel)', () => {
    const ns = new Namespace<string>();
    expect(ns.intern('first')).toBe(1);
  });

  it('allocates monotonically increasing slots', () => {
    const ns = new Namespace<string>();
    expect(ns.intern('a')).toBe(1);
    expect(ns.intern('b')).toBe(2);
    expect(ns.intern('c')).toBe(3);
  });

  it('never deduplicates — equal values get distinct slots', () => {
    const ns = new Namespace<string>();
    const s1 = ns.intern('same');
    const s2 = ns.intern('same');
    expect(s1).not.toBe(s2);
    expect(ns.lookup(s1)).toBe('same');
    expect(ns.lookup(s2)).toBe('same');
  });

  it('round-trips every interned backing independently', () => {
    const ns = new Namespace<number>();
    const slots = [10, 20, 30].map((v) => ns.intern(v));
    expect(slots.map((s) => ns.lookup(s))).toEqual([10, 20, 30]);
  });

  it('reports size excluding the null sentinel', () => {
    const ns = new Namespace<string>();
    expect(ns.size).toBe(0);
    ns.intern('x');
    ns.intern('y');
    expect(ns.size).toBe(2);
  });

  it('preserves a backing that is itself a reference type', () => {
    const ns = new Namespace<{ id: number }>();
    const obj = { id: 7 };
    const slot = ns.intern(obj);
    // Identity, not just structural equality — the slot points AT the
    // backing, it does not copy it.
    expect(ns.lookup(slot)).toBe(obj);
  });

  // ── Slot-0 / out-of-range lookup contract (non-throwing → undefined) ──

  it('returns undefined for slot 0 (the null sentinel)', () => {
    const ns = new Namespace<string>();
    ns.intern('live');
    expect(ns.lookup(0)).toBeUndefined();
  });

  it('returns undefined for a slot past the high-water mark', () => {
    const ns = new Namespace<string>();
    ns.intern('only');
    expect(ns.lookup(2)).toBeUndefined();
    expect(ns.lookup(99)).toBeUndefined();
  });

  it('returns undefined (does not throw) for negative/garbage slots', () => {
    const ns = new Namespace<string>();
    ns.intern('only');
    expect(() => ns.lookup(-1)).not.toThrow();
    expect(ns.lookup(-1)).toBeUndefined();
    expect(ns.lookup(1.5)).toBeUndefined();
  });

  // ── reset() ──

  it('reset clears back to empty; next intern is slot 1 again', () => {
    const ns = new Namespace<string>();
    ns.intern('a');
    ns.intern('b');
    expect(ns.size).toBe(2);

    ns.reset();
    expect(ns.size).toBe(0);
    expect(ns.intern('fresh')).toBe(1);
    expect(ns.lookup(1)).toBe('fresh');
  });

  it('reset invalidates previously-handed-out slots', () => {
    const ns = new Namespace<string>();
    const slot = ns.intern('stale');
    ns.reset();
    expect(ns.lookup(slot)).toBeUndefined();
  });
});

describe('ExternrefTable — three independent namespaces', () => {
  it('exposes message / map / list namespaces', () => {
    const table = new ExternrefTable();
    expect(table.message).toBeInstanceOf(Namespace);
    expect(table.map).toBeInstanceOf(Namespace);
    expect(table.list).toBeInstanceOf(Namespace);
  });

  it('each namespace starts allocating at slot 1', () => {
    const table = new ExternrefTable();
    expect(table.message.intern({ m: true })).toBe(1);
    expect(table.map.intern(new Map())).toBe(1);
    expect(table.list.intern([])).toBe(1);
  });

  it('namespaces are isolated — interning in one does not affect another', () => {
    const table = new ExternrefTable();
    table.message.intern('m1');
    table.message.intern('m2');
    table.message.intern('m3');

    // map is untouched by the three message interns → its first slot is 1.
    expect(table.map.intern('first-map')).toBe(1);
    expect(table.list.intern('first-list')).toBe(1);
    expect(table.message.size).toBe(3);
    expect(table.map.size).toBe(1);
    expect(table.list.size).toBe(1);
  });

  it('the same JS value interned in two namespaces gets independent slots', () => {
    const table = new ExternrefTable();
    const shared = { shared: 1 };

    const mSlot = table.message.intern(shared);
    table.map.intern('filler-a');
    table.map.intern('filler-b');
    const mapSlot = table.map.intern(shared);

    // Same numeric slot would be a collision; the namespaces are disjoint
    // so the indices are allocated independently.
    expect(mSlot).toBe(1);
    expect(mapSlot).toBe(3);
    // A message-table lookup never resolves a map slot and vice-versa.
    expect(table.message.lookup(mSlot)).toBe(shared);
    expect(table.map.lookup(mapSlot)).toBe(shared);
    expect(table.message.lookup(mapSlot)).toBeUndefined();
  });

  it('a slot from one namespace does not resolve in another', () => {
    const table = new ExternrefTable();
    const listSlot = table.list.intern('only-in-list');
    expect(table.list.lookup(listSlot)).toBe('only-in-list');
    expect(table.message.lookup(listSlot)).toBeUndefined();
    expect(table.map.lookup(listSlot)).toBeUndefined();
  });

  it('table.reset clears ALL three namespaces (the between-Evals contract)', () => {
    const table = new ExternrefTable();
    table.message.intern('m');
    table.map.intern('k');
    table.list.intern('l');
    expect(table.message.size).toBe(1);
    expect(table.map.size).toBe(1);
    expect(table.list.size).toBe(1);

    table.reset();

    expect(table.message.size).toBe(0);
    expect(table.map.size).toBe(0);
    expect(table.list.size).toBe(0);
    // Every namespace is back to allocating from slot 1.
    expect(table.message.intern('again')).toBe(1);
    expect(table.map.intern('again')).toBe(1);
    expect(table.list.intern('again')).toBe(1);
  });

  it('slot 0 is undefined in every namespace before and after use', () => {
    const table = new ExternrefTable();
    expect(table.message.lookup(0)).toBeUndefined();
    expect(table.map.lookup(0)).toBeUndefined();
    expect(table.list.lookup(0)).toBeUndefined();
    table.message.intern('m');
    table.map.intern('k');
    table.list.intern('l');
    expect(table.message.lookup(0)).toBeUndefined();
    expect(table.map.lookup(0)).toBeUndefined();
    expect(table.list.lookup(0)).toBeUndefined();
  });
});
