# CEL → WebAssembly AOT Compiler — Design

Status: **draft v2**. Target: a C++20 compiler that ingests CEL source + a
protobuf schema + a custom-function set, and emits one self-contained `.wasm`
module whose single exported `eval` returns a pointer to a `CelValue` tagged
union, under a three-valued logic (OK / UNKNOWN / ERROR) that falls out of the
tag field.

Sources of truth:

- Grammar: `Cel.g4` from `github.com/google/cel-cpp/parser/internal/Cel.g4`
  (vendored at `third_party/cel-cpp/parser/internal/Cel.g4`).
- Semantics: `doc/langdef.md` in this repo.
- Reusable C++: `third_party/cel-cpp/` (parser, checker, common type system).

---

## 1. Architectural stance (v3: both linear memory and externref)

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
             │  emits a standalone per-expression module whose only
             │  dependencies are imports from the shared runtime:
             │    (import "cel" "memory"     (memory …))
             │    (import "cel" "cel_refs"   (table  … externref))
             │    (import "cel" "cel_alloc"  (func   …))
             │    (import "cel" "cel_make_*" (func   …))
             │    …
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
- The `$cel_refs` externref table plus its three helpers
  (`cel_ref_intern`, `cel_ref_get`, `cel_refs_reset`).  (See §7.1.
  Authoring them inside the runtime rather than per eval module
  means every compiled expression shares one table, so interned
  refs are reusable across expressions within one host instance.)

### 7.0 Two-module architecture

Emitted expressions do **not** embed the runtime.  Every per-expression
module declares imports from a single module namespace named `"cel"`:

```wat
(module
  (import "cel" "memory"    (memory 1))
  (import "cel" "cel_refs"  (table  16 externref))
  (import "cel" "cel_alloc" (func (param i32) (result i32)))
  (import "cel" "cel_make_int" (func (param i64) (result i32)))
  ;; …one import per runtime function eval actually calls…
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
does not require wasm threads and can land as an M5+ feature if
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
(`$cel_refs`) with a small free-list. Slot 0 is reserved as the null
sentinel:

```wat
(table $cel_refs <initial> externref)
```

The free-list is a `uint32_t[]` in linear memory holding the indices of
released slots; the head is a module global. `cel_reset` also rewinds the
free-list to "all slots free except 0" so that ref-table lifetime matches
arena lifetime.

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

// Written in hand-emitted WAT/Binaryen (C can't portably execute table.set).
// Stashes the externref in a free $cel_refs slot and wraps it in a CelValue.
CelValue* cel_wrap_message(externref msg);

// Inverse: extracts the externref for a CEL_MESSAGE value, e.g. when we
// need to pass it back to a host import that expects externref.
externref cel_unwrap_message(CelValue* v);

// Low-level ref-table helpers (also WAT/Binaryen-authored).
uint32_t  cel_ref_intern(externref r);   // returns slot index
externref cel_ref_get(uint32_t slot);
void      cel_ref_release(uint32_t slot); // push onto free list
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

;; Message (externref) helpers — cross linear memory and the ref table.
(func (export "cel_wrap_message")   (param externref) (result i32))
(func (export "cel_unwrap_message") (param i32)       (result externref))
(func (export "cel_ref_intern")     (param externref) (result i32))
(func (export "cel_ref_get")        (param i32)       (result externref))

;; The compiled expression.  $arg_msg is an externref for a top-level
;; host-owned message input; the compiler emits one param per input,
;; matching each input's static type.
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
Everything else speaks `i32` (usually a `CelValue*` in linear memory). When
a field read can yield UNKNOWN / ERROR in addition to a message, the import
uses `multi-value` to return the status alongside the externref.

```wat
;; Scalar field access: host allocates a CelValue in our arena (via exported
;; constructors) and returns its pointer.
;;   params: (parent_msg, type_id, field_id)
(import "cel_host" "get_scalar_field"
        (func (param externref i32 i32) (result i32)))

(import "cel_host" "has_field"
        (func (param externref i32 i32) (result i32)))

;; Message-typed field access. The host returns one of:
;;   - (0, <ref>, 0)                → OK, <ref> is the message
;;   - (1, null_ref, <CelValue*>)   → UNKNOWN, detail in linear memory
;;   - (2, null_ref, <CelValue*>)   → ERROR,   detail in linear memory
(import "cel_host" "get_message_field"
        (func (param externref i32 i32) (result i32 externref i32)))

