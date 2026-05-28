# Foreign functions — calling an independent Go/Rust wasm module from CEL

Status: plan — drafted 2026-05-27, not yet started. Detailed design.

> **Scope.** A CEL expression calls a function whose body lives in a
> *separate* wasm module the embedder compiled from **Go** or **Rust**
> (`<alias>.fn(...)`). A foreign module has its **own** linear memory, so it
> cannot read `cel.memory` directly. The design resolution: **foreign uses
> the *same* `CelValue` slot ABI as `@host`/`@native` — it is "`@host`, but
> in its own memory, reached by a host mirror-copy."** There is **no bespoke
> canonical ABI and no error channel**: a host trampoline mirror-copies the
> argument `CelValue`s (and their reachable payloads) into the foreign
> module's memory, calls the export with slot offsets, and copies the result
> `CelValue` back. Errors and unknowns ride in the `CelValue` kind
> (`CEL_ERROR` / `CEL_UNKNOWN`), exactly as in every other backend.

> **The front half is free.** Type-checking of foreign call sites works
> *today* — the `.celfn` IDL declaration drives cel-cpp's checker for every
> backend (§6). This milestone is the **runtime/codegen half**.

This doc is the detailed, decision-resolved design. The original FFI
narrative is `modules-and-ffi.md §5`; the empirical Go results are
`foreign-go-bindgen-findings.md`; the `celfn`-generation sub-design is
`celfn-go-bindgen-design.md`; the embedder UX is `index.md §8`. **Where
this doc and those disagree, this doc wins** — in particular it replaces
the canonical-ABI + packed-status-error-channel design (`modules-and-ffi.md
§5.3`, `celfn-go-bindgen-design.md §5`) with the unified `CelValue`-slot
convention below. **Review before any code.**

---

## 0. Read order

1. `index.md §8` — the embedder UX (target). 2. **This doc** — the
engineering design. 3. `foreign-go-bindgen-findings.md` — the empirical Go
facts. 4. `celfn-go-bindgen-design.md` — the `celfn --target=go` sub-design
(its §5 error channel is superseded here; §3 three-file layout stands).

---

## 1. The problem, and the one convention

Every in-module helper, builtin, `@host` import, and `@native` body speaks
**one convention**: `(i32 out_slot, i32 arg0, …, i32 argN) → ()`, where each
`i32` is a byte offset to a 24-byte `CelValue` in the shared `cel.memory`.
Nothing is copied — every party reads the same memory, and `CEL_ERROR` /
`CEL_UNKNOWN` are first-class `CelValue` kinds, so 3VL needs no side channel.

A foreign module has its **own** linear memory, so a `cel.memory` offset
addresses unrelated bytes there. The resolution is *not* a new ABI — it is
to keep the same convention and have the host **mirror-copy** the slots
across the memory boundary:

### 1.1 Foreign = `@host`, in its own memory, reached by a mirror-copy

| Backend | Body in | Memory | Convention | Who copies the slots |
| --- | --- | --- | --- | --- |
| `@host` | embedder C++ | shared `cel.memory` | `CelValue` slot ABI | nobody (shared) |
| `@native` | the expr module (local func) | shared `cel.memory` | `CelValue` slot ABI | nobody (shared) |
| **foreign** | **separate Go/Rust module** | **its OWN memory** | **same `CelValue` slot ABI** | **the host trampoline (mirror-copy)** |

The foreign export signature is therefore identical to a runtime helper —
`<overload_id>(out_slot, arg0_slot, …, argN_slot) → ()` — except the slots
are offsets into the **foreign module's own** memory. The generated guest
stub reads `CelValue`s there with the *same* decode the runtime uses, calls
the embedder's natural function, and writes a `CelValue` (the result, or
`CEL_ERROR`) back to `out_slot`.

```
   expr module                host (C++ / wasmtime)              foreign module
   (cel.memory)                                                  (own memory)
   ───────────                ─────────────────────              ────────────
   call cel_call_foreign ──►  cel_call_foreign trampoline
   (fn_id, out_slot,           1. celfn_reset() ─────────────────►  (bump cursor → base)
    arg_slots_ptr, n)          2. mirror-copy each arg CelValue
                                  (+ payload/aggregate, relocating
                                   offsets) into foreign memory ───►  (writes via
                                                                       celfn_realloc)
                               3. call <overload_id>(f_out, f_a0…) ►  reads CelValue slots,
                                                                       calls user fn, writes
                                                                       a CelValue to f_out
                               4. mirror-copy the f_out CelValue ◄──  (incl. CEL_ERROR/UNKNOWN)
                                  back into a cel.memory arena slot
                               5. uncaught trap → write CEL_ERROR
   result at out_slot   ◄────  (out_slot written; next call resets)
```

