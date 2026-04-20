# CEL → WebAssembly AOT Compiler — Design

Status: **draft v2** (last refreshed 2026-04-19 after M3 shipped and M4 ↔ M5
were swapped).

This is the architectural reference for `celwasmc`, the CEL →
WebAssembly AOT compiler in this repo's `compiler/` tree.  It describes
the pipeline (parse, check, lower, codegen), the normative runtime
layout and host ABI, the milestone plan, and the open design questions
driving upcoming work.

The doc is **normative for the host ABI (§8)** and the **`cel.abi`
custom section (Appendix A)**: any host that instantiates a module
emitted by this compiler must match those shapes verbatim.  Everything
else — codegen strategy, IR layout, project structure — is
implementation detail.  It's correct as of the latest milestone but
subject to change; for what has actually shipped, the milestone docs
under `doc/implementation-plan/` are the source of truth.

### Sources of truth

- **Grammar** — `Cel.g4` from `github.com/google/cel-cpp/parser/internal/Cel.g4`,
  vendored at `third_party/cel-cpp/parser/internal/Cel.g4`.
- **Semantics** — `doc/langdef.md` in this repo.
- **Reusable C++** — `third_party/cel-cpp/` (parser, checker, common
  type system).
- **Per-milestone status** — `doc/implementation-plan/`: plan docs
  (`m0-*` through `m8-*`), the transverse testing grid
  (`testing-checklist.md`), and the lint backlog.

### Organization of this document

Sections §1–§4 set the frame.  Sections §5–§12 are the guts.  Sections
§13–§15 cover how the compiler itself is built, tested, and sequenced.
Appendices and Open questions are reference material at the end.

**Frame (§1–§4).**
  - **§1 Architectural stance** — why the emitted module uses both
    linear memory and `externref`, and what follows from that.
  - **§2 Goals / §3 Non-goals** — what we are and aren't building.
  - **§4 Pipeline** — end-to-end flow from CEL source to
    per-expression `.wasm` sitting next to the shared `runtime.wasm`.

**Compiler internals (§5–§12).**
  - **§5 Front-end** — grammar reuse, parser, macro expansion,
    comprehension scoping (§5.4 is load-bearing for M5).
  - **§6 Type system** — supported CEL types, checker integration,
    rejection of `dyn` / `Any` unwrap / heterogeneous collections.
  - **§7 Runtime layout (linear memory)** — memory map, `CelValue`
    shape, arena allocation, `$cel_refs` externref table,
    constructors.
  - **§8 Host ABI (normative)** — imports / exports every host must
    provide; `cel_host.get_field` / `has_field` / `message_eq`; type
    and attribute identity via `cel.abi`.
  - **§9 Three-valued logic** — OK / UNKNOWN / ERROR semantics,
    propagation rules, short-circuit tables (the M4 charter).
  - **§10 Code generation** — per-`ExprKindCase` lowering strategy,
    Binaryen IR, `Repr` dispatch.
  - **§11 Standard library** — built-in operators the compiler
    emits; what's deferred to M7.
  - **§12 Custom functions** — `.celfn` IDL, `celfnc` stub
    generator, `cel_fn.*` import convention.

**Build and sequence (§13–§15).**
  - **§13 Project layout** — directory structure of the compiler
    tree.
  - **§14 Build** — Bazel targets, cross-compile toolchain, CI.
  - **§15 Milestones** — the M0–M8 plan.  M4 ↔ M5 were swapped on
    2026-04-19 so three-valued logic lands before collections; see
    the milestone table for the rationale.

**Reference.**
  - **Appendix A** — `cel.abi` custom section format (normative).
  - **Appendix B** — minimal host worked example.
  - **Open questions** — deferred design decisions scoped to later
    milestones.

---

## 1. Architectural stance

The emitted module uses **both** storage schemes, each for what it's good
at:

| Concern                                 | Storage                                          |
| --------------------------------------- | ------------------------------------------------ |
| `CelValue` structs (tagged union)       | linear memory (bump arena)                       |
| `string`, `bytes`                       | linear memory, `(ptr, len)` span (copied in)     |
| `list`, `map` backing storage           | linear memory                                    |
| `UnknownSet`, `Error`                   | linear memory                                    |
| Interned literals, type/attribute tables | linear memory data segment                      |
| **Host-owned protobuf messages**        | **externref table** owned by the module          |

Why both:

- `string`, `bytes`, and the values the CEL program constructs are short,
  frequently manipulated, and must support byte-level operations
  (comparison, substring, regex input). Copying them into linear memory once
  gives the runtime direct access without crossing the host boundary on
  every read.
- Protobuf messages passed in from the host are large, already live in the
  host's heap, and may be shared across multiple evaluations. Keeping them
  as `externref` avoids a deep copy and preserves host-side ownership and
  lifetime semantics — matching requirement #3.

How they compose: a `CelValue` lives in linear memory and occupies 24 bytes.
When `CelValue.kind == CEL_MESSAGE`, the payload is an `i32 ref_slot` — an
index into the module's externref table. The table is private to the
module; externrefs flow across the host boundary as WASM import/export
parameters and returns, but inside the CelValue graph, messages are
addressed by i32.

This needs the `reference-types` proposal (externref + externref tables)
and the `multi-value` proposal. Both are part of WASM 2.0 / Web MVP and are
supported by all major engines (Wasmtime, V8, SpiderMonkey, JSC, wasmer).

## 2. Goals

- AOT-compile a CEL expression against a fixed protobuf schema into one
  `.wasm` module.
- Single exported `eval` returning `CelValue*`.
- Statically typed subset of CEL only; reject `dyn`, `Any` unwrap, and
  heterogeneous collections at check time.
- Three-valued logic (OK / UNKNOWN / ERROR) carried in `CelValue.kind`.
- User-defined functions declared as `FunctionSet` proto or `.celfn` IDL;
  return primitives, values, or UNKNOWN / ERROR.
- Host ABI is normative and language-independent. Any host language that can
  instantiate a WASM module and read/write linear memory can host us.

## 3. Non-goals

- Runtime parsing, JIT, or interpretation of CEL.
- `dyn`-typed CEL.
- Native codegen past WASM.
- Any single host language binding.

## 4. Pipeline

```
 CEL source ──▶ [parse: Cel.g4 + cel-cpp::parser] ──▶ Expr (proto)
                                                     │
             ┌───────────────────────────────────────┘
             │  + FileDescriptorSet (schema)
             │  + FunctionSet       (custom functions)
             │  + VarDecls          (expression inputs)
             ▼
        [check: cel-cpp::checker] ──▶ CheckedExpr (proto, every node typed,
                                                   overloads resolved,
                                                   macros expanded)
             │
             ▼
        [lower to IR] ──▶ typed SSA-ish tree carrying (CelKind, WasmType)
             │
             ▼  (Binaryen C API, linked as libbinaryen.a)
        [codegen]
             │  emits a per-expression module that imports
             │  everything it needs from the shared runtime instance
             │  and defines its own private externref table:
             │    (import "cel" "memory"     (memory …))
             │    (import "cel" "cel_alloc"  (func   …))
             │    (import "cel" "cel_make_*" (func   …))
             │    …
             │    (table  $cel_refs 16 externref)  ;; per-eval-module
             │    (func $cel_ref_intern …) (func $cel_unwrap_message …)
             │    (func $eval …) (export "eval" (func $eval))
             │    (custom section "cel.abi" …)
             ▼
   runtime.wasm      ← cross-compiled once from C, shipped with the host
   expression.wasm   ← compiled per expression, tiny (only imports + eval + cel.abi)

The host is responsible for instantiating `runtime.wasm` once and
wiring its exports as imports to every `expression.wasm` it loads.
See §7 and §10 for the specifics.
```

## 5. Front-end

### 5.1 Grammar

`Cel.g4` is taken verbatim from cel-cpp (vendored). Its productions are the
ones the spec assumes: `expr`, `conditionalOr`, `conditionalAnd`, `relation`,
`calc`, `unary`, `member`, `primary` with Select / MemberCall / Index /
GlobalCall / CreateList / CreateMap / CreateMessage / literals, plus optional
chaining (`?.`, `?[]`).

### 5.2 Parser

Reuse `cel-cpp::parser` (`third_party/cel-cpp/parser/parser.h`). It already
runs the ANTLR-generated parser, performs macro expansion, and emits a
`cel::Expr` AST. We consume that AST directly.

### 5.3 Macros

`has(...)` and comprehensions (`all`, `exists`, `exists_one`, `map`, `filter`)
are expanded by cel-cpp's parser into the canonical comprehension form, so
the type checker and code generator only ever see the lowered shape: a
`Comprehension` node with `iter_range`, `iter_var` (and, for map
comprehensions, `iter_var2`), `accu_var`, `accu_init`, `loop_condition`,
`loop_step`, and `result`.

### 5.4 Comprehension variable scoping

