# M13 — Custom-function probes

Status: **in flight — drafted 2026-05-21.**  Companion to
[m13-custom-fns.md](m13-custom-fns.md); this doc tracks the
probe sequence that validates the design's load-bearing claims
**before** any production code is committed.

## Why probes

The M13 design has three architecturally-novel claims:

  1. Two user wasms can share a host-owned `cel.memory` and exchange
     24-byte CelValues over slot offsets.
  2. The wasmtime Linker can wire one module's export as another
     module's import under an arbitrary `<alias>.<name>` namespace
     (e.g. `rules.allow_message_acme_User_string`).
  3. A wasm module compiled from a language *other* than C/C++ —
     specifically Go via TinyGo — can be the source of that export,
     interoperating with the host-owned shared memory and the
     CelValue wire layout.

Without probes that exercise these end-to-end, the design is
fiction.  Per CLAUDE.md's WAT-first rule we build the wasm shape
before the codegen C++ — and per `per-component-test-coverage.md`
discipline we land the probe with the design, not after.  Each
probe is a small standalone validator that proves one specific
claim and CAN be deleted once production code subsumes it
(the WAT lives on under `wat/`; the probe binary expires).

## Probe sequence

### Probe 1 — `<alias>.<name>` wasm-link contract (✅ shipped 2026-05-21)

**What it proves**

  - Two hand-rolled user wasms share a host-owned 2-page memory.
  - The wasmtime Linker resolves a `rules.allow_message_acme_User_string`
    import on the caller against an export of the same name on a
    sibling module — the foreign-alias namespace from §3 of the
    main design doc lands at the wasmtime level cleanly.
  - The 24-byte CelValue wire layout from `runtime/cel_data.h`
    is the right ABI surface: the caller pre-stages args + an
    out_slot, the foreign module reads + writes via slot offsets,
    and the host decodes the result cleanly.

**Artifacts**

  - `doc/implementation-plan/rewrite/wat/m13_p1_caller.wat` — what
    celwasmc would emit for `user.allow("/admin")` (hand-rolled).
  - `doc/implementation-plan/rewrite/wat/m13_p1_rules_stub.wat` —
    stand-in for TinyGo / Rust / AS output; hand-rolled stub that
    always writes `true`.
  - `compiler/probes/m13_custom_fns/m13_p1_test.cc` — the host
    harness that allocates `cel.memory`, instantiates both
    modules, wires the link, calls `eval`, and decodes the
    returned bool.

**Run**

```
bazel test //compiler/probes/m13_custom_fns:m13_p1_test --test_output=all
```

Passed in 6ms on 2026-05-21 (apple silicon, wasmtime via the
darwin_arm64 vendor repo).

