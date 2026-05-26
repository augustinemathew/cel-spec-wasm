import { describe, it, expect } from 'vitest';
import { Instance, EvalError, type RuntimeHandles } from '../src/instance.js';
import { Activation } from '../src/activation.js';
import { Value } from '../src/value.js';
import { Repr, type CelAbi, type VariableEntry } from '../src/abi.js';
import { CelKind, encodeInlineScalar, type CelValue } from '../src/celvalue.js';
import { ExternrefTable } from '../src/externref.js';
import type {
  ListBacking,
  MapBacking,
  MessageBacking,
} from '../src/host/backing.js';
import type { TrampolineContext } from '../src/host/trampolines.js';

// A test harness with a real WebAssembly.Memory (hermetic — no module
// instantiation) + a fake bump `malloc`/`arena_alloc` + a settable
// `evalFn` that returns whichever offset the test points it at, plus the
// externref table so host-backed marshal/decode is exercised without the
// runtime wasm.
function harness(variables: readonly VariableEntry[] = []): {
  inst: Instance;
  bytes: () => Uint8Array;
  refs: ExternrefTable<MessageBacking, MapBacking, ListBacking>;
  setResult: (offset: number) => void;
} {
  const memory = new WebAssembly.Memory({ initial: 1 });
  const bytes = (): Uint8Array => new Uint8Array(memory.buffer);
  let cursor = 2048;
  let resultOffset = 0;
  const alloc = (size: number): number => {
    const p = cursor;
    cursor += size;
    return p;
  };
  const abi: CelAbi = {
    version: 1,
    runtimeAbiVersion: 2,
    variables,
    fields: [],
    types: [],
  };
  const refs = new ExternrefTable<MessageBacking, MapBacking, ListBacking>();
  const ctx: TrampolineContext = {
    memory,
    refs,
    fields: new Map(),
    types: new Map(),
    arenaAlloc: alloc,
  };
  const h: RuntimeHandles = {
    ctx,
    malloc: alloc,
    evalFn: (): number => resultOffset,
    abi: variables.length > 0 ? abi : null,
  };
  return {
    inst: new Instance(h),
    bytes,
    refs,
    setResult: (offset: number): void => {
      resultOffset = offset;
    },
  };
}

function variable(
  name: string,
  slotOffset: number,
  repr: number,
): VariableEntry {
  return { name, localIndex: 0, slotOffset, repr };
}

describe('Instance.eval — no activation', () => {
  it('decodes whatever $eval points at (variable-free)', () => {
    const h = harness();
    encodeInlineScalar(h.bytes(), 64, Value.int(3n));
    h.setResult(64);
    expect(h.inst.eval()).toEqual({ kind: CelKind.Int, int: 3n });
  });

  it('an activation with no declared variables (null abi) marshals nothing', () => {
    const h = harness(); // no variables → abi is null
    encodeInlineScalar(h.bytes(), 80, Value.bool(true));
    h.setResult(80);
    // Passing an activation still works — there's just nothing to bind.
    const r = h.inst.eval(new Activation().bind('ignored', Value.int(1n)));
    expect(r).toEqual({ kind: CelKind.Bool, bool: true });
  });
});

describe('Instance.eval — scalar marshalling', () => {
  it('marshals an int binding into its slot (echoed back by $eval)', () => {
    const h = harness([variable('x', 64, Repr.Int)]);
    h.setResult(64); // $eval "returns" the marshalled slot
    const r = h.inst.eval(new Activation().bind('x', Value.int(41n)));
    expect(r).toEqual({ kind: CelKind.Int, int: 41n });
  });

  it('marshals a string binding via malloc (survives arena_reset)', () => {
    const h = harness([variable('s', 64, Repr.String)]);
    h.setResult(64);
    const r = h.inst.eval(new Activation().bind('s', Value.string('hi')));
    expect(r).toEqual({ kind: CelKind.String, value: 'hi' });
  });

  it('marshals a bytes binding via malloc', () => {
    const h = harness([variable('b', 64, Repr.Bytes)]);
    h.setResult(64);
    const payload = new Uint8Array([0, 9, 255]);
    const r = h.inst.eval(new Activation().bind('b', Value.bytes(payload)));
    expect(r.kind).toBe(CelKind.Bytes);
    if (r.kind === CelKind.Bytes) {
      expect([...r.bytes]).toEqual([0, 9, 255]);
    }
  });
});

