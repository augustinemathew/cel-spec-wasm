import { describe, it, expect } from 'vitest';
import { Activation } from '../src/activation.js';
import { Value } from '../src/value.js';

describe('Activation', () => {
  it('binds and finds', () => {
    const act = new Activation();
    act.bind('x', Value.int(41n));
    expect(act.find('x')).toEqual(Value.int(41n));
  });

  it('bind is fluent (chains) and reflects every binding', () => {
    const act = new Activation()
      .bind('x', Value.int(1n))
      .bind('s', Value.string('hi'));
    expect(act.find('x')).toEqual(Value.int(1n));
    expect(act.find('s')).toEqual(Value.string('hi'));
    expect(act.names()).toEqual(['x', 's']);
  });

  it('re-binding a name overwrites', () => {
    const act = new Activation().bind('x', Value.int(1n));
    act.bind('x', Value.int(2n));
    expect(act.find('x')).toEqual(Value.int(2n));
    expect(act.names()).toEqual(['x']);
  });

  it('find on an unbound name is undefined; has reflects membership', () => {
    const act = new Activation().bind('x', Value.int(1n));
    expect(act.find('missing')).toBeUndefined();
    expect(act.has('x')).toBe(true);
    expect(act.has('missing')).toBe(false);
  });

  it('names is empty for a fresh activation', () => {
    expect(new Activation().names()).toEqual([]);
  });
});