There is **no canonical ABI, no flattened params, no `ret_area`, and no
status word**. The expr module deals only in `cel.memory` slots + one host
import (like `@host`); all cross-memory work is the trampoline's mirror-copy.

---

## 2. Inventory — proven / designed / built / unbuilt (2026-05-27)

**PROVEN (probes under `probes/foreign_go/`,
`foreign-go-bindgen-findings.md`):** the host **writes into the Go module's
memory and the Go side reads it**, and the Go side **writes results the host
reads back** — which is exactly the mirror-copy primitive this design needs
(probes `strcase/`, `protocase/`, `layered/`). Stock-Go `wasip1
-buildmode=c-shared` exports `//go:wasmexport <name>` verbatim, exports
`memory` + a `cabi_realloc`-shaped `celfn_realloc`, is a reactor
(`_initialize` mandatory), and imports a full `wasi_snapshot_preview1`
surface. An uncaught Go panic → wasm `unreachable` → host trap; a
`defer/recover()` shim catches it. GC pinning of host-written bytes must be
handled (per-call reset — §3.5).

**DESIGNED:** the ABI/FFI narrative (`modules-and-ffi.md §5`); the three-file
`celfn` Go layout (`celfn-go-bindgen-design.md §3`). *Superseded here:* the
canonical-ABI storage table (`§5.3`) and the packed-status error channel
(`celfn-go-bindgen-design.md §5`) — replaced by §3 below.

**BUILT:** `Engine::AddModule(alias, bytes)` (validate/parse/snapshot
exports); `kForeign` decls route to `ImportModule::kUserModule`
(`compile.cc`); proto-boundary rejection at type-check
(`function_library.cc:192-209`); the engine already special-cases the
`wasi_snapshot_preview1` alias (`engine.cc:378`) and calls `_initialize` on a
foreign instance (`:513-524`).

**UNBUILT — this milestone:** (1) caller-side codegen for a `kForeign` call
(§4), (2) the `cel_call_foreign` mirror-copy trampoline (§3), (3) Plan-time
instantiation + WASI + dispatch table (§5), (4) `celfn --target=go/rust`
generating the `CelValue`-decoding stub (§8), (5) the proto path (§7,
opt-in).

---

## 3. The ABI — the unified `CelValue` slot convention

### 3.0 The foreign-module ABI contract (what every module exports)

| Export | Signature | Role |
| --- | --- | --- |
| `memory` | — | the module's own linear memory; the host mirror-copies into it |
| `celfn_realloc` | `(ptr, old, align, new) -> ptr` | bump allocator, `cabi_realloc` shape (proven) |
| `celfn_reset` | `() -> ()` | reset the per-call bump cursor; **host calls first, each call** (§3.5) |
| `<overload_id>` (one per fn) | `(out_slot, arg0_slot, …, argN_slot) -> ()` | the function; reads `CelValue` arg slots, writes a `CelValue` to `out_slot` (incl. `CEL_ERROR`/`CEL_UNKNOWN`) |
| `_initialize` | `() -> ()` | **iff a reactor** (stock-Go wasip1); host calls once post-instantiation |

Plus the toolchain's WASI imports (full preview1 for stock Go; none for Rust
`wasm32-unknown-unknown`). **No status return, no `ret_area`** — the result
is a `CelValue` at `out_slot`, and its kind carries success/error/unknown.
All slot offsets are into the **module's own** memory.

### 3.1 `cel_call_foreign` (the host trampoline)

A `cel_host.*` host function, registered like the m21 trampolines:

```wat
(import "cel_host" "cel_call_foreign" (func $cel_call_foreign (param i32 i32 i32 i32)))
;;   cel_call_foreign(fn_id, out_slot, arg_slots_ptr, num_args) -> ()
```

`fn_id` is a dense per-program id resolved at Plan time to `{foreign
instance, export func, memory, realloc, reset, IDL signature}` (§5).
`arg_slots_ptr` → a compile-time rodata `i32[num_args]` of the args'
`cel.memory` slot offsets (zero caller-side copies). Per call the trampoline:

1. `celfn_reset()` on the foreign instance;
2. for each arg, **mirror-copy** the `cel.memory` `CelValue` (and its
   reachable bytes/subtree, relocating offsets — §3.2/§3.3) into the foreign
   memory at a freshly-`celfn_realloc`'d slot;
3. call `<overload_id>(f_out_slot, f_arg0_slot, …)` with the foreign slot
   offsets;
4. **mirror-copy** the `CelValue` the guest wrote to `f_out_slot` back into a
   `cel.memory` arena slot (relocating, §3.3) and into the caller's
   `out_slot`;
5. an uncaught wasm trap (the `recover()` shim missed it) → write `CEL_ERROR`
   to `out_slot` and re-instantiate the module (§5.3).

