import * as protobuf from 'protobufjs';
import { FileDescriptorSet } from 'protobufjs/ext/descriptor/index.js';
import { describe, it, expect } from 'vitest';

import { DescriptorSet } from './descriptors.js';

// A small proto-shaped namespace used across the descriptor tests.
const ROOT_JSON = {
  nested: {
    test: {
      nested: {
        Color: { values: { RED: 0, GREEN: 1 } },
        User: {
          fields: {
            id: { type: 'int64', id: 1 },
            name: { type: 'string', id: 2 },
          },
        },
        Req: {
          fields: {
            user: { type: 'User', id: 1 },
            country: { type: 'string', id: 2 },
          },
        },
      },
    },
  },
};

function buildRoot(): protobuf.Root {
  const root = protobuf.Root.fromJSON(ROOT_JSON);
  root.resolveAll();
  return root;
}

// `Root.prototype.toDescriptor` is installed by the descriptor extension
// but not in protobufjs's public types; narrow it for this test helper.
type ToDescriptor = (edition?: string) => protobuf.Message;

/** Serialize the test namespace to the bytes `protoc --descriptor_set_out` emits. */
function buildFileDescriptorSetBytes(): Uint8Array {
  const root = buildRoot();
  const toDescriptor = (root as unknown as { toDescriptor: ToDescriptor })
    .toDescriptor;
  const fdSet = toDescriptor.call(root, 'proto3');
  return FileDescriptorSet.encode(fdSet).finish();
}

describe('DescriptorSet.fromRoot', () => {
  it('resolves a message type by FQN', () => {
    const set = DescriptorSet.fromRoot(buildRoot());
    const t = set.messageType('test.User');
    expect(t.name).toBe('User');
    expect(t.fields.id?.id).toBe(1);
  });

  it('accepts a leading-dot FQN (the cel.abi form)', () => {
    const set = DescriptorSet.fromRoot(buildRoot());
    expect(set.messageType('.test.Req').name).toBe('Req');
  });

  it('has() reports membership', () => {
    const set = DescriptorSet.fromRoot(buildRoot());
    expect(set.has('test.User')).toBe(true);
    expect(set.has('test.Req')).toBe(true);
    expect(set.has('test.Missing')).toBe(false);
  });

  it('throws on an unknown FQN', () => {
    const set = DescriptorSet.fromRoot(buildRoot());
    expect(() => set.messageType('test.Missing')).toThrow(
      /unknown message type/,
    );
  });

  it('treats an enum name as a non-message (not resolvable)', () => {
    const set = DescriptorSet.fromRoot(buildRoot());
    expect(set.has('test.Color')).toBe(false);
    expect(() => set.messageType('test.Color')).toThrow(/unknown message type/);
  });

  it("throws on the empty name (lookup('') resolves to the root namespace)", () => {
    const set = DescriptorSet.fromRoot(buildRoot());
    expect(set.has('')).toBe(false);
    expect(() => set.messageType('')).toThrow(/unknown message type/);
  });

  it('treats a namespace name as a non-message (not resolvable)', () => {
    const set = DescriptorSet.fromRoot(buildRoot());
    expect(set.has('test')).toBe(false);
    expect(() => set.messageType('test')).toThrow(/unknown message type/);
  });
});

describe('DescriptorSet.fromFileDescriptorSet', () => {
  it('loads a serialized FileDescriptorSet and resolves a type', () => {
    const set = DescriptorSet.fromFileDescriptorSet(
      buildFileDescriptorSetBytes(),
    );
    const t = set.messageType('test.User');
    expect(t.name).toBe('User');
    expect(t.fieldsById[2]?.name).toBe('name');
  });

  it('resolves a nested message reference from the loaded set', () => {
    const set = DescriptorSet.fromFileDescriptorSet(
      buildFileDescriptorSetBytes(),
    );
    const req = set.messageType('test.Req');
    const userField = req.fields.user;
    userField?.resolve();
    expect(userField?.resolvedType?.name).toBe('User');
  });

  it('throws on an unknown FQN after loading bytes', () => {
    const set = DescriptorSet.fromFileDescriptorSet(
      buildFileDescriptorSetBytes(),
    );
    expect(() => set.messageType('test.Nope')).toThrow(/unknown message type/);
  });
});