Per `doc/langdef.md` (commit 8d28382, "Add clarification on comprehension
scoping rules"): a comprehension introduces a new **lexical scope** whose
variables shadow everything else — including package-namespaced
identifiers.

Name resolution for an identifier `x` inside a comprehension body
(`loop_condition`, `loop_step`, `result`) walks:

1. The innermost comprehension's `iter_var` (and `iter_var2`) and
   `accu_var`.
2. Each enclosing comprehension's variables, innermost → outermost.
3. The enclosing expression's function parameters (`eval` arg names).
4. The package / global declaration environment.

A leading dot (`.x`) **bypasses** every comprehension scope and resolves
directly in the root declaration environment. This is the only way to
reach a global `x` when an enclosing comprehension has bound the name.

Concrete examples (from the spec):

| Expression                          | `x` inside resolves to          |
| ----------------------------------- | -------------------------------- |
| `[1].exists(x, x == 1)`             | the comprehension's `iter_var`  |
| `[1].exists(x, .x == 1)`            | the global `x`                  |
| `[1].exists(x, [2].exists(x, …))`   | the **inner** `iter_var`        |
| `[1].exists(x, [2].exists(y, x+y))` | both bindings live on the stack |

The type checker, IR, and codegen each keep their own scope stack that
mirrors this structure; see §6.2, §9.x, and §10.3.

## 6. Type system

### 6.1 Supported CEL types (static subset)

| CEL type             | Notes                                            |
| -------------------- | ------------------------------------------------ |
| `bool`               |                                                  |
| `int`                | signed 64-bit                                    |
| `uint`               | unsigned 64-bit                                  |
| `double`             | 64-bit float                                     |
| `string`             | UTF-8 bytes in linear memory                     |
| `bytes`              | raw bytes in linear memory                       |
| `null_type`          | sole inhabitant is `null`                        |
| `list(E)`            | parameterised, E concrete                        |
| `map(K, V)`          | K ∈ {int, uint, bool, string}                    |
| `message(T)`         | T is a protobuf FQN; held as externref; addressed inside `CelValue` by its `i32` ref-table slot |
| `type(T)`            | interned `i32 type_id`                           |
| `optional_type(T)`   | result of `?.` / `?[]`                           |
| `duration`           | as `(seconds: i64, nanos: i32)`                  |
| `timestamp`          | as `(seconds: i64, nanos: i32)`                  |

Rejected at check time: `dyn`, heterogeneous list literals, wrapper-type
auto-unboxing that would require `dyn`, `Any` unpacking.

### 6.2 Type checker

Reuse `cel-cpp::checker` (`third_party/cel-cpp/checker/type_checker.h`,
`standard_library.h`). Configure it with:

- A `google::protobuf::DescriptorPool` derived from the user's
  `FileDescriptorSet`.
- Variable declarations from the user's config.
- The standard library overload table (already provided by cel-cpp).
- The user's `FunctionSet` (custom functions).

Output: `cel::CheckedExpr` with `type_map` and `reference_map` populated. We
run an additional pass to reject any node whose resolved type is `dyn`.

**Comprehension scoping.** While typing a comprehension body, the checker
pushes a scope frame holding `iter_var` (type = element type of
`iter_range`), optional `iter_var2` (for map comprehensions, type = value
type), and `accu_var` (type = type of `accu_init`). Identifier nodes
inside the body resolve via the scope walk in §5.4 — inner comprehension
frames before the package environment — and a leading-dot Ident bypasses
every frame. The frame is popped on the way out.

If cel-cpp's checker version predates the §5.4 clarification, we wrap it
with a small post-resolution pass that re-walks the tree, maintains the
scope stack, and rewrites any mis-resolved `reference_map` entries. This
is also where a leading-dot Ident is normalised (strip the dot in the AST,
set the reference to the root declaration).

## 7. Runtime layout (linear memory)

The **runtime support library** is a standalone wasm module compiled
once from C (`compiler/runtime/cel_runtime.c`) and cross-compiled to
`runtime.wasm`. It provides:

- A bump allocator over a monotonically-growing region of linear memory.
- The `CelValue` struct type.
- Constructors for every `CelKind`.
- Typed accessors and scalar comparison ops.
- `string` / `bytes` / `list` / `map` operations.
- Three-valued-logic helpers (`cel_and`, `cel_or`, `cel_not`,
  `cel_status_either`, `cel_unknown_merge`).

The `$cel_refs` externref table and its helpers (`cel_ref_intern`,
`cel_ref_get`, `cel_refs_reset`, plus `cel_wrap_message` /
`cel_unwrap_message`) are **not** exported from the runtime.
Per-eval-module tables are emitted as Binaryen IR during codegen
(`compiler/codegen/cel_refs.{h,cc}`) into each expression module
that references a proto message.  This differs from the
original design sketch — which imagined one shared table in the
runtime — because `externref` is an opaque-host-reference type
with no direct WAT / C representation.  The only way to emit
`table.set` / `table.get` on `externref` is through Binaryen's IR
API at codegen time; the wasm32 cross-compile path the runtime
uses can't author these helpers.  Per-module tables also give us
`cel_refs_reset` semantics that match the module's own
`cel_reset` (rewinding module-local state at the same instant)
without cross-module coordination.  The trade-off — interned
refs are not reusable across eval modules within one runtime
instance — is fine for the current workload (one eval per
expression; the table is wiped on `cel_reset` anyway).

### 7.0 Two-module architecture

Emitted expressions do **not** embed the runtime.  Every per-expression
module declares imports from a single module namespace named `"cel"`:

```wat
(module
  (import "cel" "memory"    (memory 1))
  (import "cel" "cel_alloc" (func (param i32) (result i32)))
  (import "cel" "cel_make_int" (func (param i64) (result i32)))
  ;; …one import per runtime function eval actually calls…

  ;; Per-eval-module externref table, emitted by codegen when the
  ;; expression has at least one message variable.  Slot 0 is the
  ;; null sentinel.  See §7.1 for why this is module-local rather
  ;; than imported from the runtime.
  (table  $cel_refs 16 externref)
  (func   $cel_ref_intern (param externref) (result i32) …)
  (func   $cel_unwrap_message (param i32) (result externref) …)

  (func $eval (result i64) …)
  (export "eval" (func $eval))
)
```

The host instantiates `runtime.wasm` once, collects its exports, and
passes them as the `"cel"` imports when instantiating each
`expression.wasm`.  This gives us:

- **Shared state by construction.**  Linear memory and the externref
  table are single-sourced from the runtime instance; eval modules
  load/store against the imported memory, and arena offsets stay
  valid across calls without any host-side marshalling.
- **Tiny per-expression modules.**  A scalar expression is hundreds
  of bytes (imports header + eval body + `cel.abi` custom section).
  Deployments with N expressions ship `runtime.wasm` + N × tiny
  modules instead of N copies of the ~2 KB runtime.
- **No merge step in codegen.**  The alternative — reading the
  runtime back in via `BinaryenModuleRead` and appending eval on
  top — would force a per-compile cost and drag the runtime bytes
  into every compiled module.  With imports we just emit the
  imports header and call; no Binaryen-level merge ever happens.

The tradeoff is that hosts now instantiate two modules and wire
imports.  This is one extra call on the host side
(`wasmtime_linker_define_instance` in C, similar in other runtimes)
and is boilerplate we own in `compiler/runtime/host_loader.{h,cc}`
so embedders don't reinvent it.  See §10 for the codegen pipeline
and §7.0.1 for the single-threaded concurrency contract.

### 7.0.1 Concurrency contract

A runtime instance is **single-threaded**.  Embedders that want
parallel CEL evaluation must instantiate one runtime per worker
thread.  This is a normative contract, not a recommendation — the
runtime is not thread-safe and the compiler makes no effort to
make it so.

The sharp edges if this rule is broken:

- **Bump allocator.**  `g_cel_arena.bump` is a plain `uint32_t` in
  linear memory, read-modify-written by every `cel_alloc` call.
  Two threads hitting it concurrently will both observe the same
  bump pointer, both write back `bump + n`, and receive
  overlapping offsets — silent memory corruption, not a crash.
- **`cel_refs_next` global.**  Same race, one slot number handed
  out to two `cel_ref_intern` callers.  The second `table.set`
  wins and the first `externref` is leaked (or aliased, worse).
- **`cel_reset`.**  Rewinding the arena on one thread invalidates
  every `CelValue*` the *other* thread is holding.  Use-after-reset
  is undefined and typically presents as a `CelKind` tag reading
  back as garbage.
- **Singletons (null, true, false, optional-none) and interned
  data segments** are read-only and therefore safe across threads
  — but they are the only such region.

What the recommended pattern costs: a runtime instance is ~2 KB
of code + a 64 KiB default arena = **~66 KiB per worker**.  The
compiled eval modules themselves are stateless and can be
instantiated against any runtime instance, so the `.wasm` files
are shareable; only the runtime instance is per-worker.
Workloads that evaluate the same expression over many inputs
should keep one eval instance per runtime (instance creation is
cheap but not free) and use `cel_reset` between calls.

What it would take to lift the single-threaded contract (not on
any roadmap — noted so a future workload can cost-estimate the
change rather than rediscover it):

- Compile against wasm's threads proposal (shared memory +
  `i32.atomic.*` ops) and turn on the `threads` feature in
  `BinaryenModuleSetFeatures`.
- Replace the bump allocator with a CAS loop on the bump pointer,
  or a per-thread arena chunk with CAS-for-grow.
- Replace `cel_reset`'s bulk rewind with per-call arena frames so
  one thread's rewind doesn't stomp another's in-flight values.
- Partition or lock-free-allocate the `$cel_refs` externref table.

A simpler incremental option — per-expression sub-arenas within
one thread (to bound peak memory across many small evaluations) —
does not require wasm threads and can land as an M4+ feature if
profiling justifies it.

### 7.1 Memory map

