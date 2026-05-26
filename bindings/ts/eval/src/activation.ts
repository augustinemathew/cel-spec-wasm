/**
 * `Activation` — per-Eval variable bindings, the TS counterpart to
 * `compiler_v2/api/activation.h` (`cel::Activation`). Maps a declared
 * variable name to the `Value` the caller supplies for one `Eval`.
 * `bind` is fluent (returns `this`) and overwrites a prior binding, like
 * the C++ `Activation::Bind`. A binding is any public `Value` — a scalar,
 * a host-backed message / list / map, or an unknown.
 */
import { type Value } from './value.js';

export class Activation {
  private readonly bindings = new Map<string, Value>();

  /** Bind (or overwrite) `name` → `value`. Returns `this` so binds chain:
   *  `new Activation().bind('x', Value.int(1n)).bind('y', ...)`. */
  public bind(name: string, value: Value): this {
    this.bindings.set(name, value);
    return this;
  }

  /** The bound value, or `undefined` if `name` isn't bound. */
  public find(name: string): Value | undefined {
    return this.bindings.get(name);
  }

  public has(name: string): boolean {
    return this.bindings.has(name);
  }

  /** The bound names (insertion order), for the marshaller / diagnostics. */
  public names(): string[] {
    return [...this.bindings.keys()];
  }
}
