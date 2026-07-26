# Evaluator design — Plan, then Eval

How a compiled `Program` becomes a live `Instance`, and how an `Activation` becomes a `Value` (`eval/`). Byte-level wire facts are [`03-abi-and-memory.md`](03-abi-and-memory.md); system context is [`00-architecture.md`](00-architecture.md).

## 1. The shape of the thing

A `Program` is bytes — wasm plus an embedded `cel.abi` descriptor. The evaluator turns those bytes into answers in two phases with very different costs:

- **Plan** (once per Program): wasmtime JITs the bytes to native code; instantiate the runtime, wire host functions, set up memory. Expensive. Returns an `Instance`.
- **Eval** (cheap, repeated): write the `Activation`'s values into the instance's memory, call `$eval`, decode the result.

![Plan once, Eval per request](diagrams/plan-eval-light.svg#only-light)
![Plan once, Eval per request](diagrams/plan-eval-dark.svg#only-dark)

| Role | What it is | Lifetime |
|---|---|---|
| **`Compiler`** | produces the `Program` (pure bytes) | compile-time only; no wasmtime dependency |
| **`Engine`** | process-shared machinery: one `wasm_engine_t` + the parsed `cel_runtime.wasm` | built once, shared, thread-safe |
| **`Plan`** | the link step (`Engine::Plan(program) → Instance`) | called many times, concurrent-safe |
| **`Instance`** | one live evaluator: store, linker, instances, memory, `$eval`, decoded ABI, host env | thread-owned; outlives the Engine |

The whole arc is two call sites — `Plan` once, `Eval` in the loop:

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

(Condensed from `examples/02_variables.cc`; error handling elided. The `Instance` is reused across `Eval`s — its arena resets at the top of each call.)

Ownership and threading in three facts: the Engine is built once and shared (it holds the parsed runtime module behind a `shared_ptr`, making Plan ~34× cheaper than re-parsing per call); an `Instance` keeps a reference to that shared state, so it outlives the Engine handle that made it; registration is single-threaded (configure, *then* share), `Plan` is concurrent-safe, each `Instance` is owned by one thread.

## 2. Plan — turn bytes into a callable thing

```
  Plan(program):

   1. decode cel.abi  ──────────────►  host env: field-refs, attributes,
      │                                resolved proto descriptors
      ├─ check ABI version  ──────────► mismatch → FailedPrecondition
      ├─ verify required_functions ───► a custom fn the program calls is
      │   (existence + exact signature   missing/mismatched in the
      │    per row; §9)                  registry → FailedPrecondition
      └─ check slot extents ≤ 8 KiB ──► out of window → InvalidArgument

   2. fresh store + linker;  JIT the expr module  (before instantiating,
      │                                            so imports can be read)
      ▼
   3. link mode?  ── module imports "cel"? ──┬─ yes → DYNAMIC: instantiate
      │  (the cel.abi label is only a            │        cel_runtime.wasm
      │   tripwire — the imports decide)         └─ no  → STATIC: runtime is
      ▼                                                   already in the module
   4. bind embedder extensions: custom modules, cel_fn.* host callbacks,
      │            the REQUIRED plugins only               (§3, §4, §9)
      ▼
   5. instantiate the expr module;  pull out  $eval
      ▼
   6. clone the runtime's shared memory onto the Instance;  arena_init
      ▼
                                                            ►  Instance
```

The two step-1 gates fail loudly once, at Plan, instead of cryptically at every Eval:

- **ABI-version check** — a Program/engine runtime-ABI mismatch is a `FailedPrecondition` naming both versions, not wasmtime's opaque type-mismatch trap at the first call.
- **Slot-extents gate** — reject any Program whose ABI declares a variable slot past the 8 KiB reserved window. The compiler never emits one (`01-compiler.md` §6.4); a Program claiming one is corrupt, and honoring it would let the marshal (§6) overwrite the runtime's memory.

Step 3 is the key subtlety: **link mode is decided by what the module imports, not by the ABI label.** Imports from the `"cel"` namespace → dynamic; none → static. The label is only a tripwire — disagreement with the imports rejects the Program as mislabeled.

## 3. Registration — teaching the Engine about host code

Before Plan, the embedder registers custom functions on the Engine. Not thread-safe: configure first, share second. The recommended surface is `BindFunction` — describe the function in the same `.celfn` IDL the compiler reads, hand over a typed lambda:

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

The lambda's parameter types are checked positionally against the declared CEL types at registration, and it registers under the *synthesized* overload-id — one source, no drift. (From `examples/04_host_functions.cc`.)

The other surfaces: `AddTypedFunction(id, lambda)` is the typed adapter without a decl; `AddFunction(id, num_args, callback)` is the raw layer underneath (§4, L1); `AddModule(alias, bytes)` binds a foreign wasm module; `Use(plugin)` registers a self-describing sandboxed plugin, statically checking — against the parsed component, nothing instantiated — that it exports its declared interface and every declared function (§9); `AddPlugin(bytes, lib)` is the explicit-decls escape hatch (no static export check; resolution is Plan-time). All validate what they can at registration and defer store-dependent checks to Plan. The residual call-time surface is the export's WIT-level *FuncType*: no path compares it to the decl, so a wrong-shaped export on a hand-built plugin traps when first called (unreachable for macro-built plugins, whose WIT and decls derive from one `.idl`).

## 4. The host-call stack — L0, L1, L2

When the expression calls an `@host` function, three layers sit between the raw wasm call and the typed lambda:

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

You normally live at **L2**. For raw slot access — variadic shapes, lazy list/map views, returning an unknown — drop to **L1**:

```cpp
engine->AddFunction("double_int", /*num_args=*/2,
    [](celwasm::HostCallContext& ctx) -> absl::Status {
      auto x = ctx.ArgInt(0);                 // StatusOr<int64_t>; kind-checked
      if (!x.ok()) return x.status();
      return ctx.ReturnInt(*x * 2);           // returns absl::Status
    });
```

`num_args` counts the out-slot: params + 1. L1's accessors are kind- and bounds-checked (`ArgInt` on a string slot → `InvalidArgument`; out-of-range index → `OutOfRange`); its return setters route through the same encoder the built-in trampolines use, so output is byte-identical to a built-in's. **L0 never calls you if an argument is an error or unknown** — it absorbs and returns, matching CEL's strict-function dispatch (§8).

## 5. The cel_host surface — built-in operations

The built-ins the compiler emits calls to — field access, map/list ops, proto construction, well-known-type handling — live behind `cel_host.*` imports, in three layers so the semantics are testable without any wasm:

- **Layer 1 — backings.** Pure value semantics, no wasm types: a non-owning view over a proto `Message*`, an owning mutable proto (the only thing field-writes accept), vector-backed maps/lists, reflection views over a single proto field.
- **Layer 2 — trampoline bodies.** The operation logic, written against three abstractions: a `MemoryView` (read/write CelValue slots), an `ExternrefTable` (three independent handle namespaces — message, map, list — reset between Evals), an `ArenaAllocator` (bump-allocates string/bytes payloads).
- **Layer 3 — wasmtime glue.** Production implementations of those three, plus one `extern "C"` trampoline per import.

Registration is **bijection-checked**: the trampoline table and the ABI catalogue must list exactly the same 20 `cel_host` imports, asserted at startup.

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

The dividing line across every trampoline is §8's rule: a non-OK **Status** is infrastructure failure (→ trap); a langdef-level **error** is a `CEL_ERROR` *value* written to the out slot. Notable patterns: aggregate ops absorb 3VL first, then guard operand kinds with type-mismatch *values*; comprehension snapshots materialize a host aggregate into the arena, degrading to empty iteration on non-host input; single trampolines collapse whole overload families (one serves all ten with-timezone timestamp accessors via a kind argument).

## 6. Marshal — getting variables into memory

`Eval(activation)` writes every ABI-declared variable into its pre-assigned workspace slot before calling `$eval`:

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

A missing binding is `FailedPrecondition`; a declared-type vs bound-kind mismatch is `InvalidArgument` — with three deliberate coercions the checker allows (`Value::Null()` into any scalar slot; a WKT wrapper peeled to its scalar; a Timestamp/Duration peeled to seconds+nanos).

The easy-to-miss subtlety: **string/bytes payloads do not go in the arena.** They go in a separate per-Instance buffer, because `$eval`'s prelude calls `arena_reset` — which would wipe an arena-resident payload before the expression read it. A pre-pass sums the bytes so the buffer grows *once*, before any encoder caches a memory pointer.

**`PartialEval(activation, unknowns)`** is the same marshal with unknown-attribute patterns active:

```cpp
auto pattern  = celwasm::AttributePattern::Parse("purchase_total");
celwasm::AttributePattern unknowns[] = {*pattern};
auto result   = instance->PartialEval(activation, unknowns);
if (result->IsUnknown()) { /* purchase_total wasn't known this call */ }
```

A variable matching a pattern gets `CEL_UNKNOWN` — **whether or not it's bound** (the pattern wins) — with the unknown descriptor minted in the same outside-the-arena buffer. Patterns are cleared on every exit path, so a later plain `Eval` never sees stale partial-eval state.

## 7. Eval and decode — the cheap path

Zero-arg `Eval()` calls `$eval`, which returns one i32: the offset of the result CelValue. The decoder reads it into a host-owned `Value`. Aggregates are **deep-copied** out — their backings are per-Eval (the handle table and arena reset on the next call), so a decoded `Value` must own its state.

```cpp
absl::StatusOr<celwasm::Value> r = instance->Eval(act);
if (!r.ok())            { /* a TRAP — infra failure or $eval fault (§8) */ }
else if (r->IsError())  { /* a CEL error VALUE — e.g. 1/0, overflow (§8) */ }
else if (r->IsUnknown()){ /* partial-eval unknown */ }
else                    { bool b = r->AsBool().value();  /* a real value */ }
```

That four-way branch is the contract every embedder writes. The gap between a non-OK `Status` and an OK `Value` that `IsError()` is the subject of §8.

On the `Value` model: equality is `StructurallyEquals` — scalars by value, strings/bytes by bytes, aggregates by backing-pointer identity — deliberately not spec equality. `Value::Kind` matches the wire numbering only for the first nine kinds and diverges above on purpose; conversion is always an explicit switch, never a cast.

## 8. Errors and unknowns — three paths, not two

An expression that "goes wrong" reaches the embedder one of **three** ways:

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

The rule, stated once: **a spec-level error is a value; a non-OK `Status` means a trap.** Every Layer-2 trampoline returns non-OK Status only for infrastructure failure; every langdef error is a `CEL_ERROR` CelValue. `StatusToTrap` / `TrapFromStatus` are the only crossings.

**Why `1/0` is a value, not a trap.** wasm's `i32.div_s` hardware-traps on a zero divisor. Letting that happen would abort the whole Eval — wrong, because in CEL a division error is a *value* that propagates and can be absorbed: `false && (1/0 == 1)` must evaluate to `false`. So the kernel guards every trap-prone operation:

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

Modulo-by-zero, integer overflow, and out-of-range conversions are guarded the same way. The runtime doesn't even link compiler-rt, so a stray division can't pull in a trapping helper. The embedder almost never sees a trap from arithmetic — they see `result->IsError()` with a code.

Two specifics on the value path:

- **Error *messages* are dropped at the host→wasm boundary.** The wire carries only the error *code*; message and source location are discarded, and read-back synthesizes a generic message from the code. Known limitation (cleanup-backlog #31), not a deep invariant. It is also why a host callback's `InvalidArgument("boom")` reaches the embedder as a generic `Internal "Eval trapped"` — the trap path loses the code (§10).
- **3VL precedence for strict ops is "error dominates unknown," regardless of operand order** — oracle-confirmed against cel-cpp. Both the kernel and the trampolines implement it.

## 9. Plugins — sandboxed custom functions

The plugin path makes a plugin's exports callable as CEL functions, with the plugin in its **own** linear memory (a plugin is packaged as a Component-Model component). A plugin registers via `Engine::Use(plugin)` — the `Plugin` noun carries bytes, declarations (parsed from the artifact's embedded `cel.fns` section), and a content hash; `05-custom-functions.md` §5.0 owns that surface.

Plan runs the **required-function check** first (`eval/internal/required_fn_check`): every `required_functions` row the Program's `cel.abi` carries — `@host` and `@plugin` alike — must exist in the registry with an exactly matching signature (receiver-ness, arity, each param, return type; protos by FQN; host functions registered through raw `AddFunction`/`AddTypedFunction` are checked arity-only, since no declared types were captured). Failures are `FailedPrecondition` naming the function, the signature, and — for a plugin mismatch — the registered plugin's content hash. Then Plan instantiates **only the plugins owning at least one required row** into the per-Plan store — register ten plugins and a program calling one instantiates one — and binds each of a selected plugin's decls as a `cel_fn.<overload_id>` trampoline; the wasm import shape is identical to an `@host` decl, only the callback body differs. A legacy Program with no `required_functions` table gets the pre-verification behavior: no check, instantiate-all.

The trampoline 3VL-absorbs (same contract as §4), lifts each argument CelValue into a plugin-side value per the decl's type witness, calls, and lowers the single result back. Type mapping: strings are length-based (NUL-safe), bytes cross as `list<u8>`, durations/timestamps as `record{seconds, nanos}`, maps as `list<tuple<K,V>>`, and **protos cross as serialized bytes** (never a handle), re-materialized from the descriptor pool on the way back. `optional<T>` is rejected both directions. Unsatisfied wasi-preview2 imports are **trap-stubbed** so a runaway libc++ call traps naming the missing interface — except `wasi:random/random`, which returns deterministic bytes (the libc++ runtime reads it during static init and there is no per-store WASI context to wire instead).

## 10. Known gaps and future work

- **Host-callback status codes are lost.** A callback returning `InvalidArgument("boom")` surfaces as a generic `Internal "Eval trapped"` (cleanup-backlog #31, same root as the §8 error-message loss). Fix as one contract.
- **No WIT-level FuncType check on plugin exports** — decl↔declaration signature agreement is verified at Plan (§9) and export *existence* at `Use`, but the export's actual wasm-level shape is never compared to the decl; a hand-built plugin with a wrong-shaped export still traps at call time.
- **Per-Plan expr re-parse** — the expression module is re-parsed every Plan; a cache seam exists but is unexploited.
- **Cross-origin list concat** poisons host-involved pairs with a type-mismatch; the planned fix is to materialize into the arena.
- Smaller follow-ons: dynamic-schema descriptor pools, field-descriptor caching, the kType plugin lower stub.

Unverified questions (the exact plugin-arity trap site, zero-arg `Eval()` handle-table growth, the host-callback status-code contract) live in [`design/notes/`](https://github.com/augustinemathew/cel-wasm/tree/master/doc/design/notes).