```
┌────────────────────────────────┐ 0x00000000
│ const data segment             │  interned literals, type/attr tables,
│ (emitted by compiler)          │  error message blobs, `cel.abi` payload
├────────────────────────────────┤ data_end
│ static globals                 │  g_arena, singletons (null, true, false),
│                                │  ref-table free list head
├────────────────────────────────┤ ← cel_mem_base()  (== &g_memory[0])
│ g_memory[0]                    │  runtime arena, addressed by the rest of
│   reserved null sentinel       │  the runtime via offsets RELATIVE to
│   static singletons            │  g_memory.  `cel_alloc` returns one of
│   bump arena ↓                 │  these relative offsets; all CelValue
│ g_memory[limit]                │  sub-pointers (CelSpan.ptr, CelArray.ptr,
├────────────────────────────────┤  etc.) are relative to g_memory too.
│ unused                         │  memory.grow if allocator exhausts
└────────────────────────────────┘
```

**Offsets are g_memory-relative, not linear-memory-absolute.**  The
runtime internally reconstructs absolute linear-memory addresses as
`g_memory + rel_offset`; for the C code this is free because
`g_memory` is a `uint8_t[]` symbol the compiler has already resolved
to its absolute offset.  External callers (host code reading a
`CelValue*`, or the eval module computing an `i32.store8` address)
must do the same translation by hand: call the exported
`cel_mem_base() → i32` once and add it to every offset before
dereferencing via linear memory.  Rationale: the native host tests
link `cel_runtime.c` directly and use `g_memory + off` pointer math,
so an ABI that returned absolute wasm offsets would require a
separate native-vs-wasm code path inside the runtime itself.  Keeping
offsets arena-relative and pushing the translation to the (single)
external caller — the eval module — keeps the runtime single-sourced.

The eval-module codegen caches `cel_mem_base() + scratch` once per
string literal and reuses it for the byte-store loop; see
§10.1.

Alongside linear memory, the module declares a private externref table
(`$cel_refs`) with a **bump allocator** — slots are handed out
monotonically via a `cel_refs_next` module global, slot 0 reserved as
the null sentinel:

```wat
(table  $cel_refs <initial> externref)
(global $cel_refs_next (mut i32) (i32.const 1))
```

`cel_ref_intern` reads `cel_refs_next`, `table.set`s the externref into
that slot, increments the global, and returns the slot index.
`cel_refs_reset` resets `cel_refs_next` back to 1 so the ref-table
lifetime matches the arena lifetime and is wiped in lockstep with
`cel_reset`.  The earlier design sketched a free-list over released
slots; that's overkill for the current workload (expressions evaluate
against one input per call, then `cel_reset` wipes everything) and was
not shipped.  If a future workload holds CelValues across resets or
evaluates over long-lived message graphs, replace the bump with a
proper free-list here — at that point the global becomes the free-list
head and the release path does a LIFO push.

### 7.2 The `CelValue` struct

```c
// compiler/runtime/cel_runtime.h

#include <stdint.h>

typedef enum : uint8_t {
  CEL_NULL       = 0,
  CEL_BOOL       = 1,
  CEL_INT        = 2,   // signed 64
  CEL_UINT       = 3,   // unsigned 64
  CEL_DOUBLE     = 4,
  CEL_STRING     = 5,
  CEL_BYTES      = 6,
  CEL_LIST       = 7,
  CEL_MAP        = 8,
  CEL_MESSAGE    = 9,   // host-owned, opaque handle
  CEL_TYPE       = 10,
  CEL_DURATION   = 11,
  CEL_TIMESTAMP  = 12,
  CEL_OPTIONAL   = 13,  // value_ptr == 0 ⇒ absent
  CEL_UNKNOWN    = 14,  // payload is UnknownSet*
  CEL_ERROR      = 15,  // payload is Error*
} CelKind;

typedef struct { uint32_t ptr; uint32_t len; } CelSpan;      // string/bytes
typedef struct { uint32_t ptr; uint32_t len; } CelArray;     // CelValue*[]
typedef struct { uint32_t pairs_ptr; uint32_t len; } CelMap; // {key, val}[]

typedef struct {
  int64_t seconds;
  int32_t nanos;
  int32_t _pad;
} CelDurTs;

typedef struct CelValue CelValue;

struct CelValue {
  uint32_t kind;           // CelKind, widened to u32 for alignment
  uint32_t _pad;           // keeps union 8-byte-aligned
  union {
    int32_t    b;
    int64_t    i;
    uint64_t   u;
    double     d;
    CelSpan    s;          // string UTF-8 bytes
    CelSpan    bytes;
    CelArray   list;       // list(E): array of CelValue*
    CelMap     map;        // map(K,V): array of {CelValue* k, CelValue* v}
    uint32_t   msg_slot;   // index into $cel_refs externref table
    uint32_t   type_id;    // interned
    CelDurTs   dur;
    CelDurTs   ts;
    uint32_t   opt;        // CelValue* (0 ⇒ absent)
    uint32_t   unk;        // UnknownSet*
    uint32_t   err;        // Error*
  };
};                         // 24 bytes
```

Every sub-pointer (`CelSpan.ptr`, `CelArray.ptr`, `CelMap.pairs_ptr`,
`CelOptional.opt`, etc.) is an offset **relative to `g_memory`**, not
to linear-memory base 0.  See §7.1's "Offsets are g_memory-relative"
note for the translation rule and its rationale.

### 7.3 Auxiliary structs

```c
typedef struct {
  uint32_t ids_ptr;      // uint32_t[]
  uint32_t ids_len;
} UnknownSet;

typedef struct {
  uint32_t code;         // ErrorCode: OVERFLOW, DIV_BY_ZERO, NO_SUCH_FIELD,
                         //            NO_SUCH_KEY, NO_MATCHING_OVERLOAD, …
  CelSpan  msg;          // UTF-8 message (ptr may point into const data)
} Error;
```

### 7.4 Allocator

```c
typedef struct {
  uint32_t bump;
  uint32_t limit;
} Arena;

// Single global instance; cel_reset rewinds to the post-static boundary.
extern Arena g_arena;

// Exported to the host.
uint32_t cel_alloc(uint32_t n);   // 8-byte-aligned bump alloc
void     cel_reset(void);         // rewind arena between evals
uint32_t cel_mem_base(void);      // absolute linear-memory address of g_memory[0]
```

No `cel_free`. Hosts call `cel_reset()` after every `eval` to reclaim.

`cel_alloc` returns an offset **relative to `g_memory`**. External callers
that want to write through linear memory (eval module codegen, host
embedders, tests) must add `cel_mem_base()` to the result before doing
any `i32.store*` / memcpy. The view constructors (`cel_string_view`,
`cel_bytes_copy`, …) continue to take the arena-relative offset — the
runtime reconstructs `g_memory + rel` internally on every dereference,
which is what lets the same C sources link into both wasm32 and
native-host builds.

### 7.5 Constructors

All return a `CelValue*`. Null, true, false, and the type literals are
statically allocated singletons so construction is free.

```c
CelValue* cel_null(void);
CelValue* cel_bool(int32_t b);
CelValue* cel_int(int64_t i);
CelValue* cel_uint(uint64_t u);
CelValue* cel_double(double d);

// Copies [src, src+len) into a fresh arena span and builds a string value.
CelValue* cel_string_copy(const char* src, uint32_t len);

// Wraps an already-arena-resident span without copying.
CelValue* cel_string_view(uint32_t ptr, uint32_t len);

CelValue* cel_bytes_copy(const void* src, uint32_t len);
CelValue* cel_bytes_view(uint32_t ptr, uint32_t len);

CelValue* cel_list_new(uint32_t cap);         // empty; elem_kind inferred
void      cel_list_append(CelValue* l, CelValue* e);

CelValue* cel_map_new(uint32_t cap);
void      cel_map_put(CelValue* m, CelValue* k, CelValue* v);

// Emitted per-eval-module by codegen via compiler/codegen/cel_refs.{h,cc}
// (not in the runtime — C can't author externref table.set/.get, and
// the table itself is module-local, see §7.1).  Stashes the externref
// in a free $cel_refs slot and wraps it in a CelValue.
CelValue* cel_wrap_message(externref msg);

// Inverse: extracts the externref for a CEL_MESSAGE value.  Takes the
// arena-relative CelValue offset (not the msg_slot) and performs the
// cel_mem_base + cv + 8 load + table.get internally.  Callers hand it
// the scratch offset directly.
externref cel_unwrap_message(CelValue* v);

// Low-level ref-table helpers (same emitter, module-local).
uint32_t  cel_ref_intern(externref r);   // returns slot index
externref cel_ref_get(uint32_t slot);
void      cel_refs_reset(void);          // wipe all slots on cel_reset
CelValue* cel_type(uint32_t type_id);
CelValue* cel_duration(int64_t seconds, int32_t nanos);
CelValue* cel_timestamp(int64_t seconds, int32_t nanos);

CelValue* cel_optional_some(CelValue* inner);
CelValue* cel_optional_none(void);

CelValue* cel_unknown(uint32_t attribute_id);
CelValue* cel_unknown_merge(CelValue* a, CelValue* b);

CelValue* cel_error(uint32_t code, uint32_t msg_ptr, uint32_t msg_len);
```

### 7.6 Accessors, ops, and logic

