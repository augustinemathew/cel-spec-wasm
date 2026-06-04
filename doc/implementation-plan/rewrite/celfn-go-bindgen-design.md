# celfnc Go bindgen — mini design

Status: design — drafted 2026-05-24, evidence-backed, ready for handoff. NOT
yet implemented. This is a *mini* design: scope is the `celfnc` Go-language
backend that emits the guest shim for **foreign** CEL functions. It does NOT
cover the host trampoline (`cel_call_foreign`, hand-written C++, §5.2) or the
caller-side slot glue (codegen, §5.2) — those are separate, language-agnostic
pieces.

Every claim here is backed by a working probe under
`probes/foreign_go/` (the `layered/` dir is the reference shape).
The probe is throwaway; this doc is the durable artifact.

## 1. What this generates and why

A foreign CEL function (`<alias>.<fn>` in `.celfn`, backend `kForeign`) is
implemented in a separately-compiled wasm module with its **own** linear
memory. The host cannot reach into it, so values are marshalled across by a
hand-written C++ trampoline (`modules-and-ffi.md` §5.2). For that trampoline
to speak to *natural* Go, the module must expose a fixed-ABI seam: per-function
exports, an allocator, and lift/lower glue. `celfnc` generates that Go glue
from the parsed `.celfn` IDL — it is the "guest half" of a scoped
`wit-bindgen` (§5.6).

Input to the generator: `FunctionLibrary` / `CelfnDecl`
(`compiler/celfn/function_library.{h,cc}`), already parsed and validated.
The generator is a **pure function** `FunctionLibrary → {Go source files}`;
no I/O beyond reading the IDL.

## 2. Validated foundations (don't re-litigate these)

From `foreign-go-bindgen-findings.md` (2026-05-24), all empirically confirmed:

  - **Toolchain:** stock Go 1.24 (`GOOS=wasip1 GOARCH=wasm go build
    -buildmode=c-shared`). `//go:wasmexport` names exports verbatim. TinyGo is
    a fallback for the scalar/string path ONLY — it **traps in `reflect.NewAt`**
    on `proto.Unmarshal`, so the proto path (§8.5) is **stock-Go-only**.
  - **Module shape:** exports `memory`, `_initialize` (Go wasip1 is a reactor —
    host MUST call it once or every export traps), and whatever the generator
    emits. Imports 10–17 `wasi_snapshot_preview1.*` funcs → host must wire a
    full WASI preview1 context, not stubs.
  - **`celfn_realloc(ptr,oldLen,align,newLen)->ptr`** = the `cabi_realloc`
    shape. `proto.Unmarshal` links + runs in stock-Go wasm (validated against
    byte-identical wire input).
  - **Panics:** an uncaught Go panic (explicit OR runtime nil-deref/OOB)
    unwinds to wasm `unreachable` → host sees `TrapCode.UNREACHABLE` (NOT a
    WASI `proc_exit`). A shim-side `recover()` converts any panic to a clean
    status. This is the mandated pattern.
  - **Size:** scalar/string module ≈ 1.6 MB; +proto runtime ≈ 6.4 MB (+4.7 MB).

## 3. File layout — three files, three owners

`celfnc` emits a Go package per foreign module (one `.celfn` `alias`). Three
files, by who owns regeneration:

| File | Generated | Regen overwrites? | Contents |
|---|---|---|---|
| `celfn_abi.go` | once, **identical** for every module | yes (safe — no user content) | the common machinery: `celfn_realloc`, lift helpers, `guard`, `main()` |
| `celfn_exports.go` | from the IDL | yes | one `//go:wasmexport` trampoline per `kForeign` decl, named by `overload_id` |
| `rules.go` (user file) | once, as `//TODO` stubs | **NO — never** | the user's natural Go functions |

Reference: `probes/foreign_go/layered/` — these exact three files
build (6.4 MB) and pass the host harness (`host_layered.py`).

### 3.1 `celfn_abi.go` — the common machinery (template-constant)

