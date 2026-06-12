# Evaluator design — Plan, then Eval

Status: current — authored 2026-06-10, rewritten for clarity 2026-06-11.
This doc is `eval/`: how a compiled `Program` becomes a live `Instance`,
and how an `Activation` becomes a `Value`. Byte-level wire facts (the
CelValue layout, the kind table) belong to
[`03-abi-and-memory.md`](03-abi-and-memory.md); system context is
[`00-architecture.md`](00-architecture.md).

## 1. The shape of the thing

A `Program` is just bytes — wasm plus an embedded `cel.abi` descriptor.
The evaluator turns those bytes into answers in **two phases with very
different costs**:

- **Plan** (once per Program): hand the bytes to wasmtime, which JITs
  them to native machine code; instantiate the runtime, wire up the host
  functions, set up memory. Expensive. Returns an `Instance`.
- **Eval** (cheap, repeated): take an `Activation` (the bindings), write
  those values into the instance's memory, call the module's `$eval`
  function, decode the `Value` it points at.

That split *is* the design — you pay the JIT and link cost once and
amortize it across as many `Eval`s as you like:

```
  Compiler ──Compile(source)──►  Program            pure bytes (wasm + cel.abi);
                                  │                  no wasmtime dependency
                                  │
   ═══════════════════════════════╪══════════════  Plan: ONCE, expensive
                                  ▼                  (Cranelift JIT + link)
  Engine ────Plan(program)────►  Instance           JIT'd native code, runtime
                                  │                  wired, host fns bound,
                                  │                  memory ready
   ═══════════════════════════════╪══════════════  Eval: MANY, cheap
                                  ▼
  Activation ───Eval(act)─────►  Value               write vars → call $eval →
                                                     decode result
```

| Role | What it is | Lifetime |
|---|---|---|
| **`Compiler`** | produces the `Program` (pure bytes) | compile-time only; no wasmtime dependency |
| **`Engine`** | process-shared machinery: one `wasm_engine_t` + the parsed `cel_runtime.wasm` | built once, shared, thread-safe |
| **`Plan`** | the link step (`Engine::Plan(program) → Instance`) | called many times, concurrent-safe |
| **`Instance`** | one live evaluator: store, linker, instances, memory, `$eval`, decoded ABI, host env | thread-owned; outlives the Engine |

The whole arc is about a dozen lines of embedder code — the two phases
are literally two call sites (`Plan` once, `Eval` in the loop):

```cpp
// ── once: compile, then Plan ────────────────────────────────────────
auto builder = celwasm::Compiler::NewBuilder();
builder.DeclareVariable("age", celwasm::CelType::Int())
       .DeclareVariable("country", celwasm::CelType::String());
auto compiler = std::move(builder).Build();
auto program  = compiler->Compile("age >= 18 && country in ['US', 'CA']");

auto engine   = celwasm::Engine::NewBuilder().Build();   // process-shared
auto instance = engine->Plan(*program);                  // the expensive step

// ── many: bind values, Eval ─────────────────────────────────────────
celwasm::Activation act;
act.Bind("age", celwasm::Value::Int(25))
   .Bind("country", celwasm::Value::String("US"));
bool allowed = instance->Eval(act)->AsBool().value();    // => true
```

(Condensed from `examples/02_variables.cc`; error handling elided. The
`Instance` is reused across `Eval`s — its arena resets at the top of
each call.)

**Ownership and threading, in three facts:** the Engine is built once and
**shared** (it holds the parsed runtime module behind a `shared_ptr`, so
Plan is ~34× cheaper than re-parsing per call); an `Instance` keeps a
reference to that shared state, so it **outlives** the Engine handle that
made it; and the threading contract is — registration is
single-threaded (configure, *then* share), `Plan` is concurrent-safe
(fresh store/linker/memory per call), each `Instance` is owned by one
thread.

## 2. Plan — turn bytes into a callable thing

`Engine::Plan` reads the ABI and builds the host environment, JITs the
module, decides whether the runtime is bundled or separate, then
instantiates and grabs `$eval`:

```
  Plan(program):

   1. decode cel.abi  ──────────────►  host env: field-refs, attributes,
      │                                resolved proto descriptors
      ├─ check ABI version  ──────────► mismatch → FailedPrecondition
      └─ check slot extents ≤ 8 KiB ──► out of window → InvalidArgument

   2. fresh store + linker;  JIT the expr module  (before instantiating,
      │                                            so imports can be read)
      ▼
   3. link mode?  ── module imports "cel"? ──┬─ yes → DYNAMIC: instantiate
      │  (the cel.abi label is only a            │        cel_runtime.wasm
      │   tripwire — the imports decide)         └─ no  → STATIC: runtime is
      ▼                                                   already in the module
   4. bind embedder extensions: custom modules, cel_fn.* host callbacks,
      │                          components                        (§3, §4, §9)
      ▼
   5. instantiate the expr module;  pull out  $eval
      ▼
   6. clone the runtime's shared memory onto the Instance;  arena_init
      ▼
                                                            ►  Instance
```

The two **gates** in step 1 exist for one reason — *fail loudly once, at
Plan, instead of cryptically at every Eval*:

- **ABI-version check** — the Program's runtime-ABI version must match the
  engine's; a mismatch is a `FailedPrecondition` naming both versions,
  far better than wasmtime's opaque type-mismatch trap at the first call.
- **Slot-extents gate** — reject any Program whose ABI declares a variable
  slot reaching past the 8 KiB reserved window. The compiler never emits
  one (it validates the same bound before serializing — `01-compiler.md`
  §6.4), so a Program that claims one is corrupt, and honoring it would
  let the marshal (§6) write over the runtime's own memory.

The most important subtlety is step 3: **the link mode is decided by what
the module imports, not by the ABI label.** If the module imports
anything from the `"cel"` namespace it's dynamic; otherwise it's static.
The label is *only* a tripwire — if it disagrees with the imports, Plan
rejects the Program as mislabeled. (Why static is the default is
`00-architecture.md` §3.)

## 3. Registration — teaching the Engine about host code

Before Plan, the embedder registers any custom functions. This happens
on the Engine and is **not thread-safe** — configure first, share second.
The recommended surface is `BindFunction`: you describe the function in
the same `.celfn` IDL the compiler reads, and hand over a typed lambda.

```cpp
// The compiler sees the decl; the engine binds the impl — from ONE string,
// so the import name and the binding can't diverge.
const char* kDecl = "int @host.discount_pct(string tier);";

builder.AddFunction(kDecl);              // compiler side: declare it
engine->BindFunction(kDecl,             // engine side: implement it
    [](absl::string_view tier) -> absl::StatusOr<int64_t> {
      return tier == "gold" ? 20 : 10;
    });
```

The lambda's parameter types are checked positionally against the
declared CEL types at registration, and it registers under the
*synthesized* overload-id — so the engine-side binding and the
compiler-side import name are derived from one source and cannot drift.
(From `examples/04_host_functions.cc`.)

The other surfaces, briefly: `AddTypedFunction(id, lambda)` is the same
typed adapter without a decl; `AddFunction(id, num_args, callback)` is the
raw layer underneath (§4, L1); `AddModule(alias, bytes)` binds a foreign
wasm module under an alias; `AddComponent(bytes, lib)` registers a
Component-Model component (§9). All validate what they can at
registration and defer store-dependent checks to Plan — except component
arity, which is checked at **call time** (a wrong-shaped export traps when
first called, not when planned).

## 4. The host-call stack — L0, L1, L2

When the expression calls one of your `@host` functions, three layers sit
between the raw wasm call and your typed lambda, each raising the
abstraction one notch:

```
  wasm:   call $cel_fn.discount_pct_string(out_slot, arg_slot)
            │  args are i32 slot offsets; no wasm result
            ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │ L0  trampoline                                                     │
  │     • absorb 3VL: any CEL_ERROR / CEL_UNKNOWN arg → copy to out,   │
  │       skip the call   (error wins over unknown — §8)               │
  │     • non-OK status from above → wasm trap                         │
  ├──────────────────────────────────────────────────────────────────┤
  │ L1  HostCallContext   typed, bounds-checked slot access            │
  │       ctx.ArgString(0) -> StatusOr<string_view>                    │
  │       ctx.ReturnInt(x) -> Status                                   │
  ├──────────────────────────────────────────────────────────────────┤
  │ L2  BindTypedFunction   trait adapter; canonical types ONLY        │
  │       (a wrong C++ type is a COMPILE error naming the type)        │
  └──────────────────────────────────────────────────────────────────┘
            │
  your C++:   [](absl::string_view tier) -> absl::StatusOr<int64_t> { … }
```

