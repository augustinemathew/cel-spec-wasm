# Foreign functions — calling an independent Go/Rust wasm module from CEL

Status: **superseded, never started** — the foreign-module approach
drafted here was replaced by the Component-Model backend: m23
(component spike) → m24 (`@component` fns + `Engine::AddComponent`)
→ m26 (`celfnc` + `cel_wasm_component` hermetic build).  Kept for the
design rationale; do not implement from this doc.

> **One-line scope.** A CEL expression calls a function whose body lives
> in a *separate* wasm module the embedder compiled from **Go** or **Rust**
> (`<alias>.fn(...)`).  Unlike `@host` (C++ callback) and `@native`
> (CEL body inlined into the expr module) — both of which pass **slot
> offsets into the shared `cel.memory`** — a foreign module has its **own
> linear memory**, so values must be **marshalled across the boundary**.
> That single fact (a *different calling convention*) is the whole
> milestone.

> **The front half is already free.** Type-checking of foreign call sites
> works *today*: the `.celfn` IDL declaration drives the checker for every
> backend (see §4).  This milestone is purely the **runtime/codegen half**
> — caller-side marshalling glue, the `cel_call_foreign` host trampoline,
> Plan-time module linking, and the `celfnc` shim generator.

This doc is the milestone consolidation of the design in
`modules-and-ffi.md §5` and the empirical results in
`foreign-go-bindgen-findings.md`.  It does not re-derive the byte-level
ABI (that lives in `modules-and-ffi.md §5.3`); it frames the
calling-convention difference, inventories what is proven vs unbuilt,
and lays out the build slices, WAT-first plan, and test matrix.
**Review before any code** — code begins once approved.

---

## 1. The problem: foreign functions do NOT share the calling convention

Every in-module helper, builtin, `@host` import, and `@native` body uses
**one uniform convention**: `(i32 out_slot, i32 arg0, …, i32 argN) → ()`,
where each `i32` is a **byte offset to a 24-byte `CelValue` in the shared
`cel.memory`** (the runtime owns + exports it; the expr module imports it).
Args and results are boxed `CelValue`s addressed by offset; **nothing is
copied** because every party reads the same linear memory.

A foreign module breaks that assumption at the root:

| Backend | Body lives in | Memory | Calling convention | Cross-memory copy? |
| --- | --- | --- | --- | --- |
| `@host` | embedder C++ | shared `cel.memory` (+ externref table) | slot ABI via trampoline | no |
| `@native` | the expr module itself (local func) | shared `cel.memory` | slot ABI, `call $fn` | no |
| **foreign** (`<alias>`) | **a separate Go/Rust wasm module** | **its OWN linear memory** | **canonical-ABI exports** (pointers/lengths in *its* memory; scalars unpacked) | **yes — marshal both ways** |

A slot offset like `40` means nothing in the foreign module's address
space — byte 40 there is unrelated memory.  So a foreign call cannot be a
plain `call`; it must go through a **host trampoline** that:

1. **lowers** each CEL argument from a `cel.memory` slot *into* the foreign
   module's memory (allocating there via the module's exported
   `celfn_realloc`),
2. **calls** the foreign export with canonical-ABI params (pointers/lengths
   into *its* memory, scalars by value),
3. **lifts** the result back *out* into a `cel.memory` slot (arena-allocated),
4. maps a guest trap / error to a CEL `kError`.

This is a scoped subset of the WASI Component Model canonical ABI
(`modules-and-ffi.md §5.3-5.4`).  The trampoline dispatches **only on the
CEL type from the IDL** — the same type table drives the per-language shim
generation (§6), so the host half and the guest half stay in lock-step.

---

## 2. What already exists — do not rebuild

The survey (2026-05-27) classifies the groundwork.  **PROVEN** = validated
by a runnable probe; **DESIGNED** = specified in a doc; **BUILT** = in the
codebase; **UNBUILT** = this milestone.

