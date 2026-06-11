// Tests for the object_value expected-message builder.  Drives the real
// conformance descriptor set (the committed FDS fixture) so the textproto →
// protobufjs → `messageToObject` path is exercised end-to-end against the
// actual `TestAllTypes` descriptors the corpus references.

import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import type { CelValue } from '@cel-wasm/eval';
import type { DescriptorSet } from '@cel-wasm/eval/proto';
import { describe, expect, it } from 'vitest';

import {
  buildExpectedMessage,
  loadDescriptorSet,
  setsWellKnownField,
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

describe.skipIf(!hasFds)('setsWellKnownField', () => {
  it('detects a wrapper-typed field', () => {
    expect(
      setsWellKnownField(
        descriptors(),
        PROTO3,
        body('single_int64_wrapper: 5'),
      ),
    ).toBe(true);
  });

  it('detects a timestamp-typed field', () => {
    expect(
      setsWellKnownField(
        descriptors(),
        PROTO3,
        body('single_timestamp { seconds: 1 }'),
      ),
    ).toBe(true);
  });

  it('is false for a plain scalar field', () => {
    expect(
      setsWellKnownField(descriptors(), PROTO3, body('single_int64: 1')),
    ).toBe(false);
  });
});