You normally live at **L2** (the typed lambda above). When you need raw
slot access — variadic shapes, lazy list/map views, returning an
unknown — drop to **L1** and write the callback by hand:

```cpp
engine->AddFunction("double_int", /*num_args=*/2,
    [](celwasm::HostCallContext& ctx) -> absl::Status {
      auto x = ctx.ArgInt(0);                 // StatusOr<int64_t>; kind-checked
      if (!x.ok()) return x.status();
      return ctx.ReturnInt(*x * 2);           // returns absl::Status
    });
```

`num_args` counts the out-slot, so it's params + 1. L1's accessors are
kind- and bounds-checked (`ArgInt` on a string slot is an
`InvalidArgument`; an out-of-range index is `OutOfRange`), and its return
setters route through the *same* encoder the built-in trampolines use, so
your output is byte-identical to a built-in's. **L0 never even calls you
if an argument is an error or unknown** — it absorbs that and returns,
matching CEL's strict-function dispatch (§8).

## 5. The cel_host surface — built-in operations

The built-ins the compiler emits calls to — field access, map/list ops,
proto construction, well-known-type handling — live behind `cel_host.*`
imports, in three layers so the *semantics* are testable without any
wasm:

- **Layer 1 — backings.** Pure value semantics, no wasm types: a
  non-owning view over a proto `Message*`, an owning mutable proto (the
  only thing field-writes accept), vector-backed maps/lists, reflection
  views over a single proto field.
- **Layer 2 — trampoline bodies.** The operation logic, written against
  three abstractions: a `MemoryView` (read/write CelValue slots), an
  `ExternrefTable` (three independent handle namespaces — message, map,
  list — reset between Evals), an `ArenaAllocator` (bump-allocates
  string/bytes payloads).
- **Layer 3 — wasmtime glue.** Production implementations of those three,
  plus one `extern "C"` trampoline per import.

Registration is **bijection-checked**: the trampoline table and the ABI
catalogue must list exactly the same 20 `cel_host` imports, asserted at
startup, so they can't drift.

One call, `cel_get_field(out, msg, field_ref, attr)`, end to end:

```
  read operand (BEFORE writing out — aliasing out==msg is legal)
    └─ unknown/error? ─────────► copy to out, return
    └─ not a message? ─────────► write CEL_ERROR(type_mismatch), return
    └─ attr matches an unknown-pattern? ─► write CEL_UNKNOWN, return
  resolve field descriptor (by number, then name)
    └─ read field → apply proto presence + WKT peel
       (Any → wrapper → scalar; Timestamp/Duration → seconds+nanos)
  write result back (aggregates intern a handle; scalars inline;
                     strings arena-copied)
```

The dividing line throughout — and across every trampoline — is the one
rule from §8: a non-OK **Status** is infrastructure failure (→ trap); a
langdef-level **error** is a `CEL_ERROR` *value* written to the out slot.
A few patterns worth knowing: aggregate ops absorb 3VL first then guard
operand kinds with type-mismatch *values* (not traps); comprehension
snapshots materialize a host aggregate into the arena, degrading to empty
iteration on non-host input; and single trampolines collapse whole
overload families (one serves all ten with-timezone timestamp accessors
via a kind argument).

## 6. Marshal — getting variables into memory

`Eval(activation)` writes every ABI-declared variable into its
pre-assigned workspace slot before calling `$eval`:

```
  Activation{ age: 25, country: "US" }
        │  marshal
        ▼
  linear memory — the low 8 KiB window the expression owns:

    0      16        rodata_end     workspace                       8192
    ┌──────┬───────────┬──────────────┬──────────────┬───────────────┐
    │ null │  rodata    │ slot: age    │ slot: country│  scratch …    │
    │ sentl│  consts    │  CEL_INT 25  │  CEL_STRING   │               │
    └──────┴───────────┴──────────────┴───────┬──────┴───────────────┘
                                              │ payload offset points to ↓
                                     ┌─────────────────────────────────┐
                                     │ activation buffer  "US"          │
                                     │ (malloc'd, OUTSIDE the arena —   │
                                     │  see why below)                  │
                                     └─────────────────────────────────┘
        │  call $eval   →   result CelValue offset
        ▼
   decode → Value
```