**PROVEN (probes under `probes/foreign_go/`, recorded in
`foreign-go-bindgen-findings.md`):**
- The fixed C ABI is viable end-to-end: a stock-Go `GOOS=wasip1
  -buildmode=c-shared` module exports `//go:wasmexport <overload_id>`
  **verbatim** (no mangling), exports `memory` and a `cabi_realloc`-shaped
  `celfn_realloc`, and the host writes lowered args + reads lifted results
  through them (findings §"how celfn_realloc works", probe `strcase/`).
- **`_initialize` is mandatory** for the Go reactor — skipping it traps
  (findings §5.7-confirm).
- Stock Go imports a **real `wasi_snapshot_preview1` surface** (10 funcs for
  the string case, 17 with proto) — the engine must wire a **full WASI
  context**, not stub the imports (findings).
- **Panic → trap → CEL error** works: explicit `panic()`, nil-deref, and
  index-OOB all unwind to `unreachable` → a `wasmtime` trap the host catches
  and maps to `kError` (probe `paniccase/`); a `defer/recover()` shim turns
  the common case into a clean status return *before* it traps.
- **Proto-by-bytes** runs in stock Go (`proto.Unmarshal` inside the module)
  but costs **+4.7 MB + 7 WASI imports**, and **TinyGo cannot** (its
  reflection traps in `reflect.NewAt`) — probe `protocase/`.

**DESIGNED (`modules-and-ffi.md §5`):** the full ABI (§5.3 storage/calling
table), the recursive lower/lift algorithm (§5.4), lifetime rules (§5.5),
the generated-shim model (§5.6), `_initialize` handling (§5.7), and the
proto/`type`/`optional` boundary rejection (§5.8).

**BUILT (in the codebase):**
- `Engine::AddModule(alias, wasm_bytes)` — declared + implemented:
  validates the alias, parses the bytes, snapshots the module's exports
  (`engine.h:117`, `engine.cc:~685`).  *Plan-time linking is not yet wired.*
- Foreign decls route to `ImportModule::kUserModule` with the alias as the
  import module name (`compile.cc` `BuildOverloadTable`).
- The proto-at-boundary rejection at type-check (`function_library.cc:192`).
- WAT probes `m13_p1_caller.wat` + `m13_p1_rules_stub.wat` (the foreign
  call shape) — but note these prototype the *degenerate* shared-memory
  stub, **not** the real cross-memory marshalling (see §8).

**UNBUILT — this milestone:**
1. caller-side marshalling glue in `expr_lower` for a `kUserModule` call,
2. the `cel_call_foreign` host trampoline (lower / call / lift / trap-map),
3. Plan-time foreign-module instantiation + WASI context + `_initialize`,
4. the status/error channel (designed, **decision pending** — §3.4),
5. the `celfnc` shim generator (Go first, then Rust).

---

## 3. The calling convention / ABI (summary; bytes in `modules-and-ffi.md §5.3`)

### 3.1 Per-type storage
- `bool`/`int`/`uint`/`double` → unpacked scalars (`i32`/`i64`/`f64`).
- `string`/`bytes` → `(i32 ptr, i32 len)` into the **foreign** memory.
- `list<T>` / `map<K,V>` → `(ptr, len)` to a contiguous element/entry array
  in foreign memory; recursive.
- `Duration`/`Timestamp` → `{i64 secs, i32 nanos}` record.
- `proto(...)`, `type`, `optional<T>` → **rejected** at the boundary
  (already enforced at type-check, §4). A future opt-in passes a proto as
  serialized bytes (stock-Go only; `modules-and-ffi.md §8.5 / findings`).

### 3.2 Calling shape
Flat args when they fit; otherwise an **indirect spill** (one `i32`
pointer to an arg record in foreign memory). Scalar results by value;
aggregate results via a **return area** the host pre-allocates and passes
as a hidden pointer arg. (`modules-and-ffi.md §5.3`.)

### 3.3 The allocator + trampoline
- Each foreign module exports `celfn_realloc(ptr, old, align, new) → ptr`
  (the `cabi_realloc` shape) so the host can place lowered bytes in *its*
  memory.
- The host trampoline is `cel_call_foreign(fn_id, args_slot, out_slot)` —
  a `cel_host.*` import the expr module calls; it does the recursive
  lower → call → lift → trap-map (`modules-and-ffi.md §5.2`).

