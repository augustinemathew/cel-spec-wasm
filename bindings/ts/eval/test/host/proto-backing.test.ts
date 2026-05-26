import { describe, it, expect, beforeAll } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import {
  create,
  createFileRegistry,
  fromBinary,
  type DescMessage,
  type Message,
} from '@bufbuild/protobuf';
import { FileDescriptorSetSchema } from '@bufbuild/protobuf/wkt';
import { CelKind, type CelValue } from '../../src/celvalue.js';
import type { HostValue } from '../../src/host/backing.js';
import { ProtoMessageBacking } from '../../src/host/proto-backing.js';

const HERE = dirname(fileURLToPath(import.meta.url));

function scalar(v: HostValue | undefined): CelValue {
  if (v?.host !== 'scalar') {
    throw new Error(
      `expected a scalar HostValue, got ${v?.host ?? 'undefined'}`,
    );
  }
  return v.value;
}

describe('ProtoMessageBacking (real protobuf-es Customer)', () => {
  let Customer: DescMessage;
  let backing: ProtoMessageBacking;

  beforeAll(() => {
    const fds = fromBinary(
      FileDescriptorSetSchema,
      readFileSync(join(HERE, '../testdata/customer.fds.bin')),
    );
    const registry = createFileRegistry(fds);
    const desc = registry.getMessage('celwasm.testdata.Customer');
    if (desc === undefined) {
      throw new Error('Customer descriptor not found');
    }
    Customer = desc;
    const msg: Message = create(Customer, {
      name: 'Ann',
      age: -5, // int32 (signed)
      userId: 123n, // int64
      priority: 7, // uint32
      balanceCents: 9_000_000_000n, // uint64 (> 2^32)
      creditScore: 1.5, // double
      isPremium: true, // bool
      sessionToken: new Uint8Array([1, 2, 255]), // bytes
      billingAddress: { city: 'NYC', country: 'US' }, // nested message
      tags: ['a', 'b', 'c'], // repeated string
      metadata: { env: 'prod' }, // map<string,string>
      tierQuotas: { 5: 10 }, // map<int32,int32> (numeric key)
    });
    backing = new ProtoMessageBacking(Customer, msg);
  });

  it('maps each scalar field to the right CEL kind/value', () => {
    expect(scalar(backing.getField('name'))).toEqual({
      kind: CelKind.String,
      value: 'Ann',
    });
    expect(scalar(backing.getField('age'))).toEqual({
      kind: CelKind.Int,
      int: -5n,
    });
    expect(scalar(backing.getField('user_id'))).toEqual({
      kind: CelKind.Int,
      int: 123n,
    });
    expect(scalar(backing.getField('priority'))).toEqual({
      kind: CelKind.Uint,
      uint: 7n,
    });
    expect(scalar(backing.getField('balance_cents'))).toEqual({
      kind: CelKind.Uint,
      uint: 9_000_000_000n,
    });
    expect(scalar(backing.getField('credit_score'))).toEqual({
      kind: CelKind.Double,
      double: 1.5,
    });
    expect(scalar(backing.getField('is_premium'))).toEqual({
      kind: CelKind.Bool,
      bool: true,
    });
    expect(scalar(backing.getField('session_token'))).toEqual({
      kind: CelKind.Bytes,
      bytes: new Uint8Array([1, 2, 255]),
    });
  });

  it('reads a nested message field as a nested backing', () => {
    const addr = backing.getField('billing_address');
    expect(addr?.host).toBe('message');
    if (addr?.host === 'message') {
      expect(scalar(addr.backing.getField('city'))).toEqual({
        kind: CelKind.String,
        value: 'NYC',
      });
    }
  });

  it('reads a repeated field as a list backing', () => {
    const tags = backing.getField('tags');
    expect(tags?.host).toBe('list');
    if (tags?.host === 'list') {
      expect(tags.backing.size).toBe(3);
      expect(scalar(tags.backing.at(0))).toEqual({
        kind: CelKind.String,
        value: 'a',
      });
      expect(tags.backing.at(3)).toBeUndefined(); // OOB
      const seen: string[] = [];
      tags.backing.forEach((e) => {
        if (e.host === 'scalar' && e.value.kind === CelKind.String) {
          seen.push(e.value.value);
        }
      });
      expect(seen).toEqual(['a', 'b', 'c']);
    }
  });

  it('reads map fields as map backings (string→string, string→int32)', () => {
    const md = backing.getField('metadata');
    expect(md?.host).toBe('map');
    if (md?.host === 'map') {
      expect(md.backing.size).toBe(1);
      expect(md.backing.has({ kind: CelKind.String, value: 'env' })).toBe(true);
      expect(
        scalar(md.backing.get({ kind: CelKind.String, value: 'env' })),
      ).toEqual({ kind: CelKind.String, value: 'prod' });
      expect(
        md.backing.get({ kind: CelKind.String, value: 'nope' }),
      ).toBeUndefined();
    }

    // map<int32,int32>: numeric key (cross-type — query as int).
    const tq = backing.getField('tier_quotas');
    expect(tq?.host).toBe('map');
    if (tq?.host === 'map') {
      expect(scalar(tq.backing.get({ kind: CelKind.Int, int: 5n }))).toEqual({
        kind: CelKind.Int,
        int: 10n,
      });
    }
  });

  it('hasField: present true, absent false; getField absent → undefined', () => {
    expect(backing.hasField('name')).toBe(true);
    expect(backing.hasField('definitely_not_a_field')).toBe(false);
    expect(backing.getField('definitely_not_a_field')).toBeUndefined();
  });
});
