import { describe, it, expect, beforeEach } from 'vitest';
import { CelKind, decodeCelValue, type CelValue } from '../../src/celvalue.js';
import type { FieldEntry } from '../../src/abi.js';
import { ExternrefTable } from '../../src/externref.js';
import {
  listValue,
  mapValue,
  messageValue,
  scalarValue,
  type ListBacking,
  type MapBacking,
  type MessageBacking,
} from '../../src/host/backing.js';
import {
  ObjectListBacking,
  ObjectMapBacking,
  ObjectMessageBacking,
} from '../../src/host/object-backing.js';
import {
  TrampolineError,
  celGetField,
  celHasField,
  celListAt,
  celListSize,
  celMakeMessage,
  celMapLookup,
  celMapSize,
  celSetField,
  encodeHostValue,
  makeCelHostImports,
  type TrampolineContext,
} from '../../src/host/trampolines.js';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { TypeRegistry } from '../../src/type-registry.js';

// Slots laid out in a real (hermetic) WebAssembly.Memory.
const OUT = 0;
const A = 24;
const B = 48;

let ctx: TrampolineContext;
let dv: DataView;

function field(id: number, name: string): FieldEntry {
  return { id, fieldNumber: id, name, ownerFqn: '' };
}

beforeEach(() => {
  const memory = new WebAssembly.Memory({ initial: 1 });
  let cursor = 4096; // arena bump start, well past the slots
  ctx = {
    memory,
    refs: new ExternrefTable<MessageBacking, MapBacking, ListBacking>(),
    fields: new Map([
      [1, field(1, 'name')],
      [2, field(2, 'age')],
      [3, field(3, 'missing')],
    ]),
    types: new Map(),
    arenaAlloc: (size: number): number => {
      const p = cursor;
      cursor += size;
      return p;
    },
  };
  dv = new DataView(memory.buffer);
});

function out(): CelValue {
  return decodeCelValue(new Uint8Array(ctx.memory.buffer), OUT);
}
function outKind(): number {
  return dv.getUint32(OUT, true);
}
function outRef(): number {
  return dv.getUint32(OUT + 8, true);
}

describe('encodeHostValue', () => {
  it('encodes inline scalars, string, bytes, and error', () => {
    encodeHostValue(ctx, OUT, scalarValue({ kind: CelKind.Int, int: 5n }));
    expect(out()).toEqual({ kind: CelKind.Int, int: 5n });
    encodeHostValue(
      ctx,
      OUT,
      scalarValue({ kind: CelKind.String, value: 'hi' }),
    );
    expect(out()).toEqual({ kind: CelKind.String, value: 'hi' });
    encodeHostValue(
      ctx,
      OUT,
      scalarValue({ kind: CelKind.Bytes, bytes: new Uint8Array([7, 8]) }),
    );
    expect(out()).toEqual({
      kind: CelKind.Bytes,
      bytes: new Uint8Array([7, 8]),
    });
    encodeHostValue(
      ctx,
      OUT,
      scalarValue({ kind: CelKind.Error, errorCode: 13 }),
    );
    expect(out()).toEqual({ kind: CelKind.Error, errorCode: 13 });
  });

  it('interns aggregates and writes a host ref', () => {
    encodeHostValue(
      ctx,
      OUT,
      messageValue(new ObjectMessageBacking({ a: 1n })),
    );
    expect(outKind()).toBe(CelKind.Message);
    expect(ctx.refs.lookupMessage(outRef())).toBeDefined();
    encodeHostValue(ctx, OUT, listValue(new ObjectListBacking([1n])));
    expect(outKind()).toBe(CelKind.ListHost);
    encodeHostValue(
      ctx,
      OUT,
      mapValue(new ObjectMapBacking(new Map([['k', 1n]]))),
    );
    expect(outKind()).toBe(CelKind.MapHost);
  });
});

// Bind a backing into the externref + write its host-ref CelValue at slot.
function bindMessage(slot: number, b: MessageBacking): void {
  encodeHostValue(ctx, slot, messageValue(b));
}
function bindList(slot: number, b: ListBacking): void {
  encodeHostValue(ctx, slot, listValue(b));
}
function bindMap(slot: number, b: MapBacking): void {
  encodeHostValue(ctx, slot, mapValue(b));
}

