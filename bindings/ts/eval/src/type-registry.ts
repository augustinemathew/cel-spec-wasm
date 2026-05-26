/**
 * `TypeRegistry` — the protobuf-es descriptor pool the TS host needs but
 * the platform doesn't give it for free. C++ has the process-wide
 * `DescriptorPool::generated_pool()`; TS has no global registry, so an
 * embedder that binds, returns, or constructs real protobuf-es **messages**
 * supplies one here. It wraps protobuf-es' `Registry` (from
 * `createFileRegistry`) and bridges between protobuf-es `Message`s and the
 * host `MessageBacking` the `cel_host.*` trampolines read.
 *
 * Two directions:
 *   - **in** (`message`): a protobuf-es `Message` → a `ProtoMessageBacking`
 *     the host can read field-by-field (descriptor resolved by the
 *     message's `$typeName`).
 *   - **out** (`toMessage`): a returned `CEL_MESSAGE`'s `MessageBacking` →
 *     a typed protobuf-es `Message`. For the common pass-through case (the
 *     backing IS a `ProtoMessageBacking` — a bound or host-constructed
 *     proto message) this hands back the underlying message directly; a
 *     non-proto (e.g. object) backing can't be re-typed without a
 *     field-by-field rebuild and is rejected with a clear error.
 *
 * An embedder with no proto messages (scalars / JS-object messages / lists
 * / maps only) needs no registry — `Engine.create` takes it optionally.
 */
import { createFileRegistry, fromBinary } from '@bufbuild/protobuf';
import type { DescMessage, Message, Registry } from '@bufbuild/protobuf';
import { FileDescriptorSetSchema } from '@bufbuild/protobuf/wkt';
import { ProtoMessageBacking } from './host/proto-backing.js';
import type { MessageBacking } from './host/backing.js';

/** Thrown on a missing descriptor or an un-materializable backing. */
export class RegistryError extends Error {
  public override readonly name = 'RegistryError';
}

export class TypeRegistry {
  private constructor(private readonly registry: Registry) {}

  /** Build from a serialized `FileDescriptorSet` (e.g. `protoc -o set.bin`
   *  output, or the descriptor set the C++ compiler embeds). */
  public static fromDescriptorSet(bytes: Uint8Array): TypeRegistry {
    let registry: Registry;
    try {
      registry = createFileRegistry(fromBinary(FileDescriptorSetSchema, bytes));
    } catch (cause) {
      throw new RegistryError(
        `failed to load descriptor set: ${String(cause)}`,
      );
    }
    return new TypeRegistry(registry);
  }

  /** Wrap an existing protobuf-es `Registry` (e.g. a codegen'd file
   *  registry, or one already built with `createFileRegistry`). */
  public static fromFileRegistry(registry: Registry): TypeRegistry {
    return new TypeRegistry(registry);
  }

  /** The message descriptor for `typeName` (e.g. `celwasm.testdata.Customer`),
   *  or `undefined` if not registered. */
  public getMessage(typeName: string): DescMessage | undefined {
    return this.registry.getMessage(typeName);
  }

  /** A `MessageBacking` over a protobuf-es message — for binding a real
   *  proto message into an `Activation`. The descriptor is resolved by the
   *  message's `$typeName`; throws if it isn't registered. */
  public message(msg: Message): MessageBacking {
    const desc = this.registry.getMessage(msg.$typeName);
    if (desc === undefined) {
      throw new RegistryError(
        `message type '${msg.$typeName}' is not in the registry`,
      );
    }
    return new ProtoMessageBacking(desc, msg);
  }

  /** Materialize a returned message's backing into a typed protobuf-es
   *  message. Fast path only: the backing must be a `ProtoMessageBacking`
   *  (a bound or host-constructed proto message flows through unchanged).
   *  A non-proto backing (e.g. an object backing) is rejected — read it
   *  generically via `asMessage(...).getField(...)` instead. */
  public toMessage(backing: MessageBacking): Message {
    if (isProtoBacking(backing)) {
      return backing.message;
    }
    throw new RegistryError(
      'backing is not protobuf-backed; read it via getField(...) instead of ' +
        'materializing to a typed message',
    );
  }
}

/** True if a `MessageBacking` is a `ProtoMessageBacking` (carries an
 *  underlying protobuf-es message). */
function isProtoBacking(b: MessageBacking): b is ProtoMessageBacking {
  return b instanceof ProtoMessageBacking;
}