### 3.4 The error/status channel — **decision to resolve in this milestone**
The bare ABI returns only the result value, so a foreign failure would
masquerade as a legitimate `false`/`0`/empty. The probe (`paniccase/`)
proved the failure modes; the fix is an explicit **`status i32`** alongside
the value (leading hidden out-param or return-area field): `0 = ok`,
nonzero = foreign error → trampoline writes `kError`; the `recover()` shim
fills it on panic. The exact placement is **open** (`modules-and-ffi.md
§5.3 OPEN`) and must be nailed in the WAT-first slice (§8) before the
trampoline and the `celfnc` templates lock to it.

---

## 4. The IDL's role — declaration drives type-checking (already working)

A foreign function is declared in the `.celfn` IDL with an `<alias>.`
prefix (no `@` sigil — that is reserved for the two built-in backends):

```celfn
/// True if `subject` may perform `action`, per the Go policy module.
bool rules.allow(string subject, string action);     // alias = "rules"
```

`ParseCelfnSource` parses this into a `CelfnDecl` carrying the signature
(name, params, return type) and `Backend::kForeign`. Registered on the
`Compiler` (`AddFunction` / `AddLibrary`), it becomes a checker overload
declaration — so `rules.allow(subject, "read")` **type-checks today**:
arg kinds, arity, and the return type are all resolved from the IDL, with
no runtime module present. This is identical to how `@host` decls check
(verified this session: the m21 e2e tests `Compile` host call sites that
only the IDL decl makes resolvable).

Two checking facts specific to foreign:
- **Proto rejection at the boundary is a type-check error**, not a runtime
  one: `MentionsProto()` over params + return rejects `proto(...)`,
  `list<proto…>`, `map<…,proto…>` for a `kForeign` decl
  (`function_library.cc:192-209`) — a `msg_slot` handle is meaningless in
  another module's memory.
- The IDL is a **compile-time-only** input: nothing about it is needed to
  *run* a compiled program — the call is already lowered to a trampoline
  keyed by alias + overload id; the run side supplies module **bytes**, not
  the IDL (`index.md §9.1`).

**Implication for this milestone:** the frontend/checker is done. Every
slice below is codegen, runtime, or tooling.

---

## 5. Toolchains — Go and Rust

The embedder compiles the foreign module from their language of choice.
Two are in scope; the differences are in *memory ownership* and *whether
WASI is pulled in*.

### 5.1 Go (`GOOS=wasip1`, reactor) — proven
```bash
GOOS=wasip1 GOARCH=wasm go build -buildmode=c-shared -o rules.wasm ./rules
```
- Defines its own memory; is a **reactor** → exports `_initialize`
  (**mandatory**, host calls it once post-instantiation).
- Imports a **full `wasi_snapshot_preview1`** surface → engine wires a real
  WASI context (proven: 10 imports string / 17 proto).
- `//go:wasmexport <overload_id>` exports verbatim; `func main(){}` is
  required even though it never runs.
- Proto path: **stock Go only** (~6.4 MB); **not TinyGo** (reflection trap).
  TinyGo is fine (~118 KB) for the **scalar/string** path with
  `-target=wasip1 -buildmode=c-shared`.

### 5.2 Rust — to be probed in this milestone
Two candidate targets (mirroring `index.md §8.3`):
- **`wasm32-unknown-unknown`** (`no_std` or minimal) — defines its own
  memory, **no WASI**, no `_initialize`; smallest, closest to the
  hand-WAT shape. Likely the recommended default for a pure compute
  function. Exports the fixed-ABI entry + `celfn_realloc` + `memory`.
- **`wasm32-wasip1`** — full libc/std, WASI imports, reactor-style init —
  when the function genuinely needs std facilities.

> **Probe gap (this milestone owns it):** the existing probes are Go-only.
> Before the Rust shim template is written, a `probes/foreign_rust/`
> experiment must confirm, for both targets: the export-name shape, the
> `celfn_realloc` contract, whether `_initialize` is needed
> (`wasm32-unknown-unknown` should *not* need it; `wasip1` will), the WASI
> import surface, and the panic→trap→`kError` mapping (Rust `panic =
> "abort"` → `unreachable`). Record the confirmed facts in
> `foreign-rust-bindgen-findings.md` with a dated callout, the same way
> the Go probe did. **No Rust shim is written from memory.**