describe('celGetField', () => {
  it('reads a field (object backing)', () => {
    bindMessage(A, new ObjectMessageBacking({ name: 'Ann', age: 20n }));
    celGetField(ctx, OUT, A, 1);
    expect(out()).toEqual({ kind: CelKind.String, value: 'Ann' });
    celGetField(ctx, OUT, A, 2);
    expect(out()).toEqual({ kind: CelKind.Int, int: 20n });
  });

  it('nested message field → CEL_MESSAGE ref readable as a backing', () => {
    bindMessage(A, new ObjectMessageBacking({ name: { city: 'NYC' } }));
    celGetField(ctx, OUT, A, 1);
    expect(outKind()).toBe(CelKind.Message);
    const nested = ctx.refs.lookupMessage(outRef());
    expect(nested?.getField('city')).toEqual({
      host: 'scalar',
      value: { kind: CelKind.String, value: 'NYC' },
    });
  });

  it('FIELD_NOT_FOUND: unknown field id, and backing has no such field', () => {
    bindMessage(A, new ObjectMessageBacking({ name: 'Ann' }));
    celGetField(ctx, OUT, A, 99); // id not in fields map
    expect(out()).toEqual({ kind: CelKind.Error, errorCode: 20 });
    celGetField(ctx, OUT, A, 3); // field 'missing' absent on backing
    expect(out()).toEqual({ kind: CelKind.Error, errorCode: 20 });
  });

  it('HOST_ADAPTER error when the externref slot is empty', () => {
    dv.setUint32(A, CelKind.Message, true);
    dv.setUint32(A + 8, 999, true); // never interned
    celGetField(ctx, OUT, A, 1);
    expect(out()).toEqual({ kind: CelKind.Error, errorCode: 41 });
  });

  it('absorbs an UNKNOWN / ERROR operand (3VL)', () => {
    dv.setUint32(A, CelKind.Unknown, true);
    dv.setUint32(A + 8, 42, true);
    celGetField(ctx, OUT, A, 1);
    expect(outKind()).toBe(CelKind.Unknown);
    expect(outRef()).toBe(42);

    dv.setUint32(B, CelKind.Error, true);
    dv.setUint32(B + 8, 13, true);
    celGetField(ctx, OUT, B, 1);
    expect(out()).toEqual({ kind: CelKind.Error, errorCode: 13 });
  });
});

describe('celHasField', () => {
  it('true / false / backing-missing', () => {
    bindMessage(A, new ObjectMessageBacking({ name: 'Ann', nick: null }));
    celHasField(ctx, OUT, A, 1);
    expect(out()).toEqual({ kind: CelKind.Bool, bool: true });
    celHasField(ctx, OUT, A, 3); // 'missing' absent
    expect(out()).toEqual({ kind: CelKind.Bool, bool: false });
    dv.setUint32(B, CelKind.Message, true);
    dv.setUint32(B + 8, 999, true);
    celHasField(ctx, OUT, B, 1);
    expect(out()).toEqual({ kind: CelKind.Error, errorCode: 41 });
  });

  it('FIELD_NOT_FOUND on unknown field id; absorbs UNKNOWN operand', () => {
    bindMessage(A, new ObjectMessageBacking({ name: 'Ann' }));
    celHasField(ctx, OUT, A, 99); // id not in fields map
    expect(out()).toEqual({ kind: CelKind.Error, errorCode: 20 });
    dv.setUint32(B, CelKind.Unknown, true);
    dv.setUint32(B + 8, 5, true);
    celHasField(ctx, OUT, B, 1);
    expect(outKind()).toBe(CelKind.Unknown);
  });
});

