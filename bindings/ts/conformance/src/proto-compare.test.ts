// Tests for the object_value expected-message builder.  Drives the real
// conformance descriptor set (the committed FDS fixture) so the textproto →
// protobufjs → `messageToObject` path is exercised end-to-end against the
// actual `TestAllTypes` descriptors the corpus references.

import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import type { CelValue } from '@cel-wasm/eval';
import { messageToObject, type DescriptorSet } from '@cel-wasm/eval/proto';
import { describe, expect, it } from 'vitest';

import {
  buildBindingInput,
  buildBoundMessage,
  buildExpectedMessage,
  loadDescriptorSet,
} from './proto-compare.js';
import { parseTextproto, type TextprotoMessage } from './textproto.js';

const FDS_PATH = fileURLToPath(
  new URL('../fixtures/cel_conformance_protos.fds', import.meta.url),
);
const PROTO3 = 'cel.expr.conformance.proto3.TestAllTypes';

const hasFds = existsSync(FDS_PATH);

// The descriptor set, loaded lazily inside the guarded suites.  `descriptors()`
// throws if the FDS fixture is absent — but the suites only run when `hasFds`,
// so the throw is unreachable in practice and keeps the type non-nullable
// without a `!` assertion.
function descriptors(): DescriptorSet {
  if (!hasFds) {
    throw new Error('descriptor-set fixture absent');
  }
  return loadDescriptorSet(new Uint8Array(readFileSync(FDS_PATH)));
}

// Parse a textproto message body fragment (the `Any` expansion body) into a
// `TextprotoMessage`, reusing the corpus reader.
function body(text: string): TextprotoMessage {
  return parseTextproto(text);
}

describe.skipIf(!hasFds)('buildExpectedMessage', () => {
  it('builds a singular int64 field with defaults materialised', () => {
    const msg = buildExpectedMessage(
      descriptors(),
      PROTO3,
      body('single_int64: 17'),
    ) as Record<string, CelValue>;
    expect(msg.single_int64).toBe(17n);
    // A default scalar field is present in the decoded object.
    expect(msg.single_int32).toBe(0n);
  });

  it('builds a repeated field as an array', () => {
    const msg = buildExpectedMessage(
      descriptors(),
      PROTO3,
      body('repeated_int32: 1 repeated_int32: 2'),
    ) as Record<string, CelValue>;
    expect(msg.repeated_int32).toEqual([1n, 2n]);
  });

  it('builds a map field as a Map', () => {
    const msg = buildExpectedMessage(
      descriptors(),
      PROTO3,
      body('map_string_string { key: "k" value: "v" }'),
    ) as Record<string, CelValue>;
    const m = msg.map_string_string;
    expect(m instanceof Map).toBe(true);
    expect((m as Map<CelValue, CelValue>).get('k')).toBe('v');
  });

  it('throws on a field the type does not declare', () => {
    expect(() =>
      buildExpectedMessage(descriptors(), PROTO3, body('no_such_field: 1')),
    ).toThrow();
  });
});

describe.skipIf(!hasFds)('buildBoundMessage', () => {
  it('builds a protobufjs message carrying the set field', () => {
    const msg = buildBoundMessage(
      descriptors(),
      PROTO3,
      body('single_int64: 17'),
    );
    // A protobufjs Message instance: carries its reflection type and the
    // field value verbatim (the Long-typed field is set from "17").
    expect(msg.$type.fullName.endsWith('TestAllTypes')).toBe(true);
    expect(
      String((msg as unknown as Record<string, unknown>).single_int64),
    ).toBe('17');
  });

  it('round-trips: the bound message decodes to the expected-object form', () => {
    const text = 'single_string: "hi" repeated_int32: 1 repeated_int32: 2';
    const bound = buildBoundMessage(descriptors(), PROTO3, body(text));
    expect(messageToObject(bound)).toEqual(
      buildExpectedMessage(descriptors(), PROTO3, body(text)),
    );
  });

  it('throws on an unknown FQN', () => {
    expect(() =>
      buildBoundMessage(descriptors(), 'no.such.Type', body('f: 1')),
    ).toThrow(/no\.such\.Type/);
  });

  it('throws on a field the type does not declare', () => {
    expect(() =>
      buildBoundMessage(descriptors(), PROTO3, body('no_such_field: 1')),
    ).toThrow(/no_such_field/);
  });
});

describe.skipIf(!hasFds)('buildBindingInput', () => {
  it('lowers a google.protobuf.Duration body to the duration record', () => {
    const v = buildBindingInput(
      descriptors(),
      'google.protobuf.Duration',
      body('seconds: 123 nanos: 321456789'),
    );
    expect(v).toEqual({ kind: 'duration', seconds: 123n, nanos: 321456789 });
  });

  it('lowers a google.protobuf.Timestamp body to the timestamp record', () => {
    const v = buildBindingInput(
      descriptors(),
      'google.protobuf.Timestamp',
      body('seconds: 1234567890 nanos: 5'),
    );
    expect(v).toEqual({
      kind: 'timestamp',
      seconds: 1234567890n,
      nanos: 5,
    });
  });

  it('lowers an empty Duration body to the zero record (defaults)', () => {
    const v = buildBindingInput(
      descriptors(),
      'google.protobuf.Duration',
      body(''),
    );
    expect(v).toEqual({ kind: 'duration', seconds: 0n, nanos: 0 });
  });

  it('binds any other message type as the protobufjs message itself', () => {
    const v = buildBindingInput(descriptors(), PROTO3, body('single_int64: 1'));
    expect(typeof v === 'object' && v !== null && '$type' in v).toBe(true);
  });

  it('throws on an unknown FQN (same contract as buildBoundMessage)', () => {
    expect(() =>
      buildBindingInput(descriptors(), 'no.such.Type', body('f: 1')),
    ).toThrow(/no\.such\.Type/);
  });
});
