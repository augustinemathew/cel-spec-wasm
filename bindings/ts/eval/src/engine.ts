// Engine — the descriptor + host-function registry a Program is planned
// against.
//
// `Engine.create` loads the proto descriptors (a serialized
// `FileDescriptorSet` or a protobufjs `Root`) message-typed variables /
// literals resolve against; `defineFunction` registers a JS host function
// exposed to a Program as a `cel_fn.*` import; `plan` instantiates a
// static Program with the full host import object assembled and returns a
// ready-to-eval {@link Instance}.
//
// The Engine is the reusable, Program-independent half: one Engine plans
// many Programs.  Each `plan` builds a fresh per-Instance externref table,
// codec, and host-trampoline context, so two Instances of the same Engine
// never share mutable eval state.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.5.

import type * as protobuf from 'protobufjs';

import { instantiateProgram } from './instance.js';
import type { Instance } from './instance.js';
import { DescriptorSet } from './proto/descriptors.js';
import type { CelValue, HostFunction, Program } from './types.js';

/** Options for {@link Engine.create}. */
export interface EngineOptions {
  /**
   * Proto descriptors message-typed variables / literals resolve against:
   * a serialized `FileDescriptorSet` (`protoc --descriptor_set_out`) or an
   * already-loaded protobufjs `Root`.  Required only to evaluate
   * message-typed Programs; scalar / aggregate Programs need none.
   */
  readonly descriptors?: Uint8Array | protobuf.Root;
}

/**
 * A registered host function — the overload id it is exposed under as a
 * `cel_fn.*` import, plus its JS implementation.  A `decl` like
 * `"my_fn(string): bool"` reduces to the leading identifier as the
 * import name; the full IDL parse is the compiler binding's concern (the
 * Program already carries the resolved overload id), so the registry keys
 * on the declared name.
 */
interface RegisteredFunction {
  readonly name: string;
  readonly impl: HostFunction;
}

/**
 * The descriptor + host-function registry a {@link Program} is planned
 * against.  Construct via {@link Engine.create}; plan one or many
 * Programs.
 */
export class Engine {
  private readonly descriptors: DescriptorSet | undefined;
  private readonly functions = new Map<string, RegisteredFunction>();

  private constructor(descriptors: DescriptorSet | undefined) {
    this.descriptors = descriptors;
  }

  /**
   * Build an Engine, loading any supplied descriptors.  Async to match
   * the §A.5 API (descriptor loading may become async for a remote
   * descriptor source); today the load is synchronous under the hood.
   */
  static create(opts: EngineOptions = {}): Promise<Engine> {
    const descriptors = loadDescriptors(opts.descriptors);
    return Promise.resolve(new Engine(descriptors));
  }

  /**
   * Register a host function under `decl`'s leading identifier, exposed to
   * a planned Program as a `cel_fn.<name>` import.  The implementation
   * receives already-decoded {@link CelValue} arguments and returns a
   * {@link CelValue}; a CEL spec error it wants to surface is a
   * {@link CelError} value, never a thrown exception (§A.4.5).  Throws on
   * a malformed `decl` (no leading identifier).
   */
  defineFunction(decl: string, impl: HostFunction): void {
    const name = leadingIdentifier(decl);
    this.functions.set(name, { name, impl });
  }

  /**
   * Instantiate `program` with the full host import object (the runtime is
   * bundled in a static Program; the host supplies the `cel_host`
   * trampolines, the registered `cel_fn` functions, `cel_env.cel_log`, and
   * the WASI stubs) and return a ready-to-eval {@link Instance}.
   */
  plan(program: Program): Promise<Instance> {
    const fns = new Map<string, HostFunction>();
    for (const [name, fn] of this.functions) {
      fns.set(name, fn.impl);
    }
    return instantiateProgram(program, this.descriptors, fns);
  }
}

/** A JS host function's CelValue contract, re-exported for callers. */
export type { HostFunction, CelValue };

function loadDescriptors(
  descriptors: Uint8Array | protobuf.Root | undefined,
): DescriptorSet | undefined {
  if (descriptors === undefined) {
    return undefined;
  }
  if (descriptors instanceof Uint8Array) {
    return DescriptorSet.fromFileDescriptorSet(descriptors);
  }
  return DescriptorSet.fromRoot(descriptors);
}

/**
 * The leading identifier of a host-function declaration — `"f(int): bool"`
 * → `"f"`.  The Program already carries the resolved overload id this maps
 * to; the registry only needs the call-site name.
 */
function leadingIdentifier(decl: string): string {
  const match = /^\s*([A-Za-z_][A-Za-z0-9_.]*)/.exec(decl);
  if (match?.[1] === undefined) {
    throw new Error(
      `defineFunction: declaration '${decl}' has no leading identifier`,
    );
  }
  return match[1];
}
