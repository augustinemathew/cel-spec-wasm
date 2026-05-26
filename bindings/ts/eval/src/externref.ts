/**
 * Externref table — the TS mirror of
 * `compiler_v2/api/internal/cel_host_wasmtime.h::HostExternrefTable`.
 *
 * `CEL_MESSAGE` / `CEL_MAP_HOST` / `CEL_LIST_HOST` CelValues don't carry
 * their data inline — the payload is a `slot` index into this host-side
 * table that maps slot → host object (a protobuf-es message, a JS Map, a
 * JS array, …). Three **independent** slot namespaces: a list slot will
 * not resolve as a message and vice-versa (matches the C++ contract).
 *
 * Slot 0 is the reserved sentinel ("not interned") in every namespace —
 * `intern*` returns slots starting at 1, and `lookup*(0)` is `undefined`.
 * Interning is monotonic within an Eval; `reset()` clears all three
 * between Evals so slots can't leak across invocations.
 *
 * The backing types are generic — the host (`instance.ts` / `host/*.ts`)
 * binds concrete types (e.g. a `HostMessageBacking`); the table itself
 * only owns identity + lifetime.
 */
export class ExternrefTable<
  TMessage = unknown,
  TMap = unknown,
  TList = unknown,
> {
  // Index 0 is the sentinel slot in each namespace.
  private readonly messages: (TMessage | undefined)[] = [undefined];
  private readonly maps: (TMap | undefined)[] = [undefined];
  private readonly lists: (TList | undefined)[] = [undefined];

  public internMessage(backing: TMessage): number {
    this.messages.push(backing);
    return this.messages.length - 1;
  }

  public internMap(backing: TMap): number {
    this.maps.push(backing);
    return this.maps.length - 1;
  }

  public internList(backing: TList): number {
    this.lists.push(backing);
    return this.lists.length - 1;
  }

  /** `undefined` for the sentinel slot 0, an out-of-range slot, or a slot
   *  interned in a different namespace. */
  public lookupMessage(slot: number): TMessage | undefined {
    return this.messages[slot];
  }

  public lookupMap(slot: number): TMap | undefined {
    return this.maps[slot];
  }

  public lookupList(slot: number): TList | undefined {
    return this.lists[slot];
  }

  /** Drop every interned backing (called between Evals). Slot numbering
   *  restarts at 1; the sentinel at 0 is preserved. */
  public reset(): void {
    this.messages.length = 1;
    this.maps.length = 1;
    this.lists.length = 1;
  }
}