### 3.2 Mirror-copying one `CelValue` — scalars and strings

The `CelValue` is a 24-byte tagged union (`runtime/cel_data.h:142`). Copying
across memories is **kind-driven**:

- **Scalars** (`bool`/`int`/`uint`/`double`/`Duration`/`Timestamp`/`null`):
  the value is fully inline in the 24 bytes → a **plain 24-byte memcpy**, no
  payload, no relocation.
- **`string`/`bytes`** (`CelSpan{ptr,len}`): memcpy the 24-byte `CelValue`,
  then `celfn_realloc(len)` in the foreign memory, copy the `len` payload
  bytes there, and **rewrite the copied `CelValue`'s `s.ptr`** to the foreign
  offset. One pointer fix.
- **`CEL_ERROR`/`CEL_UNKNOWN`**: inline → plain memcpy (this is how a guest
  result error/unknown rides back with no side channel).
- **aggregates** (`list`/`map`) and **`proto`**: §3.3 and §7.

### 3.3 Lists and maps — recursive serialize + relocate

This is the one case that is not a memcpy. A `list`/`map` `CelValue` points
into a tree of **absolute** linear-memory offsets, so it is rebuilt in the
foreign memory with the offsets relocated. The concrete arena layout
(`cel_data.h`):

```
list CelValue {CEL_LIST_ARENA, arena_list.header_ptr = H}
   H → ArenaListHeader { count=n, capacity, elements_offset = E, _pad }   (16 B)
   E → n × 24-byte CelValue   (the elements, back-to-back)

map  CelValue {CEL_MAP_ARENA, arena_map.header_ptr = H}
   H → ArenaMapHeader  { count=n, capacity, entries_offset  = E, _pad }   (16 B)
   E → n × 48-byte { key:CelValue, val:CelValue }   (pairs, back-to-back)
```

**Lower (host → foreign), one recursive pass** `MirrorCelValue(cv, src,
dst, dst_alloc)`:
1. allocate the header (16 B) + the element/entry run (`n×24` / `n×48`) in
   the foreign memory via `celfn_realloc`;
2. copy each element/entry `CelValue` and **recurse** on it — a `string`
   element copies its payload and fixes `s.ptr`; a nested `list`/`map`
   recurses and fixes its `header_ptr`; a scalar is a plain copy;
3. write the foreign `ArenaListHeader`/`ArenaMapHeader` with
   `elements_offset`/`entries_offset` pointing at the foreign run;
4. set the result slot `CelValue` to `{CEL_LIST_ARENA/CEL_MAP_ARENA,
   header_ptr = <foreign header offset>}`.

This is a **single generic routine** driven by `CelValue` kinds — not
per-user-type. It is the same recursion a canonical-ABI lowering would need;
it merely preserves the `CelValue`/arena shape instead of transcoding, which
is why the guest can decode it with the runtime's own arena walk.

**Host-backed aggregates** (`CEL_LIST_HOST`/`CEL_MAP_HOST`, a `ref_slot` into
the externref table — data in C++ host objects, not linear memory) are
**materialized first**: walk the `HostListBacking`/`HostMapBacking`
(`cel_host.h`; `.Size()`/`.At(i)`/`.Get(k)` → element `CelValue`s) and lower
each into the foreign arena run. So host- and arena-backed aggregates funnel
through the same recursive lower.

**Return direction** is symmetric: the guest builds an arena `list`/`map` in
its **own** memory (generated `lowerList`/`lowerMap` — §8) and writes
`{CEL_LIST_ARENA, header_ptr}` to `out_slot`; the host's `MirrorCelValue`
copies that subtree back into the **cel arena**, relocating offsets, and
writes the result `CelValue` to `cel.memory`.

> **Cost (the honest trade).** Aggregates are *fatter* on the wire (full
> 24-byte `CelValue`s per element vs a packed canonical array) and require
> the recursive relocate — the one place foreign is not a memcpy. It is
> bounded, generic (one routine), and reuses the arena format; v1 foreign
> policy functions rarely pass deep aggregates. Scalars/strings — the common
> case — stay near-memcpy.

### 3.4 Errors and unknowns — no channel

Because the result is a `CelValue`, a foreign failure is just a `CEL_ERROR`
`CelValue` the guest writes to `out_slot`, and an unknown is `CEL_UNKNOWN` —
identical to every other backend's 3VL. The `celfn` `recover()` shim, on a
caught panic or a decode failure, **writes `CEL_ERROR` to `out_slot`** and
returns normally (no status bit, no `ret_area`). The host distinguishes a
genuine `false` from a failure for free: `false` is `{CEL_BOOL, 0}`, a
failure is `{CEL_ERROR, …}`. The only thing the host maps itself is an
**uncaught** trap (the rare panic the shim missed) → `CEL_ERROR` +
re-instantiate (§5.3) — which it does for every backend. **This deletes the
canonical-ABI status word / packed-bit entirely** (superseding
`celfn-go-bindgen-design.md §5`).