describe('celListAt / celListSize', () => {
  beforeEach(() => {
    bindList(A, new ObjectListBacking([10n, 20n, 30n]));
  });

  it('reads an element + size', () => {
    encodeHostValue(ctx, B, scalarValue({ kind: CelKind.Int, int: 1n }));
    celListAt(ctx, OUT, A, B);
    expect(out()).toEqual({ kind: CelKind.Int, int: 20n });
    celListSize(ctx, OUT, A);
    expect(out()).toEqual({ kind: CelKind.Int, int: 3n });
  });

  it('INDEX_OUT_OF_BOUNDS + non-int index TYPE_MISMATCH', () => {
    encodeHostValue(ctx, B, scalarValue({ kind: CelKind.Int, int: 9n }));
    celListAt(ctx, OUT, A, B);
    expect(out()).toEqual({ kind: CelKind.Error, errorCode: 17 });
    encodeHostValue(ctx, B, scalarValue({ kind: CelKind.String, value: 'x' }));
    celListAt(ctx, OUT, A, B);
    expect(out()).toEqual({ kind: CelKind.Error, errorCode: 13 });
  });

  it('backing-missing + 3VL absorb (list and index)', () => {
    dv.setUint32(B, CelKind.Message, true);
    dv.setUint32(B + 8, 999, true);
    celListSize(ctx, OUT, B);
    expect(out()).toEqual({ kind: CelKind.Error, errorCode: 41 });
    celListAt(ctx, OUT, B, B); // bad list ref → HOST_ADAPTER
    expect(out()).toEqual({ kind: CelKind.Error, errorCode: 41 });
    // unknown list operand — absorbed by both at() and size()
    dv.setUint32(B, CelKind.Unknown, true);
    dv.setUint32(B + 8, 7, true);
    celListAt(ctx, OUT, B, A);
    expect(outKind()).toBe(CelKind.Unknown);
    celListSize(ctx, OUT, B);
    expect(outKind()).toBe(CelKind.Unknown);
    // unknown index operand
    encodeHostValue(ctx, B, scalarValue({ kind: CelKind.Int, int: 0n }));
    dv.setUint32(48 + 0, CelKind.Unknown, true); // overwrite B kind
    celListAt(ctx, OUT, A, B);
    expect(outKind()).toBe(CelKind.Unknown);
  });
});

describe('celMapLookup / celMapSize', () => {
  beforeEach(() => {
    bindMap(A, new ObjectMapBacking(new Map([['k', 7n]])));
  });

  it('gets a value + size', () => {
    encodeHostValue(ctx, B, scalarValue({ kind: CelKind.String, value: 'k' }));
    celMapLookup(ctx, OUT, A, B);
    expect(out()).toEqual({ kind: CelKind.Int, int: 7n });
    celMapSize(ctx, OUT, A);
    expect(out()).toEqual({ kind: CelKind.Int, int: 1n });
  });

  it('NO_SUCH_KEY + backing-missing + absorb', () => {
    encodeHostValue(
      ctx,
      B,
      scalarValue({ kind: CelKind.String, value: 'nope' }),
    );
    celMapLookup(ctx, OUT, A, B);
    expect(out()).toEqual({ kind: CelKind.Error, errorCode: 15 });
    dv.setUint32(B, CelKind.Message, true);
    dv.setUint32(B + 8, 999, true);
    celMapSize(ctx, OUT, B);
    expect(out()).toEqual({ kind: CelKind.Error, errorCode: 41 });
    celMapLookup(ctx, OUT, B, A); // bad map ref
    expect(out()).toEqual({ kind: CelKind.Error, errorCode: 41 });
    dv.setUint32(B, CelKind.Error, true);
    dv.setUint32(B + 8, 13, true);
    celMapLookup(ctx, OUT, A, B); // unknown/error key
    expect(out()).toEqual({ kind: CelKind.Error, errorCode: 13 });
    // map size absorbs an UNKNOWN operand too.
    dv.setUint32(B, CelKind.Unknown, true);
    dv.setUint32(B + 8, 1, true);
    celMapSize(ctx, OUT, B);
    expect(outKind()).toBe(CelKind.Unknown);
  });
});

