# Foreign-Go bindgen — empirical findings

Status: experiment complete — 2026-05-24. Throwaway probe under
`probes/foreign_go/`. Validates the `celfnc` "mini-wit-bindgen"
Go-glue generation against `modules-and-ffi.md` §5 and `user-guide.md` §8.4/§8.5.
**Both cases run end-to-end and pass.** Verdict + doc corrections at the bottom.

## TL;DR verdict

- The §5 foreign FFI shape (per-fn `//go:wasmexport`, `celfn_realloc`,
  reactor `_initialize`, exported `memory`, host writes args into the module's
  own memory) is **viable as-built** with stock Go 1.24. Confirmed empirically.
- The §8.5 **proto-via-serialization** path is **viable** — `proto.Unmarshal`
  links and runs inside a `GOOS=wasip1` Go wasm module. Validated with byte
  exact wire input (cross-checked against the real Go protobuf encoder).
- **Critical caveat:** the proto path requires **stock Go**, NOT TinyGo.
  TinyGo's incomplete reflection traps in `reflect.NewAt` at runtime when
  `proto.Unmarshal` runs. TinyGo is fine for the scalar/string path only.
- Two corrections to the docs (details at bottom): (a) the proto path needs
  stock Go and pulls a ~4.7 MB protobuf runtime into the module; (b) the host
  must provide a full WASI preview1 context (Go imports 10–17 WASI funcs,
  including `fd_write`/`fd_prestat_*`), not a couple of stubs.
- **Panics are handled** (validated 2026-05-24, see "Panic / trap behavior"
  below): a Go panic — explicit `panic()` OR a runtime panic (nil deref, index
  OOB) — unwinds to `runtime.abort` → wasm `unreachable`, surfacing to the host
  as a catchable `Trap` with `TrapCode.UNREACHABLE` (NOT a WASI `proc_exit`).
  The trampoline catches it and writes `kError` per §5.2 step 4. The
  **recommended** celfnc strategy is a shim-side `defer/recover()` wrapper that
  turns any panic into a clean error return (no trap) — confirmed to catch both
  explicit and runtime panics. The instance also survived reuse after an
  uncaught panic in this probe, but recover() is the path to standardize on.

## Toolchain found (this machine, 2026-05-24)

| Tool | Version | Role |
|---|---|---|
| Go | `go1.24.9 darwin/arm64` | `//go:wasmexport` + `GOOS=wasip1 -buildmode=c-shared` |
| TinyGo | `0.41.1` (LLVM 20.1.1) | lighter fallback (string OK; **proto NOT OK**) |
| wasmtime CLI | `44.0.1` | sanity |
| wasmtime-py | `44.0.0` (pip) | the host harness (stands in for the C++ trampoline) |
| protoc | `libprotoc 35.0` (brew) | `.proto` → descriptors |
| protoc-gen-go | `v1.36.11` (`go install`, /tmp/gobin) | Go bindings |
| wasm-objdump | wabt (brew) | export/import introspection |

Exact build flags that worked:
```bash
# string case + proto case (stock Go reactor):
GOOS=wasip1 GOARCH=wasm go build -buildmode=c-shared -o rules.wasm .
# Go bindings:
protoc --go_out=userpb --go_opt=paths=source_relative user.proto
```

## What ran, and the actual host output

`probes/foreign_go/build.sh` regenerates bindings, builds both
modules, runs both Python host harnesses. Output:

```
STRCASE:
  [PASS] isValidName('Alice') = True   [PASS] isValidName('bob') = False
  [PASS] isValidName('')      = False  [PASS] isValidName('Zoe') = True
  [PASS] isValidName('9x')    = False
PROTOCASE:
  [PASS] isAdult(User{age:20})            = True   wire=0814
  [PASS] isAdult(User{age:10})            = False  wire=080a
  [PASS] isAdult(User{age:18,name:Alice}) = True   wire=08121205416c696365
  [PASS] isAdult(User{age:17,name:Bob})   = False  wire=08111203426f62
  [PASS] isAdult(User{age:0, name:Zero})  = False  wire=12045a65726f
```

