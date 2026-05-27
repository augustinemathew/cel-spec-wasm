# Modules and FFI

Status: design — drafted 2026-05-24.  Consolidates and revises the
module/ABI material previously scattered across
[`m13-custom-fns.md`](m13-custom-fns.md) §4.3–§4.6 / §10.5 and
[`wasi/DESIGN.md`](wasi/DESIGN.md) §4, reflecting two decisions taken
2026-05-24:

  1. **Foreign modules use one fixed C ABI + IDL-generated shims** (the
     host trampoline is language-agnostic — it dispatches on CEL type,
     never on source language).
  2. **The foreign type surface is full-recursive** (scalars, string,
     bytes, `list<T>`, `map<K,V>`, nested aggregates, `Duration`,
     `Timestamp`).  Proto messages, `type`, and `optional` remain
     rejected at the boundary.

This doc is the authoritative reference for **how wasm modules relate
to one another at runtime** and **how data crosses between them** (the
FFI mechanics). The **memory substrate** those modules share is owned
by [`memory-layout-design.md`](memory-layout-design.md) — read it first
for the regions/lifetimes/invariants; this doc references it rather than
duplicating it. `m13-custom-fns.md` remains the milestone doc (IDL
grammar, slice plan, CLI surface).

> **Plan-vs-execution delta vs `m13-custom-fns.md` §4.5.2/§4.5.4.**
> Those sections describe a pre-Phase-C model in which a *foreign*
> module could export its own memory and have it adopted as the shared
> `cel.memory` for the whole link ("loose producer requirements,
> strict engine negotiation").  Phase C moved the shared-memory
> definition into `cel_runtime.wasm` (`(memory 4 1024 shared)`), which
> every other module now **imports**.  A foreign module therefore can
> no longer be the memory definer — it either (a) imports the shared
> memory, which is fragile for most toolchains (TinyGo can't easily;
> a foreign allocator on the shared memory collides with the runtime's
> statics + arena), or (b) keeps its own memory and the host marshals
> across.  §5 picks (b).  The CelValue-over-shared-memory shape the
> probes verified still holds for **CEL-defined** library modules (§4);
> it is retired for **foreign** modules (§5).

---

## 0. Reading guide

  - §1 — the four module kinds and who owns memory.
  - §2 — the post-Phase-C shared-memory layout (the substrate).
  - §3 — slots + the CelValue ABI (the unit that crosses calls).
  - §4 — **CEL-defined** functions: intra-module FFI (v1 single-module),
    zero-copy slots, disjoint static bands.  (We produce these.)
  - §5 — **Foreign** library modules: cross-memory FFI, the fixed C
    ABI, recursive lift/lower, generated shims.  (User produces these;
    we marshal.)  This is the bulk of the new material.
  - §6 — the `.celfn` IDL → both backends at compile time.
  - §7 — `sayhello` worked end-to-end through both backends.
  - §8 — deltas against `m13-custom-fns.md`.
  - §9 — WAT-first plan + slice order.
  - §10 — testing obligations.
  - §11 — open questions / future work.

---

## 1. The module zoo

A planned program links **up to four kinds** of wasm module into one
wasmtime store:

| Kind | Count | Who produces it | Memory | Imports | Lifetime |
|---|---|---|---|---|---|
| **Runtime** (`cel_runtime.wasm`) | 1 | us, prebuilt (`wasm32-wasi-threads`) | **defines** `(memory 4 1024 shared)`, exports as `cel.memory` | — | process |
| **Expr** (the compiled expression) | 1 | us, per `Compile()` | imports `cel.memory` | runtime helpers (`cel_*`), host (`cel_host.*`), custom (`cel_fn.*`, `<lib>.*`, `<alias>.*`) | program |
| **CEL-defined functions** (v1: in the expr module) | N internal funcs | **us**, from `.celfn` bodies | *(same module as expr; no separate module in v1)* | runtime helpers, host, sibling customs | program |
| **Foreign library** | 0..N | **user** (TinyGo/Rust/clang/…) | **its own** memory | only what its toolchain emits + `celfn_realloc` (its own export) | program |

> v1 compiles CEL-defined bodies **into the expr module** as internal
> functions (§4); the "separate `foo.wasm` + `env.__memory_base`" row is
> deferred future work (§4.5).

Two structurally different FFI boundaries fall out of this table:

  - **Intra-memory FFI** (§4): expr ↔ runtime ↔ CEL-defined functions.
    All share one linear memory.  A "call" passes **slot offsets**;
    the callee reads/writes 24-byte CelValue cells in place.
    Zero-copy.  The only complication is keeping each function's static
    data in a disjoint **band** of `[0,8192)`, assigned at layout time
    (§4.2).
  - **Cross-memory FFI** (§5): expr/CEL-defined ↔ foreign library.
    Two distinct linear memories.  A slot offset is meaningless across
    the gap, so the call goes through a **host trampoline** that
    copies (lowers) args into the foreign memory, calls the foreign
    export, and copies (lifts) the result back out.  This is a scoped
    WASI-canonical-ABI.

---

## 2. The shared-memory substrate (post-Phase-C)

**→ Full detail in [`memory-layout-design.md`](memory-layout-design.md)
§1–§4.** The minimum needed to follow the FFI below:

`cel_runtime.wasm` (built `wasm32-wasi-threads`, `--global-base=8192`)
**defines and exports** the one shared memory; the expr module imports
it. Three regions: **reserved low `[0,8192)`** = the expr module's
compile-time static slots (rodata + workspace), bounded by a
`ResourceExhausted` guard; **runtime statics + shadow stack
`[8192,__heap_base)`**; **dlmalloc heap** holding the per-Eval bump
**arena**, the per-Instance **activation buffer**, and Plan-lifetime
objects.

The reason this matters for FFI: there is exactly **one** reserved low
region and it belongs to the expr module. Intra-memory callees (runtime
kernels, CEL-defined bodies) read/write slots in this shared memory in
place (§3–§4). A foreign module has its own memory and cannot — hence
the marshalling boundary (§5).

---

## 3. Slots and the CelValue ABI

The unit that crosses every intra-memory call is the **24-byte
CelValue**:

```c
typedef struct {
  uint32_t kind;        // cel_kind_t  (@ +0)
  uint32_t tag;         // kind-specific: string len, list count, …  (@ +4)
  uint8_t  payload[16]; // kind-specific union  (@ +8)
} cel_value_t;          // sizeof == 24
```

  - Scalars (`bool`/`int`/`uint`/`double`/`null`) live **inline** in
    `payload`.
  - `string`/`bytes` carry `{ptr, len}` — `ptr` is an **absolute**
    offset into the shared memory (rodata bytes, activation buffer, or
    arena).
  - Aggregates (`list`/`map`) carry a header pointer / slot into the
    arena.
  - Message (`11`) is a host-resident handle (`msg_slot`) — valid only
    in shared memory (§5.8 forbids it crossing into foreign).

A **slot** is the byte offset of one such cell.  `LayoutPass` assigns
static slots in `[0,8192)`: rodata slots for constants, workspace
slots for variables and intermediates (the `SlotAllocator` recycles
workspace cells, tracking `peak_slots`).  Wasm values flowing through
codegen are i32 **slot offsets**, not the payloads themselves.

Every intra-memory helper — runtime kernels, CEL-defined exports,
host-backed trampolines — has the same shape:

```wat
(func $helper (param $out_slot i32) (param $arg0_slot i32) … (result))
;; reads *argN_slot, writes *out_slot.  void return.
```

3VL absorption (`UNKNOWN`/`ERROR` operands short-circuit) is emitted by
codegen *around* the call, so a custom body only ever sees concrete arg
CelValues (`m13-custom-fns.md` §4.1).

---

## 4. CEL-defined functions — intra-module FFI (v1 single-module)

A CEL-defined function is `.celfn` source with a body:

```
Module foo;
string @native.sayhello(string name) = "hello " + name;
int    @native.double(int x)         = x * 2;
```

> **v1 decision (2026-05-24): single module.** Each body is compiled
> into the **same wasm module as the expression** — *not* a separate
> `foo.wasm`. It becomes an internal wasm function under its
> `overload_id`; the expr's call site emits a direct
> `BinaryenCall(overload_id)` that resolves to that function. **No
> separate module instantiation, no cross-module linking, no
> `__memory_base`.** The separate-module model (and the `__memory_base`
> machinery it needs) is deferred to §4.5 future work.

### 4.1 The boundary is zero-copy

All functions live in the one expr module over the one shared
`cel.memory`, so an arg slot offset means the same byte address
everywhere. The call passes the caller's slot offsets directly; the
body reads the arg cells and writes the result cell in place. No
marshalling, no copy. A string result is `arena_alloc`'d in the shared
arena and its handle written to the caller's `out_slot`.

### 4.2 Disjoint static bands within `[0,8192)`

The expr and every CEL-defined body share the one reserved low region.
They are **both live during a call** (the caller's args + intermediates
are live while the callee runs), so a callee's workspace cells must not
overlap a caller's live cells. The single-module model partitions
**statically at layout time**: each compiled function (the expr first,
then each body) gets a **disjoint band** assigned by running its
`LayoutPass` with `rodata_base_override` = the cumulative high-water of
the prior bands, and `reserved_region_limit_bytes` bounding the total.
Overrunning `[0,8192)` is a hard `ResourceExhausted` at compile time
(the guard, `layout_pass.cc`), never a silent stomp.

This is the intra-module analogue of `__memory_base` — partition by
**static offset at layout time**, not by a runtime-relocated base.
Because every band's base is known at compile time and ≥ 0, string /
aggregate literals bake an **absolute** `ptr` exactly like the expr's
own literals — the pointer-relocation wrinkle (§4.5) does **not** arise
in v1.

### 4.3 The function ABI

Each body lowers to `(func $<overload_id> (param $out_slot i32) (param
$arg0_slot i32) … (result))` via `LowerToCustomFn`: wasm params instead
of nullary; **no `arena_reset`** (the arena belongs to the outer
`$eval`); the result is written into the caller-supplied `out_slot` via
`cel_copy_slot` rather than returned by value. Params wire to the
body's free variables positionally (param 0 = out_slot, param i+1 =
the i-th declared param; a `this` receiver is param 1).