---

## 6. `celfnc` — the shim generator (Go first, Rust next)

`celfnc` is a "scoped `wit-bindgen`": from the `.celfn` IDL it generates
the per-language guest glue so the embedder writes a *natural* function
while the wire contract stays fixed (`foreign-go-bindgen-findings.md
§"how celfnc generates this"`). Per `kForeign` decl it emits:
1. the fixed-ABI export under the **overload id**, params per the §3.1
   storage table;
2. **lift** of each param from foreign memory into a native value;
3. a **panic-guarded** call into the user's function
   (`defer/recover()` → status, per §3.4);
4. **lower** of the return through the storage table;
5. once per module: `celfn_realloc`, memory helpers, the WASI/`_initialize`
   shape for the target, and (iff `MentionsProto()`, future opt-in) the
   proto import + `Unmarshal` wrapper.

The Go template shape is proven (~80 lines, in the findings doc). The Rust
template is **new** and blocked on the §5.2 probe. `wit-bindgen` itself is
**rejected for v1** (it needs the Component Model stock Go doesn't emit,
and proto has no WIT equivalent) — verdict recorded in the findings doc.

---

## 7. Build slices

Ordered so each slice is independently testable; WAT-first (§8) precedes
the codegen/trampoline slices.

- **F0 — Error-channel ABI decision + WAT.** Resolve §3.4 (status slot
  placement); freeze it in the foreign-call WAT trace and the `wat_runner`
  `cel_call_foreign` stub.
- **F1 — Caller-side marshalling glue (`expr_lower`).** For a
  `kUserModule` call, emit: pack args into an `args_slot` run, call
  `cel_host.cel_call_foreign(fn_id, args_slot, out_slot)`, leave the result
  at `out_slot`. Output must match the F0 WAT byte-for-byte.