A missing binding is a `FailedPrecondition`; a declared-type vs bound-kind
mismatch is an `InvalidArgument` — with three deliberate coercions the
checker allows (`Value::Null()` into any scalar slot; a WKT wrapper
message peeled to its scalar; a Timestamp/Duration peeled to
seconds+nanos).

The one easy-to-miss subtlety, drawn above: **string/bytes payloads do
not go in the arena.** They go in a separate per-Instance buffer, because
`$eval`'s prelude calls `arena_reset` — which would wipe an arena-resident
payload before the expression body ever read it. A pre-pass sums the bytes
so the buffer grows *once*, before any encoder caches a memory pointer.

**`PartialEval(activation, unknowns)`** is the same marshal with
unknown-attribute patterns active:

```cpp
auto pattern  = celwasm::AttributePattern::Parse("purchase_total");
celwasm::AttributePattern unknowns[] = {*pattern};
auto result   = instance->PartialEval(activation, unknowns);
if (result->IsUnknown()) { /* purchase_total wasn't known this call */ }
```

A variable whose attribute matches a pattern gets a `CEL_UNKNOWN` —
**whether or not it's bound** (the pattern wins over a present value) —
and the unknown descriptor is minted in that same outside-the-arena buffer
for the same reason. Patterns are cleared on every exit path, so a later
plain `Eval` can never see stale partial-eval state.

## 7. Eval and decode — the cheap path

Zero-arg `Eval()` calls `$eval`, which returns one i32: the offset of the
result CelValue. The decoder reads it into a host-owned `Value`.
Aggregates are **deep-copied** out, because their backings are per-Eval —
the handle table and arena reset on the next call, so a decoded `Value`
must own its state to outlive that.

```cpp
absl::StatusOr<celwasm::Value> r = instance->Eval(act);
if (!r.ok())            { /* a TRAP — infra failure or $eval fault (§8) */ }
else if (r->IsError())  { /* a CEL error VALUE — e.g. 1/0, overflow (§8) */ }
else if (r->IsUnknown()){ /* partial-eval unknown */ }
else                    { bool b = r->AsBool().value();  /* a real value */ }
```

That four-way branch *is* the contract every embedder writes, and the gap
between line 2 and line 3 — a non-OK `Status` vs an OK `Value` that
`IsError()` — is the whole subject of §8.

(On the `Value` model itself: equality is `StructurallyEquals` — scalars
by value, strings/bytes by bytes, aggregates by backing-pointer identity
— and is **deliberately not spec equality**. `Value::Kind` matches the
wire numbering only for the first nine kinds and diverges above on
purpose, so conversion is always an explicit switch, never a cast.)

## 8. Errors and unknowns — three paths, not two

This is the part that trips people up, so here it is as a decision tree.
An expression that "goes wrong" reaches the embedder one of **three**
ways, and they are not interchangeable:

```
  something goes wrong during Eval
    │
    ├─ it's a SPEC error (1/0, overflow, type mismatch, no such key…)
    │     → kernel/trampoline writes a CEL_ERROR *value* to the out slot
    │     → Eval returns OK;  result->IsError() == true
    │     → it PROPAGATES like a value:  false && (1/0==1)  ==  false
    │
    ├─ it's an INFRASTRUCTURE failure (bad handle, arena OOM, missing
    │  reflection, a host callback returning non-OK Status)
    │     → StatusToTrap / TrapFromStatus  → wasm trap
    │     → Eval returns absl::Internal "Eval trapped: <msg>"
    │
    └─ it's a genuine WASM fault inside $eval (a real trap)
          → WasmTrapToStatus  → Eval returns absl::Internal "Eval trapped"
```

The rule, stated once: **a spec-level error is a value; a non-OK `Status`
means a trap.** Every Layer-2 trampoline returns non-OK Status *only* for
infrastructure failure; every langdef error is a `CEL_ERROR` CelValue.
`StatusToTrap` / `TrapFromStatus` are the only places the two cross.

**Why `1/0` is a value, not a trap — the design decision worth
understanding.** wasm's `i32.div_s` *hardware-traps* on a zero divisor.
If cel-wasm let that happen, `1/0` would abort the entire Eval — and that
would be wrong, because in CEL a division error is a *value* that
propagates and can be absorbed. `false && (1/0 == 1)` must evaluate to
`false`; `[1/0].exists(x, x == 2)` must propagate the error, not crash.
So the kernel **guards every trap-prone operation** and writes an error
value instead:

```c
// runtime/cel_arith.c — the kernel deliberately avoids the wasm trap.
void cel_int_div_at_vv(uint32_t out, uint32_t a, uint32_t b) {
  ...
  if (b->payload.i == 0) {
    poison(out, CEL_ERR_DIVIDE_BY_ZERO);   // a VALUE, not a trap
    return;
  }
  // (INT64_MIN / -1 overflow is guarded the same way.)
```

The same applies to modulo-by-zero, integer overflow, and out-of-range
conversions: all are guarded into `CEL_ERROR` values. The runtime doesn't
even link compiler-rt, precisely so a stray division can't pull in a
trapping helper. So the embedder almost never sees a trap from arithmetic
— they see `result->IsError()` with a code.

Two specifics on the value path:

- **Error *messages* are dropped at the host→wasm boundary.** The wire
  carries only the error *code*; the message and source location are
  discarded, and read-back synthesizes a generic message from the code.
  Known limitation (cleanup-backlog #31), not a deep invariant — the wire
  *could* carry the message; it just doesn't yet. This is also why a host
  callback's `InvalidArgument("boom")` reaches the embedder as a generic
  `Internal "Eval trapped"` — the trap path loses the code (§10).
- **3VL precedence is "error dominates unknown," regardless of operand
  order** — oracle-confirmed against cel-cpp, which scans arguments for an
  error before merging unknowns. Both the kernel and the trampolines
  implement this.

## 9. Components — sandboxed custom functions

The component path makes a Component-Model component's exports callable as
CEL functions, with the component running in its **own** linear memory
(the strong-isolation story is the security model's; the mechanics are
here). At Plan, each component is instantiated into the per-Plan store and
each foreign decl is bound as a `cel_fn.<overload_id>` trampoline — the
wasm import shape is identical to an `@host` decl; only the callback body
differs.

The trampoline 3VL-absorbs (same contract as §4), lifts each argument
CelValue into a component value per the decl's type witness, calls, and
lowers the single result back. The type mapping is the interesting part:
strings are length-based (NUL-safe), bytes cross as `list<u8>`,
durations/timestamps as a `record{seconds, nanos}`, maps as a
`list<tuple<K,V>>`, and **protos cross as serialized bytes** (never a
handle), re-materialized from the descriptor pool on the way back.
`optional<T>` is rejected both directions. Unsatisfied wasi-preview2
imports are **trap-stubbed** so a runaway libc++ call traps naming the
missing interface, with one deliberate exception: `wasi:random/random`
returns deterministic bytes, because the libc++ runtime reads it during
static init and there's no per-store WASI context to wire instead.

## 10. Known gaps and future work

- **Host-callback status codes are lost.** A callback returning, say,
  `InvalidArgument("boom")` surfaces as a generic `Internal "Eval
  trapped"` — the code and message don't survive the trap path
  (cleanup-backlog #31, same root as the error-message loss in §8). These
  should be fixed as one contract.
- **No Plan-time component signature check** — a wrong-arity component
  export fails at call time, not Plan; whether to add a Plan-time
  `FuncType` comparison (and fix the stale header claiming one) is open.
- **Per-Plan expr re-parse** — the expression module is re-parsed every
  Plan; a cache seam exists but is unexploited.
- **Cross-origin list concat** poisons host-involved pairs with a
  type-mismatch; the planned fix is to materialize into the arena instead.
- Smaller follow-ons: dynamic-schema descriptor pools, field-descriptor
  caching, the kType component lower stub.

The unverified questions catalogued during the notes pass (the exact
component-arity trap site, zero-arg `Eval()` handle-table growth, the
host-callback status-code contract) live in the
[`design/notes/`](https://github.com/augustinemathew/cel-spec-wasm/tree/master/doc/design/notes)
working material rather than inline, so they don't clutter the design.

## History

This doc supersedes the evaluator-surface content of the milestone-era
plans under `doc/implementation-plan/rewrite/` — `cel-host-surface.md`
(surface sections; its wire sections went to `03-abi-and-memory.md`),
`m21-host-call-adapter.md`, the eval half of
`two-phase-runtime-isolation.md`, and the eval sections of
`m24-foreign-fn-component-backend.md` — each carrying an archive banner
pointing here. Where this doc contradicts a stale public-header comment
(`engine.h`'s AddComponent/memory text, `value.h`'s builder and numbering
claims, `instance.h`'s decode claims), this doc is the corrected record.
