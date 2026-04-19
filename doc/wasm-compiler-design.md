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
             │  module =
             │    runtime/cel_runtime.wasm      ← compiled once from C
             │  + generated eval(...)           ← per expression
             ▼
         .wasm  (+ optional .wat for debugging)
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

Every generated module embeds a **runtime support library** compiled once
from C (`compiler/runtime/cel_runtime.c`). It provides:

- A bump allocator over a monotonically-growing region of linear memory.
- The `CelValue` struct type.
- Constructors for every `CelKind`.
- Typed accessors and scalar comparison ops.
- `string` / `bytes` / `list` / `map` operations.
- Three-valued-logic helpers (`cel_and`, `cel_or`, `cel_not`,
  `cel_status_either`, `cel_unknown_merge`).

The runtime is linked into every emitted module — the compiler loads the
pre-built `cel_runtime.wasm` as a starting Binaryen `Module`, then appends
the generated `eval` function and any per-expression constants.

### 7.1 Memory map

```
┌────────────────────────────────┐ 0x00000000
│ const data segment             │  interned literals, type/attr tables,
│ (emitted by compiler)          │  error message blobs, `cel.abi` payload
├────────────────────────────────┤ data_end
│ static globals                 │  g_arena, singletons (null, true, false),
│                                │  ref-table free list head
├────────────────────────────────┤ static_end
│ bump arena                     │  grows upward until limit
│       ↓                        │  host may call cel_reset between evals
├────────────────────────────────┤ g_arena.limit
│ unused                         │  memory.grow if allocator exhausts
└────────────────────────────────┘
```

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
`CelOptional.opt`, etc.) is a linear-memory offset from 0.

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
```

No `cel_free`. Hosts call `cel_reset()` after every `eval` to reclaim.

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

Host-to-module flow for scalar inputs: host calls `cel_alloc`, writes bytes
directly into `memory`, then `cel_string_view(ptr, len)`. For message
inputs the host passes its `externref` straight into `eval`; the compiler
emits a `cel_wrap_message` inside `eval` before the first use. Return is
always `CelValue*` (i32), which the host reads from linear memory and —
when `kind == CEL_MESSAGE` — unwraps via `cel_unwrap_message`.

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

1. Loads the pre-compiled `cel_runtime.wasm` as a starting `BinaryenModuleRef`
   via `BinaryenModuleRead`.
2. Emits interned literal blobs (strings, regex patterns, error messages,
   type/attribute tables, `cel.abi` proto) into fresh data segments.
3. Appends generated helper functions and the exported `eval` via
   `BinaryenAddFunction` / `BinaryenAddFunctionExport` / etc.
4. Serialises the merged module to `.wasm` with `BinaryenModuleWrite`.

### 10.1 Lowering per expression kind

Every `CheckedExpr` node lowers to a single Binaryen expression returning an
`i32` — the pointer to a `CelValue`.

| CEL expression        | Emitted IR                                                                        |
| --------------------- | --------------------------------------------------------------------------------- |
| Literal               | `cel_<kind>(const)` for scalars; `cel_string_view(ptr, len)` for interned strings |
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
| M2  | `m2-codegen-mvp.md`                        | IN PROGRESS  | Codegen of pure-primitive expressions (`1 + 2 * 3`, `&&`, `||`, `?:`); still open: `cel_refs.wat`, wasm32 cross-compile, `cel.abi` custom section |
| M3  | `m3-proto-and-strings.md`                  | PLANNED      | Proto field reads (`cel_host.get_field`, `has_field`), `has()`, string constants / equality / concat / size |
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

```c
// Pseudocode — any host language that can instantiate a WASM module works.
WasmInstance m = instantiate("expr.wasm", host_imports);

// Pass the top-level proto message as an externref directly — no copy,
// no interning step on the host side.
ExternRef user_request_ref = make_externref(user_request);

// Evaluate.  `eval` takes externref(s) for message inputs and i32
// (CelValue*) for scalar inputs, per the static signature emitted in
// the cel.abi custom section.
uint32_t result = m.call("eval", user_request_ref, /*other args*/);

// Read the CelValue struct directly from linear memory.
CelValueView v = read_value(m.memory, result);
switch (v.kind) {
  case CEL_BOOL:    return v.b;
  case CEL_MESSAGE: {
      ExternRef msg = m.call("cel_unwrap_message", result);
      // msg is the original (or a derived) host message.
      break;
  }
  case CEL_UNKNOWN: schedule_resolve_and_retry(v.unknown_ids); break;
  case CEL_ERROR:   log_error(v.err); break;
  default:          ...
}

// Reclaim arena (and rewind the ref-table free list) for the next eval.
m.call("cel_reset");
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