```c
int32_t   cel_is_ok(const CelValue* v);
int32_t   cel_kind(const CelValue* v);
int32_t   cel_truthy(const CelValue* v);     // bool extraction

// Scalar ops (overflow → CEL_ERROR)
CelValue* cel_int_add(CelValue* a, CelValue* b);
CelValue* cel_int_sub(CelValue* a, CelValue* b);
// … mul, div, mod, uint variants, double variants
CelValue* cel_num_lt(CelValue* a, CelValue* b);
CelValue* cel_num_eq(CelValue* a, CelValue* b);
// … le, gt, ge, ne

// String / bytes
CelValue* cel_string_eq(CelValue* a, CelValue* b);
CelValue* cel_string_size(CelValue* s);
CelValue* cel_string_contains(CelValue* s, CelValue* sub);
CelValue* cel_string_starts_with(CelValue* s, CelValue* pfx);
CelValue* cel_string_ends_with(CelValue* s, CelValue* sfx);
CelValue* cel_string_matches(CelValue* s, uint32_t pattern_id);
                                             // regex pre-compiled; see §10

// Collections
CelValue* cel_list_size(CelValue* l);
CelValue* cel_list_get(CelValue* l, CelValue* idx);
CelValue* cel_list_contains(CelValue* l, CelValue* e);
CelValue* cel_map_size(CelValue* m);
CelValue* cel_map_get(CelValue* m, CelValue* k);
CelValue* cel_map_has(CelValue* m, CelValue* k);

// Three-valued logic
CelValue* cel_and(CelValue* a, CelValue* b);
CelValue* cel_or(CelValue* a, CelValue* b);
CelValue* cel_not(CelValue* a);

// First non-OK wins; helper used for non-absorbing operators' propagation.
CelValue* cel_status_either(CelValue* a, CelValue* b);
```

These are regular WASM functions exported from the runtime module, not host
imports. They inline well in Binaryen; most are straight struct manipulation.

## 8. Host ABI (normative, language-independent)

All imports live in module `cel_host` (built-ins) and `cel_fn` (user
functions). Every signature speaks only `i32` / `i64` / `f64`. Return values
that are CEL-typed are `CelValue*`. The host MAY read/write linear memory
directly via the module's exported `memory`.

### 8.1 Module exports (visible to the host)

```wat
(memory (export "memory") <initial> <max>)

(func (export "cel_alloc") (param i32) (result i32))
(func (export "cel_reset"))
(func (export "cel_mem_base") (result i32))  ;; §7.1: absolute address of g_memory[0]

;; Constructors the host needs to build inputs or fill field values.
(func (export "cel_null")           (result i32))
(func (export "cel_bool")           (param i32) (result i32))
(func (export "cel_int")            (param i64) (result i32))
(func (export "cel_uint")           (param i64) (result i32))
(func (export "cel_double")         (param f64) (result i32))
(func (export "cel_string_copy")    (param i32 i32) (result i32))
(func (export "cel_string_view")    (param i32 i32) (result i32))
(func (export "cel_bytes_copy")     (param i32 i32) (result i32))
(func (export "cel_list_new")       (param i32) (result i32))
(func (export "cel_list_append")    (param i32 i32))
(func (export "cel_map_new")        (param i32) (result i32))
(func (export "cel_map_put")        (param i32 i32 i32))
(func (export "cel_unknown")        (param i32) (result i32))
(func (export "cel_error")          (param i32 i32 i32) (result i32))

;; Message (externref) helpers.  These are NOT runtime exports — they
;; are emitted per-eval-module by codegen (see §7.0 / §7.1).  Listed
;; here only because the host SDK reaches them through the eval
;; module when unwrapping a CEL_MESSAGE return value.
;;   (func (export "cel_ref_intern")     (param externref) (result i32))
;;   (func (export "cel_ref_get")        (param i32)       (result externref))
;;   (func (export "cel_wrap_message")   (param externref) (result i32))
;;   (func (export "cel_unwrap_message") (param i32)       (result externref))

;; The compiled expression.  $arg_msg is an externref for a top-level
;; host-owned message input; the compiler emits one param per input,
;; matching each input's static type.  Return shape follows the top-level
;; expression's Repr: i64 for int, f64 for double, i32 for bool /
;; string* / bytes* / CelValue*, externref for message.
(func (export "eval")
      (param $arg_msg externref) (param $arg_scalar i32) ... (result i32))
```

Host-to-module flow for scalar inputs: host calls `cel_alloc` to reserve
`len` bytes in the arena, translates the returned arena offset to an
absolute linear-memory address via the exported `cel_mem_base()` (see
§7.1), writes the payload bytes there, and then calls
`cel_string_view(rel_offset, len)` — the view constructor takes the
**arena-relative** offset, not the absolute one. For message inputs the
host passes its `externref` straight into `eval`; the compiler emits a
`cel_wrap_message` inside `eval` before the first use. Return is always
`CelValue*` (i32, arena-relative), which the host translates through
`cel_mem_base` before reading from linear memory and — when
`kind == CEL_MESSAGE` — unwraps via `cel_unwrap_message`.

### 8.2 Imports (module satisfies from host)

Imports that take or return a host-owned message use `externref` directly.
Everything else speaks `i32` (either a `CelValue*` in linear memory or a
field number / interned id).