A CEL-defined body may freely call built-ins, macros (expanded by
cel-cpp before codegen), **sibling** CEL-defined functions (direct
`call` to another internal function — same module), and host functions
(`cel_fn.*` import). All compose because they share the module + memory.

### 4.4 Compile model

Integrated into `Compile()` (single pass): after `$eval` is lowered,
each `kCelDefined` decl is type-checked with its params injected as
free variables (sharing the parent's descriptor pool / container /
optimize settings so a `proto(...)` param resolves against the same
pool), run through `ResolvePass → LayoutPass` (at its band offset), and
emitted via `LowerToCustomFn` into the same module. Import installation
**skips CEL-defined overload-ids** (a dedicated `kCelDefined`
ImportModule kind distinguishes them from foreign), so the direct call
binds to the internal function rather than an unresolved import.

### 4.5 Deferred — separate reusable library modules (`__memory_base`)

If CEL-defined libraries later need to be **separately instantiated /
reused** across expressions (rather than recompiled into each expr
module), each library module would import the shared `cel.memory` and
need a **disjoint heap region** for its own static data, with the base
bound at instantiate via the `env.__memory_base` PIC global (the engine
`malloc`s the region, `memcpy`s the data segment, defines the const
global — the same host-side mechanism as the activation buffer; not
expressible in pure WAT, which only *consumes* the global). In that
model a string/aggregate **literal** can't bake an absolute `ptr`
(the base isn't known until instantiate); the body constructs it at
eval time as `ptr = (i32.add (global.get $__memory_base) (i32.const
K))`. Prototyped by the running traces
[`wat/m13_celfn_double_lib.wat`](wat/m13_celfn_double_lib.wat) +
[`wat/m13_celfn_double_caller.wat`](wat/m13_celfn_double_caller.wat)
(`wat_runner` test `DoubleViaMemoryBaseLibKeepsSlotsDisjoint`,
2026-05-24). **None of this is in the v1 single-module path** — it is
recorded here so the prototype isn't lost when reuse becomes a goal.

---

## 5. Foreign library modules — cross-memory FFI

A foreign library is a **prebuilt** `.wasm` (TinyGo/Rust/clang/…) the
user supplies, declared in `.celfn` as `<type> <alias>.<fn>(...)` with
no body.  At compile time we need the **IDL** (to type-check + select
marshalling) and the module's **export list** (to wire the import); we
**do not** recompile the module.

### 5.1 Why foreign is different (post-Phase-C)

The foreign module has its **own** linear memory.  wasm cannot reach
into another module's memory, and post-Phase-C it cannot adopt the
shared memory as its own (the runtime already defines it; see the delta
callout at the top).  Forcing it to *import* the shared memory is
fragile — TinyGo's runtime expects to own its layout, and any foreign
allocator running on the shared memory collides with the runtime's
statics + the engine arena.  So foreign FFI is **cross-memory**: the
host trampoline owns the boundary and copies across.

### 5.2 The host trampoline

The expr/CEL-defined caller does not call the foreign export directly.
It calls a host import:

```wat
(import "cel_host" "cel_call_foreign"
  (func $cel_call_foreign (param i32 i32 i32) (result)))
;; (fn_id, args_slot, out_slot) -> void
```

`args_slot` points at a packed run of the caller's arg CelValue cells
(shared memory); `out_slot` is where the lifted result goes.  The host
trampoline, keyed by `fn_id` → the IDL signature:

  1. **lowers** each arg from its shared-memory CelValue into the
     foreign module's memory (§5.3–5.4),
  2. calls the foreign export with the lowered (flat or pointer) args,
  3. **lifts** the result back into a CelValue written to `out_slot`,
  4. writes `kError` for a foreign trap or a contract violation.

A Go (or Rust) **panic** — explicit or runtime (nil deref, index out of
range) — surfaces at step 4 as a catchable `TrapCode.UNREACHABLE`: it
unwinds to wasm `unreachable`, **not** a WASI `proc_exit`, so the plain
trap → `kError` path already covers it and the host does not crash
(probe-validated, `foreign-go-bindgen-findings.md`). Open: after an
*uncaught* trap, Go's `fatalpanic` is nominally fatal, so the engine's
safe policy is to **re-instantiate** the foreign module rather than
reuse it — the recover() shim (§5.6) makes uncaught traps rare anyway.

The trampoline dispatches **only on CEL type** (from the IDL) — never
on source language.  That language-agnosticism is what decision (1)
buys: TinyGo/Rust/clang all present the *same* fixed ABI via their
generated shim (§5.6).

**Caller-side slot glue (codegen) — the third generated piece.** The
trampoline takes `args_slot` / `out_slot`, but the per-argument
CelValues start out wherever each arg sub-expression left them — a
SlotAllocator workspace slot, a rodata constant, the result of a nested
call.  So for every foreign call site **codegen emits glue in the expr
wasm that reads each evaluated argument's 24-byte CelValue from its slot
and writes it into the packed `args_slot` run** (then the parent
consumes the lifted result from `out_slot`).  This caller-side
slot↔args-area marshalling is generated from the call's arity + arg
types — it is the **third** generated half of the FFI, alongside the
language-agnostic host trampoline (§5.2, hand-written C++ that does the
cross-memory lift/lower) and the per-language foreign shim (§5.6).
Concretely the glue is a short run of `cel_copy_slot`-style writes that
gather the args into `args_slot`, the `cel_call_foreign` call, and the
out_slot read — emitted by `expr_lower` just as the kCall arm emits the
`(out_slot, arg0_slot, …)` wiring for a built-in.

(For a `@host` call the same principle applies but the glue is
*degenerate*: the args are already CelValues in shared memory, so
codegen passes the `out_slot` + per-arg slot offsets straight to the
`cel_fn.<id>` import — no packing/copy. The typed `FunctionImpl` adapter
(`m13-custom-fns.md` §0.5) then reads/writes those same slots host-side.
The slot read/write is the universal contract; only the cross-memory
copy is foreign-specific.)

### 5.3 The fixed celfn-foreign C ABI

Adopt the WASI-Component-Model canonical ABI's lowering rules, scoped
to our type set.  Every foreign module — regardless of source language
— presents exactly this shape.

**Per-type storage form** (how a value of each type is laid out inside
the foreign module's own memory):

| CEL type | size | align | in-foreign-memory form |
|---|---|---|---|
| `bool` | 1 | 1 | `0/1` |
| `int` / `uint` | 8 | 8 | `i64` |
| `double` | 8 | 8 | `f64` |
| `null` | 0 | 1 | (absent / unit) |
| `string` / `bytes` | 8 | 4 | `(i32 ptr, i32 len)` → byte run |
| `list<T>` | 8 | 4 | `(i32 ptr, i32 len)` → contiguous array of T's storage form |
| `map<K,V>` | 8 | 4 | `(i32 ptr, i32 len)` → array of `(K,V)` records (K then V, aligned) |
| `Duration` / `Timestamp` | 16 | 8 | record `{i64 secs, i32 nanos}` |

**Calling convention** (also from the canonical ABI):

  - **Args.**  If *all* params flatten to ≤ the flat limit of core wasm
    values (scalars do), pass them flat on the wasm stack.  Otherwise
    the host lowers the whole argument tuple into one record in foreign
    memory and passes a single `i32` pointer to it ("indirect" spill).
  - **Results.**  A single scalar result is returned by value.  An
    aggregate (or multi-value) result uses a **return area**: the host
    pre-allocates space in foreign memory and passes its pointer as a
    hidden leading arg; the shim writes the lowered result there.
  - **Allocator.**  Every foreign module exports
    `celfn_realloc(ptr, old_size, align, new_size) -> ptr` (the
    `cabi_realloc` shape).  The host calls it to allocate inbound arg
    buffers and the return area; the shim calls it to allocate the
    lowered result.  `ptr==0` ⇒ fresh allocation.

That table + those three conventions are the **entire** producer
contract.  No CelValue layout leaks into the foreign module — the shim
speaks idiomatic strings/slices; the host speaks CelValue; the fixed
ABI is the seam.

> **OPEN — the fixed ABI has no error channel (probe-surfaced gap,
> needs a decision).** As written above, a foreign export returns *only*
> its result value — there is no slot to say "the call failed."  So a
> foreign-side failure (a recovered panic, a `proto.Unmarshal` error on
> a malformed message, a contract violation) can today only masquerade
> as a legitimate `false` / `0` / empty result — the probe's "return 0
> on Unmarshal error" conflates "bad input" with "not an adult," which
> is wrong for production.  **Proposed fix (TBD):** add a `status i32`
> to the calling convention — a leading hidden out-param (or reuse the
> return-area mechanism for multi-value) carrying `ok` vs an error code,
> which the trampoline maps to `kError` and the recover() shim (§5.6)
> writes on a caught panic.  This is an **ABI addition**, not just shim
> codegen, so it's flagged for an explicit decision before the foreign
> backend is built.  See `foreign-go-bindgen-findings.md` "Open
> questions".

### 5.4 Recursive lift / lower (full type surface)

The trampoline is two mutually-recursive routines driven by the IDL
type:

  - `Lower(CelValue, Type) → flat values | foreign pointer`
  - `Lift(foreign bytes, Type) → CelValue` (allocated in the shared
    arena via `cel.arena_alloc`)

Per-type:

| Type | Lower (shared → foreign) | Lift (foreign → shared) |
|---|---|---|
| scalar | push the core value | read the core value |
| `string`/`bytes` | `celfn_realloc(0,0,1,len)`, `memcpy` bytes in, pass `(ptr,len)` | read `(ptr,len)`, `arena_alloc(len)`, copy out, build CelValue |
| `Duration`/`Timestamp` | write `{secs,nanos}` record | read record → CelValue |
| `list<T>` | alloc `len * sizeof(T)`, **recurse** `Lower(elem,T)` into each slot | read `(ptr,len)`, **recurse** `Lift` per element into an arena list |
| `map<K,V>` | alloc array of `(K,V)` records, recurse both halves | recurse both halves into an arena map |

Nesting (`list<map<string,list<int>>>`, …) falls out of the recursion.
Element storage size/alignment come from the §5.3 table.

### 5.5 Allocation & lifetime

  - **Inbound args** live in the foreign module's memory, allocated via
    its `celfn_realloc`.  They are valid for the duration of the call.
  - **The result** is allocated by the foreign code in its own memory
    (via `celfn_realloc` / its language allocator) and the shim returns
    the pointer (or writes the return area).  The host copies it into
    the shared **arena immediately and synchronously** — single-thread,
    no intervening foreign call — then the foreign allocation may be
    reclaimed.  For **TinyGo** (GC), synchronous copy means the
    returned pointer need not be pinned: it stays live across the
    single return edge, and the host reads it before any further
    foreign entry.
  - The shared-arena copy is reset by `arena_reset()` between Evals
    like any other intermediate; the caller reads the lifted result
    before the next reset.

### 5.6 Generated shims (mini-wit-bindgen)

The user writes idiomatic code:

```rust
// Rust
pub fn sayhello(name: &str) -> String { format!("hello {name}") }
```
```go
// TinyGo
func sayhello(name string) string { return "hello " + name }
```

`celfnc` generates, **from the IDL signature**, a thin per-language
shim that:

  - exports the fixed-ABI entry point under the `overload_id`
    (`sayhello_string`),
  - on the language side, *lifts* the incoming `(ptr,len)` into a
    `&str` / Go `string` (the bytes are in the module's own memory),
  - calls the user's function **inside a panic guard** — a Go
    `defer func(){ if recover()!=nil { /* report failure */ } }()` (and
    the equivalent `catch_unwind` in Rust) — so a user panic / runtime
    fault becomes a typed error *return* rather than a wasm trap that
    leaves the instance in Go's nominally-fatal post-`fatalpanic` state
    (probe-validated catching explicit + nil-deref panics),
  - *lowers* the return via `celfn_realloc` into the fixed-ABI form
    (and into the return area for aggregates),
  - exports `celfn_realloc` (wrapping the language allocator) and, for
    reactor toolchains, leaves `_initialize` in place.

This is effectively a scoped
[`wit-bindgen`](https://github.com/bytecodealliance/wit-bindgen) keyed
off the `.celfn` IDL instead of a WIT interface — the same lift/lower
glue-generation job, restricted to our fixed ABI (§5.3) and type set.
The generator (`celfnc`) is our "wit-gen": one template per target
language, driven by `CelfnDecl`. Because the trampoline (§5.2) is
language-agnostic, adding a language is **shim-generation only** — no
host changes. If/when wasmtime's Component Model runtime is adopted
wholesale, this hand-rolled generator collapses into real `wit-bindgen`
+ a WIT projection of the IDL (§11).

> **Probe-validated for Go (2026-05-24 — `foreign-go-bindgen-findings.md`,
> `probes/foreign_go/`).** A `string→bool` and a
> `proto→bool` Go module were built and called end-to-end through a
> wasmtime host. Confirmed: `//go:wasmexport` names the export
> **verbatim** (celfnc emits under the overload id, no mangling — as
> §5.6 assumes); `celfn_realloc` works as the `cabi_realloc` shape;
> `memory` is exported for the host to write lowered args. **Two
> corrections to the as-written design:** (1) the §8.5 proto path
> requires **stock Go, not TinyGo** — TinyGo's incomplete reflection
> traps in `proto.Unmarshal` (`reflect.NewAt`); TinyGo carries only the
> scalar/string/aggregate path. (2) a stock-Go module is **not just a
> reactor needing `_initialize`** — it imports a real
> `wasi_snapshot_preview1` surface (10 funcs for string, 17 for proto:
> `fd_write`/`random_get`/`clock_time_get`/`fd_prestat_*`…), so the
> engine must instantiate it against a **full WASI preview1 context**,
> not stubs. Cost: string module 1.6 MB, proto module 6.4 MB
> (+~4.7 MB protobuf runtime); TinyGo string 118 KB.

### 5.7 `_initialize` / reactor coexistence

Unchanged from `m13-custom-fns.md` §4.5.5: the engine calls a foreign
module's zero-arg `_initialize` exactly once after instantiation **iff**
it exports one (TinyGo `wasm-unknown`, wasi-libc reactor C, stock Go
wasip1, Rust `wasm32-wasi`).  Standalone modules (Rust no_std, bare
clang, hand-WAT) skip it.  Reactor and standalone foreign modules
coexist in one engine.

### 5.8 What may not cross

Rejected **at type-check** (`FunctionLibrary::Builder::Build`, already
enforced for proto — `function_library.cc` `MentionsProto`):

  - `proto(<fqn>)` and any aggregate carrying a proto — a message is a
    host-resident `msg_slot` handle, meaningless in foreign memory; see
    `m13-custom-fns.md` §4.5.1.  Two future paths (copy-to-arena
    serialize, externref bridge) are deferred.
  - `type` values and `optional<T>` — host-side / wrapper
    representations with no foreign storage form in v1.

Each gets a negative type-check test with a diagnostic naming the
offending decl + this section.

---

## 6. The `.celfn` IDL → both backends

Grammar in [`compiler/celfn/Celfn.g4`](../../../compiler/celfn/Celfn.g4);
parser in `function_library.cc`.  Three backends (`CelfnDecl::Backend`):

| Backend | IDL form | Memory model | Compile-time inputs | Eval path |
|---|---|---|---|---|
| **Host** (`@host.f`) | `T @host.f(...) ;` | shared (host trampoline writes CelValue) | signature only | `cel_fn.*` import → C++ `FunctionImpl` |
| **CEL-defined** (`f`) | `T f(...) = <cel-expr> ;` | shared, **same module as expr** (v1) | signature **+ body** (we compile, single-pass) | direct `call` to internal fn, slot args (§4) |
| **Foreign** (`alias.f`) | `T alias.f(...) ;` | **own** memory | signature **+ prebuilt `.wasm`** (export list only) | `cel_call_foreign` trampoline (§5) |

Overload-id synthesis, `this`-receiver placement, alias rules, and the
proto-on-foreign rejection already live in `function_library.cc`.

---

## 7. `sayhello` end-to-end, both backends

Body `= "hello " + foo`, call `sayhello("world")`.

**CEL-defined** (internal `$sayhello_string` in the expr module, shared
memory): `"world"` stays in the caller's rodata, read in place via the
absolute arg slot. `"hello "` lives in the body's own static **band**
of `[0,8192)` with an absolute `ptr` (base is 0 in the single module —
no relocation). `cel_string_concat_at_vv` `arena_alloc`s `"hello
world"` in the shared arena and writes the CelValue into the caller's
out_slot. **Zero copies across the call.**

**Foreign** (TinyGo/Rust, own memory):
the trampoline `celfn_realloc`s space in the foreign memory and copies
`"world"` in (lower), calls `sayhello_string(ret_area, ptr, len)`; the
shim builds a `&str`/`string`, the user code produces `"hello world"`
in foreign memory via its own allocator, the shim writes `(ptr,len)` to
the return area; the trampoline reads it out, `arena_alloc`s a copy in
the shared arena, writes the CelValue to out_slot (lift).  **Two copies
+ foreign allocator + (TinyGo) `_initialize`.**  Isolated — the foreign
module can never touch the caller's slots.

---

## 8. Deltas against `m13-custom-fns.md`

  - **§4.5.2 point 3 (foreign module defines/adopts shared memory)** —
    retired for foreign modules post-Phase-C (the runtime owns the
    shared memory).  Foreign modules keep their own memory; the host
    marshals (§5).  Still accurate for CEL-defined libraries, which
    import the shared memory.
  - **§4.5.2 "loose producer requirements"** — replaced for foreign by
    decision (1): one fixed C ABI (§5.3) that generated shims conform
    to.  The looseness now lives one layer down (the user writes
    idiomatic code; the shim absorbs language differences), not in the
    engine negotiating per-module shapes.
  - **§4.5.1 allowed type set** — unchanged in *membership* (protos
    out, recursive list/map in) but the v1 *implementation* now targets
    the full recursive surface (decision 2), where the older text
    implied scalars/strings first.
  - **§10.5 (shared linear memory vs Component Model)** — resolved:
    CEL-defined = shared linear memory; foreign = a hand-rolled scoped
    canonical ABI (Component-Model lowering rules without the full
    Component Model runtime).

These deltas are reflected back into `m13-custom-fns.md` in the same
commit that lands this doc (callouts pointing here).

---

## 9. WAT-first plan + slice order

Per the repo's WAT-first rule, each new ABI surface gets a `.wat` trace
+ `wat_runner` execution before any codegen C++.

  - **F-CEL — CEL-defined body, single module** (`double`/`sayhello`):
    one module with `$eval` calling an internal `$<overload_id>` in a
    disjoint static **band**, direct `call` (no import, no
    `__memory_base`). Proves the band stays disjoint from the caller's
    live cells through the call. *(The committed `m13_celfn_double_*`
    `__memory_base` traces are the §4.5 separate-module prototype, kept
    for that future path — not the v1 single-module shape.)*
  - **F0 — foreign boundary trace**: a hand-WAT foreign module
    exporting `sayhello_string` in the fixed ABI (own memory +
    `celfn_realloc` + return area) plus a `wat_runner` host-trampoline
    stub that lowers a string in / lifts a string out.  Proves the
    allocator dance + return-area convention before any trampoline C++.
  - **F1 — scalars + string/bytes**, Rust first (simplest reactor
    story): trampoline + generated shim for flat + single-pointer
    cases.  `sayhello("world")` runs foreign.
  - **F2 — recursion**: list/map/nested lower/lift + indirect-spill
    args + aggregate return area.
  - **F3 — `Duration`/`Timestamp`**, then fan shims out to TinyGo +
    clang (trampoline already language-agnostic ⇒ shim-gen only).

Negative type-check tests (protos/type/optional rejected) land with F1.

---

## 10. Testing obligations

Per-component, positive + negative, per the repo testing rules:

  - **IDL/type-check** — foreign proto/type/optional rejection
    (negative); recursive list/map signatures accepted (positive).
    `function_library_test.cc`.
  - **CEL-defined codegen** — `LowerToCustomFn` per-arm + prelude
    wiring; disjoint static-band offsetting (two bodies don't overlap;
    callee workspace doesn't clobber caller live cells); the region
    guard (`layout_pass_test.cc`, landed 2026-05-24) + e2e overflow
    (`compile_test.cc`, landed 2026-05-24). See `m13-custom-fns.md` §14.
  - **Foreign trampoline** — per-type lower/lift round-trip (scalar,
    string, bytes, list, map, nested, Duration/Timestamp); return-area
    aggregate; foreign-trap → `kError`; indirect-spill arg path.
  - **WAT equivalence** — every `wat/` trace re-assembled + re-run by
    `wat_runner_test.cc` each build.
  - **e2e (manual-tagged)** — `sayhello` through both backends under
    wasmtime; mixed CEL-defined + foreign + host in one program.

Flip the matching rows in `testing-checklist.md` and
`per-component-test-coverage.md` as each slice lands.

---

## 11. Open questions / future work

  - **Proto across the foreign boundary** — copy-to-arena serialize vs
    externref bridge (`m13-custom-fns.md` §4.5.1).  Both deferred.
  - **Foreign result error channel** — beyond trap→`kError`, do we want
    a structured error return (a `result<T, string>` lowering)?
  - **`celfn_realloc` free discipline** — v1 leaks within a call and
    relies on the foreign allocator's per-instance reuse; revisit if a
    long-lived foreign instance accumulates.
  - **ABI version negotiation** — the `cel.toolchain` custom section
    (`m13-custom-fns.md` §4.5.2 point 6) vs the fixed-ABI contract
    here; reconcile when the first ABI bump lands.
  - **Component Model migration** — if/when wasmtime's component
    runtime is adopted wholesale, §5's hand-rolled canonical ABI
    becomes a thin adapter; the fixed-ABI choice is deliberately a
    subset of the canonical ABI to make that migration mechanical.
