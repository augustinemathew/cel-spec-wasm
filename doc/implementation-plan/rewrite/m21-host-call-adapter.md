# Host-call adapter — typed `HostCallContext` for `@host` functions

Status: shipped 2026-05-26.

> **What landed (as-built deltas from the as-written plan):**
>
> - **All three layers shipped**, but the *implementation order* was
>   Layer 1 → Layer 0 → Layer 2 (not 1→2→3), because Layer 2's
>   `BindTypedFunction` returns the new context-form `HostCallback`, so
>   the signature change + trampoline wiring (Layer 0) is a prerequisite
>   for Layer 2.  Layer 1 (`HostCallContext`) is standalone and landed
>   first.
> - **`Bytes` newtype dropped (revises §9.3).** Owner decision during
>   execution: Layer 2 uses `absl::string_view` for *both* CEL `string`
>   and `bytes` — a `string_view` parameter accepts either kind (string
>   first, then bytes); a `std::string` return encodes as `string`.  A
>   `bytes`-specific result uses the raw `HostCallContext::ReturnBytes`.
>   `HostCallContext` itself keeps the distinct, strict `ArgString` /
>   `ArgBytes` / `ReturnString` / `ReturnBytes`.
> - **Polymorphic proto param added (extends §5).** In addition to the
>   concrete `const M&` (dynamic_cast) spelling, Layer 2 admits
>   `const google::protobuf::Message*` (and `const M*`) — ArgProto
>   straight through, no cast, for reflection-generic host fns.
> - **Encoder reuse via `EncodeValueToSlot`** (cel_host.h): a thin public
>   entry that wraps the existing `EncodeFieldResult` with an empty
>   bindings span, so return setters reuse the exact encoder the built-in
>   trampolines use.  Unknown returns bypass it (write CEL_UNKNOWN
>   directly), as §3.6 specified.
> - **`WasmtimeMemoryView` promoted to cel_host_wasmtime.h** (was anon in
>   the .cc) so the `@host` trampoline in engine.cc reuses it; `HostFnEnv`
>   now points at the per-Instance `CelHostCallbackEnv` so the `@host`
>   path shares the same externref table / memory / arena as the cel_host
>   trampolines (the prerequisite that makes proto / list / map args work).
> - **Tests:** `host_call_context_test.cc` (Layer 1, full matrix over
>   fakes incl. `list<customer>` and `map<string,list<customer>>`),
>   `typed_function_test.cc` (Layer 2, canonical-type round-trips +
>   `static_assert` canonical-type predicate for the must-not-compile
>   set), and the migrated `e2e/host_fn_test.cc` (the full type matrix
>   through real wasmtime — scalar/string/bytes/duration/timestamp/proto/
>   list/map args+returns on both the context and typed registration
>   paths, arena `ReturnString`, function-origin `ReturnUnknown` sentinel,
>   error/trap, receiver/composition, and PartialEval known-args dispatch
>   + cross-kind auto-propagation).  All green.
>
> **Future work surfaced during execution:**
> - Touching `cel_host.cc` (the 12-line `EncodeValueToSlot`) drags the
>   whole file into lint scope, surfacing ~16 pre-existing backlog
>   function-size / analyzer items (all in `lint-backlog.md`).  Not
>   cleared here — out of m21 scope.
> - `instance.cc::DecodeCelValueAt` (wasmtime-bound) and
>   `host_call_context.cc::DecodeCelValue` (abstract MemoryView) now
>   duplicate the wire→Value decode logic.  A future refactor could
>   extract one decoder over the abstract primitives.

Status (original): plan — drafted 2026-05-26, not yet started.

This is the concrete realization of the "typed `FunctionImpl` adapter"
named as the closing move for the **Host complex-type gap** in
`m13-custom-fns.md` §"Host complex-type gap" (lines 56–78).  It turns
the raw slot ABI a `@host` callback sees into typed C++ accessors, and
— as the prerequisite that makes proto / list / map args possible at
all — widens the host-call context to carry the externref table and the
arena allocator.

> Scope decisions (owner, 2026-05-26): the type-type (`type` as a value)
> is **out of scope**; the adapter targets the 12 IDL-expressible types.
> All three layers ship together — **nothing deferred**: the wider
> context (Layer 0), the typed `HostCallContext` accessors (Layer 1), AND
> the `BindTypedFunction` template (Layer 2, §5).  The host-call
> signature becomes a **single context object**
> (`std::function<absl::Status(HostCallContext&)>`), replacing the raw
> 4-arg signature and migrating existing callbacks.  This doc is written
> for review **before any code** — code begins once it's approved.

---

## 1. The problem: the host-call context is too thin

A built-in `cel_host` trampoline runs with a rich env
(`CelHostCallbackEnv`, `eval/internal/cel_host_wasmtime.h:71-81`) that
owns the per-eval **externref table** (`HostExternrefTable refs`) and
the **arena allocator** (`arena_alloc_fn`).  A user `@host` function
registered through `Engine::AddFunction` runs with a stripped env —
`HostFnEnv` (`eval/internal/instance_impl.h:30`):

```cpp
struct HostFnEnv {
  const celwasm::HostCallback* callback = nullptr;   // borrowed
  wasmtime_sharedmemory_t* memory = nullptr;         // borrowed
};
```

and the trampoline (`eval/engine.cc:421-453`) hands the callback only
`(mem, mem_size, out_slot, arg_slots)`.  Mapping that against the frozen
wire format (`runtime/cel_data.h`):

| Arg / return | Slot payload | Reachable from `HostFnEnv` today |
| --- | --- | --- |
| bool / int / uint / double | inline in the 24-byte CelValue | ✅ |
| string / bytes | `CelSpan{ptr,len}` → linear memory | ✅ |
| Duration / Timestamp | inline `CelDurTs` | ✅ |
| null | kind only | ✅ |
| list / map **arena** | header byte-offset → linear memory | ✅ (not yet decoded) |
| **proto** | `msg_slot` → externref table | ❌ no table handle |
| list / map **host** | `ref_slot` → externref table | ❌ no table handle |
| **new** string / list / map / message **return** | needs `arena_alloc` | ❌ no allocator handle |