describe('makeCelHostImports', () => {
  it('exposes implemented trampolines and traps the rest', () => {
    const ns = makeCelHostImports(ctx);
    const call = (name: string): ((...a: number[]) => void) =>
      ns[name] as (...a: number[]) => void;

    bindMessage(A, new ObjectMessageBacking({ name: 'Ann' }));
    call('cel_get_field')(OUT, A, 1);
    expect(out()).toEqual({ kind: CelKind.String, value: 'Ann' });
    call('cel_has_field')(OUT, A, 1);
    expect(out()).toEqual({ kind: CelKind.Bool, bool: true });

    bindList(A, new ObjectListBacking([1n]));
    encodeHostValue(ctx, B, scalarValue({ kind: CelKind.Int, int: 0n }));
    call('cel_list_at')(OUT, A, B);
    expect(out()).toEqual({ kind: CelKind.Int, int: 1n });
    call('cel_list_size')(OUT, A);
    expect(out()).toEqual({ kind: CelKind.Int, int: 1n });

    bindMap(A, new ObjectMapBacking(new Map([['k', 7n]])));
    encodeHostValue(ctx, B, scalarValue({ kind: CelKind.String, value: 'k' }));
    call('cel_map_lookup')(OUT, A, B);
    expect(out()).toEqual({ kind: CelKind.Int, int: 7n });
    call('cel_map_size')(OUT, A);
    expect(out()).toEqual({ kind: CelKind.Int, int: 1n });

    // The implemented construction trampolines route through the import
    // table too: an unknown type_id writes CEL_ERROR (no trap)…
    call('cel_make_message')(0, OUT); // (type_id=0, out_slot) → error
    expect(out().kind).toBe(CelKind.Error);
    // …and set_field on a non-message slot traps.
    expect(() => {
      call('cel_set_field')(OUT, 1, A);
    }).toThrow(TrampolineError);

    // A still-unimplemented trampoline traps (the remaining Slice-C+ stubs).
    expect(() => {
      call('cel_message_eq')(0, 0, 0);
    }).toThrow(TrampolineError);
  });
});

describe('cel_make_message + cel_set_field (message construction)', () => {
  const HERE = dirname(fileURLToPath(import.meta.url));
  const registry = TypeRegistry.fromDescriptorSet(
    readFileSync(join(HERE, '../testdata/customer.fds.bin')),
  );
  const CUSTOMER = 'celwasm.testdata.Customer';

  function ctxWith(reg: TypeRegistry | undefined): {
    ctx: TrampolineContext;
    dv: DataView;
  } {
    const memory = new WebAssembly.Memory({ initial: 1 });
    let cursor = 4096;
    const c: TrampolineContext = {
      memory,
      refs: new ExternrefTable<MessageBacking, MapBacking, ListBacking>(),
      fields: new Map([
        [1, { id: 1, fieldNumber: 0, name: 'name', ownerFqn: '' }],
      ]),
      types: new Map([[1, { id: 1, fullyQualifiedName: CUSTOMER }]]),
      registry: reg,
      arenaAlloc: (size: number): number => {
        const p = cursor;
        cursor += size;
        return p;
      },
    };
    return { ctx: c, dv: new DataView(memory.buffer) };
  }

  it('make_message(type_id) interns an empty message; set_field mutates it', () => {
    const { ctx: c, dv } = ctxWith(registry);
    celMakeMessage(c, 1, 0); // type_id=1 (Customer) → out_slot=0
    expect(dv.getUint32(0, true)).toBe(CelKind.Message);
    const slot = dv.getUint32(8, true);
    // value "Ann" at slot 64, then set field_ref 1 (name).
    encodeHostValue(c, 64, scalarValue({ kind: CelKind.String, value: 'Ann' }));
    celSetField(c, 0, 1, 64);
    expect(c.refs.lookupMessage(slot)?.getField('name')).toEqual({
      host: 'scalar',
      value: { kind: CelKind.String, value: 'Ann' },
    });
  });

  it('make_message with no registry → CEL_ERROR(type_mismatch)', () => {
    const { ctx: c, dv } = ctxWith(undefined);
    celMakeMessage(c, 1, 0);
    expect(dv.getUint32(0, true)).toBe(CelKind.Error);
  });

  it('make_message with an unknown type_id → CEL_ERROR', () => {
    const { ctx: c, dv } = ctxWith(registry);
    celMakeMessage(c, 99, 0); // not in the types map
    expect(dv.getUint32(0, true)).toBe(CelKind.Error);
  });

  it('set_field on a non-constructed message slot traps', () => {
    const { ctx: c } = ctxWith(registry);
    // slot 0 holds a scalar, not a CEL_MESSAGE ref to a proto backing.
    encodeHostValue(c, 0, scalarValue({ kind: CelKind.Int, int: 1n }));
    expect(() => {
      celSetField(c, 0, 1, 64);
    }).toThrow(TrampolineError);
  });

  it('set_field with an unknown field_ref traps', () => {
    const { ctx: c } = ctxWith(registry);
    celMakeMessage(c, 1, 0);
    encodeHostValue(c, 64, scalarValue({ kind: CelKind.String, value: 'x' }));
    expect(() => {
      celSetField(c, 0, 99, 64);
    }).toThrow(TrampolineError);
  });
});