The host-side wire bytes are hand-encoded (protobuf binary is
language-agnostic — the §8.5 premise) and were cross-checked **byte-identical**
against `proto.Marshal` from the real Go protobuf runtime (`age=20 -> 0814`,
`age=18,name="Alice" -> 08121205416c696365`, …).

## Empirically-confirmed ABI

| Property | string case (stock Go) | proto case (stock Go) | TinyGo string |
|---|---|---|---|
| `.wasm` size | 1,636,475 B (1.6 MB) | 6,377,988 B (6.4 MB) | 118,000 B (118 KB) |
| Exports `memory` | yes | yes | yes |
| Exports `_initialize` | yes (reactor) | yes (reactor) | yes |
| Per-fn export name | `isValidName` (verbatim) | `isAdult` (verbatim) | `isValidName` |
| Exports `celfn_realloc` | yes | yes | yes |
| WASI imports | 10 `wasi_snapshot_preview1.*` | 17 `wasi_snapshot_preview1.*` | 2 |

- **Export naming.** The `//go:wasmexport isValidName` pragma names the export
  EXACTLY (`func[1132] <isValidName> -> "isValidName"`). So celfnc emits the
  export under the CEL overload id directly (`isValidName`, `sayhello_string`)
  — no name mangling, matching §5.6 ("exports the fixed-ABI entry point under
  the overload_id").
- **Memory is exported** as `"memory"` — the host writes lowered args straight
  into the module's linear memory via `memory.write(store, bytes, ptr)`.
  Confirms the §5.1/§5.5 cross-memory copy model works as designed.
- **Reactor `_initialize` is MANDATORY.** Go wasip1 c-shared is a reactor
  (`_rt0_wasm_wasip1_lib -> "_initialize"`). Skipping it and calling
  `celfn_realloc`/`isValidName` **traps** (verified: "WITHOUT _initialize
  TRAP"). The host must call `_initialize` exactly once after instantiation,
  before any export — exactly §5.7.
- **`celfn_realloc(ptr,oldLen,align,newLen)->ptr`** behaves as the cabi_realloc
  shape requires: `newLen==0 => 0`; `ptr==0 => fresh`; `ptr!=0 => grow+copy`.
  In Go the natural backing is a `make([]byte, newLen)` whose backing-array
  address is returned via `uintptr(unsafe.Pointer(unsafe.SliceData(buf)))`,
  with the slice kept reachable (a pin map) so the GC can't move/collect it
  across the host write + the call. Works; see gotcha below on GC pinning.
- **WASI imports are real, not optional.** Stock Go imports 10
  (string) / 17 (proto) `wasi_snapshot_preview1` functions — including
  `fd_write`, `random_get`, `clock_time_get`, and (proto) the `fd_prestat_*`
  / `fd_fdstat_*` / `fd_read` family. The host MUST supply a WASI preview1
  context (wasmtime: `store.set_wasi(WasiConfig())` + `linker.define_wasi()`).
  A handful of no-op stubs would NOT satisfy instantiation.

## The Go-glue templates celfnc must generate

### (a) `string -> bool`  (`bool isValidName(string foo)`)

```go
package main
import "unsafe"

// USER CODE (celfnc never touches this):
func IsValidName(foo string) bool { return len(foo) > 0 && foo[0] >= 'A' && foo[0] <= 'Z' }

// GENERATED GLUE:
func strFromMem(ptr, length uint32) string {            // lift (ptr,len) -> Go string,
    if length == 0 { return "" }                        // zero-copy from THIS module's mem
    return unsafe.String((*byte)(unsafe.Pointer(uintptr(ptr))), int(length))
}

//go:wasmexport isValidName                              // export name == overload id
func isValidName(ptr, length uint32) uint32 {
    if IsValidName(strFromMem(ptr, length)) { return 1 }
    return 0
}

//go:wasmexport celfn_realloc                            // host allocs arg bytes via this
func celfn_realloc(ptr, oldLen, align, newLen uint32) uint32 {
    if newLen == 0 { return 0 }
    buf := make([]byte, newLen)
    p := uint32(uintptr(unsafe.Pointer(unsafe.SliceData(buf))))
    pins[p] = buf                                        // pin against GC (see gotcha)
    if ptr != 0 { if old, ok := pins[ptr]; ok { copy(buf, old); delete(pins, ptr) } }
    return p
}
var pins = map[uint32][]byte{}
func main() {}                                           // required for the reactor build
```

### (b) `proto -> bool`  (`bool isAdult(User user)` via serialization, §8.5)

Same `celfn_realloc` + `main()` + a `bytesFromMem` (the `unsafe.Slice`
variant of `strFromMem`). The proto-specific glue:

```go
import (
    "unsafe"
    "google.golang.org/protobuf/proto"
    userpb "protocase/userpb"                            // from `protoc --go_out`
)

func IsAdult(u *userpb.User) bool { return u.GetAge() >= 18 }   // USER CODE

func bytesFromMem(ptr, length uint32) []byte {
    if length == 0 { return nil }
    return unsafe.Slice((*byte)(unsafe.Pointer(uintptr(ptr))), int(length))
}

//go:wasmexport isAdult
func isAdult(ptr, length uint32) uint32 {
    var u userpb.User
    if err := proto.Unmarshal(bytesFromMem(ptr, length), &u); err != nil { return 0 }
    if IsAdult(&u) { return 1 }
    return 0
}
```

Host side (the C++ trampoline, modeled by `host_protocase.py`): serialize the
`acme.User` → protobuf wire bytes, `celfn_realloc(0,0,1,len)`, write bytes into
module memory at the returned ptr, call `isAdult(ptr,len)`, read the bool.

## How celfnc generates this (the model — answers "how does mini-wit-bindgen work")

The two templates above are the *output*. The generator is a **pure
function `CelfnDecl → Go source`** — no new analysis; everything it needs
is already on the parsed struct (`compiler_v2/celfn/function_library.{h,cc}`):
`fn_name`, `module_name` (alias), `overload_id`, `params`, `return_type`.

The algorithm: **iterate `library.decls()`, keep `kForeign`, and for each
instantiate one Go template by switching on each param's and the
return's `CelfnType::Kind`** per the §5.3 storage table — the *same*
table the host trampoline's recursive Lower/Lift implements (§5.4). Per
decl it emits:

1. **the per-fn export**, named `decl.overload_id` verbatim
   (`//go:wasmexport` — probe-confirmed no mangling), with the flattened
   fixed-ABI params: `kBool`→`(i32)`, `kInt/kUint`→`(i64)`,
   `kDouble`→`(f64)`, `kString/kBytes/kProto/kList/kMap`→`(i32 ptr,i32 len)`;
2. **the lift** of each param from the module's own memory
   (`unsafe.String` / `unsafe.Slice` / `proto.Unmarshal` / recurse for
   aggregates);
3. **the panic-guarded user call** (`defer/recover()` → status), matched
   to the user's `//celfn:export <alias>.<fn>` annotation by
   `module_name.fn_name`;
4. **the result lowering** of `return_type` through the same table;
5. **once-per-module boilerplate**: `celfn_realloc`, the mem helpers, the
   `import` block (adds `…/protobuf/proto` + the generated `…pb` package
   **iff** any param/return `MentionsProto()` — that predicate already
   exists, `function_library.cc:120`), and `func main(){}`.

Reused as-is: `CelfnType::Argkind()` (`function_library.cc:30`) names the
export; `MentionsProto()` gates the proto imports. **The one input not on
`CelfnDecl` today:** the *Go import path* of a `proto(...)` type — it has
`proto_fqn="acme.User"` but not the Go package; the generator needs a
`fqn → Go-package` config (or derives it from the `protoc --go_out`
layout).

**Why it's "a scoped wit-bindgen" (§5.6).** `wit-bindgen` is the
Component-Model code generator: a WIT IDL + the Canonical ABI →
per-language guest+host glue. The mapping:

| | `wit-bindgen` | `celfnc` |
|---|---|---|
| IDL | WIT `.wit` | `.celfn` (`CelfnDecl`) |
| ABI seam | full Canonical ABI (`cabi_realloc`, `(ptr,len)`, return area, spill) | §5.3 subset; `celfn_realloc` *is* `cabi_realloc` |
| Guest glue | generated per language | generated per language ← **this is celfnc** |
| Host glue | generated per language | **one hand-written C++ trampoline** (`cel_call_foreign`), dispatches on CEL type |
| Types | records/variants/resources/option/result | CEL scalars/string/bytes/list/map/Duration/Timestamp (+ proto-by-bytes) |

So `celfnc` is exactly the **guest half** of a `wit-bindgen` bind, with
§5.3 standing in for the Canonical ABI and a single hand-written
trampoline replacing the generated host half — which is why adding a
language is guest-shim-generation only. Endgame (§11): if wasmtime's
Component Model is adopted wholesale, `celfnc` collapses into real
`wit-bindgen` + a `.wit` projection of `CelfnDecl`; the proto-by-bytes
arm (§8.5) has no WIT equivalent and stays a celfnc extension
(`list<u8>` + a `proto.Unmarshal` wrapper).

### Can we reuse the real `wit-bindgen`? — verdict: not for v1

Partially, and not for free — it's a *migration*, not a drop-in. You'd
reuse `wit-bindgen`'s mature per-language **guest** templates, but only
after: (a) writing a `CelfnDecl → .wit` projection (it takes `.wit`, not
`.celfn`); (b) bolting a proto extension on top — **`proto(...)` has no
WIT type**, so the §8.5 path (the single most valuable thing celfnc
does) can only be `list<u8>` + a hand-written `proto.Unmarshal` wrapper
regardless. Two hard blockers make it expensive for v1:

  1. **`wit-bindgen` produces Component Model *components*, not the core
     wasip1 *modules* §5.2/`AddModule` assume.** A component hides its
     memory behind the canonical-ABI machinery; our host trampoline
     pokes an exported `memory` + `celfn_realloc` directly. Adopting it
     means a full Component-Model host runtime — a different architecture
     than what the probe validated.
  2. **The proto path the probe proved (stock-Go `wasip1` +
     `proto.Unmarshal`) has no component story.** Stock Go emits core
     modules, not components; TinyGo emits components but **can't run
     `proto.Unmarshal`** (the `reflect.NewAt` trap). The toolchain that
     could feed wit-bindgen's component path is the one that can't carry
     proto, and vice-versa — the two findings collide head-on.

What's *already* reused, by design: `celfn_realloc` **is** `cabi_realloc`
(byte-compatible), §5.3/§5.4 **is** the Canonical-ABI flattening scoped
to our types, and the one-IDL→guest-glue + hand-written-host split **is**
wit-bindgen's architecture. **Recommendation:** v1 hand-rolls the
`celfnc` Go template (~80 lines of deterministic codegen over
`CelfnDecl`, per this probe); keep `celfn_realloc`/§5.3 byte-compatible
with `cabi_realloc` so the door stays open; revisit `CelfnDecl → .wit →
wit-bindgen` for the scalar/string/list/map arms only if/when the
Component-Model host is adopted (§11), keeping a celfnc proto extension
alongside. (Neither `wit-bindgen` nor `wasm-tools` is installed on this
machine; the migration claim is reasoned, not yet probe-validated.)

## Panic / trap behavior (validated 2026-05-24)

This answers §5.2 step 4 ("write `kError` for a foreign trap or a contract
violation") concretely for Go. Probe: `paniccase/rules.go` exports failure
modes; `host_paniccase.py` calls each and classifies the outcome. Results:

| Failure mode | Outcome at the host boundary | Instance reusable after? |
|---|---|---|
| explicit `panic("…")` | `Trap`, `TrapCode.UNREACHABLE` | yes (`ok()`→7) |
| nil pointer deref | `Trap`, `TrapCode.UNREACHABLE` | yes |
| index out of range | `Trap`, `TrapCode.UNREACHABLE` | yes |
| panic + shim `recover()` | clean `RET` (sentinel `0xFFFFFFFF`), **no trap** | yes |
| runtime panic + shim `recover()` | clean `RET` sentinel, **no trap** | yes |

Confirmed mechanics:

- **A Go panic is a wasm `unreachable`, not a WASI `proc_exit`.** The backtrace
  is `main.doPanic → runtime.gopanic → runtime.fatalpanic → runtime.abort →
  unreachable`. wasmtime reports `TrapCode.UNREACHABLE`. So the trampoline
  catches it as an ordinary wasm trap (a `wasmtime::Trap` in the C API) — it
  does **not** need to special-case a WASI exit code, and the exit-status path
  is irrelevant here.
- **The host CAN catch it and continue** — no host-side crash; the trap unwinds
  the wasm stack and control returns to the caller (`enter_wasm` raises the
  trap object). The trampoline maps it to `kError`.
- **Instance reuse after an uncaught panic appeared safe** in the probe: after
  a panic-trap, repeated `celfn_realloc` (touches the Go heap/GC) + `ok()`
  calls all returned correctly across many iterations. BUT this relies on Go's
  runtime being re-entrant after an aborted goroutine, which is not a
  contractual guarantee — `fatalpanic` is meant to be fatal. **Do not depend on
  it.** The conservative engine policy: on an uncaught foreign trap, write
  `kError` and **discard/re-instantiate** the foreign module before the next
  Eval (it's a fresh `_initialize` away). Open question flagged below.
- **`recover()` is the path to standardize on.** A shim-side
  `defer func(){ if recover()!=nil { /* error return */ } }()` catches BOTH
  explicit `panic()` AND runtime panics (nil deref validated) and converts them
  to a clean error return with no trap — so the instance is never left in the
  ambiguous post-abort state. **celfnc should wrap every generated per-fn
  export's user-call in this recover guard.** The error signal needs an ABI
  slot, though — see "Recommended generated-shim shape" and open question #2.

### Recommended generated-shim shape (with panic guard)

For a `bool` return, a panic must be distinguishable from `false`. Two options;
the probe used a sentinel, but the cleaner ABI is an out-param status:

```go
//go:wasmexport isAdult
func isAdult(ptr, length uint32) (status uint32) {  // status: 0=ok, 1=foreign-error
    defer func() {
        if r := recover(); r != nil {
            status = 1                                // contract violation -> host writes kError
        }
    }()
    var u userpb.User
    if err := proto.Unmarshal(bytesFromMem(ptr, length), &u); err != nil {
        status = 1; return
    }
    // write the real bool result into a return area / second out slot here;
    // status stays 0. (For a scalar bool the trampoline reads result+status.)
    _ = IsAdult(&u)
    return
}
```

This folds the existing §5.3 "return area" mechanism (already there for
aggregates) into carrying a `(status, value)` pair so a panic or an
`Unmarshal` failure becomes a typed `kError` rather than a masquerading
`false`. The probe's swallow-to-0 (`isAdult` returning 0 on Unmarshal error)
is WRONG for production — it conflates "not an adult" with "bad input".

## Gotchas

1. **TinyGo + proto = runtime trap.** `tinygo build -target=wasip1` *builds*
   the proto module (3.2 MB) but `proto.Unmarshal` **traps at runtime in
   `reflect.NewAt`** (`main!runtime.panicOrGoexit` ← `reflect.NewAt`). TinyGo's
   reflection is incomplete. So the §8.5 proto path is **stock-Go-only**;
   TinyGo remains valid only for scalar/string/bytes/list/map fns. The
   string case ran clean on TinyGo (118 KB, 2 WASI imports).
2. **Proto runtime cost.** Linking the Go protobuf runtime grows the module
   from 1.6 MB → 6.4 MB (+4.7 MB) and from 10 → 17 WASI imports. That's the
   per-foreign-module cost of the serialization path with stock Go.
3. **GC pinning of arg buffers.** `celfn_realloc` returns the address of a
   Go-managed slice's backing array. Go's GC may move/collect it; the glue
   MUST keep it reachable across the host write + the call. The probe uses a
   `map[uint32][]byte` pin table (leaks — fine for single-shot). A production
   celfnc shim should reset the pins per-call (the host copies the result into
   the shared arena synchronously before the next foreign entry — §5.5 — so the
   pin only needs to survive one call edge).
4. **`_initialize` is not optional** (see ABI section) — trap if skipped.
5. **`main(){}` is required** even though it never runs — the reactor build
   refuses to link without it.
6. The Python protobuf runtime on this box was version-skewed
   (`gencode 7.35 vs runtime 6.33`), so cross-validation of the wire bytes was
   done with Go's `proto.Marshal` instead. Not a finding about the design.

## Corrections to propose (do NOT edit the docs — user reviews first)

1. **`modules-and-ffi.md` §5.8 / §5 + `user-guide.md` §8.5** — the proto path
   is real and works, but BOTH should state it requires **stock Go (`GOOS=wasip1`),
   NOT TinyGo**, because TinyGo's reflection traps in `proto.Unmarshal`. The
   §8.3 table currently lists TinyGo as a general foreign target; add a note
   that TinyGo cannot carry the proto-serialization path.
2. **`user-guide.md` §8.4 step 3** — the TinyGo one-liner
   `tinygo build -target=wasi -o rules.wasm ./rules` is missing
   `-buildmode=c-shared`, which is what produces the `//go:wasmexport`
   reactor shape (without it there are no per-fn exports to call). The
   `-target=wasi` alias itself still resolves in TinyGo 0.41.1 (verified), so
   that part is fine. (And per gotcha 1, TinyGo can't carry the §8.5 proto
   variant regardless.)
3. **`user-guide.md` §8.3 + §8.4** — make explicit that the host must wire a
   **full WASI preview1 context** for stock-Go modules (they import
   `fd_write`/`random_get`/`clock_time_get`/`fd_prestat_*`…), not just call
   `_initialize`. The §8.4 prose ("calls `_initialize`… lowers the strings")
   omits the WASI-context requirement that instantiation actually needs.
4. **§8.5 cost note** — quantify the proto-runtime cost: +~4.7 MB module size
   and +7 WASI imports vs a scalar foreign module, reinforcing why it's opt-in.
5. **§5.6 export naming** — confirmed: `//go:wasmexport` names the export
   verbatim, so celfnc emits under the overload id with no mangling. (Doc is
   already correct; this is a positive confirmation, not a correction.)
6. **§5.2 step 4 (panic/trap → kError) + §5.6 (shim duties)** — add the panic
   contract: (a) a Go panic surfaces as `TrapCode.UNREACHABLE`, not WASI exit,
   so the trampoline catches a plain wasm trap → `kError`; (b) the generated
   per-fn shim MUST wrap the user call in `defer/recover()` so panics become a
   typed error return rather than a trap; (c) the fixed ABI needs an error
   channel — a `status` out-slot or sentinel — so a foreign error is
   distinguishable from a legitimate `false`/`0`/empty result (today's
   swallow-to-0 conflates them).

## Open questions surfaced by the panic probe

1. **Instance recovery policy on an uncaught trap.** Reuse appeared safe in the
   probe, but Go's `fatalpanic` is nominally fatal. Decide the engine policy:
   re-instantiate the foreign module after any uncaught trap (safe, costs an
   `_initialize`) vs. trusting reuse (faster, unproven). Recommend
   re-instantiate until proven otherwise — and the recover() shim makes uncaught
   traps rare anyway.
2. **Error channel in the fixed ABI.** §5.3 has no slot for "the foreign call
   failed" distinct from its return value. Needs adding (a leading/trailing
   `status i32`, reusing the return-area mechanism) before the shim's recover()
   has somewhere to report. This is an ABI addition, not just shim codegen.

## Files created (probe artifacts)

- `probes/foreign_go/build.sh` — reproduces everything from scratch.
- `probes/foreign_go/strcase/{rules.go,go.mod,rules.wasm}` — string case.
- `probes/foreign_go/protocase/{user.proto,rules.go,go.mod,go.sum,userpb/user.pb.go,rules.wasm}` — proto case.
- `probes/foreign_go/paniccase/{rules.go,go.mod,rules.wasm}` — panic/trap behavior.
- `probes/foreign_go/host_strcase.py`, `host_protocase.py`, `host_paniccase.py` — wasmtime-py host harnesses (stand-in for the C++ `cel_call_foreign` trampoline).