### 3.5 Lifetime — bump + host-driven reset

Foreign-memory allocations (mirrored args, payloads, aggregate subtrees, and
the guest's result) come from a **per-call bump arena the host resets via
`celfn_reset()` before it mirror-copies the args** (NOT a shim-entry reset —
that would clobber the just-written args mid-read). Reclamation is the cursor
move; no per-allocation frees. The bump arena is one long-lived allocation
(never freed/moved) → no GC relocation hazard for host-written bytes. The
host copies each result `CelValue` *out* into the per-Eval **cel arena**
before the next call resets, so foreign bytes are never needed after the
call. **Invariant:** v1 foreign functions are pure leaf computes (no callback
into CEL) → a single bump frame can't be re-entered. *(Probe item: confirm
the bump+reset shim for Go and Rust — §10.)*

---

## 4. Caller-side codegen (`expr_lower`) + the WAT

For a `kCallExpr` resolving to `ImportModule::kUserModule` (foreign), the
kCall arm emits a rodata `i32[num_args]` of the operands' `cel.memory` slot
offsets (compile-time constants) and a `cel_call_foreign` call. **Unchanged
from the canonical design — the expr side never knew about the foreign ABI.**

### Worked example — `rules.allow(subject, "read")` (Go `strcase`)

```wat
;; bool rules.allow(subject, "read") — foreign call, fn_id = 0
(call $cel_call_foreign
      (i32.const 0)        ;; fn_id  → {rules instance, allow_string_string, sig}
      (i32.const <S_out>)  ;; out_slot in cel.memory: result CelValue lands here
      (i32.const <A>)      ;; arg_slots_ptr → rodata [S_subj, S_read]
      (i32.const 2))       ;; num_args
;; result CelValue at S_out
```

The trampoline `celfn_reset()`s `rules.wasm`, mirror-copies the two `string`
`CelValue`s (24 B each + their bytes, fixing `s.ptr`) into `rules.wasm`
memory, calls `allow_string_string(f_out, f_subj, f_act)`, and mirror-copies
the `bool` (or `CEL_ERROR`) `CelValue` from `f_out` back to `S_out`. Codegen
output matches the §10 WAT byte-for-byte.

---

## 5. fn_id, the dispatch table, instantiation, WASI, `_initialize`

### 5.1 fn_id (compile time)
`BuildOverloadTable` assigns each foreign overload a dense `fn_id` `[0..F)`
and emits a `cel.abi` **foreign-call table**: per `fn_id`,
`{alias, export_name = overload_id, arg CEL types, return CEL type}`. The run
side is then self-describing for foreign linkage (`index.md §3.3/§9.1`).

### 5.2 Plan-time dispatch table (run time)
At `Plan`, for each `fn_id`: find the registered module by `alias`;
**instantiate it** with a (sandboxed — §5.4) WASI context; bind its `memory`,
`celfn_realloc`, `celfn_reset` handles; **call `_initialize`** iff exported;
resolve `export_name` → `wasmtime_func_t`; store
`{instance, func, memory, realloc, reset, arg types, ret type}` at
`table[fn_id]`. `cel_call_foreign` indexes `table[fn_id]` — O(1).