- **F2 — `cel_call_foreign` trampoline (`eval/`, new TU).** Recursive
  `Lower(CelValue,Type)` → foreign bytes (via the module's `celfn_realloc`);
  invoke the export; `Lift` → arena-allocated `CelValue`; map trap/status
  → `kError`. Unit-test `Lower`/`Lift` per type over a fake foreign memory.
- **F3 — Plan-time linking + WASI.** Instantiate each registered foreign
  module in the store, provide the WASI context, call `_initialize` iff
  exported, bind `celfn_realloc`/`memory`, wire `cel_call_foreign` into the
  dispatch. End-to-end with the real Go `strcase` module.
- **F4 — `celfnc` Go generator** (`tools/celfnc/`): IDL → Go shim;
  round-trip the `strcase` example through generated glue.
- **F5 — Rust probe + `celfnc` Rust generator.** `probes/foreign_rust/`
  findings (§5.2), then the Rust template; round-trip a Rust `strcase`.
- **F6 (stretch, opt-in) — proto-by-bytes** (`modules-and-ffi.md §8.5`):
  stock-Go-only serialized-proto path, behind an explicit opt-in.

`AddModule` registration (BUILT) and the proto-rejection check (BUILT) are
prerequisites already in place.

---

## 8. WAT-first plan (mandatory before F1/F2)

Per CLAUDE.md, the caller-side ABI is frozen at the WAT level first. The
existing `m13_p1_caller.wat` + `m13_p1_rules_stub.wat` prototype a foreign
call where the stub module **shares `cel.memory`** — that is the
*degenerate* shape and does **not** exercise the cross-memory marshalling
that defines this milestone. The real WAT-first artifacts:

- **`m22_foreign_call_caller.wat`** — the expr module: pack args into an
  `args_slot` run, `call $cel_call_foreign (fn_id) (args_slot) (out_slot)`,
  return `out_slot`. This is exactly what F1's codegen must emit.
- **A `cel_call_foreign` stub in `wat_runner`** — like the existing
  `cel_host.*` stubs (`wat_runner.h`): a caller-supplied `std::function`
  that reads the `args_slot` CelValues and writes a result (and status) to
  `out_slot`, simulating the trampoline before F2 exists. This lets F1's
  WAT run end-to-end and pins the caller/trampoline interface.
- **`m22_foreign_module_stub.wat`** (its **own** `(memory …)`, **not**
  `cel.memory`) + a real two-store harness step — deferred to F2/F3, where
  the trampoline's lower/lift is verified against the stubbed baseline
  (byte-identical), per the WAT-first workflow's "remove the stub once the
  real impl lands" step.

> **Harness extension owned here:** `wat_runner` today instantiates a
> single shared memory. The foreign path needs (a) a `cel_call_foreign`
> stub (cheap, mirrors the existing stub pattern) for F1, and (b) a second
> module with its **own** memory for the F2/F3 cross-memory test. (b) is the
> first genuine harness extension since the host-stub work.

---

## 9. Test matrix

Per-type, on both the lower (arg) and lift (return) side, plus the
negatives — the same discipline as m21. A type is not done until it is
green positive and rejecting negative.

**Lower/lift type matrix** (F2 unit + F3 e2e through a real module):
`bool / int / uint / double / string / bytes / Duration / Timestamp`,
`list<T>` (empty / 1 / many / nested), `map<K,V>` (each key kind; missing
key), nested aggregates. Boundaries: `INT64_MIN/MAX`, `UINT64_MAX`,
empty/embedded-NUL/multi-byte strings.

**Negatives / failure paths:**
- `proto(...)` / `type` / `optional` at a foreign decl → **type-check
  error** (already enforced; pin a test per shape).
- foreign guest **panic** (explicit, nil-deref, index-OOB) → `kError`, not
  a wrong value and not a process crash (probe-proven; pin e2e).
- the **status channel** distinguishes a real `false`/`0` result from a
  foreign error (the load-bearing §3.4 test).
- missing/duplicate alias, malformed module bytes → `AddModule` /
  `Plan` error, not UB.
- `_initialize` skipped (Go reactor) → caught/handled, not a silent trap.

**Toolchain coverage:** the Go `strcase` module end-to-end (F3); the Rust
`strcase` module end-to-end (F5); both through generated `celfnc` glue
(F4/F5). The committed probes (`probes/foreign_go/*`) are the fixtures.

---

## 10. Open questions / decisions

1. **Error-channel placement (§3.4)** — leading hidden status param vs
   return-area field. Resolve in F0; it ripples into the trampoline and
   every `celfnc` template, so it must be first.
2. **Rust default target (§5.2)** — `wasm32-unknown-unknown` (lean, no
   WASI) as the recommended default vs `wasm32-wasip1` (std). Decide after
   the Rust probe.
3. **Instance recovery policy** — on an *uncaught* foreign trap, the engine
   writes `kError`; does it re-instantiate the foreign module before the
   next eval (Go's `fatalpanic` is nominally fatal) or trust reuse (the
   probe saw reuse work)? The `recover()` shim makes uncaught traps rare;
   pick the conservative re-instantiate unless cost says otherwise.
4. **`fn_id` encoding** — how the caller names the foreign overload to the
   trampoline (a dense per-program id vs the overload-id string). A dense
   id keeps the hot path cheap; decide in F1 alongside the caller glue.

---

## 11. References

- `modules-and-ffi.md §5` — the byte-level ABI, trampoline, lift/lower,
  shim model, `_initialize`, boundary rejection.
- `foreign-go-bindgen-findings.md` — every empirically-confirmed Go fact,
  with the probe that proved it; the `celfnc` Go template; the
  wit-bindgen-rejection verdict.
- `probes/foreign_go/{strcase,protocase,paniccase,layered}/` — the runnable
  Go experiments + `wasmtime-py` host harnesses + `build.sh`.
- `index.md §8` — the embedder-facing foreign guide (the target UX).
- `wat/m13_p1_{caller,rules_stub}.wat` — the degenerate (shared-memory)
  prototype; superseded by the §8 cross-memory WATs.
- Code anchors: `engine.{h,cc}` `AddModule`; `compile.cc`
  `BuildOverloadTable` (`kUserModule` routing); `function_library.cc`
  (`MentionsProto` boundary rejection); `overload_table.h` `ImportModule`.