describe('Instance.eval — marshal errors', () => {
  it('throws when a declared variable is not bound', () => {
    const h = harness([variable('x', 64, Repr.Int)]);
    expect(() => h.inst.eval(new Activation())).toThrow(
      /variable 'x' declared but not bound/,
    );
  });

  it('throws on an inline-scalar kind mismatch', () => {
    const h = harness([variable('x', 64, Repr.Int)]);
    expect(() =>
      h.inst.eval(new Activation().bind('x', Value.bool(true))),
    ).toThrow(EvalError);
  });

  it('throws on a string/bytes kind mismatch', () => {
    const h = harness([variable('s', 64, Repr.String)]);
    expect(() =>
      h.inst.eval(new Activation().bind('s', Value.int(1n))),
    ).toThrow(/kind mismatch/);
  });

  it('throws on a genuinely unsupported repr (e.g. duration)', () => {
    const h = harness([variable('d', 64, Repr.Duration)]);
    expect(() =>
      h.inst.eval(new Activation().bind('d', Value.int(1n))),
    ).toThrow(/marshal not implemented for repr 12/);
  });

  it('throws on a host-backed repr/kind mismatch', () => {
    const h = harness([variable('m', 64, Repr.Message)]);
    expect(() =>
      h.inst.eval(new Activation().bind('m', Value.int(1n))),
    ).toThrow(/binding kind mismatch: repr 10/);
  });
});

describe('Instance.eval — host-backed marshalling (intern + ref)', () => {
  // The bound aggregate is interned into the externref table and stamped
  // as a {kind, slot} ref at the variable's slot; `$eval` echoes the slot
  // back, so the decode resolves the same backing.
  it('marshals + decodes a message binding (round-trip through refs)', () => {
    const h = harness([variable('u', 64, Repr.Message)]);
    h.setResult(64);
    const r = h.inst.eval(
      new Activation().bind('u', Value.object({ name: 'Ann' })),
    );
    expect(r.kind).toBe(CelKind.Message);
    if (r.kind === CelKind.Message) {
      expect(r.backing.getField('name')).toEqual({
        host: 'scalar',
        value: { kind: CelKind.String, value: 'Ann' },
      });
    }
  });

  it('marshals + decodes a list binding', () => {
    const h = harness([variable('xs', 64, Repr.List)]);
    h.setResult(64);
    const r = h.inst.eval(
      new Activation().bind('xs', Value.list([Value.int(1n), Value.int(2n)])),
    );
    expect(r.kind).toBe(CelKind.ListHost);
    if (r.kind === CelKind.ListHost) {
      expect(r.backing.size).toBe(2);
    }
  });

  it('marshals + decodes a map binding', () => {
    const h = harness([variable('m', 64, Repr.Map)]);
    h.setResult(64);
    const r = h.inst.eval(
      new Activation().bind(
        'm',
        Value.map([[Value.string('k'), Value.int(7n)]]),
      ),
    );
    expect(r.kind).toBe(CelKind.MapHost);
    if (r.kind === CelKind.MapHost) {
      expect(r.backing.size).toBe(1);
    }
  });
});

describe('Instance.eval — result decode', () => {
  it('decodes each scalar result kind', () => {
    const cases: CelValue[] = [
      { kind: CelKind.Null },
      { kind: CelKind.Bool, bool: true },
      { kind: CelKind.Uint, uint: 7n },
      { kind: CelKind.Double, double: 2.5 },
    ];
    for (const want of cases) {
      const h = harness();
      encodeInlineScalar(h.bytes(), 80, want);
      h.setResult(80);
      expect(h.inst.eval()).toEqual(want);
    }
  });

  it('decodes UNKNOWN as the unknown sentinel', () => {
    const h = harness();
    new DataView(h.bytes().buffer).setUint32(80, CelKind.Unknown, true);
    h.setResult(80);
    expect(h.inst.eval()).toEqual(Value.unknown());
  });

  // The success arms (resolving a slot back to its backing) are covered by
  // the marshal round-trip tests above; here we cover the dangling-slot
  // throw for each host kind (a ref to a slot the table never interned —
  // `refs.reset()` runs at eval start, so slot 99 is always empty).
  it.each([
    [CelKind.Message, /message slot 99 is not interned/],
    [CelKind.ListHost, /list slot 99 is not interned/],
    [CelKind.MapHost, /map slot 99 is not interned/],
  ] as const)('throws on a dangling host-ref result (kind %i)', (kind, re) => {
    const h = harness();
    const dv = new DataView(h.bytes().buffer);
    dv.setUint32(96, kind, true);
    dv.setUint32(96 + 8, 99, true); // slot 99 — never interned
    h.setResult(96);
    expect(() => h.inst.eval()).toThrow(re);
  });
});