;; Repeated & map access.
(import "cel_host" "repeated_len"
        (func (param externref i32 i32) (result i32)))

;; Returns CelValue* for scalar element types.
(import "cel_host" "repeated_get_scalar"
        (func (param externref i32 i32 i32) (result i32)))

;; Returns (tag, element_ref, detail_cv_ptr) for message element types.
(import "cel_host" "repeated_get_message"
        (func (param externref i32 i32 i32) (result i32 externref i32)))

(import "cel_host" "map_keys_count"
        (func (param externref i32 i32) (result i32)))

;; Scalar map values: (parent, type, field, key_cv_ptr) → CelValue*.
(import "cel_host" "map_get_scalar"
        (func (param externref i32 i32 i32) (result i32)))

;; Message map values: (parent, type, field, key_cv_ptr)
;;                        → (tag, value_ref, detail_cv_ptr).
(import "cel_host" "map_get_message"
        (func (param externref i32 i32 i32) (result i32 externref i32)))

(import "cel_host" "map_iter"
        (func (param externref i32 i32) (result i32)))
(import "cel_host" "map_iter_next"
        (func (param i32) (result i32)))
                                     ;; returns a CelValue of kind CEL_LIST
                                     ;; wrapping {key, val} or CEL_NULL when done

;; Message equality (delegates to protobuf equality, incl. unknown-field
;; byte equality per spec §1110).
(import "cel_host" "message_eq"
        (func (param externref externref) (result i32)))

;; Type of a host-owned message (returns interned type_id).
(import "cel_host" "message_type_of"
        (func (param externref) (result i32)))

;; Regex (pattern_id pre-assigned at compile time).
(import "cel_host" "string_matches"
        (func (param i32 i32) (result i32)))                  ;; (cv_str, pattern_id) → CelValue*

;; String ops that are too expensive or spec-subtle to inline.
(import "cel_host" "string_normalize"
        (func (param i32) (result i32)))                      ;; future use
```

Split rationale: scalar fields are small and cheaply wrapped into CelValue
host-side, so one `i32` return is enough. Message fields need to preserve
externref identity, and the three-valued status would otherwise require
allocating a CelValue just to report UNKNOWN/ERROR — the multi-value path
keeps the hot path allocation-free.

### 8.3 Custom functions

```wat
(import "cel_fn" "<overload_id>"
        (func (param i32 i32 ...) (result i32)))
```

Each parameter is a `CelValue*`. Return is a `CelValue*`. The host registers
implementations under `overload_id`; these may return UNKNOWN or ERROR by
returning a `CelValue` of that kind, constructed via the exported helpers.

### 8.4 Type identity & attribute identity

Emitted in a WASM custom section `cel.abi` (see Appendix B):

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
| `e.f` (scalar field)  | `cel_host.get_scalar_field(unwrap(e), type_id, field_id)`                          |
| `e.f` (message field) | `(tag, ref, detail) = cel_host.get_message_field(unwrap(e), ...); if tag==0 cel_wrap_message(ref) else detail` |
| `has(e.f)`            | `cel_host.has_field(...)`                                                         |
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

`cel_int_add` and friends implement the spec's checked arithmetic internally
and return a `CEL_ERROR` value with `code=OVERFLOW` on overflow. Codegen
never inlines raw `i64.add` for CEL ints — it always calls the runtime op,
which inlines at the WASM level but keeps the error path correct.

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
| M3  | `m3-proto-and-strings.md`                  | IN PROGRESS  | Proto field reads (`cel_host.get_field`, `has_field`), `has()`, string constants / equality / concat / size. Slices A+B (runtime wiring, host loader, string literals + equality), C (scalar `kIdentExpr`), D (`+`/`==`/`!=`/`size` on string) all landed 2026-04-18/19. Remaining: slice E (member calls `startsWith`/`endsWith`/`contains`), slice F (bytes constants + operators), proto-field reads (`kSelectExpr` + `has()` + `cel_host.*` imports). |
| M4  | `m4-collections-and-comprehensions.md`     | PLANNED      | List, map, struct literals + every comprehension macro + nested-shadowing scoping (§5.4) |
| M5  | `m5-three-valued.md`                       | PLANNED      | Partial eval: attribute interning, UnknownSet, overflow / div-by-zero / NaN → ERROR, `cel_status_either` |
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