So `list<customer>` and `map<string, list<customer>>` — the motivating
cases — cannot work: `customer` is a `CEL_MESSAGE` carried as an
externref handle the callback can't dereference.  This is structural,
not ergonomic: the callback lacks the table and the allocator.

The wire format itself is already complete and already exercised by the
`cel_host` trampolines (`CelGetFieldImpl`, `CelListAtImpl`,
`CelMapLookupImpl`, …) — **no wire-format change and no new WAT trace is
required.**  The fix is host-side C++: give the `@host` callback the same
context the built-in trampolines already have.

---

## 2. The new host-call signature

Replace the raw 4-arg `HostCallback` with a single context object
(`eval/engine.h`):

```cpp
// Was:
//   std::function<absl::Status(uint8_t* memory, size_t mem_size,
//                              uint32_t out_slot,
//                              absl::Span<const uint32_t> arg_slots)>;
using HostCallback = std::function<absl::Status(HostCallContext&)>;
```

`Engine::AddFunction(overload_id, num_args, HostCallback)` keeps its
shape; only the callback type changes.  The 21 existing callbacks in
`e2e/host_fn_test.cc` and the one in
`eval/instance_test.cc` (`InstanceCustomFnEvalTest`) migrate to the
context form (see §6).

---

## 3. `HostCallContext` — the typed accessor surface

Lives at `eval/host_call_context.{h,cc}` (public; the typed face of a
host call).  Built by the trampoline; not embedder-constructible.

```cpp
namespace celwasm {

// Typed, bounds-checked view over one host-function invocation.
// Every arg accessor validates the slot kind and returns
// InvalidArgument on mismatch; every return setter encodes into the
// out_slot (allocating in the arena for new strings / aggregates).
class HostCallContext {
 public:
  // ── arity ──
  int NumArgs() const;

  // ── scalar args ──
  absl::StatusOr<bool>     ArgBool(int i) const;
  absl::StatusOr<int64_t>  ArgInt(int i) const;
  absl::StatusOr<uint64_t> ArgUint(int i) const;
  absl::StatusOr<double>   ArgDouble(int i) const;

  // ── string / bytes args — slice straight into linear memory ──
  absl::StatusOr<absl::string_view> ArgString(int i) const;
  absl::StatusOr<absl::string_view> ArgBytes(int i) const;

  // ── temporal args ──
  absl::StatusOr<absl::Duration> ArgDuration(int i) const;
  absl::StatusOr<absl::Time>     ArgTimestamp(int i) const;

  // ── null ──
  bool ArgIsNull(int i) const;

  // NOTE: there are no ArgIsUnknown / ArgIsError accessors.  Unknown /
  // error args are auto-propagated by the trampoline BEFORE the callback
  // runs (§3.4 / §3.6), so a callback body only ever sees all-known
  // args.  To *emit* an unknown, see ReturnUnknown below.

  // ── proto arg — resolves msg_slot via the externref table ──
  absl::StatusOr<const google::protobuf::Message*> ArgProto(int i) const;

  // ── aggregate args — arena- OR externref-backed, uniformly ──
  // HostListView / HostMapView wrap the existing HostListBacking /
  // HostMapBacking interfaces (cel_host.h) so callers get
  // .Size() / .At(i) / .Get(key) returning StatusOr<Value>.
  absl::StatusOr<HostListView> ArgList(int i) const;
  absl::StatusOr<HostMapView>  ArgMap(int i) const;

  // ── escape hatch: the fully-decoded Value (any kind) ──
  absl::StatusOr<Value> ArgValue(int i) const;

  // ── return setters — write the out_slot ──
  // ALL of these build a celwasm::Value and route through the existing
  // EncodeFieldResult (cel_host.cc:886): scalars/null/duration/timestamp
  // inline, string/bytes via arena.Alloc + memcpy (EncodeSpan), and
  // message/list/map by interning a backing into the externref table and
  // writing the CEL_*_HOST handle (EncodeAggregateIfAny).  The return
  // half is therefore mostly assembly of existing parts.
  absl::Status ReturnBool(bool v);
  absl::Status ReturnInt(int64_t v);
  absl::Status ReturnUint(uint64_t v);
  absl::Status ReturnDouble(double v);
  absl::Status ReturnString(absl::string_view v);   // arena-allocated copy
  absl::Status ReturnBytes(absl::string_view v);     // arena-allocated copy
  absl::Status ReturnDuration(absl::Duration v);
  absl::Status ReturnTimestamp(absl::Time v);
  absl::Status ReturnNull();
  // Proto: OWNING — the backing must outlive the call (it lives in the
  // per-eval externref table until Reset()).  Borrowing a stack/temp
  // message would dangle.  See §9.4.
  absl::Status ReturnProto(std::unique_ptr<google::protobuf::Message> m);
  absl::Status ReturnList(absl::Span<const Value> elems);          // copies
  absl::Status ReturnMap(absl::Span<const std::pair<Value, Value>> entries);
  // Explicit unknown / error RETURN (3VL — §3.6).  No attribute arg:
  // ReturnUnknown stamps {CEL_UNKNOWN, payload.unk = <function-origin
  // sentinel>}, marking the unknown as function-returned (distinct from
  // an auto-propagated input unknown, which carries a real attribute id).
  // (Auto-propagation of unknown/error ARGS is automatic in the
  // trampoline — there is no Propagate* method to call.)
  absl::Status ReturnUnknown();
  absl::Status ReturnError(/* ErrorPayload / code */);
  absl::Status ReturnValue(const Value& v);          // any kind, incl. Unknown

 private:
  friend class /* trampoline */;
  // The trampoline pre-resolves each externref arg slot into its C++
  // object before constructing the context (see §4.1), so the embedder
  // never sees a slot index or the table.  `refs` is retained privately
  // ONLY for the return path (`ReturnProto`/`ReturnList`/`ReturnMap`
  // must Intern a new value to obtain a slot).
  HostCallContext(MemoryView& mem, ExternrefTable& refs,
                  ArenaAllocator& arena, uint32_t out_slot,
                  absl::Span<const uint32_t> arg_slots);
};

}  // namespace celwasm
```