The core shape is a **unified out-parameter**: the module pre-allocates a
24-byte `CelValue` in the arena via `cel_alloc(24)` and hands its
arena-relative offset to the host.  The host writes `kind` and the
appropriate `payload` bytes in place — including `CEL_UNKNOWN` or
`CEL_ERROR` if resolution fails.  This sidesteps the
static-return-shape problem that split `get_scalar_field` /
`get_message_field` had (unknown / error is reachable at every field
read, regardless of the field's static type).

The M3 surface — what codegen emits today — is three imports under the
`"cel_host"` namespace:

```wat
;; Field read.  Writes the payload + kind into *out_cv in place.
;; For string / bytes the host also calls cel_alloc(len) to reserve
;; span storage and populates payload.s.{ptr,len}.  For message fields
;; the host writes kind=CEL_MESSAGE and payload.msg_slot = intern(ref).
;; UNKNOWN and ERROR propagate via kind=CEL_UNKNOWN / CEL_ERROR.
(import "cel_host" "get_field"
        (func (param externref i32 i32)))     ;; (msg, field_number, out_cv)

;; Presence test.  Host dispatches on proto3 presence rules (submessage
;; explicit-set, scalar-non-default) via google::protobuf::Reflection.
(import "cel_host" "has_field"
        (func (param externref i32) (result i32)))

;; Message equality (delegates to descriptor-aware protobuf equality —
;; per spec §1110 this is the unknown-fields-byte-equality variant that
;; can't be done module-side).
(import "cel_host" "message_eq"
        (func (param externref externref) (result i32)))
```

Field numbers are emitted as codegen immediates (the externref already
carries the descriptor pool, so no interning table is needed).  The
imports are declared unconditionally on every eval module — the "don't
gate `cel_host.*` imports on AST inspection" rule avoids a class of
bugs where a host provides a linker that doesn't know about imports
the module didn't happen to call.

The M3 surface does **not** cover repeated fields, maps, regex, or
type-of-message — those land with the collections milestones:

- `repeated_len` / `repeated_get_*` — **M5** (list codegen).
- `map_keys_count` / `map_get_*` / `map_iter` — **M5** (map codegen).
- `string_matches(cv, pattern_id)` — **M7** (regex stdlib).
- `message_type_of(ref) → type_id` — **M7** (type reflection).

When they land, extend this section with the new imports under the
same out-parameter convention (hand the host a pre-allocated CelValue
to write into, rather than multi-value returning status + ref + cv).
The pattern ID and type ID immediates wire through `cel.abi.patterns`
and `cel.abi.types` respectively (Appendix A).

### 8.3 Custom functions

```wat
(import "cel_fn" "<overload_id>"
        (func (param i32 i32 ...) (result i32)))
```

Each parameter is a `CelValue*`. Return is a `CelValue*`. The host registers
implementations under `overload_id`; these may return UNKNOWN or ERROR by
returning a `CelValue` of that kind, constructed via the exported helpers.

### 8.4 Type identity & attribute identity

Emitted in a WASM custom section `cel.abi` (see Appendix A):

- `type_id: i32` — one per distinct message FQN referenced by the expression.
- `attribute_id: i32` — one per distinct (var, field-path) that could be
  unknown at input.
- `pattern_id: i32` — one per regex literal (pre-compiled at build time).
- `error_msg_id: i32` — one per potential error message (pre-interned).

Hosts parse the custom section, align their runtime tables, and only then
instantiate the module.

### 8.5 Partial-evaluation entry point

The host may mark specific inputs as UNKNOWN:

```c
CelValue* x = cel_unknown(attribute_id_for_user_email);
eval(x, ...);
```

UNKNOWNs propagate by construction through every op that reads them.

## 9. Three-valued logic

### 9.1 Propagation in non-absorbing operators

Every op (arithmetic, comparison, field access, call, ternary condition,
collection construction) evaluates children left-to-right. The first child
whose `CelValue->kind` ∈ {CEL_UNKNOWN, CEL_ERROR} is returned immediately —
the op is skipped.

### 9.2 Absorption in `&&` / `||`

Per `doc/langdef.md` §"Logical Operators": commutative, both sides always
evaluated, error/unknown absorbed by a concrete short-circuit value.
`cel_and(a, b)`:

```c
CelValue* cel_and(CelValue* a, CelValue* b) {
  int a_false = a->kind == CEL_BOOL && a->b == 0;
  int b_false = b->kind == CEL_BOOL && b->b == 0;
  if (a_false || b_false)                      return cel_bool(0);
  if (a->kind == CEL_BOOL && b->kind == CEL_BOOL) return cel_bool(1);
  return cel_status_either(a, b);
}
```

`cel_or` is dual.

### 9.3 Priority between ERROR and UNKNOWN

The spec leaves this implementation-defined. We choose **UNKNOWN wins over
ERROR** in non-absorbing combinations. Rationale: if the unknown inputs were
resolved, the expression might yet succeed; returning UNKNOWN preserves the
"resolve and re-run" contract. `cel_status_either`:

```c
CelValue* cel_status_either(CelValue* a, CelValue* b) {
  if (a->kind == CEL_UNKNOWN && b->kind == CEL_UNKNOWN)
      return cel_unknown_merge(a, b);
  if (a->kind == CEL_UNKNOWN) return a;
  if (b->kind == CEL_UNKNOWN) return b;
  if (a->kind == CEL_ERROR)   return a;
  if (b->kind == CEL_ERROR)   return b;
  return a;                                    // both OK (unused path)
}
```

## 10. Code generation

Codegen uses **Binaryen's C API** (`binaryen-c.h`) — the officially
stable public surface that Binaryen's own CMake `install` target
exports.  Binaryen itself is built through its own CMakeLists.txt
inside Bazel via `rules_foreign_cc`, using a tarball vendored at
`third_party/binaryen/` and pinned in `third_party/binaryen.sha`.
The result is a single `cc_library` exposing `libbinaryen.a` +
`binaryen-c.h`, which `compiler/codegen/*` links against.

The C API is preferred over the C++ API because (a) it is the
interface Binaryen commits to keeping stable across versions, (b) it
is what `cmake --install` emits publicly, and (c) it is the surface
that third-party tooling (Emscripten, wasm-opt users, etc.) already
drives — so version bumps are low-risk.

The compiler:

1. Creates a fresh `BinaryenModuleRef` via `BinaryenModuleCreate`.
2. Emits imports against the `"cel"` namespace for every runtime
   entity the per-expression lowering references: memory, the
   `$cel_refs` externref table, and the subset of `cel_*`
   functions this expression actually calls (walk the IR once and
   record them — no reason to import all 24 constructors if the
   expression is `1 + 2`).
3. Emits interned literal blobs (strings, regex patterns, error
   messages, type/attribute tables, `cel.abi` proto) into fresh
   data segments on the imported memory.  The data-segment offsets
   must be coordinated with the runtime's static region — the
   runtime reserves `[0, static_end)` and exports `static_end` as
   a global so codegen can place its interned blobs right after.
4. Appends generated helper functions and the exported `eval` via
   `BinaryenAddFunction` / `BinaryenAddFunctionExport` / etc.
5. Attaches the `cel.abi` custom section (per-expression metadata).
6. Serialises the module to `.wasm` with `BinaryenModuleWrite`.

No merge step: the runtime never enters the compiler's memory as a
Binaryen module.  The runtime `.wasm` is an artefact of the build
system (`compiler/runtime/BUILD.bazel`), not of the codegen path.

### 10.1 Lowering per expression kind

Every `CheckedExpr` node lowers to a single Binaryen expression returning an
`i32` — the pointer to a `CelValue`.

| CEL expression        | Emitted IR                                                                        |
| --------------------- | --------------------------------------------------------------------------------- |
| Literal               | `cel_<kind>(const)` for scalars; for strings/bytes: `cel_alloc(len)` → cache `cel_mem_base() + rel` in a local, store literal bytes through that absolute pointer, then `cel_string_view(rel, len)` (view takes the arena-relative offset) |
| Variable              | `local.get $argN`                                                                 |
| `e.f` (any field)     | `scratch = cel_alloc(24); cel_host.get_field(unwrap(e), field_number, scratch);` then a per-`Repr` payload load from `cel_mem_base() + scratch + 8` — scalars load inline, strings/bytes become a `cel_make_*_view` over `payload.s.{ptr,len}`, messages call `cel_unwrap_message(scratch)` to re-hydrate the externref |
| `has(e.f)`            | `cel_host.has_field(unwrap(e), field_number)` — returns i32 0/1 directly; no scratch CelValue needed |
| `e1 == e2` (message)  | `cel_host.message_eq(unwrap(e1), unwrap(e2))` — wrapped in `i32.eqz` for `!=` |
| `m[k]` (list)         | `cel_list_get(m, k)`                                                              |
| `m[k]` (map)          | `cel_map_get(m, k)`                                                               |
| `a OP b` (arith)      | `cel_int_add(a, b)` (etc.) — op emits error on overflow                           |
| `a OP b` (relation)   | `cel_num_lt(a, b)` (etc.)                                                         |
| `a && b`              | evaluate both; `cel_and(a, b)`                                                    |
| `a ? t : e`           | evaluate `a`; if non-OK return it; else branch on `cel_truthy(a)`                 |
| Call (stdlib inline)  | inlined sequence of runtime calls                                                 |
| Call (host stdlib)    | import call in `cel_host`                                                         |
| Call (user fn)        | import call in `cel_fn.<overload_id>`                                             |
| List literal          | `cel_list_new(n)` + `n` × `cel_list_append`                                       |
| Map literal           | `cel_map_new(n)` + `n` × `cel_map_put`                                            |
| Message literal       | host constructor import (`cel_host.message_new_<type_id>`)                        |
| Comprehension         | WASM `loop` over list/map; accu written to a local `CelValue*`                    |

For non-absorbing operators, the propagation is a two-instruction guard
inlined at each call site:

```wat
call $cel_sub_a              ;; leaves CelValue* on the stack
local.tee $a
call $cel_is_ok              ;; returns i32
i32.eqz
if (result i32)
  local.get $a               ;; propagate as-is (kind is UNKNOWN or ERROR)
else
  call $cel_sub_b
  local.tee $b
  call $cel_is_ok
  i32.eqz
  if (result i32)
    local.get $b
  else
    local.get $a
    local.get $b
    call $cel_int_add        ;; actual op
  end
end
```

Binaryen optimizes these away when a branch is a constant.

### 10.2 Overflow handling

`cel_int_add_ii` / `_sub_ii` / `_mul_ii` / `_div_ii` / `_mod_ii` and their
`_uu` uint counterparts implement the spec's checked arithmetic internally
and return a boxed `CelValue*` that is either OK(int/uint) or an ERROR with
`code=OVERFLOW` / `DIVIDE_BY_ZERO` / `MODULUS_BY_ZERO`.  Codegen never
inlines raw `i64.add` for CEL ints — it always calls the runtime op, which
inlines at the WASM level but keeps the error path correct.

**Current trap-on-ERROR stopgap (M4 Slice B, 2026-04-19).**  The helpers
return CEL_ERROR values correctly, but the codegen for an arithmetic node
currently emits

```
Block(
  LocalSet($tmp, Call(cel_int_add_ii, …)),     ;; boxed CelValue*
  If(i32.load8_u offset=0 ($tmp) == CEL_ERROR,  ;; kind byte
     Unreachable),                              ;; trap
  i64.load offset=8 ($tmp))                    ;; unbox the scalar payload
```

so on ERROR the module traps and wasmtime surfaces it as
`absl::InternalError("... trapped: ...")`.  That is observable at the host
(the testing-checklist row is closed) but it is **not** the spec-correct
three-valued behaviour: `1/0 || true` should short-circuit to `true`, not
trap, and the 3VL retrofit in slices C+ of M4 needs every arithmetic node
to hand back a real `CelValue*` that `cel_and` / `cel_or` / `?:` can
absorb.  The gap is tracked as an open question at the end of this
document ("Trap-vs-observable-CEL_ERROR").

### 10.3 Comprehension lowering and scope management

The codegen maintains a **scope stack** that mirrors the checker's:

```cpp
struct ScopeFrame {
  absl::flat_hash_map<std::string, WasmLocalIndex> bindings;
  ScopeFrame* parent;  // nullptr at the eval()-root frame
};

// Lookup walks inner → outer. A name never found here has already been
// resolved by the checker to a global / host input — the reference_map
// entry tells codegen to emit a local.get on the corresponding eval()
// parameter instead.
WasmLocalIndex ResolveIdent(absl::string_view name);
```

A comprehension's body is lowered under a **fresh scope frame**:

1. Before emitting `loop_condition` / `loop_step` / `result`, push a new
   `ScopeFrame` whose parent is the current frame.
2. Allocate three (or four for map comprehensions) WASM locals:
   `$iter_var_N`, `$iter_var2_N` (optional), `$accu_var_N`, plus internal
   `$i_N`, `$n_N`, `$range_N`, `$cond_N`. `N` is a fresh integer per
   comprehension so no two nested comprehensions collide on local names
   even if they reuse the same CEL variable name (`x` shadowing `x`).
3. Insert `bindings[iter_var_name]  = $iter_var_N` etc. into the new
   frame. If the CEL source also had a leading-dot reference to the same
   name, that reference's `reference_map` entry points to the global
   declaration, not the local, so codegen naturally routes it to the
   `eval` parameter — no extra work.
4. Lower the body. Nested comprehensions push additional frames; their
   `ResolveIdent` walks the chain correctly.
5. Pop the frame after `result` is lowered.

Binaryen locals are function-level (not block-level) in WASM 1.0, so every
scope frame's allocations live in the same `$eval` function's local pool.
Uniqueness is enforced by the `_N` suffix at codegen time; the scope stack
is what controls **visibility** during name resolution.

Worked example of shadowing: `[1].exists(x, [2].exists(x, x == 2))`

```wat
(func $eval (result i32)
  (local $range_0 i32) (local $i_0 i32) (local $n_0 i32)
  (local $iter_0  i32) (local $accu_0 i32) (local $cond_0 i32)
  (local $range_1 i32) (local $i_1 i32) (local $n_1 i32)
  (local $iter_1  i32) (local $accu_1 i32) (local $cond_1 i32)
  ;; outer exists: binds x -> $iter_0 in scope frame 0
  ;;   body: [2].exists(x, x == 2)
  ;;     inner exists: pushes frame 1, binds x -> $iter_1
  ;;       body: x == 2   → resolves x to $iter_1 (innermost wins)
  ;;     after inner: pops frame 1; x in outer frame is still $iter_0
  ...
)
```

Pseudocode for emitting a single comprehension:

```wat
;; === push scope frame, allocate locals $iter_N, $accu_N, $cond_N, ... ===

;; accu_var ← accu_init (typed in the *parent* scope — accu_init doesn't
;; see iter_var yet)
(local.set $accu_N (call $accu_init))

;; iter_range (also typed in the parent scope)
(local.set $range_N (call $compute_range))

;; partial-eval guard: if iter_range is UNKNOWN/ERROR, propagate
;; immediately — we never execute the loop, and the scope frame is
;; popped without ever binding iter_var.
(if (i32.eqz (call $cel_is_ok (local.get $range_N)))
  (then (return (local.get $range_N))))

(local.set $i_N (i32.const 0))
(local.set $n_N (call $cel_list_size_raw (local.get $range_N)))

(loop $L_N
  (br_if $end_N (i32.ge_u (local.get $i_N) (local.get $n_N)))

  ;; bind iter_var for this iteration
  (local.set $iter_N
             (call $cel_list_get_raw (local.get $range_N)
                                     (local.get $i_N)))

  ;; cond = loop_condition(accu, iter_var) — body references resolve
  ;; via the scope stack, so any `iter_var` in the body here reads
  ;; $iter_N.
  (local.set $cond_N (call $loop_condition))

  ;; Short-circuit when cond is a concrete false. UNKNOWN/ERROR do not
  ;; terminate the loop (the status is already captured in accu via
  ;; loop_step's propagation).
  (if (i32.and (call $cel_is_ok (local.get $cond_N))
               (i32.eqz (call $cel_truthy (local.get $cond_N))))
    (then (br $end_N)))

  (local.set $accu_N (call $loop_step))
  (local.set $i_N    (i32.add (local.get $i_N) (i32.const 1)))
  (br $L_N)
)

;; result expression still sees the scope's bindings
(local.set $result (call $result_expr))

;; === pop scope frame ===
```

### 10.4 Map comprehensions

When `iter_range` is a `map(K, V)`, cel-cpp's parser emits a
`Comprehension` with two iteration variables (`iter_var` = key,
`iter_var2` = value). Lowering is identical except:

- Iteration uses `cel_map_iter` / `cel_map_iter_next` (or in-arena map's
  `pairs_ptr` directly).
- The scope frame binds **both** `iter_var` → `$iter_N_k` and
  `iter_var2` → `$iter_N_v`.
- Partial-eval on an UNKNOWN/ERROR iter_range propagates identically.

## 11. Standard library

Every overload in `doc/langdef.md` §"Standard Definitions" gets a
`StdlibEntry` in a static table:

```cpp
struct StdlibEntry {
  std::string    overload_id;
  cel::Type      result;
  std::vector<cel::Type> params;
  enum { RuntimeFn, HostImport } backend;
  std::string    symbol;          // runtime function name or import name
};
```

Every inlineable op is a runtime function (`cel_int_add`, `cel_string_eq`,
…). Regex and protobuf equality go to host imports (`cel_host.string_matches`,
`cel_host.message_eq`). Macros are handled in the checker / parser — never
reach codegen as calls.

## 12. Custom functions

### 12.1 `FunctionSet` proto

```proto
syntax = "proto3";
package cel.wasm.compiler;

import "google/api/expr/v1alpha1/checked.proto";

message FunctionSet {
  repeated FunctionDef functions = 1;
}

message FunctionDef {
  string name = 1;                     // CEL-visible name
  repeated Overload overloads = 2;
  string doc = 3;
}

message Overload {
  string id = 1;                       // stable id, used as import name
  repeated google.api.expr.v1alpha1.Type params = 2;
  google.api.expr.v1alpha1.Type result = 3;
  google.api.expr.v1alpha1.Type receiver = 4;   // set ⇒ member call
  bool is_pure = 5;                              // enables CSE
  map<string, string> annotations = 6;
}
```

We reuse `google.api.expr.v1alpha1.Type` so the checker and code generator
share one type representation with cel-cpp.

### 12.2 `.celfn` IDL

```
# validators.celfn
import "cel/validators/address.proto"

fn isEmail(s: string) -> bool
fn string.matchesRegex(pat: string) -> bool
fn validate(a: cel.validators.Address) -> bool          [pure]
fn normalize(a: cel.validators.Address)
    -> cel.validators.Address                           [pure]
fn lookupUser(id: int) -> cel.users.User
```

Hand-written recursive-descent parser compiles to `FunctionSet`. IDs default
to `name_flatparams` (`isEmail_string`, `matchesRegex_string_string`). Tool:
`celfnc`.

## 13. Project layout

```
cel-spec-wasm/
  doc/
    wasm-compiler-design.md           # this file
  third_party/
    cel-cpp/                          # shallow clone; reused parser/checker/common
  compiler/
    BUILD.bazel                        # aggregator
    build_rules/
      wasm.bzl                         # wasm_cc_library rule (clang+wasm-ld)
    runtime/
      BUILD.bazel
      cel_runtime.h                    # CelValue, API
      cel_runtime.c                    # bump allocator, constructors, ops
      cel_runtime_refs.wat             # ref-table helpers (table.set/get)
    proto/
      BUILD.bazel
      function_set.proto
      cel_abi.proto
    frontend/
      BUILD.bazel
      parse_and_check.cc / .h          # wraps cel-cpp parser + checker
    ir/
      BUILD.bazel
      ir.h
      lower_ast.cc
    codegen/
      BUILD.bazel
      wasm_abi.h                       # import/export names, ABI constants
      codegen.cc                       # Binaryen builder
      stdlib.cc                        # StdlibTable
      runtime_loader.cc                # loads cel_runtime.wasm at startup
    idl/
      BUILD.bazel
      celfn_parser.cc
      celfnc_main.cc
    cli/
      BUILD.bazel
      celwasmc_main.cc
    tests/
      BUILD.bazel
      unit/                            # gtest
      golden/                          # .cel → .wat fixtures
      host_cpp/                        # minimal C++ host built on Wasmtime
      conformance/                     # drives tests/simple/testdata/
```

## 14. Build

This repo and cel-cpp are both Bazel-native; we stay on Bazel (bzlmod).

- Root `MODULE.bazel` already declares `module(name = "cel-spec")`. We add
  cel-cpp via:
  ```starlark
  bazel_dep(name = "cel-cpp", version = "0.0.0")
  local_path_override(module_name = "cel-cpp", path = "third_party/cel-cpp")
  ```
  cel-cpp's own `bazel_dep(name = "cel-spec", version = "0.25.1")` resolves
  to the root module automatically (root wins in bzlmod version selection),
  so we do not ship a self-dependency cycle.
- Direct C++ dependencies we pull in: `@cel-cpp//parser:parser`,
  `@cel-cpp//checker:...`, `@com_google_absl//...`,
  `@com_google_protobuf//:protobuf`, and `@com_googlesource_code_re2//:re2`.
  All of these arrive transitively through cel-cpp's `MODULE.bazel`.
- Binaryen: vendored as a pinned tarball under `third_party/binaryen/`
  (bootstrapped by `third_party/fetch_binaryen.sh`, SHA pinned in
  `third_party/binaryen.sha`) and built through its own CMakeLists.txt
  via `rules_foreign_cc`'s `cmake` rule.  We consume the installed
  `libbinaryen.a` + `binaryen-c.h`.  We do not hand-write a Bazel BUILD
  over Binaryen's source tree (avoids tracking its internal source-set
  churn) and we do not vendor a second codegen library.