Emitted verbatim, byte-identical regardless of the IDL. Contents (full source:
`layered/celfn_abi.go`):

  - **`celfn_realloc`** — `cabi_realloc`-shaped. Backs each allocation with a
    Go `[]byte` whose backing-array address it returns, pinned in `argPins`
    (a `map[uint32][]byte`) so the GC neither moves nor frees it across the
    host write + the call. `ResetArgPins()` exposed for per-call reset.
  - **`liftString(ptr,len) string` / `liftBytes(ptr,len) []byte`** — zero-copy
    `unsafe.String`/`unsafe.Slice` views over this module's memory. Valid only
    for the call's duration.
  - **`guard(body func()) (ok bool)`** — runs `body` under `recover()`; returns
    false on any panic. The trampoline uses this; an uncaught panic would
    otherwise trap the whole instance.
  - **`main(){}`** — required to link the reactor.

### 3.2 `celfn_exports.go` — the per-function trampolines (IDL-driven)

One export per `kForeign` decl. Each:

  1. is named `decl.overload_id` (`isValidName_string`,
     `isAdult_message_acme_User`) — `//go:wasmexport` uses it verbatim.
  2. takes the **flattened fixed-ABI params** (§4 table).
  3. inside `guard(...)`: lifts each arg, calls the user stub, captures result.
  4. returns the result packed with a **status flag** (§5).

Reference: `layered/celfn_exports.go`.

### 3.3 `rules.go` — user stubs (generated once, never overwritten)

Initial scaffold per decl is a signature with a `//TODO` body that panics:

```go
// IsAdult implements `bool isAdult(proto(acme.User) user)`.
func IsAdult(user *userpb.User) bool {
    // TODO: implement isAdult
    panic("celfn: isAdult not implemented")
}
```

The user fills the body. Nice property: the `panic` in an unimplemented stub is
caught by `guard()` and surfaces as a clean `kError` at the host — an
unimplemented function fails loud-but-safe, never traps the instance. celfnc
matches stub→decl by the `//celfn:export <alias>.<fn>` annotation (user-guide
§8.4) or by exported-name convention (`fn_name` → Go `PascalCase`).

## 4. Type lowering — the §5.3 storage table as a codegen switch

The generator switches on `CelfnType::Kind` (and recurses for list/map). This
mirrors the host trampoline's `Lower`/`Lift` exactly (§5.4) — the two sides are
generated from the same table, which is why they agree.

| `CelfnType::Kind` | flat wasm param(s) | Go lift in the trampoline |
|---|---|---|
| kBool | `i32` | `arg != 0` |
| kInt | `i64` | `int64(arg)` |
| kUint | `i64` | `uint64(arg)` |
| kDouble | `f64` | `arg` |
| kString | `i32 ptr, i32 len` | `liftString(ptr,len)` |
| kBytes | `i32 ptr, i32 len` | `liftBytes(ptr,len)` |
| kProto | `i32 ptr, i32 len` | `proto.Unmarshal(liftBytes(...), &msg)` |
| kList\<T\> | `i32 ptr, i32 len` | loop, lift each elem by T's storage form |
| kMap\<K,V\> | `i32 ptr, i32 len` | loop over (K,V) records |
| kDuration/kTimestamp | `i64 secs, i32 nanos` | build the Go type |

  - `CelfnType::Argkind()` (`function_library.cc:30`) already produces the
    overload-id slug (`message_acme_User`, `list_int`) — reuse it for the
    export name; do NOT re-derive.
  - `MentionsProto(t)` (`function_library.cc:118`) already exists — use it to
    decide whether to emit the `google.golang.org/protobuf/proto` + generated
    `…pb` imports.
  - **Gap:** `CelfnType` carries `proto_fqn` ("acme.User") but NOT the Go
    import path of the generated message type. The generator needs an
    `fqn → (goImportPath, goTypeName)` map, supplied as generator config
    (or derived from the `protoc --go_out` layout). This is the one piece of
    state not already on the IDL. See §7.

## 5. The error channel (a required ABI addition)

A `bool`-returning trampoline returning bare `0/1` cannot distinguish a genuine
`false` from "the user panicked / the proto failed to decode." So scalar
returns are **packed with a status bit**:

  - return type widens to `i64`: low 32 bits = the scalar value,
    `bit 32 = CELFN_ERR`.
  - the trampoline returns `celfnErr` when `guard` reports a panic OR a decode
    error (the decode arm `panic`s on `proto.Unmarshal` error so `guard`
    catches it uniformly).
  - the host masks bit 32 → writes `kError`; else lifts the value.

