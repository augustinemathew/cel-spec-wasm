/**
 * `TypeRegistry` — the protobuf-es descriptor pool bridge. Hermetic: uses
 * the committed `customer.fds.bin` descriptor set + protobuf-es (no wasm),
 * so it runs in the unit gate at 100% coverage. Covers both directions
 * (`message` in, `toMessage` out), the missing-descriptor rejects, and the
 * non-proto-backing reject.
 */
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { create, type Message } from '@bufbuild/protobuf';
import { TypeRegistry, RegistryError } from '../src/type-registry.js';
import { ObjectMessageBacking } from '../src/host/object-backing.js';
import { CelKind } from '../src/celvalue.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const FDS = readFileSync(join(HERE, 'testdata/customer.fds.bin'));
const CUSTOMER = 'celwasm.testdata.Customer';

function registry(): TypeRegistry {
  return TypeRegistry.fromDescriptorSet(FDS);
}

describe('construction', () => {
  it('fromDescriptorSet resolves a registered message', () => {
    expect(registry().getMessage(CUSTOMER)?.typeName).toBe(CUSTOMER);
  });

  it('getMessage returns undefined for an unregistered type', () => {
    expect(registry().getMessage('not.Registered')).toBeUndefined();
  });

  it('fromDescriptorSet throws RegistryError on garbage bytes', () => {
    // A truncated/garbage descriptor set the protobuf reader rejects.
    expect(() =>
      TypeRegistry.fromDescriptorSet(new Uint8Array([0xff, 0xff, 0xff])),
    ).toThrow(RegistryError);
  });

  it('fromFileRegistry wraps an existing protobuf-es Registry', () => {
    const reg = registry();
    const desc = reg.getMessage(CUSTOMER);
    if (desc === undefined) throw new Error('Customer descriptor missing');
    // Round-trip the descriptor's file registry back through the wrapper.
    const wrapped = TypeRegistry.fromFileRegistry({
      getMessage: (name: string) => (name === CUSTOMER ? desc : undefined),
      getEnum: () => undefined,
      getExtension: () => undefined,
      getService: () => undefined,
      getFile: () => undefined,
      getMessageFor: () => undefined,
      [Symbol.iterator]: () => [][Symbol.iterator](),
    } as never);
    expect(wrapped.getMessage(CUSTOMER)?.typeName).toBe(CUSTOMER);
  });
});

describe('message() — proto message → backing (input)', () => {
  it('reads scalar fields through the resolved backing', () => {
    const reg = registry();
    const desc = reg.getMessage(CUSTOMER);
    if (desc === undefined) throw new Error('Customer descriptor missing');
    const msg = create(desc, { name: 'Ann', age: 30 });
    const backing = reg.message(msg);
    expect(backing.getField('name')).toEqual({
      host: 'scalar',
      value: { kind: CelKind.String, value: 'Ann' },
    });
    expect(backing.getField('age')).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Int, int: 30n },
    });
  });

  it('throws RegistryError for an unregistered $typeName', () => {
    const fake = { $typeName: 'not.Registered' } as unknown as Message;
    expect(() => registry().message(fake)).toThrow(RegistryError);
  });
});

describe('toMessage() — backing → typed message (output)', () => {
  it('hands back the underlying message for a proto backing (pass-through)', () => {
    const reg = registry();
    const desc = reg.getMessage(CUSTOMER);
    if (desc === undefined) throw new Error('Customer descriptor missing');
    const msg = create(desc, { name: 'Bob' });
    const backing = reg.message(msg);
    // Identity: the same object flows straight back, not a rebuilt copy.
    expect(reg.toMessage(backing)).toBe(msg);
  });

  it('rejects a non-proto (object) backing', () => {
    expect(() =>
      registry().toMessage(new ObjectMessageBacking({ name: 'Ann' })),
    ).toThrow(RegistryError);
  });
});