### 3.1 Type → accessor → wire mapping (the 12 IDL types)

| celfn IDL type | arg accessor | return setter | decode path (reused) |
| --- | --- | --- | --- |
| `bool` | `ArgBool` | `ReturnBool` | `CelValue.payload.b` |
| `int` | `ArgInt` | `ReturnInt` | `payload.i` |
| `uint` | `ArgUint` | `ReturnUint` | `payload.u` |
| `double` | `ArgDouble` | `ReturnDouble` | `payload.d` |
| `string` | `ArgString` | `ReturnString` | `ReadSpan(s.ptr,s.len)` / `EncodeSpan` |
| `bytes` | `ArgBytes` | `ReturnBytes` | same as string |
| `Duration` | `ArgDuration` | `ReturnDuration` | `payload.dur` / `DecomposeAbslDuration` |
| `Timestamp` | `ArgTimestamp` | `ReturnTimestamp` | `payload.ts` |
| `null` | `ArgIsNull` | `ReturnNull` | kind == `CEL_NULL` |
| `proto(fqn)` | `ArgProto` | `ReturnProto` | `refs.Lookup(msg_slot)->message()` / `Intern` |
| `list<T>` | `ArgList` | `ReturnList` | arena: `ReadArenaListElement`; host: `refs.LookupList`; encode via arena |
| `map<K,V>` | `ArgMap` | `ReturnMap` | arena: header walk + `DecodeKey`; host: `refs.LookupMap`; encode via arena |

Aggregates are recursive — `ArgList(i)->At(j)` yields a `Value` that may
itself be a message or a nested list/map, so `list<customer>` and
`map<string, list<customer>>` fall out of the same machinery.

### 3.2 Machinery reuse (no reinvention)

Every accessor is a thin wrapper over existing eval-side units:

- `MemoryView::ReadCelValue / WriteCelValue / ReadSpan / WriteU32`
  (`cel_host.h:299`) — slot read/write.
- `DecodeKey`, `EncodeValue`, `EncodeSpan` (`cel_host.cc:724 / 785 / 746`)
  — Value↔CelValue codec.
- `ReadArenaListElement` (`cel_host.cc:1703`) + the `ArenaListHeader` /
  `ArenaMapHeader` layouts (`cel_data.h:71 / 90`) — arena navigation.
- `ExternrefTable::Lookup / LookupList / LookupMap / Intern`
  (`cel_host.h:319`) — handle resolution for proto / host aggregates.
- `HostListBacking` / `HostMapBacking` / `ProtoList` / `ProtoMap`
  (`cel_host.h:231 / 146 / 276 / 202`) — aggregate views.
- `Value` factories + `As*` accessors (`value.h:88 / 156`).
- `WasmtimeArenaAllocator` (`cel_host_wasmtime.h:100`) — return allocation.

`HostListView` / `HostMapView` are new but trivial: a borrowed
`const HostListBacking*` (+ element type) with `Size()` / `At()`, and
the map analogue.  They exist so a caller never has to know whether the
backing is arena- or externref-resident.

`HostCallContext` is the **public, typed counterpart of the internal
`TrampolineContext`** (`cel_host.h`), which the built-in `cel_host`
trampolines already use:

```cpp
struct TrampolineContext {           // internal — what the trampolines see
  const CelHostBindings& bindings;
  MemoryView& mem;
  ExternrefTable& refs;
  ArenaAllocator& alloc;
};
```

The adapter is "give `@host` callbacks the same context the built-in
trampolines have, but typed and kind-checked."

### 3.3 Reading the externref table

A proto / host-aggregate value does **not** live in linear memory — its
CelValue stores a **u32 index** into the host-owned externref table
(`HostExternrefTable`, three parallel `std::vector<shared_ptr<const
Backing>>` for messages / maps / lists; slot 0 is a nullptr sentinel;
`Reset()` between Evals — cel_host_wasmtime.cc:28-76).  The read flow
(real code, `CelListAtImpl`, cel_host.cc:899-935):

```cpp
CelValue cv = ctx.mem.ReadCelValue(slot);          // 1. read 24-byte slot
if (cv.kind != CEL_LIST_HOST) { /* type error */ } // 2. CHECK THE TAG
auto* backing = ctx.refs.LookupList(cv.payload.ref_slot);  // 3. index→backing
if (backing == nullptr) { /* OOB index → FailedPrecondition */ }
auto v = backing->At(i, elem_type);                // 4. call the live object
```

`Lookup` / `LookupList` / `LookupMap` are bounds-checked (return
`nullptr` for an out-of-range index).  The returned `const Message*` /
backing IS the live host object (see §4.1), so step 4 is a direct call,
not a decode.

### 3.4 Kind-tag safety (the non-bypassable check)

A `CelValue` is a **tagged union**: `kind:u32@0` is the tag, `payload@8`
is a `union` where `payload.i` (int64), `payload.s` (`CelSpan{ptr,len}`),
and `payload.ref_slot` (u32) **all alias the same bytes**
(cel_data.h:141).  The union is **not** self-describing — the payload
bytes alone never tell you which member is live.

The hazard this creates is worse than a wrong value.  Reading
`payload.s` when the value is actually an `int` reinterprets the
integer's 64 bits as a `{ptr, len}` pair → `ptr` is an arbitrary offset
into linear memory → an **out-of-bounds / garbage read**.  So the
`kind` tag is **load-bearing for memory safety**, not just correctness.

The single safeguard is: **verify `kind == expected` before touching the
union.**  The adapter makes that check non-bypassable — every accessor
performs it on the only path that reaches the payload, and returns
`StatusOr<T>` (never a bare `T`) so a mismatch surfaces as an error
status instead of a garbage read:

```cpp
absl::StatusOr<absl::string_view> HostCallContext::ArgString(int i) const {
  if (i < 0 || i >= NumArgs()) return absl::OutOfRangeError(...);
  CelValue cv = mem_.ReadCelValue(arg_slots_[i]);
  if (cv.kind != CEL_STRING) {                          // tag check
    return absl::InvalidArgumentError(absl::StrCat(
        "arg ", i, ": expected string, got ", KindName(cv.kind)));
  }
  return mem_.ReadSpan(cv.payload.s.ptr, cv.payload.s.len);  // only now: union
}
```