- C++ toolchain: C++20, `compilation_mode=opt` for releases.
- Targets:
  - `//compiler/cli:celwasmc` — the compiler binary.
  - `//compiler/idl:celfnc` — the `.celfn` → `FunctionSet` tool.
  - `//compiler/runtime:cel_runtime_wasm` — produces `cel_runtime.wasm`
    by driving clang against `cel_runtime.c` with
    `--target=wasm32-unknown-unknown -nostdlib -O2`, then `wasm-ld`. We
    register a small `wasm_cc_library` rule in
    `compiler/build_rules/wasm.bzl` that pins these flags.
  - `//compiler/tests/...` — GoogleTest unit + golden tests.
- The runtime `.wasm` is packaged as a Bazel `data` dependency of
  `celwasmc`; the binary mmaps it at startup via `runtime_loader.cc`.
- Conformance harness runs under Bazel via `wasmtime-cpp` pulled from its
  Bazel module.

## 15. Milestones

Each row has a matching implementation-plan file under
`implementation-plan/`; that file holds the authoritative deliverable
list + testing obligations.  This table is a one-line summary for
orientation only.

| M   | Plan file                                  | Status       | Scope                                                                      |
| --- | ------------------------------------------ | ------------ | -------------------------------------------------------------------------- |
| M0  | `m0-parser-cli.md`                         | DONE         | Build cel-cpp parser + checker through Bazel; `celwasmc` prints CheckedExpr |
| M1  | `m1-type-checker.md`                       | DONE         | `cel_runtime.{c,h}` + constructors + type-checker integration + Repr IR |
| M2  | `m2-codegen-mvp.md`                        | DONE*        | Codegen of pure-primitive expressions (`1 + 2 * 3`, `&&`, `||`, `?:`); `cel_refs`, wasm32 cross-compile, and `cel.abi` custom section all landed 2026-04-18/19. *Asterisk: Linux cross-compile portability is still gated on the darwin-only brew path — tracked under the M2 "testing gaps" list, must close before the runtime becomes a hard CI dep.* |
| M3  | `m3-proto-and-strings.md`                  | DONE         | Proto field reads (`cel_host.get_field`, `has_field`), `has()`, string constants / equality / concat / size. Slices A+B (runtime wiring, host loader, string literals + equality), C (scalar `kIdentExpr`), D (`+`/`==`/`!=`/`size` on string), E (`startsWith`/`endsWith`/`contains`), F (bytes constants + operators), G1 (message params as externref + `$cel_refs` table / wrappers), G2 (`kSelectExpr` → `cel_host.get_field` with scalar payload loads + realistic `Customer` proto e2e fixture), G3 (`has(msg.field)` → `cel_host.has_field` via the `test_only` branch of `LowerSelect`, sharing field-number + operand validation with G2 through `LowerSelectOperand`), G4 (nested-message select via a `Repr::kMessage` arm in `LoadSelectPayload` calling `cel_unwrap_message` + `_==_` on `Repr::kMessage` dispatching to `cel_host.message_eq`; host-side `BindEvalInterner` now runs after eval instantiation so `get_field` can intern submessages), CLI schema integration (`--schema <file.proto>` parses textual proto source in-process via `google::protobuf::compiler::Parser`; new `--schema_descriptorset <file.pb>` accepts a pre-compiled `FileDescriptorSet`; flags are mutually exclusive, both land in the same `DescriptorPool`), and a richer e2e compose-test suite (seven cases spanning multi-param + mixed-stage shapes) all landed 2026-04-19. |
| M4  | `m4-three-valued.md`                       | PLANNED      | Partial eval: attribute interning, UnknownSet, overflow / div-by-zero / NaN → ERROR, `cel_status_either`. **Swapped with M5 on 2026-04-19** — the §8.2 host ABI leaks UNKNOWN / ERROR statuses that codegen needs a story for before collections can build on top. |
| M5  | `m5-collections-and-comprehensions.md`     | PLANNED      | List, map, struct literals + every comprehension macro + nested-shadowing scoping (§5.4) |
| M6  | `m6-custom-fns.md`                         | PLANNED      | User functions: `.celfn` IDL + `FunctionSet` proto, `celfnc` stub gen, `cel_fn.*` host imports |
| M7  | `m7-stdlib.md`                             | PLANNED      | Stdlib completeness: timestamps, durations, regex, bytes, string ext, format directives, proto ext |
| M8  | `m8-conformance.md`                        | PLANNED      | Conformance run against `tests/simple/testdata/` (static subset) — the release gate |

