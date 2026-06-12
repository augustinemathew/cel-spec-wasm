// Externref tables — the JS side of the host-handle ABI (§A.4.5).
//
// A host-backed message / map / list does NOT live in wasm linear memory;
// its CelValue carries only an opaque u32 *slot* into a host-side table,
// and the actual JS backing (a protobufjs message, a `Map`, an array)
// stays on the host.  This module is that table.
//
// Three INDEPENDENT namespaces — message, map, list — exactly mirroring
// the C++ contract in `eval/internal/cel_host.h:390-423`:
//
//   * Slot 0 is reserved as the null sentinel in every namespace; the
//     first `intern` returns slot 1.  A slot is meaningful only within
//     its own namespace: a message slot and a map slot may share the
//     numeric value 1 yet point at unrelated backings.  Interning the
//     same JS value into two namespaces yields two independent slots.
//   * `intern(backing)` is monotonic within an Eval — it always appends
//     and returns a fresh slot; it never deduplicates.
//   * `reset()` clears all three namespaces back to just the null
//     sentinel between Evals, so the next `intern` returns slot 1 again.
//
// Lookup contract (documented, non-throwing — the TS mirror of the
// `absl_nullable` pointer the C++ `Lookup*` methods return): `lookup`
// returns `undefined` for slot 0 (the null sentinel) and for any slot
// that was never interned or lies past the high-water mark.  Callers
// distinguish "null backing" from "live backing" by the `undefined`
// result, never by catching an exception — a buggy/malicious guest that
// passes a wild slot through a trampoline must observe a benign
// `undefined`, not crash the host.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.4.5.

/**
 * One opaque handle namespace: a monotonically-growing table of JS
 * backings keyed by a u32 slot.  Slot 0 is the null sentinel and is
 * never handed out by {@link intern}.
 *
 * Generic over the backing type `T` so the message / map / list
 * namespaces stay statically distinct (a `Namespace<MessageBacking>`
 * can never lend a slot a `Namespace<ListBacking>` would accept).
 */
export class Namespace<T> {
  // Index 0 is the reserved null sentinel; it is never a valid backing,
  // so it is modelled as `undefined` and `intern` starts appending at
  // index 1.
  private readonly backings: (T | undefined)[] = [undefined];

  /**
   * Append `backing` and return its slot index.  Always `>= 1`;
   * monotonic within the table's lifetime (until {@link reset}); never
   * deduplicates — interning the same value twice yields two slots.
   */
  intern(backing: T): number {
    const slot = this.backings.length;
    this.backings.push(backing);
    return slot;
  }

  /**
   * Resolve `slot` to its backing, or `undefined` for the null sentinel
   * (slot 0) and for any out-of-range / never-interned slot.  Does not
   * throw — see the module header's lookup contract.
   */
  lookup(slot: number): T | undefined {
    // `noUncheckedIndexedAccess` already widens the element type to
    // include `undefined`; an out-of-range index yields `undefined`
    // here exactly as a never-interned slot does.
    return this.backings[slot];
  }

  /** Current count of live backings (excludes the null sentinel). */
  get size(): number {
    return this.backings.length - 1;
  }

  /** Clear back to just the null sentinel; next {@link intern} is slot 1. */
  reset(): void {
    this.backings.length = 1;
  }
}

/**
 * The three host-handle namespaces an Eval uses, bundled so they reset
 * together.  Each namespace is independent (§A.4.5); a single
 * {@link reset} clears all three, matching the C++ `ExternrefTable::Reset`
 * called between Evals (`cel_host.h:422`).
 *
 * The namespaces are typed `unknown` at this layer because the concrete
 * backing types (the protobufjs `MessageBacking`, the host `Map`/array
 * backings) land in sibling work items; callers that know the backing
 * type narrow on read.  The independence + slot-allocation invariants
 * this module guarantees hold regardless of `T`.
 */
export class ExternrefTable {
  readonly message: Namespace<unknown> = new Namespace<unknown>();
  readonly map: Namespace<unknown> = new Namespace<unknown>();
  readonly list: Namespace<unknown> = new Namespace<unknown>();

  /** Reset all three namespaces to empty (the between-Evals contract). */
  reset(): void {
    this.message.reset();
    this.map.reset();
    this.list.reset();
  }
}