`ArgString(0)` on an int slot returns an error — it can never hand back
a garbage `string_view`.  This is *why* every accessor returns
`StatusOr<T>`: the type tag is checked on the only path that reaches the
union, so the embedder cannot read a string from an int (or a `ptr` from
an int).  The §7 matrix exercises exactly this — every accessor against
a deliberately wrong-kind slot, asserting `InvalidArgument`, not a
crash.

> **3VL operands — auto-propagated at Layer 1.**  A host-fn arg can be
> `CEL_UNKNOWN` or `CEL_ERROR` (partial eval / a poisoned subexpression).
> The **trampoline checks every arg slot before invoking the callback**;
> if any is unknown/error it writes that value straight to the out-slot
> (attribute id preserved, like the trampoline convention at
> `CelListAtImpl` lines 908-915) and **does not call the callback at
> all**.  This matches CEL dispatch semantics — a function is not invoked
> on unknown operands; the unknown propagates.  So at *every* layer
> (raw `HostCallContext`, typed `BindTypedFunction`) the callback body
> only ever runs with all-known args; it never has to test for or
> forward an unknown arg.  (See §3.6 for the *explicit* unknown a
> function may still choose to return.)

### 3.5 Returning to the caller (string / proto / list / map)

All return setters funnel through the existing universal encoder
`EncodeFieldResult` (cel_host.cc:886), so the wire shape a host fn
produces is byte-identical to what every built-in trampoline produces.
Per kind:

- **string / bytes** — `EncodeSpan` (cel_host.cc:746): `arena.Alloc(len)`
  bump-allocates in `cel.memory`, the bytes are `memcpy`'d in, and the
  out-slot CelValue gets `{kind:CEL_STRING, payload.s={ptr,len}}`.  The
  bytes physically live in linear memory; the caller reads them in place.
  This is the `arena_alloc` the raw callback lacks today — handed over by
  Layer 0.  (So `ReturnString` is a real new-allocation, not the
  in-place / length-preserving hack the current e2e is limited to.)
- **proto** — `EncodeAggregateIfAny` (cel_host.cc:859): the message is
  wrapped in a `HostMessageBacking`, `refs.Intern(backing)` returns a
  `msg_slot`, and the out-slot gets `{kind:CEL_MESSAGE, msg_slot}`.  When
  the caller later selects a field on the result, codegen routes it
  through the `cel_get_field` trampoline, which `Lookup`s the backing.
- **list** — `refs.InternList(backing)` → `{kind:CEL_LIST_HOST,
  ref_slot}`; the caller's `result[i]` routes through `cel_list_at`.
- **map** — `refs.InternMap(backing)` → `{kind:CEL_MAP_HOST, ref_slot}`;
  the caller's `result[k]` routes through `cel_map_lookup`.

So aggregate returns use the **host (externref) representation** — they
intern a backing and return its handle, *not* an arena-serialized blob.
Lifetime: the interned backing and the arena bytes both live in per-eval
state cleared at `Reset()` *after* the Eval, so they outlive the call and
the caller can consume them.  Aggregates are recursive — a returned
`list<customer>` is a `HostList` of `Value::Message`s, each interned in
turn, so `map<string, list<customer>>` composes without special-casing.

### 3.6 Unknown / error (3VL) — two distinct mechanisms

There are **two** ways an unknown reaches the caller, and they are
deliberately distinguishable:

**(1) Automatic propagation of an unknown *input* — Layer 1, no opt-in.**
If any arg is `CEL_UNKNOWN`/`CEL_ERROR`, the trampoline writes that value
to the out-slot **verbatim** (its `attribute_id` preserved) and never
calls the callback (§3.4).  This is the dominant case and requires
nothing from the function — the unknown carries the *input attribute* it
came from, so a downstream consumer learns *which* input was unknown.

**(2) Explicit unknown *returned by the function* — with a sentinel.**
A function whose args are all known may still decide its result is
unknown.  It calls:

```cpp
absl::Status ReturnUnknown();   // no attribute argument
```

which stamps a `CEL_UNKNOWN` carrying a **reserved function-origin
sentinel** in `payload.unk` (not a real interned attribute id).  The
sentinel marks the unknown as *"a function returned this"* — distinct
from a propagated input unknown, which carries an actual attribute id.
A consumer can tell the two apart (e.g. `Value::UnknownIsFunctionReturned()`
/ comparing the id to the sentinel constant).

This sentinel approach is what **resolves §9.5**: a function does **not**
mint a fresh attribute id (it has no basis to attribute the unknown to a
specific input/qualifier, and the instance's per-eval interner — see
instance.cc:931-998 — owns id allocation).  Instead it emits the one
reserved sentinel meaning "function-returned unknown."  `ReturnError`
behaves analogously for `CEL_ERROR`.