## Appendix A: the `cel.abi` custom section

```proto
message CelAbi {
  uint32 version = 1;
  string cel_source = 2;
  google.api.expr.v1alpha1.CheckedExpr checked = 3;
  repeated TypeIdEntry    types       = 4;   // (type_id, fqn)
  repeated AttributeEntry attributes  = 5;   // (attribute_id, var, path)
  repeated PatternEntry   patterns    = 6;   // (pattern_id, regex_src)
  repeated ErrorMessage   error_msgs  = 7;   // (error_msg_id, text)
  FunctionSet             function_set = 8;
  MemoryLayout            layout      = 9;   // initial/max pages, reserved regions
}
```

Hosts MUST read and verify this section before instantiation.

## Appendix B: minimal host worked example

Scenario: evaluate `request.user.name + "!" == greeting` under the
static subset, where `request` is a proto message the host already
owns, `greeting` is a host-provided `string`, and the expression was
compiled to `expr.wasm` by `celwasmc --emit_wasm`.

The host loads **two** modules: the shared `runtime.wasm` (one instance
per process, reused across every eval module) and the per-expression
`expr.wasm`.  The runtime's exports are rebound under the module
namespace `"cel"` via a linker before the eval module is instantiated —
`expr.wasm`'s imports name `(cel, cel_alloc)`, `(cel, memory)`,
`(cel, cel_string_concat)`, and so on, and the linker resolves them
against the runtime instance.  The C++ helper at
`compiler/host/host_loader.{h,cc}` (wasmtime) owns this dance; hosts in
other languages follow the same shape.

```c
// Pseudocode — any wasm runtime with a linker and externref support works.

// ---- One-time: instantiate the runtime and set up a linker. -------------
WasmEngine   engine = make_engine();
WasmStore    store  = make_store(engine);
WasmModule   rt_mod = compile(engine, load_file("runtime.wasm"));
WasmInstance rt     = instantiate(store, rt_mod, /*imports=*/{});
WasmLinker   linker = make_linker(engine);
linker.define_instance("cel", rt);          // re-export runtime under "cel"
linker.define_func("cel_host", "get_field",    host_get_field);
linker.define_func("cel_host", "has_field",    host_has_field);
linker.define_func("cel_host", "message_eq",   host_message_eq);

// ---- Per-expression: instantiate the eval module against the linker. ----
WasmModule   ev_mod = compile(engine, load_file("expr.wasm"));
WasmInstance ev     = linker.instantiate(store, ev_mod);

// Runtime exports, reached through the eval instance's imports or
// directly through `rt`.  Grabbed once and reused across evals.
Func cel_alloc            = rt.get_func("cel_alloc");
Func cel_mem_base         = rt.get_func("cel_mem_base");
Func cel_string_view      = rt.get_func("cel_string_view");
Func cel_unwrap_message   = rt.get_func("cel_unwrap_message");
Func cel_reset            = rt.get_func("cel_reset");
Memory mem                = rt.get_memory("memory");
Func eval                 = ev.get_func("eval");

// ---- Build inputs.  The eval signature is mixed, per Repr:
//        (param externref)        -- request      : message
//        (param i32)              -- greeting     : string (CelValue*)
//      bool -> i32, int/uint -> i64, double -> f64, string/bytes -> i32,
//      message -> externref.  See cel.abi.MemoryLayout for the per-param
//      mapping; numeric scalars travel as their native wasm type, not
//      as CelValue*.
ExternRef request_ref = make_externref(user_request);       // zero-copy

// Strings DO need a CelValue.  Allocate arena space, translate the
// g_memory-relative offset to an absolute linear-memory address via
// cel_mem_base(), write the UTF-8 bytes, then build a view whose ptr
// field is the ORIGINAL arena-relative offset (see §7.1, §7.4).
const char*  g     = "hello!";
uint32_t     g_len = strlen(g);
uint32_t     rel   = cel_alloc.call(g_len);                 // arena-relative
uint32_t     abs_  = cel_mem_base.call() + rel;             // absolute
memcpy(mem.data() + abs_, g, g_len);
uint32_t     greeting_cv = cel_string_view.call(rel, g_len);// CelValue*

// ---- Evaluate. ----------------------------------------------------------
uint32_t result = eval.call(request_ref, greeting_cv);      // i32 CelValue*

// Result is also arena-relative: translate before reading.
CelValueView v = read_value(mem.data() + cel_mem_base.call(), result);
switch (v.kind) {
  case CEL_BOOL:    return v.b;
  case CEL_MESSAGE: {
      ExternRef msg = cel_unwrap_message.call(result);
      // msg is the original (or a derived) host message.
      break;
  }
  case CEL_UNKNOWN: schedule_resolve_and_retry(v.unknown_ids); break;
  case CEL_ERROR:   log_error(v.err); break;
  default:          ...
}

// Reclaim arena (and rewind the ref-table free list) for the next eval.
// Invalidates every CelValue*, string payload, and ref slot — do any
// further reads from `v` BEFORE this call.
cel_reset.call();
```

## Open questions

- **Regex**: patterns are constants in a well-formed expression. Pre-compile
  each regex literal at build time, emit a `pattern_id`, and keep the
  compiled DFA on the host side under `cel_host.string_matches(s, pattern_id)`.
- **Timestamp / duration literals**: parse at build time, store
  `(seconds, nanos)` in the const data segment, materialize via
  `cel_timestamp` / `cel_duration`.
- **Message equality**: delegated to host (`cel_host.message_eq`) because
  unknown-field byte-equality (per spec §1110) needs the descriptor pool.
- **Map key ordering**: map literals must evaluate keys in source order and
  detect duplicates per spec. `cel_map_put` rejects duplicates with a
  `DUPLICATE_KEY` error.
- **Memory growth**: the allocator calls `memory.grow` when the arena fills.
  We budget an initial 64 KiB page and grow as needed; the host can request
  a larger initial size via the `cel.abi.layout` section.
