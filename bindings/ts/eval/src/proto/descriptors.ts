// Descriptor loading + message-type resolution for the proto backing.
//
// The eval binding takes proto descriptors from the caller — either a
// serialized `FileDescriptorSet` (the bytes `protoc --descriptor_set_out`
// emits) or an already-loaded protobufjs `Root` — and resolves message
// types against that set only.  There is no global registry discovery
// (out of scope, §A.3): a type the supplied set does not contain is an
// error, not a lookup miss to be filled from elsewhere.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.4.6.

import * as protobuf from 'protobufjs';
import { FileDescriptorSet } from 'protobufjs/ext/descriptor/index.js';

// The `protobufjs/ext/descriptor` extension installs `Root.fromDescriptor`
// at runtime but does not augment the public `Root` type.  Type the static
// it provides (input is a decoded `FileDescriptorSet` message or the raw
// bytes; output a resolved `Root`) through a single narrowed reference.
type RootFromDescriptor = (
  descriptor: protobuf.Message | Uint8Array,
) => protobuf.Root;

function rootFromDescriptor(
  descriptor: protobuf.Message | Uint8Array,
): protobuf.Root {
  const fn = (
    protobuf.Root as unknown as { fromDescriptor?: RootFromDescriptor }
  ).fromDescriptor;
  if (typeof fn !== 'function') {
    throw new Error(
      'protobufjs descriptor extension not loaded (Root.fromDescriptor missing)',
    );
  }
  return fn(descriptor);
}

/**
 * A resolver over a fixed set of proto descriptors.  Message types are
 * looked up by their fully-qualified name (matching
 * {@link TypeEntry.fullyQualifiedName} / the `cel.abi` type table), with
 * or without a leading dot.
 */
export class DescriptorSet {
  private readonly root: protobuf.Root;

  private constructor(root: protobuf.Root) {
    this.root = root;
  }

  /**
   * Load descriptors from a serialized `FileDescriptorSet` — the bytes
   * `protoc --descriptor_set_out=… ` (or any proto compiler) emits.
   * protobufjs decodes them via its `descriptor` extension and rebuilds a
   * `Root` from the contained `FileDescriptorProto`s.
   */
  static fromFileDescriptorSet(bytes: Uint8Array): DescriptorSet {
    const decoded = FileDescriptorSet.decode(bytes);
    const root = rootFromDescriptor(decoded);
    root.resolveAll();
    return new DescriptorSet(root);
  }

  /**
   * Adopt an already-loaded protobufjs `Root` (e.g. one a caller built via
   * `protobuf.load` / `Root.fromJSON`).  The `Root` is resolved if it is
   * not already.
   */
  static fromRoot(root: protobuf.Root): DescriptorSet {
    root.resolveAll();
    return new DescriptorSet(root);
  }

  /**
   * Resolve a message type by fully-qualified name.  Throws if the name is
   * not a message type in this set — there is no fallback registry.
   */
  messageType(fqn: string): protobuf.Type {
    const normalized = normalizeFqn(fqn);
    // `lookup` with a Type filter returns null on a miss (and on a name
    // that resolves to an enum / namespace rather than a message);
    // `lookupType` would throw, which loses the offending name.  The
    // filter does not cover every non-Type resolution — `lookup('')`
    // returns the root namespace itself — so the instanceof check is
    // load-bearing for the empty-name case.
    const resolved = this.root.lookup(normalized, [protobuf.Type]);
    if (!(resolved instanceof protobuf.Type)) {
      throw new Error(`unknown message type '${fqn}' in descriptor set`);
    }
    return resolved;
  }

  /** Whether {@link messageType} would resolve `fqn` to a message type. */
  has(fqn: string): boolean {
    const normalized = normalizeFqn(fqn);
    return (
      this.root.lookup(normalized, [protobuf.Type]) instanceof protobuf.Type
    );
  }
}

/**
 * protobufjs looks types up relative to a namespace; a leading dot anchors
 * the lookup at the root.  Strip it so callers may pass either form.
 */
function normalizeFqn(fqn: string): string {
  return fqn.startsWith('.') ? fqn.slice(1) : fqn;
}