Validated: `host_layered.py` shows a corrupt proto returns `('ERR', None)`,
NOT `('OK', False)`. **Aggregate returns** carry the status in a separate word
alongside the §5.3 return-area pointer — same principle, not yet probed.

> This is an addition to `modules-and-ffi.md` §5.3, which has no error slot.
> Flagged in `foreign-go-bindgen-findings.md` open-question #2. The host
> trampoline and the caller-side codegen must both learn the status word.

## 6. The generator (celfnc Go backend) — implementation sketch

```
GenerateGo(lib FunctionLibrary, cfg GoGenConfig) -> map[filename]source:
  emit celfn_abi.go            // template constant
  for decl in lib.decls() where decl.backend == kForeign:
    params  := flatten(decl.params, table §4)          // (name,wasmType) list
    lifts   := [ liftExpr(p.type, cfg) for p in decl.params ]
    ret     := lowerReturn(decl.return_type)            // packed scalar | return-area
    emit into celfn_exports.go:
      //go:wasmexport <decl.overload_id>
      func <overload_id>(<params>) <ret.wasmType> { guard{ lifts; user-call; }; pack }
  emit rules.go stubs IFF rules.go absent (never overwrite)
```

  - **One template per language** (§5.6). Go is one backend; Rust/C are siblings
    sharing the same host trampoline.
  - Deterministic + pure → trivially unit-testable: assert the emitted Go source
    for a given `CelfnDecl` (golden-file tests), then a build+run e2e per type
    arm (the probe harnesses are the e2e shape to port).

## 7. Open items handed off (decide before/while implementing)

  1. **proto fqn → Go import path map.** Not on `CelfnType`. Needs a generator
     config input. Smallest form: a `map<string,string>` from the embedder, or
     parse it out of the generated `*.pb.go`'s `go_package`.
  2. **Error channel in the fixed ABI (§5).** The packed-status convention (§5)
     must be mirrored in the C++ host trampoline and the caller-side slot glue.
     This is an ABI change, not just Go codegen — coordinate with whoever owns
     `cel_call_foreign`.
  3. **Instance recovery policy on an uncaught trap.** `recover()` makes traps
     rare, but if one escapes, the engine should re-instantiate the foreign
     module (cheap: a fresh `_initialize`). Decide + document in the engine.
  4. **Aggregate (list/map/proto) RETURNS.** §4 lift table covers args; returns
     beyond scalars use the §5.3 return area + status word — designed, NOT yet
     probed. First implementer should probe a `list<int>` return before relying
     on it.
  5. **`wit-bindgen` reuse** (deferred, §11 of modules-and-ffi). Not viable for
     v1: it targets Component Model components, not the core wasip1 modules the
     host trampoline assumes; and WIT has no proto type, so the §8.5 path can't
     be expressed. Revisit only if/when the engine adopts a Component Model
     host. Until then, hand-roll this generator (it's ~80 lines/decl of
     deterministic codegen).

## 8. Reference artifacts in this repo

  - `probes/foreign_go/layered/` — the canonical 3-file shape
    (`celfn_abi.go`, `celfn_exports.go`, `rules.go`) + `user.proto` + generated
    `userpb/`. Builds to `rules.wasm` (6.4 MB), passes `host_layered.py`
    (incl. the corrupt-proto → kError case).
  - `probes/foreign_go/strcase/`, `protocase/`, `paniccase/` — the
    earlier single-file probes that established the string, proto, and panic
    findings respectively.
  - `probes/foreign_go/host_*.py` — wasmtime-py harnesses standing
    in for the C++ `cel_call_foreign` trampoline.
  - `doc/implementation-plan/rewrite/foreign-go-bindgen-findings.md` — the full
    empirical record (toolchain, ABI, panic behavior, doc-correction list).
  - `doc/implementation-plan/rewrite/modules-and-ffi.md` §5 — the parent design
    this refines (foreign FFI, fixed ABI, shim generation).
  - `compiler/celfn/function_library.{h,cc}` — the `CelfnDecl` /
    `CelfnType` data model the generator consumes; `Argkind()` (`:30`),
    `CelTypeSpec()` (`:65`), `MentionsProto()` (`:118`).