- **Unified symbol table (decide before M5).** Today name/type/scope
  information is split across three partial structures with no shared
  owner:
    1. Frontend — `CheckOptions::variable_specs` holds user-provided
       `"name:Type"` strings that cel-cpp parses into `VariableDecl`s;
       only cel-cpp's internal scope stack is live, and only during
       checking.
    2. IR — `TypedAst::variables()` carries a flat
       `std::vector<Variable{name, Repr}>` for top-level params, and
       `WasmAnnotations` keeps a per-node `Repr` keyed by `expr_id`.
       The type info is a lossy projection of `cel::CheckedExpr`'s
       `type_map`, and the name list covers only the top-level frame.
    3. Codegen — `LoweringContext.idents`
       (`compiler/codegen/expr_lower.cc`) is a
       `flat_hash_map<string, BinaryenIndex>` rebuilt per compile,
       with no scope stack (`ComprehensionExpr` returns
       `UnimplementedKind`).
  .
  This is fine for the static subset we lower today (scalars, string
  ops, top-level idents) because everything resolves against the
  single top-level frame.  It stops being fine the moment
  comprehensions land in M5, because every macro-expanded
  `kComprehensionExpr` binds `iter_var` / `iter_var2` / accumulator in
  an inner scope that the codegen has to mirror exactly the way the
  checker did — and again in M6 when user functions introduce param
  frames, and again if we add a leading-dot rewrite pass (§5.4) that
  needs to see the checker's binding graph.
  .
  Two viable shapes, to be chosen **before the first M5 slice ships**:
    - **Option A — promote to a proper `SymbolTable` on `TypedAst`.**
      Built once during IR construction; carries `Repr`, wasm-local
      index, and scope parent per binding; codegen becomes a pure
      walker that never does string lookup.  More code up-front, but
      room for our own passes (leading-dot rewrite, attribute
      interning) without patching cel-cpp.
    - **Option B — side-table keyed off cel-cpp's `reference_map`.**
      For each `expr_id` that references a decl, store a
      `WasmBinding{repr, local_index}` entry; scopes become implicit
      in the AST because the checker already resolved every name.
      Less code, tighter coupling to cel-cpp's resolution model;
      makes our own passes harder.
  .
  Decision owner: whoever picks up M5 Slice A.  The design doc §10.3
  `ScopeFrame` sketch is Option A; if we take Option B, §10.3 needs a
  rewrite.  See also the checker-integration bullet in
  `doc/implementation-plan/m5-collections-and-comprehensions.md`.

- **Trap-vs-observable-CEL_ERROR for checked arithmetic (decide in
  M4 Slice C).**  Today the checked-arithmetic helpers return boxed
  `CelValue*` with a CEL_ERROR tag on overflow / div0 / mod0 (see
  §10.2), but the arithmetic codegen unboxes immediately: it traps
  (`BinaryenUnreachable`) on ERROR and then loads the `i64` payload so
  the rest of the emitted expression can consume a scalar.  The trap
  is observable at the host (wasmtime surfaces it as
  `absl::InternalError("... trapped: ...")`, which is good enough to
  close the testing-checklist row for "INT_MAX + 1") but it violates
  the 3VL propagation rules the spec actually requires:
    - `1/0 || true` must short-circuit to `true`, not trap.
    - `(1/0) == 0 ? "a" : "b"` must return the ternary's else-branch
      under the spec's "ERROR propagates through non-absorbing ops"
      rule, not trap at the divide.
    - Comprehension aggregators (`all` / `exists_one`) need to
      *inspect* an inner ERROR to decide whether to short-circuit.
  .
  The trap is a deliberate stopgap because today's arithmetic subtrees
  hand i64 / f64 scalars up to their parent op — there is no status
  channel in that ABI.  The fix is to give every arithmetic result
  room to carry a `kind` byte alongside its payload so `cel_and` /
  `cel_or` / `?:` / comprehension steps can absorb a CEL_ERROR
  without trapping.  Three ways to close the gap, in order of
  runtime cost:
    - **Option A — arena-boxed `CelValue*` (i32 offset).**  Every
      checked helper returns an i32 offset into the arena; every
      consumer of arithmetic (comparisons, `&&`, `||`, `?:`,
      comprehension bodies, calls) unboxes.  Uniform, matches the
      rest of the runtime ABI, but bumps the arena per op — even
      when the checker has proved the operands definite.  Mitigated
      by a checker-driven fast-path: when the parent already accepts
      a boxed value (like `cel_and`) skip the unbox; when two
      arithmetic ops compose with both operands definite, call a
      `_scalar` variant (`cel_int_add_scalar`) that returns a plain
      `i64` and traps on overflow (the M4 slice B retrofit already
      left room for this shape).
    - **Option B — sret / linear-memory "stack" CelValue.**  Helpers
      stay written in C; the caller reserves 24 bytes in a
      software-managed stack frame at the top of `eval` (decrement a
      stack-pointer global, compute an i32 offset), passes the
      offset as a hidden first argument, and the callee writes the
      CelValue through it.  This is clang's native wasm32 C ABI for
      struct returns, so no exotic toolchain dance is needed — the
      helper signature is just
      `void cel_int_add(CelValue* out, CelValue a, CelValue b)`
      (with `a` / `b` themselves passed by sret-style hidden ptr,
      since clang also passes 24-byte structs by indirect ref on
      wasm32).  No arena bump, lifetime ends with the `eval` call,
      one memory round-trip per op.
    - **Option C — true WASM multi-value on the value stack.**
      Helpers return `(i32 kind, i64 payload)` — or `(i32, i64, i64)`
      when ERROR metadata needs a second payload slot — directly
      via WASM's multi-value return.  Zero memory traffic for
      arithmetic chains: values stay in WASM locals / the value
      stack.  Catch: **clang's wasm32 C ABI does not emit multi-value
      returns for structs** — it always lowers struct returns to sret.
      Reaching Option C means either writing the checked helpers in
      hand-WAT, or emitting them directly via Binaryen's C API the
      same way codegen emits `$eval` today.  Both are buildable
      (wasmtime and every major engine enable multi-value by default,
      Binaryen's `BinaryenAddFunction` accepts tuple result types),
      but they move the runtime out of C.
  .
  Current lean: **Option A with a checker-driven scalar fast-path**
  as the near-term landing shape, **Option B as the mid-term
  upgrade** once we have a software stack frame for any other
  reason (comprehensions will need one anyway for accumulator
  spills, M5+).  Option C is only worth the C-to-WAT migration cost
  if benchmarking shows arena pressure dominates evaluation time,
  which we don't have data for yet.
  .
  Decision owner: whoever picks up M4 Slice C.  When the decision
  ships, §10.2 needs a rewrite that drops the "trap on ERROR"
  paragraph.

- **compiler-rt / wasi-sdk cross-platform strategy for the runtime
  wasm32 build (revisit when the next runtime dep appears).**  The
  current runtime compile is `clang -target wasm32 -ffreestanding
  -nostdlib -O2 …`, which deliberately excludes compiler-rt so the
  only wasm32 toolchain required on contributor machines is a
  reasonably modern clang.  That constraint shaped the M4 Slice B
  multiply: `__builtin_mul_overflow` on i64 lowers to `__multi3`
  (128-bit multiply in compiler-rt), which is absent from a
  freestanding link, so the runtime hand-rolls a 32×32→64 hi/lo split
  in `u64_mul_full` to defeat clang's overflow-idiom recognizer.
  .
  The question: **where does the line move when the next helper
  needs something wider than "basic clang"?**  Mul-overflow was a
  20-line workaround; popcount + ctz were free (clang lowers them as
  wasm instructions); but if a future milestone needs `fmod`,
  `strtod`, regex DFA construction, or 128-bit int division, the
  hand-roll cost climbs sharply.  Three options, in order of
  preference:
    - **Option A — keep hand-rolling** while the scope is small (one
      or two helpers per milestone), same as today.
    - **Option B — pull compiler-rt + wasi-libc via a Bazel
      `http_archive` pointing at an official
      [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) release
      tarball, hash-pinned.**  wasi-sdk ships pre-built cross-platform
      binaries (Linux / Mac / Windows) and a CMake toolchain file; the
      runtime `genrule` switches from `clang -target wasm32
      -ffreestanding -nostdlib` to `$(location @wasi_sdk//:clang)
      --sysroot=$(location @wasi_sdk//:sysroot)`.  Hermetic, no
      contributor setup step beyond `bazel build`.
    - **Option C — require `brew install wasi-runtimes` /
      `apt install wasi-sdk` / `choco install wasi-sdk` per OS.**
      **Rejected.**  Platform-specific package managers are not an
      acceptable required setup step for this repo; runtime builds
      must work on Linux, macOS, and Windows without a
      per-OS install recipe.  (Brew's `llvm` is grandfathered in
      because it was there before this constraint was articulated;
      that is not a licence to add more brew formulas.)
  .
  Current posture: **stay on Option A** until a slice genuinely
  can't be hand-rolled in <~50 lines of C.  At that point, switch
  the whole runtime wasm32 build to Option B in one go — don't do a
  half-migration that leaves some helpers hand-rolled and others
  pulling compiler-rt.  Decision owner: whoever first hits a helper
  that needs more than "basic clang + a bit of C".  When the
  decision ships, §10.2 and `doc/contributing.md` need updates.