### 5.3 Instance recovery on an uncaught trap
The `recover()` shim makes uncaught traps rare; on one that escapes, the
trampoline writes `CEL_ERROR` and **re-instantiates** the module before the
next call (fresh `_initialize`; Go's `fatalpanic` is nominally fatal).

### 5.4 WASI and threading — three layers, kept separate
1. **The CEL runtime/expr side uses `wasm32-wasi-threads` — mostly an
   accident of cctz, and it is what makes the memory *shared*.** `-threads`
   was forced by `absl::time`/cctz needing `<mutex>`
   (`runtime/BUILD.bazel:565-569`; `wasi/DESIGN.md` Phase C delta), not by
   concurrency. Its consequence is load-bearing: `-threads` mandates a
   **shared** linear memory, so the runtime exports `(memory 4 1024 shared)`
   and the expr module imports it matching-shared — the substrate the slot
   ABI rides on. The runtime instance also takes a small WASI context for the
   `wasi_snapshot_preview1` surface absl/wasi-libc pull in (unused imports
   dead-stripped). Effectively single-threaded (one `Instance` per worker).
2. **A foreign module has its OWN WASI and its OWN (non-shared) memory** —
   the premise. Stock Go imports a full preview1 surface and is a reactor;
   each foreign module is a separate instance the host gives its own WASI
   context (hooks: `engine.cc:378`, `:513-524`). Rust `wasm32-wasip1` is
   similar; Rust `wasm32-unknown-unknown` needs **no WASI / no `_initialize`**.
3. **Foreign modules do NOT use `wasi-threads` / shared memory** — they are
   single-threaded leaf computes; their memory is private and non-shared,
   which is *why* the trampoline mirror-copies. (Distinct from the deferred
   `@native` Model-B in `§4.5` of `modules-and-ffi.md`, where CEL-defined
   library modules *do* share the runtime memory via `__memory_base`.) One
   store holds the runtime's shared memory and a foreign module's non-shared
   memory with no conflict.

**Sandboxing / determinism (m22 owns it).** Foreign WASI is a capability +
non-determinism surface. Each foreign instance gets a **deliberately scoped**
WASI context: **no preopened dirs** (deny fs), no sockets, `fd_write` →
captured/discarded sink (panic text), **host-controlled clock/RNG** (deny or
deterministically seeded). Critical for a policy engine: identical inputs →
identical verdict. Each foreign instance's WASI is isolated from the
runtime's and from other foreign modules'.

---

## 6. Type-checking & the IDL — already working (backend-agnostic)

A foreign decl carries an `<alias>.` prefix (no `@` sigil):

```celfn
/// True if `subject` may perform `action`, per the Go policy module.
bool rules.allow(string subject, string action);     // alias = "rules"
```

`ParseCelfnSource` → `CelfnDecl{backend=kForeign}`. Registered on the
`Compiler`, it becomes a cel-cpp checker overload: `RegisterCelfnLibraries`
(`parse_and_check.cc:779-805`) builds a `cel::FunctionDecl` via
`MakeFunctionDecl`, adds each shape as a `cel::OverloadDecl`
(`BuildOverloadFromCelfn`, :759) with the synthesized **overload-id** (the
string codegen keys on), and registers it via
**`TypeCheckerBuilder::AddFunction`** (:805). So `rules.allow(subject,
"read")` **type-checks today** — identical machinery to `@host`/`@native`;
the backend changes codegen routing only, never checking.

Boundary rejection is a **type-check error**: `MentionsProto()` rejects
`proto(...)`, `list<proto…>`, `map<…,proto…>` for a `kForeign` decl
(`function_library.cc:192-209`) — a `msg_slot` handle is meaningless across
memories (until §7 lifts it via serialization).

---

## 7. Proto across the foreign boundary — host-push serialization (opt-in)

The blanket rejection is a usability gap, and it can be lifted cheaply on the
host side (the host holds the live `Message*` + the descriptor pool;
`SerializeToString()` is free). The `protocase/` probe proved the round trip.
Under the unified convention a proto arg becomes a **`CEL_BYTES` `CelValue`**
(the serialized wire bytes), mirror-copied like any bytes; a returned proto is
bytes the host `ParseFromString`s against the descriptor. The cost is on the
**guest decode** side, giving two staged strategies:

- **Strategy 1 (probe-proven, Go):** the guest links a proto runtime and
  `Unmarshal`s the bytes. Language-agnostic wire; **+4.7 MB**, stock-Go-only
  (TinyGo reflection traps).
- **Strategy 2 (wit-bindgen-style, strategic):** `celfn` reads the proto
  **descriptor** and generates field-by-field lift/lower into a mirror
  struct — no proto runtime in the guest → TinyGo + Rust `no_std` reach. Real
  generator work + a fidelity risk (presence/defaults/`repeated`/`map`/
  `oneof`/nested/WKT). This is the record/variant codegen wit-bindgen is
  built around; we borrow its generator *structure*, not the Component Model.

Proto stays **opt-in**; until a strategy lands, the type-check rejection
stands and is the documented behavior.

---

## 8. `celfn --target=go` — generate the `CelValue`-decoding stub

**Goal: `celfn --target=go policy.celfn` emits a buildable Go package; the
embedder fills in only the function bodies.** All glue — the slot-ABI
exports, the `CelValue` decode/encode (incl. arena walks), `celfn_realloc`/
`celfn_reset`, `_initialize`, the panic guard — is generated. Three files
(probe-validated by `probes/foreign_go/layered/`,
`celfn-go-bindgen-design.md §3`):

| File | Generated | Overwritten on regen? | Contents |
| --- | --- | --- | --- |
| `celfn_abi.go` | once, identical for all modules | yes (safe) | `celfn_realloc`, `celfn_reset`, the **`CelValue` codec + arena walk** (a generated mirror of `cel_data.h`), `guard()` (recover), `main(){}` |
| `celfn_exports.go` | from the IDL | yes | one `//go:wasmexport <overload_id>` per `kForeign` decl |
| **`rules.go`** | once, as `//TODO` stubs | **never** | the user's natural Go function bodies |

Each generated export, given `(out_slot, arg0_slot, …) -> ()`:
1. reads each arg `CelValue` from its own memory via the generated codec, and
   **lifts** it to a native value (scalar; `liftString`; `liftList`/`liftMap`
   arena walk → Go slice/map; proto via §7);
2. calls the user's `rules.go` function inside `guard(...)`;
3. **encodes** the result `CelValue` into `out_slot` (scalar inline; string
   via `celfn_realloc`; aggregate via the generated arena builder);
4. on a recovered panic / decode error, writes `CEL_ERROR` to `out_slot`
   (no status word).

The generator is a **pure function** `FunctionLibrary → {Go files}`,
switching on `CelfnType::Kind` (reusing `Argkind()` for export names,
`MentionsProto()` for proto imports). The one input not on the IDL is the
proto `fqn → (Go import, type)` map (`--proto-map` flag). `--target=rust` is
the sibling (§9.2). CLI form aligns with `cel-cli-design.md`'s `celfn gen`.

> **Delta vs `celfn-go-bindgen-design.md`:** §3's three-file layout stands;
> but the per-export body now **reads/writes `CelValue` slots** (the unified
> convention), not flattened params, and there is **no packed-status return**
> (§3.4 here supersedes that doc's §5). `celfn_abi.go` grows the `CelValue`
> codec + arena walk; that doc is updated when F4 lands.

---

## 9. Toolchains — Go (proven) and Rust (to probe)

**Go** (`GOOS=wasip1 -buildmode=c-shared`, reactor): own memory;
`_initialize` mandatory; full WASI; `//go:wasmexport` verbatim; `main(){}`
required. Proto = stock-Go only (~6.4 MB); TinyGo OK for scalar/string
(~118 KB) but not proto.

**Rust** — milestone owns the probe (`probes/foreign_rust/`). Candidates:
`wasm32-unknown-unknown` (`no_std`, no WASI, no `_initialize`; smallest;
likely the default) and `wasm32-wasip1` (std + WASI). Confirm export-name
shape, `celfn_realloc`/`celfn_reset`, WASI surface, the bump shim, and
panic→trap→`CEL_ERROR` (`panic = "abort"` → `unreachable`; `catch_unwind`
where available). Record in `foreign-rust-bindgen-findings.md`. **No Rust
shim from memory.**

---

## 10. WAT-first plan

The existing `m13_p1_{caller,rules_stub}.wat` are the *degenerate* shared-
memory shape. The real artifacts:
- **`m22_foreign_call_caller.wat`** — the expr side: rodata `i32[n]` of arg
  slots + `cel_call_foreign(fn_id, out_slot, arg_slots_ptr, n)`. (§4.)
- **A `cel_call_foreign` stub in `wat_runner`** — reads the arg-slot
  `CelValue`s and writes a result `CelValue` to `out_slot`, simulating the
  mirror-copy trampoline before it exists. Mirrors the existing `cel_host.*`
  stub pattern.
- **`m22_foreign_module_min.wat`** — a hand-WAT foreign module with its
  **own** `(memory …)`, `celfn_realloc`/`celfn_reset`, and a
  `(out_slot, arg0_slot) -> ()` export that reads/writes `CelValue` slots
  (incl. a `CEL_ERROR` path). Drives the F2/F3 two-instance harness verifying
  the real mirror-copy against the stubbed baseline.

> **Harness extension owned here:** `wat_runner` is single-shared-memory
> today; foreign needs (a) the `cel_call_foreign` stub, and (b) a **second
> instance with its own memory** for the cross-memory mirror-copy + aggregate
> relocate test.

---

## 11. Build slices

- **F0 — Freeze the convention (WAT).** `m22_foreign_call_caller.wat` + the
  `cel_call_foreign` stub + a green `wat_runner` test; confirm the
  `(out_slot, arg_slots…) -> ()` + `CEL_ERROR`-via-`CelValue` shape (no
  status word).
- **F1 — Caller-side codegen.** Emit the rodata arg-slot array +
  `cel_call_foreign`; assign `fn_id`s; emit the `cel.abi` foreign-call table.
  Byte-matches F0.
- **F2 — `cel_call_foreign` mirror-copy trampoline.** `MirrorCelValue`:
  scalar memcpy, string payload+ptr-fix, **recursive list/map relocate**
  (§3.3), host-backed materialization, `CEL_ERROR`/trap mapping. Unit-test
  per kind over a fake foreign memory + the `m22_foreign_module_min.wat`
  two-instance harness.
- **F3 — Plan-time linking + sandboxed WASI + dispatch table** (§5).
  End-to-end with the real Go `strcase` module.
- **F4 — `celfn --target=go`.** The three-file generator; the `CelValue`
  codec + arena walk in `celfn_abi.go`; round-trip `strcase`.
- **F5 — Rust probe + `celfn --target=rust`.** §9.
- **F6 — Proto Strategy 1 (wire bytes, stock-Go).** Opt-in; host
  serialize/parse; guest `Unmarshal`. Fixture `protocase/`.
- **F7 — Proto Strategy 2 (structural, wit-bindgen-style).** TinyGo + Rust
  reach. May split.

---

## 12. Test matrix

Per-type, lower (arg) **and** lift (return), plus negatives (m21 discipline).

**Type matrix** (F2 unit + F3 e2e): `bool/int/uint/double/string/bytes/
Duration/Timestamp`; `list<T>` (empty/1/many/**nested**) and `map<K,V>`
(each key kind; missing key) — exercising the **recursive relocate** both
ways, and **host-backed** vs **arena-backed** aggregates; boundaries
(`INT64_MIN/MAX`, `UINT64_MAX`, empty/embedded-NUL/multi-byte strings).

**Errors/unknowns (no channel):** a guest that writes `CEL_ERROR` to
`out_slot` → the caller sees `kError`, **not** a spurious `false` (the test
that the kind-tag *is* the channel); a guest panic (explicit/nil-deref/OOB) →
`recover()` writes `CEL_ERROR` → `kError`; an **uncaught** trap → `kError` +
re-instantiate (§5.3); a `CEL_UNKNOWN` result propagates as unknown.

**Boundary/negatives:** `proto`/`type`/`optional` at a foreign decl →
type-check error (until §7); missing/duplicate alias, malformed bytes →
`AddModule`/`Plan` error; `_initialize` skipped → caught; `fn_id`/arity
mismatch → error.

**Toolchains:** Go `strcase` e2e (F3); Rust `strcase` (F5); both via
generated `celfn` glue; proto via `protocase/` (F6). Fixtures:
`probes/foreign_go/*`.

---

## 13. Decisions (resolved) + open questions

**Resolved:**
1. **Foreign uses the unified `CelValue` slot ABI** — "`@host` in its own
   memory, reached by a mirror-copy." No canonical ABI, no flattening, no
   `ret_area`. (Supersedes `modules-and-ffi.md §5.3`.)
2. **No error channel** — `CEL_ERROR`/`CEL_UNKNOWN` ride in the result
   `CelValue`; the `recover()` shim writes `CEL_ERROR`. (Supersedes
   `celfn-go-bindgen-design.md §5`.)
3. **`cel_call_foreign` is a host mirror-copy trampoline**; args via a
   compile-time rodata slot-offset array; the foreign export is a fixed-arity
   `(out_slot, arg…) -> ()`.
4. **Aggregates: one recursive `MirrorCelValue` relocate** reusing the arena
   format; host-backed materialized first.
5. **Per-call bump + host-driven `celfn_reset`** (§3.5).
6. **Proto: opt-in, host-push serialization → `CEL_BYTES`, staged** (§7).
7. **Sandboxed, deterministic foreign WASI** (§5.4).

**Open (decide in the owning slice):**
- The **coupling trade**: the foreign module speaks `cel_data.h`'s binary
  layout, so a layout change means regenerate + rebuild every foreign module.
  Accepted (we own the `celfn` pipeline); **version the wire format** so a
  bump is detectable. (vs the canonical ABI's decoupling — given up for
  uniformity + the free error channel.)
- **Arena offset encoding** — absolute (relocate on copy, as today) vs
  relative-to-subtree (flat memcpy, no per-node relocate). Relative would
  make aggregate copy a memcpy; evaluate in F2.
- **Rust default target** (`unknown-unknown` vs `wasip1`) — after F5.
- **Proto Strategy 2 fidelity** (oneof/WKT coverage) — F7.

---

## 14. References

- `modules-and-ffi.md §5` — original FFI narrative (canonical ABI superseded
  here).
- `foreign-go-bindgen-findings.md` — empirical Go facts + probes.
- `celfn-go-bindgen-design.md` — `celfn --target=go` sub-design (§3 layout
  stands; §5 error channel superseded by §3.4 here).
- `probes/foreign_go/{strcase,protocase,paniccase,layered}/` — runnable Go
  experiments + harnesses.
- `index.md §8` — embedder UX.
- `runtime/cel_data.h` — `CelValue` (24 B), `ArenaListHeader`/`ArenaMapHeader`
  (the arena layout §3.3 relocates).
- Code anchors: `engine.{h,cc}` `AddModule` / `_initialize` / wasi alias;
  `compile.cc` `BuildOverloadTable`; `function_library.cc` `MentionsProto`;
  `parse_and_check.cc:759-805` (overload registration with cel-cpp).

---

## 15. Appendix — canonical ABI vs `CelValue` ABI, worked

This records *why* §3 chose the `CelValue` slot convention over the
canonical (wit-bindgen-style) ABI it supersedes, so a future reader sees the
trade rather than re-deriving it. One function — `bool allow(string subject,
string action)` — crossing the boundary each way.

### 15.1 The foreign export

**Canonical ABI** — args flattened to native reps; result needs a
return-area; a bare `i32` bool has no kind tag, so errors need a side-channel
status word:

```wat
(func (export "allow_string_string")
      (param $subjPtr i32) (param $subjLen i32)   ;; string → (ptr,len)
      (param $actPtr  i32) (param $actLen  i32)   ;; string → (ptr,len)
      (param $ret_area i32)                        ;; where to write the bool
      (result i32))                                ;; i32 status: 0=ok, !=0=error
```

**`CelValue` ABI** — a runtime helper: slot offsets in, a `CelValue` out:

```wat
(func (export "allow_string_string")
      (param $out_slot i32)                        ;; write a CelValue here
      (param $subj_slot i32) (param $act_slot i32));; read CelValues here
      ;; no result — success/error/unknown is the KIND TAG of *out_slot
```

### 15.2 What differs

| | Canonical ABI | `CelValue` ABI |
| --- | --- | --- |
| what crosses | per-type native rep (scalar; string `(ptr,len)`; list packed `(ptr,count)`) | the **24-byte `CelValue`** + its reachable payload, verbatim |
| host marshalling | **per-type `Lower`/`Lift`** (transcode) | one **generic `MirrorCelValue`** (memcpy + payload copy + offset fix) |
| error / unknown | **separate status word** (value can't carry it) | the **`CelValue` kind** (`CEL_ERROR`/`CEL_UNKNOWN`) — free |
| guest stub | natural params → value + status | reads/writes `CelValue`s, decodes by kind |
| aggregate | packed canonical array (lean, no relocation) | arena subtree of full `CelValue`s (fatter, recursive relocate) |
| coupling | decoupled from `cel_data.h` (wit-bindgen-portable) | coupled to `cel_data.h` layout |
| same as `@host`/`@native`/runtime? | **no** — a bespoke second ABI | **yes** — bit-for-bit the slot ABI |

### 15.3 How the `CelValue` path works — byte-level

`subject` is `{kind=CEL_STRING, s.ptr=P, s.len=L}` in `cel.memory`, bytes at
`P`. Per call:

1. `celfn_reset()` — foreign bump cursor → base.
2. `fP = celfn_realloc(L)`; copy `cel.memory[P..P+L]` → `foreign[fP..]`.
3. `fS = celfn_realloc(24)`; write `{CEL_STRING, s.ptr=fP, s.len=L}` to
   `foreign[fS]` — **the same 24 bytes, the one `ptr` field rebased `P→fP`**
   (likewise `action` → `fA`).
4. `fOut = celfn_realloc(24)`.
5. `call allow_string_string(fOut, fS, fA)`.
6. guest reads `foreign[fS]` → `{CEL_STRING, fP, L}` → `liftString(fP,L)`,
   calls `Allow(...)` → `true`, writes `{CEL_BOOL, 1}` to `foreign[fOut]`.
7. host memcpys the 24 bytes `foreign[fOut]` → `cel.memory[S_out]`. A `bool`
   is inline → step 7 is a pure 24-byte memcpy.

A panic instead makes the `recover()` shim write `{CEL_ERROR, code}` to
`fOut`; step 7 carries it back unchanged. **That is the entire error
channel — the kind tag.**

### 15.4 Where they diverge: aggregates

```
canonical list<int> :  [i64, i64, i64]                          (packed, no pointers)
CelValue  list<int> :  ArenaListHeader{3, E} + 3×CelValue{CEL_INT,i}  (E relocated)
```

Canonical is leaner on the wire for deep aggregates and needs no relocation;
the `CelValue` path is fatter and recursive there (§3.3) — but it is one
generic routine, reuses the arena format the runtime already speaks, and is
the same recursion canonical lowering would have needed. Scalars / strings /
proto-as-bytes (the dominant cases) win.

### 15.5 Why `CelValue` wins here

Canonical's only real edge is **decoupling** (a stranger's wit-bindgen module
could link without depending on `cel_data.h`). But `celfn` generates the
guest anyway, so we never needed that portability. In exchange: **one ABI
across all four call paths**, the **3VL/error channel for free**, a
**generic mirror-copy** instead of per-type transcoding, and a foreign
backend that is the *least* special, not the most. The cost accepted is the
layout coupling (version the wire format; regenerate on a bump — §13) and the
aggregate relocation.