**Implementation note — the encoder's `kUnknown` ban.**  `EncodeValue`
(cel_host.cc:817) `ABSL_CHECK(false)`s on `kUnknown` ("backings don't
return unknowns").  That invariant is right for built-in field-read
backings but wrong for a user host fn.  Resolution: both the automatic
propagation (1) and `ReturnUnknown` (2) write the `CEL_UNKNOWN` CelValue
**directly**, *not* through `EncodeValue` — so the shared backing
invariant at :817 stays intact and the host-call path owns its
unknown/error encoding.  `ReturnValue(Value::Unknown)` routes the same
way.

> Two misses this corrects from the first draft: the return surface
> omitted unknown/error entirely (and the reused encoder forbade them),
> and auto-propagation was scoped to Layer 2 (`BindTypedFunction`) when
> it must be a **Layer 1** behavior — automatic at every layer.

---

## 4. Layer 0 wiring (what actually changes under the hood)

1. **`HostFnEnv` gains two borrowed pointers** — `ExternrefTable* refs`
   and the arena reentry handle (`wasmtime_func_t arena_alloc_fn` +
   `wasmtime_context_t*`, mirroring `CelHostCallbackEnv`).  These come
   from the same `InstanceImpl` members the `cel_host` trampolines
   already use, so it is wiring, not new state.
2. **`RegisterHostCallbacks`** (`eval/engine.cc:512`) fills the new
   fields when it builds each per-Plan `HostFnEnv`.
3. **`HostCallbackTrampoline`** (`eval/engine.cc:421`) constructs a
   `WasmtimeMemoryView`, a `WasmtimeArenaAllocator`, wraps them + `refs`
   + the slots into a `HostCallContext`, and invokes
   `(*env->callback)(ctx)`.  Non-OK status → `TrapFromStatus` exactly as
   today (the NUL-terminated-message fix at `engine.cc:405` stays).
4. **Externref lifetime** — `refs` is per-eval and `Reset()` between
   Evals; a `Message*` from `ArgProto` is valid only for the callback's
   duration, which matches the existing trampoline contract.

No change to: the slot wire format, the runtime `.wasm`, the codegen
import (`cel_fn.<overload_id>` signature is unchanged — still
`(out_slot, arg0, …) → ()`).  The only newly-relied-on host import for
`@host` callbacks is `cel.arena_alloc`, already bound at
`InstantiateRuntime` time and per `feedback_no_lazy_imports` always
linked.

### 4.1 Externrefs are host-owned → arg resolution is zero-copy

Every entry in the externref table was interned by **host** code — an
embedder binding, or a `cel_make_message` trampoline during this eval.
The wasm side never holds the object, only an i32 index (`msg_slot` /
`ref_slot`) into the host-owned `HostExternrefTable`
(`std::vector<shared_ptr<…>>`).  Two consequences shape the API:

1. **No copy across the boundary.** `ArgProto` returns the *exact*
   `const google::protobuf::Message*` the host interned —
   `refs.Lookup(msg_slot)->message()`.  Same for host list/map backings.
   Nothing is serialized; the callback reads the live host object.
2. **The trampoline pre-resolves args, so `HostCallContext` never
   exposes `ExternrefTable`.** The trampoline resolves each externref
   arg slot into its C++ object *before* constructing the context; the
   context stores already-resolved handles.  The table remains a private
   ctor dependency used **only by the return path** (`ReturnProto` /
   `ReturnList` / `ReturnMap` must `Intern` a newly-produced value to
   obtain a slot for the out-slot CelValue).  The embedder never sees a
   slot index or the table.

This is purely a host-side win: it does **not** remove the Layer 0
wiring (the per-eval table lives on `InstanceImpl`, so the trampoline
must still be handed the pointer), but it makes that handoff free of
marshalling and keeps the public surface in terms of C++ objects, not
handles.

---

## 5. Layer 2: `BindTypedFunction` (in scope)

A variadic template adapts a plain typed lambda into a `HostCallback`,
mapping each parameter type to its `ArgXxx` accessor and the return to
its `ReturnXxx`.  This is the user-facing payoff — the embedder writes no
marshalling and the kind-checks happen for them:

```cpp
engine.AddTypedFunction("double_it_int",
    [](int64_t x) -> absl::StatusOr<int64_t> { return x * 2; });

engine.AddTypedFunction("headline_message_acme_User",
    [](const acme::User& u) -> absl::StatusOr<std::string> {
      return absl::AsciiStrToUpper(u.first());
    });
```

`AddTypedFunction` is a thin `Engine` wrapper that builds a
`HostCallback` closure.  Unknown/error args are already handled — the
trampoline auto-propagates them before the closure runs (§3.4), so the
closure executes only with all-known args.  It then, for each parameter
`j`, invokes the matching `ctx.ArgXxx(j)` (propagating its error status),
calls the user lambda, and `ctx.ReturnXxx(result)`.  A lambda that wants
to *emit* an unknown returns `absl::StatusOr<cel::Value>` and returns
`Value::Unknown()` (the function-origin sentinel form, §3.6).  The
parameter→accessor map is resolved at compile time by a trait on each
admitted C++ type:

| C++ param / return type | accessor / setter | CEL type |
| --- | --- | --- |
| `bool` | `ArgBool` / `ReturnBool` | `bool` |
| `int64_t` | `ArgInt` / `ReturnInt` | `int` |
| `uint64_t` | `ArgUint` / `ReturnUint` | `uint` |
| `double` | `ArgDouble` / `ReturnDouble` | `double` |
| `absl::string_view` (param), `std::string` (return) | `ArgString` / `ReturnString` | `string` |
| `Bytes` newtype (param `string_view`, return `std::string`) | `ArgBytes` / `ReturnBytes` | `bytes` |
| `absl::Duration` | `ArgDuration` / `ReturnDuration` | `Duration` |
| `absl::Time` | `ArgTimestamp` / `ReturnTimestamp` | `Timestamp` |
| `const M&` where `M : google::protobuf::Message` | `ArgProto`+`dynamic_cast<const M*>` / `ReturnProto` | `proto(M)` |
| `HostListView` (param), `std::vector<Value>` (return) | `ArgList` / `ReturnList` | `list<T>` |
| `HostMapView` (param), `std::vector<pair<Value,Value>>` (return) | `ArgMap` / `ReturnMap` | `map<K,V>` |
| `Value` | `ArgValue` / `ReturnValue` | any |

Disambiguation notes baked into the traits:

- **`string` vs `bytes`** share `absl::string_view` at the C++ level, so
  `bytes` uses a one-field `Bytes` newtype wrapper to pick `ArgBytes`.
  (Alternative considered — a per-call type tag — rejected as less
  type-safe.)
- **proto** params are `const M&`; the trait calls `ArgProto(j)` then
  `dynamic_cast<const M*>` (null ⇒ wrong message type ⇒ `InvalidArgument`,
  matching the m13 §"Value → concrete proto" sketch at line 80).
- **`int` only maps to `int64_t`** (not `int`/`int32_t`) so the overload
  is unambiguous and matches CEL's 64-bit `int`.

Arity is checked against `num_args` at registration (the template knows
`sizeof...(Args)`), surfacing a mismatch as `InvalidArgument` from
`AddTypedFunction` rather than at call time.  Both registration paths
coexist: `AddFunction` (raw `HostCallContext&`, full control) and
`AddTypedFunction` (typed lambda, zero marshalling).

### 5.1 Canonical types only — no implicit conversions

This is the load-bearing rule for Layer 2 in C++.  The danger: if the
trait maps the CEL `int` to `int64_t` but the embedder writes
`[](int x)`, `[](long x)`, or `[](float x)`, C++'s implicit conversions
would either **silently narrow** (`int64_t`→`int` drops the top 32 bits)
or pick the wrong accessor.  `bool` is the worst — it is implicitly
constructible from nearly any scalar or pointer.  The template must
accept **only** a closed set of canonical C++ types and reject every
other parameter type at **compile time**, with no conversion fallback.

The mechanism that guarantees this — and *why* it works:

1. **Decompose the declared signature, don't call-and-convert.**  A
   function-traits helper extracts each parameter's exact declared type
   `T_j` from the callable (`&Lambda::operator()`, function pointer, or
   `std::function`).  We decode each arg to **exactly** `T_j` and invoke
   the lambda with exactly `T_j`.  Because our decoded value is never
   passed *into* a parameter of a different type, **no implicit
   conversion site exists** — the classic narrowing/`bool` traps simply
   never arise.

2. **`ArgTrait<T>` hard-errors by default; canonical types specialize
   it.**  The primary template is an always-false `static_assert`; only
   the canonical types have specializations.  A non-canonical `T_j` has
   no specialization → a clear compile error, never a silent coercion:

   ```cpp
   template <typename T>
   struct ArgTrait {
     static_assert(sizeof(T) == 0,
         "host-fn parameter is not a canonical CEL type. Use exactly: "
         "int64_t, uint64_t, double, bool, absl::string_view, Bytes, "
         "absl::Duration, absl::Time, const M& (M:Message), HostListView, "
         "HostMapView, or Value. (e.g. use int64_t, not int/long; "
         "double, not float.)");
   };
   template <> struct ArgTrait<int64_t> {
     static absl::StatusOr<int64_t> Decode(const HostCallContext& c, int j) {
       return c.ArgInt(j);
     }
   };
   // ...one specialization per canonical type...
   // proto is a partial specialization, constrained:
   template <typename M>
   struct ArgTrait<const M&> {            // enabled iff M : Message
     static_assert(std::is_base_of_v<google::protobuf::Message, M>, "...");
     static absl::StatusOr<const M*> Decode(const HostCallContext& c, int j) {
       auto m = c.ArgProto(j);            // const Message*
       if (!m.ok()) return m.status();
       auto* d = dynamic_cast<const M*>(*m);
       if (d == nullptr) return absl::InvalidArgumentError("wrong message type");
       return d;
     }
   };
   ```

   No `std::is_convertible`, no "find the closest overload" — exact type
   match or compile error.  `int`, `int32_t`, `long`, `unsigned`,
   `float`, `char*`, `const std::string&` (where `string_view` is the
   canonical) all fail to compile with the message above.

3. **Cv/ref handling is per-type, not blanket-decayed.**  Value types
   (`int64_t`, `double`, `absl::string_view`, …) are matched after
   stripping top-level `const`/`&` (so `int64_t`, `const int64_t&`, and
   `int64_t&&` are all the int case).  Proto is matched as `const M&`
   *without* decaying away the reference, because passing a message by
   value is wrong (slicing) and must itself be a compile error — the
   only admitted proto spelling is `const M&`.

4. **Return type is checked the same way.**  The lambda must return
   `absl::StatusOr<R>` where `ReturnTrait<R>` exists; a bare `R`, or a
   `StatusOr<non-canonical>`, hard-errors.  (CEL host fns are fallible by
   contract, so `StatusOr` is mandatory, not optional.)

The net contract: **`AddTypedFunction` compiles iff every parameter and
the return are spelled as canonical CEL host types.**  Anything else is
a compile-time error naming the offending type and the canonical
substitute — never a silent narrowing or mis-dispatch at runtime.  Tests
for this live in `typed_function_test.cc` as `static_assert`-based
compile-fixture checks plus a `// must-not-compile` doc block enumerating
the rejected spellings.

---

## 6. Migration

- `e2e/host_fn_test.cc` — all 21 callbacks rewrite from the
  4-arg lambda + hand-rolled `ReadKind`/`ReadPayload`/`ReadSpan`/
  `WriteKind`/`WritePayload` to `HostCallContext` accessors.  The file's
  local wire helpers (`ReadKind` … `WriteSpan`, `SlotInBounds`,
  `WireSpan`) are deleted — they become the adapter's job.  This is the
  proof that the adapter actually cleans up call sites.

> Plan-vs-execution delta: the file was further rewritten so that **every
> test inlines the real customer flow** (`Compiler::NewBuilder()…Build()` →
> `Compile` → `Engine::NewBuilder().Build()` → `AddTypedFunction` /
> `AddFunction` → `Plan` → `Eval`/`PartialEval`) — the `CompilerSpec` /
> `RegFn` / `RunExpr` / `RunTyped` / `RunPartial` DTO wrappers were dropped
> (a typed-API row never types an arg count).  The matrix runs on **both**
> the typed and context registration paths, 67 cases.  Bytes / unknown /
> error returns through the typed API use the `Value` escape hatch
> (`StatusOr<Value>` → `Value::Bytes` / `Value::Unknown` / `Value::Error`),
> since a `std::string` return is `string`-kinded; proto return is
> first-class via `std::unique_ptr<M>`.
- `eval/instance_test.cc::InstanceCustomFnEvalTest` — the one
  `is_number(string)→bool` callback migrates likewise.
- The file header's "Known ABI limitation" note (no `arena_alloc`) is
  removed; string/bytes/aggregate returns now allocate.

---

## 7. Test matrix (the hardening target) — at every layer

"Make solid what we have" = a case **per type, on both the arg side and
the return side**, plus the negatives.  Cases that need a producer not
yet wired get a `GTEST_SKIP` naming the blocker (never silent omission).

**The matrix below is the shared spec, and it runs at ALL THREE
layers** — each layer gets its own positive *and* negative cases for the
full type set, at the test level appropriate to that layer.  A type is
not "done" until it is green (positive) and rejecting (negative) at every
layer it passes through:

| Layer | Test file | Harness | What "positive" / "negative" mean here |
| --- | --- | --- | --- |
| **Layer 1 — `HostCallContext`** | `host_call_context_test.cc` | unit, over **fake** `MemoryView` / `ExternrefTable` / `ArenaAllocator` (no wasm) | +: each `ArgXxx` decodes a correctly-typed slot; each `ReturnXxx` encodes.  −: each `ArgXxx` on a **wrong-kind** slot → `InvalidArgument` (the kind-tag check, §3.4); OOB arg index; OOB externref slot → error not UB. |
| **Layer 2 — `BindTypedFunction`** | `typed_function_test.cc` | unit (template) + **compile-fixtures** | +: each canonical type round-trips through a typed lambda; arity derived correctly.  −: wrong-kind arg surfaces as the lambda seeing an error status / the call erroring; arity mismatch → `InvalidArgument` at register; **must-not-compile** `static_assert` fixtures for every non-canonical spelling (`int`, `float`, `char*`, by-value proto, bare-`R` return). |
| **Layer 0 + integration** | `host_fn_test.cc` | **e2e through real wasmtime** | +: the trampoline populates the context (table + arena) so a callback resolves a proto arg and allocates a return end-to-end.  −: wrong-kind arg through the real pipeline → CEL error/trap as specified; OOB slot; null/empty env handled. |
| **PartialEval dimension** | `host_fn_test.cc` (PartialEval) | e2e through `Instance::PartialEval` with an unknown pattern | +: a fn arg **marked unknown** by a pattern → the callback is NOT invoked and the unknown auto-propagates (attribute id preserved, §3.4).  −: confirm a function-origin `ReturnUnknown()` (all args known) coexists with pattern-driven unknowns and is still distinguishable (sentinel intact). |

So the arg-decode and return-encode tables below are instantiated across
**all of these**: fake-backed unit cases (Layer 1), typed-lambda +
compile cases (Layer 2), wasmtime e2e cases (Layer 0 / integration), AND
the **PartialEval** runs (same fns, but an argument marked unknown via a
pattern — exercising the auto-propagation path and the function-origin
sentinel under partial evaluation).  The negative half is load-bearing at
each — a kind-mismatch that silently returned garbage at Layer 1, a
non-canonical type that silently compiled at Layer 2, or an unknown that
failed to propagate under PartialEval, is exactly the class of bug this
milestone exists to prevent.

**Arg-decode matrix** — one `@host` fn per row, asserting the callback
decoded correctly:

| type | positive | negative (kind mismatch) | boundary |
| --- | --- | --- | --- |
| bool | ✓ | non-bool slot → InvalidArgument | — |
| int | ✓ | non-int | `INT64_MIN`, `INT64_MAX` |
| uint | ✓ | non-uint | `0`, `UINT64_MAX` |
| double | ✓ | non-double | `±inf`, `nan`, `-0.0` |
| string | ✓ | non-string | empty, embedded NUL, multi-byte UTF-8 |
| bytes | ✓ | non-bytes | empty, NUL bytes |
| Duration | ✓ | non-duration | max/min range (`langdef.md` §1176) |
| Timestamp | ✓ | non-timestamp | min/max serializable |
| null | ✓ | non-null treated as present | — |
| proto | ✓ (`acme.User` field read) | non-message / wrong fqn | unset field default |
| list\<int\> | ✓ | non-list | empty, 1, many |
| list\<customer\> | ✓ (proto elements) | element wrong type | nested message access |
| map\<string,int\> | ✓ | non-map | missing key, dup-equal keys |
| map\<string,list\<customer\>\> | ✓ | — | nested aggregate value |

**Return-encode matrix** — one fn per row, asserting Eval sees the
encoded result: every scalar, string/bytes (incl. newly-allocated, not
just in-place), Duration/Timestamp, null, a constructed proto, a
constructed `list<int>` / `list<customer>`, a constructed
`map<string,list<customer>>`, **plus the 3VL returns (§3.6):**

- **unknown — auto-propagated input** — give the fn an unknown arg;
  assert the **callback is NOT invoked** (e.g. a flag the body would
  set stays false) and the caller sees `IsUnknown()` with the **same**
  attribute id (id preserved by the trampoline).
- **unknown — explicit (function-origin)** — fn (all args known) calls
  `ReturnUnknown()`; assert the caller sees `IsUnknown()` AND that it is
  flagged function-origin (the sentinel), distinct from a propagated
  input unknown's attribute id.
- **error — auto-propagated / explicit** — same two shapes for
  `CEL_ERROR`.
- **negative** — confirm the shared `EncodeValue` `kUnknown` CHECK
  (cel_host.cc:817) is **not** on the host-call path (a regression test
  that both an auto-propagated and an explicit host unknown do not trip
  it); and that the function-origin sentinel is never confused with a
  real interned attribute id.

**Composition / shape** — receiver (`this`) vs global; multi-arg order;
a fn whose result feeds another fn; callback returning a CEL error
(propagates as `kError`); callback returning non-OK status (traps);
arity / bounds (too few args, out-of-bounds slot).

Rows ticked in `testing-checklist.md` and the per-type coverage logged
in `per-component-test-coverage.md` at close.

---

## 8. Files touched (feature-pipeline-checklist view)

| Stage | File | Change |
| --- | --- | --- |
| public API | `eval/engine.h` | `HostCallback` typedef → context form; doc |
| public API | `eval/host_call_context.h` | **new** — `HostCallContext`, `HostListView`, `HostMapView`, `Bytes` newtype |
| impl | `eval/host_call_context.cc` | **new** — accessors over the reuse machinery |
| public API | `eval/typed_function.h` | **new** — `BindTypedFunction` template + type traits (§5); `Engine::AddTypedFunction` |
| impl | `eval/engine.cc` | `AddTypedFunction` wraps the template into a `HostCallback` |
| test | `eval/typed_function_test.cc` | **new** — template traits + arity-mismatch + each type round-trip |
| impl | `eval/internal/instance_impl.h` | `HostFnEnv` gains `refs` + arena handle |
| impl | `eval/engine.cc` | `RegisterHostCallbacks` fills env; `HostCallbackTrampoline` builds `HostCallContext` |
| build | `eval/BUILD.bazel` | `host_call_context` `cc_library` (public); dep wiring |
| test | `eval/host_call_context_test.cc` | **new** — unit matrix over a fake `MemoryView`/`ExternrefTable`/`ArenaAllocator` |
| test | `e2e/host_fn_test.cc` | migrate 21 callbacks; inline the real customer flow (no DTO wrappers); both paths × full kind matrix (67 cases) |
| test | `eval/instance_test.cc` | migrate the one custom-fn callback |
| docs | `m13-custom-fns.md` | mark the "Host complex-type gap" closed, link here |
| docs | `testing-checklist.md`, `per-component-test-coverage.md` | tick rows at close |

Unit-testable in isolation: `HostCallContext` over fake
`MemoryView` / `ExternrefTable` / `ArenaAllocator` (the abstract bases
already exist at `cel_host.h:299/319/355`), so the §7 e2e isn't the only
coverage — the accessor logic gets component-level tests too.

---

## 9. Open questions

1. ~~`HostListView` / `HostMapView` vs. returning `Value` directly~~ —
   **RESOLVED (owner, 2026-05-26): lazy view is primary.**
   `ArgList`/`ArgMap` return a `HostListView`/`HostMapView` that reads
   elements on demand (no eager decode of the whole aggregate); `ArgValue`
   stays as the eager escape hatch for callers that want the full
   `Value` tree up front.
2. ~~Arena-allocated return lifetime~~ — **RESOLVED (owner,
   2026-05-26): safe, pinned by test.**  The arena (and the externref
   table) reset is per-Eval, *after* the call returns, so a return's
   arena bytes / interned backing outlive the callback and stay valid
   for the caller's later reads.  A dedicated lifetime test pins this:
   a host fn returns a string/list/map, and the expr reads it *after* a
   subsequent host call, asserting the bytes/handle are still valid.
3. ~~`bytes` ergonomics~~ — **RESOLVED (owner, 2026-05-26): `Bytes`
   newtype.**  CEL `string` and `bytes` are *both* `absl::string_view`
   in C++, so the typed Layer-2 API cannot distinguish them by parameter
   type alone.  A one-field `Bytes` wrapper (over `absl::string_view` for
   args, `std::string` for returns) is the disambiguator: `[](Bytes b)`
   dispatches to `ArgBytes`, `[](absl::string_view s)` to `ArgString`.
   This is the single place a typed param isn't a bare std type — an
   accepted, deliberate cost of distinguishing the two CEL types that
   share a C++ representation.
4. ~~Proto-return ownership~~ — **RESOLVED (owner, 2026-05-26):
   OWNING.**  `ReturnProto(std::unique_ptr<Message>)` →
   `OwnedProtoBacking`, interned into the per-eval externref table.  It
   is the only option that is *both* zero-copy *and* dangle-proof: the
   signature forces the embedder to surrender ownership, so no
   stack-local is left to dangle.  (A borrowing `const Message&` over a
   stack temporary is use-after-free once the callback returns — the
   externref table would hold a pointer to a destroyed object that the
   caller dereferences later in the same Eval; a copying overload would
   pay a full `CopyFrom` per return.)  In the Layer-2 typed API a lambda
   returning `absl::StatusOr<std::unique_ptr<acme::User>>` maps to it.
   Lists/maps have no such hazard — `ReturnList`/`ReturnMap` copy the
   `Value`s, which own/share their own storage.
5. ~~Unknown returns: propagate-only, or mint fresh?~~ — **RESOLVED
   (owner, 2026-05-26): two mechanisms, function-origin sentinel.**
   (a) An unknown/error *input* is **auto-propagated** by the trampoline
   before the callback runs (§3.4), carrying its real `attribute_id`.
   (b) A function may **explicitly** emit an unknown via `ReturnUnknown()`
   (no attribute), which stamps a reserved **function-origin sentinel**
   in `payload.unk` — *not* a minted attribute id.  This is the key
   decision: a function has no basis to attribute an unknown to a
   specific input, and the per-eval interner (instance.cc:931-998) owns
   id allocation, so we do NOT mint; instead the sentinel marks "a
   function returned this unknown," and a consumer distinguishes it from
   a propagated input unknown.
   **Sentinel value — RESOLVED (owner, 2026-05-26): `UINT32_MAX`.**
   `0` is NOT free (checked): attribute ids are **0-based indices** into
   `bindings.attributes` — `instance.cc:968` interns
   `static_cast<uint32_t>(i)`, so id `0` is the *first real attribute*,
   and `0` is already an overloaded fallback ("`return 0u` … still
   surfaces UNKNOWN", instance.cc:971).  So the sentinel is the **other**
   edge: `payload.unk == UINT32_MAX` means function-origin.  Attribute
   tables hold one row per referenced ident (tiny), so a real index never
   approaches `UINT32_MAX`.  Non-invasive: no change to the existing
   0-based interning or to the decode at `instance.cc:309`; only
   `ReturnUnknown` writes the reserved value and the consumer compares
   against it.  Define it once as a named constant (e.g.
   `kFunctionUnknownSentinel = 0xFFFF'FFFFu`) shared by the encoder and
   the consumer.  (Rejected: shifting real ids to 1..N to free `0` —
   invasive across the intern site, the decode, the `return 0u` fallback,
   and `cel_host.cc:1554`, for no gain.)
   **Consumer detection API — RESOLVED (owner, 2026-05-26): expose the
   constant.**  Publish `kFunctionUnknownSentinel` (the `0xFFFF'FFFF`
   above) as public API; a consumer tests
   `v.IsUnknown() && *v.UnknownAttribute() == cel::kFunctionUnknownSentinel`.
   No dedicated `Value` predicate — smaller surface, and the comparison
   reads clearly at the call site.  (`UnknownAttribute()` already returns
   the raw id, so this needs no new accessor — just the exported
   constant.)
   **Implementation-time check (not a design decision):** the sentinel
   must survive the runtime's unknown-merge / short-circuit paths
   unchanged — a merge of two unknowns must not turn a function-origin
   sentinel into a bogus attribute id.  Confirm against the 3VL helpers
   (`cel_unknown_merge` etc.) when wiring Layer 0; pin with a test.