**What it deliberately does not prove**

  - That a non-C++ toolchain can hit the ABI (that's Probe 2).
  - That arena_alloc is necessary / sufficient for outbound
    string/list/map results (that's a follow-up probe; bool fits
    inline in CelValue.payload so no allocation crosses the
    boundary here).
  - That `Engine::Plan` + `RuntimeBindings::AddModule` correctly
    walk the user module's exports and produce equivalent wiring
    (that's tested at the production-API level, separately).

### Probe 2 — TinyGo-built `rules.wasm` (✅ shipped 2026-05-21)

**What it proves**

  - TinyGo (0.41.1, `-target=wasm-unknown -no-debug`) produces an
    827-byte wasm module that:
       - exports `allow_message_acme_User_string` with the canonical
         `(out_slot, *arg_slots) → void` signature (probe-1 caller
         imports the same name with the same type — wasmtime's
         link succeeds),
       - reads CelValue bytes at the arg slot offsets and writes
         a CelValue at the out slot, all through the 24-byte
         layout from `cel_data.h`,
       - remains drop-in compatible with the Probe 1 caller WAT —
         literally byte-identical to what Probe 1 uses.
  - The cross-language CelValue wire format is real and portable.
    A Go function, compiled to wasm, can be called from a CEL
    expression's wasm with no glue code.

**Artifacts**

  - `compiler/probes/m13_custom_fns/rules/rules.go` — typed
    Go body authoring the foreign export.  Uses `//go:wasmexport`
    to land the canonical export name verbatim.
  - `compiler/probes/m13_custom_fns/rules/go.mod` — minimal
    module file (TinyGo requires it).
  - `compiler/probes/m13_custom_fns/rules/build_rules.sh` —
    rebuild script; runs `tinygo build` + sanity-checks the
    export name.
  - `compiler/probes/m13_custom_fns/rules/rules.wasm` — the
    built artifact, **checked in** so the probe can run without
    TinyGo on CI.
  - `compiler/probes/m13_custom_fns/m13_p2_test.cc` — same
    harness shape as Probe 1 with the stub WAT swapped for the
    Go-built `rules.wasm`.

**Run**

```
bazel test //compiler/probes/m13_custom_fns:m13_p2_test --test_output=all
```

Passed in 8ms on 2026-05-21.

**Architectural realizations from Probe 2**

These are the load-bearing things this probe surfaced that the
M13 design doc's first draft did NOT anticipate.  Worth folding
into `m13-custom-fns.md` §4.5 before Slice A starts.

  - **The foreign module owns memory, not the host.**  TinyGo's
    `wasm-unknown` target defines its own `(memory 2)` and exports
    it; it does NOT accept a host-provided `cel.memory` import.
    Workable wiring: instantiate the foreign module first,
    extract its `memory` export, then bind THAT as `cel.memory`
    for the caller.  In production this means
    `RuntimeBindings::AddModule` for a `Foreign` alias takes the
    instance AND extracts its memory; subsequent imports of
    `cel.memory` route there.  Other wasm-producing toolchains
    (Rust, AssemblyScript) likely have flags to import memory
    instead, but TinyGo doesn't expose that knob today —
    accommodating the foreign-owns-memory shape is the realistic
    v1 default.

  - **TinyGo needs `_initialize` called once before exports.**  At
    the disassembly level: every TinyGo-exported function loads
    a flag at offset 65536 and traps if zero.  `_initialize`
    sets the flag.  This is the WASI-preview1 module
    initialization pattern; TinyGo applies it even on
    `wasm-unknown`.  Production wiring: `Engine::Instantiate`
    must call `_initialize` after instantiating any foreign
    module that exports it.  Treat missing `_initialize` as
    expected (Rust/AS modules may not export one); call it if
    present, no-op if not.

  - **Memory pages allocated at minimum 2.**  TinyGo's default
    is 2 pages (128 KiB) for `wasm-unknown` — matches the M1
    runtime baseline.  No clash for the probe.  If a foreign
    module declares more pages, the cel_runtime + caller's data
    segments still fit; they live at low offsets (0..16384 for
    most of what we emit today).  TinyGo's runtime statics start
    at offset 65536+, leaving 0..65535 free for the
    caller-staged args.  Document this as a soft contract.

  - **The 5-point ABI contract in m13-custom-fns.md §4.5 needs
    relaxation around memory ownership.**  Specifically point 2
    ("Both modules import the same `(memory)` from the engine")
    should become "the engine binds a single shared memory to
    every module that imports `cel.memory`; if the foreign
    module *exports* memory instead (TinyGo's default), that
    becomes the shared memory."  Symmetric, accommodates both
    cases.  Cel_runtime.wasm + celwasmc-emitted exprs continue
    to import; foreign modules may either.

  - **No `cel.arena_alloc` needed for bool/scalar returns.**
    Confirmed by 8ms latency on the round-trip.  Outbound
    strings / lists / maps will require arena binding (a
    follow-up probe); for primitives this is allocation-free.

**What it deliberately does not prove**

  - That arena_alloc routes correctly when the foreign module
    needs to return a string / list / map (Probe 5, gated on
    Slice A + a small extension here).
  - That the embedder's `RuntimeBindings::AddModule` properly
    extracts memory from a foreign instance + bridges it to
    cel_runtime.wasm (production wiring; tested at the
    `Engine::Plan` level once Slice E lands).
  - That the same Go source survives a TinyGo version bump
    without ABI drift (the `cel.toolchain` custom section
    versioning from §4.5 point 5 is the eventual answer; not
    yet wired into the probe).

### v1 proto constraint — captured 2026-05-21

After Probe 2 shipped, an architectural realization landed: **proto
messages cannot cross into a foreign wasm module's address space
in v1**.  They enter the expression's wasm as externrefs from the
host adapter (host-resident table + `payload.msg_slot`), and that
representation is meaningless to a separately-instantiated foreign
module.  Two future paths (serialize-to-arena, externref-bridge
via host trampolines) are documented in `m13-custom-fns.md` §4.5.1
but both are real engineering and deferred.

**Impact on probes:**

  - Probes 1 + 2 originally used `bool rules.allow(this proto(acme.User) u, string r);`.
    Rewritten to `bool rules.allow(this string userId, string resource);`
    — both args become CEL_STRING; the call shape is identical at
    the wasm boundary.  This is a deliberate choice: the probes
    must reflect v1's allowed type set, not a future extension.
  - A parser-level **negative probe** is now on the to-do list:
    once Slice B's `.celfn` parser exists, it must refuse a
    `bool rules.X(proto(Y) z)` decl with a parse error citing
    §4.5.1.  That probe runs in the parser test suite, not at the
    wasmtime level — no new wasm artifacts needed.

### Probe 3 — multi-toolchain foreign modules (✅ shipped 2026-05-21)

**What it proves**

Three additional toolchains (bare C, WASI C, Rust) — on top of
TinyGo from Probe 2 — produce wasm modules that interop with the
M13 cross-language ABI **without per-toolchain accommodation in
the host harness**.  The harness is the same `m13_p2_test`-style
shape for each; only the `rules.wasm` path differs.

This is the headline validation for §4.5 of the main design doc:
the producer-side contract is loose enough to fit four real
toolchains (and counting), and the engine's instantiation policy
(call `_initialize` if exported; use the foreign module's memory
if it defines one) handles all the variation we've seen so far
with no special cases.

**Artifacts shipped**

| Probe target | Source | Built wasm | Test |
|---|---|---|---|
| `m13_p3_c_test`      | `rules_c/rules.c`      | 331 bytes | bare C, `clang --target=wasm32 -nostdlib` |
| `m13_p3_c_wasi_test` | `rules_c_wasi/rules.c` | 448 bytes | wasi-libc, `--target=wasm32-wasip1 -mexec-model=reactor` |
| `m13_p3_rust_test`   | `rules_rust/rules.rs`  | 180 bytes | Rust no_std, `--target=wasm32-unknown-unknown` |

Plus the toolchain probe from Probe 2:

| `m13_p2_test`        | `rules/rules.go`       | 816 bytes | TinyGo `wasm-unknown` |

**Toolchain matrix — verified**

| Toolchain | Memory ownership | Exports `_initialize`? | Binary size | Notes |
|---|---|---|---|---|
| TinyGo `wasm-unknown` | defines (2 pages) | **yes — must call** | 816 B | runtime statics at offset 65536; checks initialized flag on every export entry |
| Rust no_std `wasm32-unknown-unknown` | defines (16 pages = 1 MiB) | no | 180 B | exports `__data_end` + `__heap_base` globals; smallest of the four |
| Bare C clang `--target=wasm32 -nostdlib` | defines (configurable via `--initial-memory`) | no | 331 B | exports `__stack_pointer` global |
| WASI C `--target=wasm32-wasip1 -mexec-model=reactor` | defines | **yes — must call** | 448 B | imports nothing for trivial sources; full libc available for non-trivial bodies |

**Discoveries / open items**

  - **All four default to defining memory.**  Of the toolchains
    surveyed, NONE imports memory by default.  Importing memory
    is opt-in via linker flags on every toolchain that supports
    it (Rust + clang via `--import-memory`; TinyGo doesn't expose
    a flag).  The engine's "if a foreign module exports memory,
    use it" rule from §4.5.2 isn't a fallback — it's the
    realistic primary path.
  - **`_initialize` is split evenly.**  TinyGo + WASI-C export it;
    Rust no_std + bare-C don't.  The engine's "call if exported,
    skip if not" rule is exercised by all four probes.
  - **No surprises on the CelValue ABI.**  The 24-byte layout from
    `runtime/cel_data.h` decodes identically in Go,
    Rust, and C.  No alignment / endianness / packing issues.
  - **Memory page-size constraint is real.**  Caller WAT imports
    `(memory 2)` (min 2 pages = 128 KiB).  Bare-C defaulted to 1
    page and failed to link; fixed with `-Wl,--initial-memory=131072`.
    WASI-C and TinyGo default to ≥ 2 pages; Rust defaults to 16.
  - **Compile-time toolchain installs.**  This slice took: brew
    install tinygo + brew install rustup + rustup init + rustup
    target add wasm32-unknown-unknown + brew install wasi-libc +
    brew install wasi-runtimes.  All cleanly installable on
    apple-silicon darwin; ~5 min wall clock total.

**What it deliberately does not prove**

  - C++ specifically (covered transitively by bare-C + WASI-C
    since they're the same compiler; a `clang++` build would
    behave identically modulo libcxx for non-trivial code).
    Adding a dedicated C++ probe is straightforward when desired.
  - Stock Go via `GOOS=wasip1`.  Bigger surface (full WASI
    preview1 stdlib).  Defer until a user actually needs it.
  - AssemblyScript / Zig / etc.  Same pattern; copy a probe
    directory when desired.

### Probe 4 — Engine-owned module model (✅ shipped 2026-05-21)

**What it proves**

Validates the M13 §2.1 refinement where the engine holds a
`map<alias, Instance>` and resolves a caller's wasm imports against
that map at `Plan` time.

Six tests:

  1. `AddModule` registers a foreign wasm under an alias; calls
     `_initialize` if exported; snapshots function-export names for
     conflict detection.
  2. Re-registering the same alias errors with `AlreadyExists` at
     `AddModule` time (not at Plan).
  3. Cross-module overload-id conflicts are observable from raw
     wasm export tables — registering the same wasm twice under
     two aliases (`rules`, `policy`) surfaces `allow_string_string`
     as a conflict.  No `cel.toolchain` custom section needed.
  4. `Plan` walks the caller's import table; resolves
     `<alias>.<helper>` against registered modules; instantiates +
     calls eval; result decodes to `CEL_BOOL` / `true` (same
     assertion as Probe 2 / Probe 3, but via the engine).
  5. `Plan` fails with a clear `FailedPrecondition` citing the
     unmet import when an alias isn't registered.
  6. `Plan` fails when a registered module is missing the helper
     the caller WAT expects (`allow_string_string`).

**Artifact**: `compiler/probes/m13_custom_fns/m13_p4_engine_test.cc`
— a `ProbeEngine` helper class wraps the engine-owned semantics;
the same model will land inside `cel::Engine` (production) in
Slice C.

**Run**: `bazel test //compiler/probes/m13_custom_fns:m13_p4_engine_test`

**Architectural realizations**:

  - **Linker conflict detection has the right shape, wrong key.**
    wasmtime's Linker conflict-checks by `(module_name, item_name)`
    tuples.  Re-registering the same `(alias, helper)` pair errors
    naturally.  Cross-module overload-id conflicts (different aliases,
    same helper name) require a separate export-walking pass — what
    `CrossModuleOverloadConflicts()` does in the probe.
  - **First-exporter wins for `cel.memory`.**  When multiple foreign
    modules each define their own memory, the engine picks the
    first-registered one.  This needs explicit conflict policy
    in production (probably "first wins + warn"; the probe FIFO
    matches that).
  - **`_initialize` is a per-module post-instantiation hook.**  Called
    iff the module exports it.  Decouples WASI-reactor and
    standalone-wasm modules cleanly — the engine adapts to either.

### Probe 5 — Host-backed callback via C++ (✅ shipped 2026-05-21)

**What it proves**

Symmetric to Probe 2 (TinyGo foreign module) but for the
`@host.X` backend.  A C++ function — registered with wasmtime as
the impl for `cel_fn.length_string` via
`wasmtime_linker_define_func` — fulfills the host-callback role
that `RuntimeBindings::AddFunction(overload_id, impl)` will use in
production.

Single test (`HostCallbackReadsStringWritesIntLength`):

  1. Allocate 2-page host-owned `cel.memory`.
  2. Register `HostLengthCallback` under `(cel_fn, length_string)` in
     the wasmtime linker with type `(i32, i32) → ()`.
  3. Instantiate the caller WAT (imports `cel.memory` +
     `cel_fn.length_string`; stages "hello world" at offset 80; calls
     `length_string(out=40, s=16)`).
  4. Callback reads `*s_slot` as a CEL_STRING CelValue, extracts the
     string length (11), writes a CEL_INT CelValue to `*out_slot`.
  5. Test decodes `out_slot` and asserts `kind=CEL_INT, payload.i=11`.

**Artifacts**

  - `doc/implementation-plan/rewrite/wat/m13_p5_caller.wat` — the
    caller; imports `cel_fn.length_string` (host-backend namespace).
  - `compiler/probes/m13_custom_fns/m13_p5_host_test.cc` — the
    host harness + callback impl.

**Run**: `bazel test //compiler/probes/m13_custom_fns:m13_p5_host_test`

**Architectural realizations**

  - **`wasmtime_func_callback_t` is the right shape for `FunctionImpl`.**
    Takes `(env, caller, args, nargs, results, nresults)` — `caller`
    is the access point for the caller's exported memory via
    `wasmtime_caller_export_get`.  The `env` pointer lets the C++
    impl carry a closure / state (e.g. a host adapter handle).
  - **Host-backend doesn't need allocation for primitive returns.**
    Bool / int / uint / double fit inline in the 24-byte CelValue
    payload.  String / bytes / list / map returns will need
    `cel.arena_alloc`; that's a follow-up probe.
  - **Caller MUST export memory** when the host callback uses
    `wasmtime_caller_export_get(caller, "memory", …)` — same shape
    as `wat_runner`'s cel_host stubs.  Production: the engine
    arranges this by binding `cel.memory` to whichever module
    defines it; the host callback closes over the engine's memory
    reference instead of going through `caller_export_get`.

**What it doesn't prove**

  - Outbound string/list/map allocation via `cel.arena_alloc`.
    Probe 6 (TBD) covers that.
  - Higher-level `Value` ↔ `CelValue` typed coercion (Slice C library).
  - Engine-orchestrated host-callback registration.  The probe
    wires the callback directly via wasmtime; the engine-owned
    model from Probe 4 will gain an `AddFunction(overload_id, impl)`
    method that does this internally.

### Probe 6 — Negative probe: parser rejects proto on foreign (planned, gated on Slice B)

**Goal**: Lock in the §4.5.1 constraint at parse time.  When Slice
B's `.celfn` parser lands, the negative probe asserts:

  - `bool rules.allow(proto(X) p)` → parse error citing the v1 rule
  - `bool rules.f(this string s, proto(X) p)` → parse error
  - `proto(X) rules.f(string s)` → parse error (return type too)
  - `bool rules.f(list<proto(X)> ps)` → parse error (transitively)
  - All four equivalents for `@host.` declarations succeed (host is
    allowed to carry protos).

Runs at the C++ unit-test level over `ParseCelfnFile`; no wasm
artifacts.

### Probe 7 — celwasmc-emitted caller (gated on Slice A → B → C)

**What it will prove**

  - The Slice-A refactor of `ImportModule` (enum-of-2 → tagged
    `(kind, module_name)` value, see §5.3 of the main design)
    correctly emits `rules.allow_message_acme_User_string` imports
    when the IDL contains `bool rules.allow(...);`.
  - The byte-emitted wasm matches `m13_p1_caller.wat`
    disassembly (modulo Binaryen-assigned names) — the WAT-first
    rule's verification step.

**Artifacts**

  - `compiler/probes/m13_custom_fns/fns.celfn` — a real `.celfn`
    file driving the codegen path.
  - `compiler/probes/m13_custom_fns/m13_p3_test.cc` — invokes
    the Slice-A-extended celwasmc, asserts the emitted wasm
    bytes match the m13_p1_caller WAT disassembly.

This probe blocks on Slice A (M13.A in the main slice plan).

### Probe 8 — End-to-end (TinyGo + celwasmc + engine.Plan)

**What it will prove**

The full M13 acceptance test from §8.3 of the main design doc:

```
fns.celfn  ──celfnc──►  rules_stubs.go  ──┐
                                          │
user writes:  rules_impl.go  ─────────────┤
                                          │
                                 TinyGo  ──┴──►  rules.wasm
                                                  │
celwasmc -e 'user.allow("/admin")' --functions=fns.celfn  ──►  expr.wasm
                                                  │
                            engine.Plan ──────────┴───────────►  Instance
                                                  │
                                               .Eval()
                                                  │
                                                true (Value{kBool, true})
```

This is the slice-E acceptance test, not a probe — it lives
under `e2e/` and is what proves the entire pipeline
ships.

## Probe 2 Go source (preview)

The Go side, when TinyGo's available, looks like:

```go
// compiler/probes/m13_custom_fns/rules/rules.go
//
// Foreign-wasm-backed CEL custom function `rules.allow`.  The Go
// side of the M13 cross-language ABI contract from §4.5 of
// m13-custom-fns.md.
//
// Built with TinyGo:
//
//   tinygo build -target=wasm-unknown -no-debug \
//     -o rules.wasm ./compiler/probes/m13_custom_fns/rules/
//
// In Probe 2, this replaces m13_p1_rules_stub.wat.  Probe 2 is
// otherwise byte-identical to Probe 1 — the win condition is
// "TinyGo can hit the ABI without help."

package main

import "unsafe"

// CelKind tags — must match runtime/cel_data.h.  Frozen
// as of M13.A; cel.abi.toolchain_abi_version embedded by celfnc
// will assert match at Engine::Instantiate.
const (
    CEL_BOOL    = 1
    CEL_STRING  = 5
    CEL_MESSAGE = 10
)

// CelValue layout — must match runtime/cel_data.h's
// 24-byte struct.  We don't read message references here (probe 2
// doesn't decode the User proto; that's a follow-up); we just
// read enough to validate args were received.
type celValue struct {
    Kind    uint32
    _pad    uint32
    Payload [16]byte
}

func slotAt(ptr uint32) *celValue {
    return (*celValue)(unsafe.Pointer(uintptr(ptr)))
}

// allow(out, user_slot, resource_slot)
//
// The exported name MUST match the overload-id synthesised by the
// .celfn parser:
//
//     bool rules.allow(this proto(acme.User) u, string r);
//                                    ↓
//     overload-id = `allow_message_acme_User_string`
//
//go:wasmexport allow_message_acme_User_string
func allow(out, user, resource uint32) {
    u := slotAt(user)
    r := slotAt(resource)

    // For probe 2: validate args have the expected CelKind tags
    // (proves we read them correctly from shared memory) and write
    // a bool result.  Probe 4 will actually decode the proto
    // message + string and make a real decision.
    isAdmin := u.Kind == CEL_MESSAGE && r.Kind == CEL_STRING

    o := slotAt(out)
    o.Kind = CEL_BOOL
    *(*uint64)(unsafe.Pointer(&o.Payload[0])) = boolU64(isAdmin)
}

func boolU64(b bool) uint64 {
    if b {
        return 1
    }
    return 0
}

// Required by TinyGo `wasm-unknown` target.
func main() {}
```

The host (probe 2 test) is byte-identical to probe 1's harness
with the WAT swap, so it isn't reproduced here.

## What probe 1 surfaced that's worth knowing

  - **wasmtime's Linker is the right ABI seam.**  Wiring
    `rules.allow_*` to a foreign module's export is two
    `wasmtime_linker_define` calls — nothing magical, no special
    component-model machinery needed for the v1 shape.  Matches the
    §10.5 decision to go shared-memory-first.
  - **Pre-staged `(data …)` segments work fine for arg marshalling.**
    The caller WAT initializes args at fixed memory offsets via
    `(data (i32.const …))` segments; in production those bytes get
    written by the codegen-emitted prelude (loading from activation
    slots, lowering string literals into rodata, etc.).  The wire
    shape is the same.
  - **No cel_runtime.wasm needed for a bool round-trip.**  The
    runtime is only required when the foreign fn allocates an
    outbound string / list / map (then it needs `cel.arena_alloc`).
    For primitives, the cross-module link works in isolation.
    Useful pedagogy for the Probe 2 / 4 documentation.
  - **`wasmtime_func_call` returns `wasmtime_val_t` with `.of.i32`
    in our ABI.**  Confirmed; matches what `wat_runner` already
    relies on.  No surprises.

## Next steps

  - **Probe 2** lands as soon as TinyGo is installed.  ETA: same
    afternoon if `brew install tinygo` is run; otherwise blocked.
  - **Slice A** of the main M13 slice plan (`ImportModule` refactor)
    is independent of probes 2–4 and can start in parallel.
  - **Probe 3** is gated on Slice A.
  - **Probe 4 / Slice E** is gated on probes 2 + 3 + Slice D
    (CEL-defined backend).
