# m34 — `celc`: build-time CEL → native object-file compiler

Status: plan — drafted 2026-06-15, not yet started.

A second, **build-time** backend off the existing typed IR: turn a CEL
expression + variable declarations into a linkable C++ `cc_library` whose
`Eval()` runs **in-process, native**, with no wasm, no wasmtime, no JIT,
and no LLVM. The expression is fixed at build time; the artifact is
per-arch and trusted.

> **Scope guard.** Independent of m31 (static aggregates), m32 (SwissTable
> map index), m33 (constant folding). Do not touch, depend on, or wait for
> them. Reuse the existing runtime kernels as-is (maps/lists are linear-scan
> today — fine). No SwissTable / hash-index / materialization changes here.

## 1. Problem

The production path is CEL → wasm `Program` → wasmtime at eval time. That
buys portability + sandbox + compile-once-run-anywhere, at the cost of a
wasm↔host boundary on every host data read and a per-Eval instance floor.
For a large, common class of embedders the expression is **known at build
time** (authz / validation / routing policies compiled into a binary),
the host is **single-tenant / trusted**, and only the **target arch**
matters. For them the sandbox is overhead and the boundary is pure tax:

  - host proto field reads pay a boundary crossing + reflection per access
    (measured: singular `m.i64` ~156 ns vs cel-cpp in-process ~82 ns);
  - the per-Eval floor (~67 ns) is wasm instance/boundary setup;
  - the artifact must ship a wasm runtime + wasmtime.

A build-time native compiler removes all three for this audience: native
machine code, **direct generated-getter** proto reads (~sub-ns, no
reflection, no boundary), and zero runtime beyond a small kernel library.

## 2. Design in one sentence

Keep the frontend, IR, analysis passes, and the (already-native) runtime
kernels; replace the Binaryen backend with a **C++-source emitter** that
lowers the typed IR to a generated `Eval()` function, compiled by the
normal host toolchain into a `.o` the embedder links — with the **same
flat-buffer + u32-slot memory model** the wasm backend uses, only the
base is a native `char[]` instead of wasm linear memory.

## 3. Scope / non-goals

  - **Goal:** `celc` CLI + a `cel_cc_library` bazel rule producing a typed
    `Eval()` entry point.
  - **Non-goals (by design):** runtime/dynamic expressions (the wasm path
    owns that); sandbox; cross-arch artifact; partial-eval / unknowns in
    phase 1.

## 4. Pipeline & reuse map

```
CEL src + var decls
  → frontend parse+check (cel-cpp)         [REUSE  compiler/frontend/parse_and_check]
  → IR (typed AST + annotations)           [REUSE  compiler/ir]
  → resolve / layout / slot_allocator      [REUSE  compiler/codegen/{resolve,layout,slot_allocator}]
  → C++ emitter backend                    [NEW    — mirrors expr_lower.cc, emits C++ text]
  → generated <name>.{h,cc}
  → host C++ toolchain → <name>.o → embedder links
```

| reuse as-is | build new |
|---|---|
| frontend parse+check | C++ emitter backend (~mirror of `expr_lower.cc`, ≈2.7k LOC) |
| IR + annotations | `celc` CLI (`cc_binary`) |
| `resolve_pass`/`layout_pass`/`slot_allocator` | `cel_cc_library` bazel rule (`bazel/cel_cc_library.bzl`) |
| **native kernels** (`runtime/cel_runtime.c`, ≈6.4k LOC) | thread-local-base harness + `WriteXxx`/`DecodeXxx` (`libcelnative`) |
| `CelValue` + error model (`runtime/cel_data.h`, `eval/error`) | generated-fn ABI |

Only the backend is new. The hard 80% (parse/check, IR, semantics) is reused.

## 5. Memory model (the crux)

CEL's entire memory model is **"a flat byte buffer addressed by u32
offsets relative to a base."** Wasm linear memory is one instance of that;
natively the buffer is a `char[]`. A `CelValue` (24 B) carries u32
**self-relative** payload offsets (string `ptr`, `arena_map.header_ptr`),
so the buffer is self-contained and relocatable, and `base + offset`
addressing works identically in wasm and native.

```
   ┌──────────────── one contiguous buffer (base = &buf[0]) ────────────────┐
   │  RODATA               │  WORKSPACE                 │  ARENA            │
   │  constants +          │  intermediate CelValue     │  runtime-built    │
   │  string/bytes payloads│  slots (slot_allocator)    │  lists/maps       │
   └───────────────────────┴────────────────────────────┴───────────────────┘
        wasm:   this buffer == linear memory
        native: this buffer == a char[] (stack below a size threshold; arena above)
```

`layout_pass` already sizes these regions. The **one** harness change:
wasm kernels read a single global memory base; natively make
`cel_memory_base_()` return a **thread-local** base that the generated
`Eval()` sets on entry — no kernel-signature changes:

```c
// native-only harness
static _Thread_local uint8_t* g_base;
uint8_t* cel_memory_base_(void) { return g_base; }        // wasm: linear-mem base
void cel_native_enter(uint8_t* buf) { g_base = buf; arena_init(); }
```

`Eval` is a leaf; per thread no two buffers interleave. That is the whole
native memory model: same slots, same `CelValue` layout, same kernels —
`base` is a `char*`.

## 6. Slots

`slot_allocator` (reused verbatim) assigns each IR node a workspace
byte-offset slot for its 24-byte `CelValue`. The emitter emits kernel
calls with those exact offsets — identical to the wasm codegen minus the
`call $cel.` prefix. The **result slot** is decoded into the public
return type.

## 7. Public ABI

The expression's static result type (known from the checker) picks the
signature; bound variables become typed C++ args. Errors are values
internally (`CEL_ERROR` `CelValue`) and convert to `absl::StatusOr<T>` at
the boundary (reuse `eval/error` code→message). Proto-message variables
are `const Msg&`; scalar field reads lower to **direct generated getters**
(`request.field()`) — no reflection, no boundary (the perf win, free
because the types are linked at build time).

```cpp
// generated authz.h
namespace authz {
absl::StatusOr<bool> Eval(const myapp::Request& request);
}
```

## 8. The C++ emitter backend

Mirror `expr_lower.cc`'s `EmitK*` arms, emitting C++ source text:

| IR node | emit |
|---|---|
| `kConst` | bake into rodata; slot points at it |
| `kIdent` (var) | `WriteXxx(buf, slot, arg)` |
| `kSelect` (proto field) | direct getter `msg.field()` → slot (scalar leaves; presence via `has_x()`) |
| `kCall` (op) | `cel_<overload>(out_slot, a_slot, b_slot)` kernel call |
| `kComprehension` | emit a C++ `for` loop calling element kernels |
| 3VL `&&`/`\|\|`/`?:` | `cel_3vl` / `cel_logic_*` kernels (3VL + error absorption; do **not** naively C++-short-circuit unless both operands are pure-bool and errorless) |

Two emission styles: start **all kernel-call** (correct), add an
**inline-native peephole** later (`a + b` directly where CEL semantics ==
C++ and no error is possible). Only **scalar** proto leaves monomorphize
in phase 1; repeated/map/nested fields stay on the existing
host-aggregate path (phase 2). Any unsupported node → a **clear
compile-time error**, never a silent fallback.

### Concrete generated body — `request.quantity * 2 + 1`, result `int`

Slot layout from `slot_allocator`: `q@0, two@24(rodata), mul@48,
one@72(rodata), res@96`.

```cpp
absl::StatusOr<int64_t> authz::Eval(const myapp::Request& request) {
  alignas(8) uint8_t buf[WORKSPACE_BYTES];   // arena for large; layout_pass sizes it
  std::memcpy(buf, kRodata, kRodataLen);     // bake constants 2, 1
  cel_native_enter(buf);                     // set thread-local base + init arena

  WriteInt(buf, /*q@*/0, request.quantity()); // kSelect: direct getter → slot
  cel_arith_mul(/*out*/48, /*a*/0,  /*b*/24); // kernel: q * 2 (overflow→error)
  cel_arith_add(/*out*/96, /*a*/48, /*b*/72); // kernel: (..) + 1

  return DecodeInt(buf, /*res@*/96);          // CelValue→StatusOr (error slot→Status)
}
```

## 9. Proto field monomorphization

Because the embedder links the concrete C++ proto classes, `request.action`
lowers to `request.action()` — a generated getter, ~sub-ns, no reflection,
no boundary (there is no boundary; it is all native). The compiler knows
the concrete type from the variable declaration and the field from the
checker's `FieldDescriptor`. Per-field `cpp_type → Value::ctor` mapping
(`int64→Value::Int(m.x())`, `string→Value::String(m.x())`, enum→`Value::Int`,
presence via `m.has_x() ? … : Value::Null()`). This is the best-case proto
read and the default here — no snapshot machinery needed (there is no
sandbox to amortize a crossing over).

## 10. Build integration

  - **`celc` CLI** (`cc_binary`): `celc --expr '...' --var request:myapp.Request
    --out authz`. Internally drives
    `Compiler::NewBuilder().DeclareVariable(...)` → IR → C++ emitter → writes
    `authz.{h,cc}`.
  - **`cel_cc_library` rule** (`bazel/cel_cc_library.bzl`): runs `celc` at
    build time, emits `.h/.cc`, wraps in `cc_library(deps=[<proto cc_protos>,
    "//runtime:libcelnative"])`. The embedder `#include`s the header and calls
    `Eval`.

```python
cel_cc_library(
    name = "authz_policy",
    expr = "request.action in ['read','list'] && request.quantity <= 100",
    vars = {"request": "myapp.Request"},
    deps = [":request_cc_proto"],
)
```

## 11. Phasing

  - **Slice A** (~2–3 wks): scalars, arithmetic, comparison, 3VL logic,
    `bool/int/uint/double/string` results, proto **scalar** reads + `has()`,
    errors→`StatusOr`, the bazel rule + thread-local-base harness. Proves the
    pipeline end-to-end.
  - **Slice B**: lists/maps (existing arena kernels), `[]` / `in` / `size`,
    comprehensions.
  - **Slice C**: WKTs, duration/timestamp, optionals, aggregate/nested proto
    fields, string/time/regex extensions.
  - **Slice D** (optional): inline-native peephole; partial-eval / unknowns.

## 12. Testing — the derisking

**Differential against the existing wasm backend.** Same frontend, same
kernels, same IR — only the backend differs, so for any expression+inputs
the native `Eval()` MUST produce the **byte-identical** `Value` as the
wasm `Eval()`. Therefore:

  - reuse the conformance corpus + `e2e` to cross-check native ≡ wasm —
    free validation against the ~2,035 passing rows;
  - add a differential test target (`e2e/celc_diff_test.cc`): curated +
    random exprs/inputs, assert `native == wasm`;
  - per CLAUDE.md every new `.h/.cc` gets its own `_test.cc`
    (positive + negative + boundary); cite spec/oracle for spec-mandated
    behavior.

This is the strongest possible safety net: we validate the *wiring* of the
new emitter, not the semantics (the kernels are shared and already
conformance-tested).

## 13. Files touched

  - `compiler/codegen/cpp_emit.{h,cc}` (new) + `cpp_emit_test.cc` — the
    C++-source emitter (arm map §8).
  - `runtime/libcelnative.{h,cc}` (new) — thread-local base harness,
    `cel_native_enter`, `WriteXxx`/`DecodeXxx`, error→`Status`; `cc_library`
    over `//runtime:cel_runtime`.
  - `runtime/cel_memory.c` — `cel_memory_base_()` native arm reads the
    thread-local (guarded so the wasm build is unchanged).
  - `tools/celc/celc.{cc}` (new) + BUILD — the CLI.
  - `bazel/cel_cc_library.bzl` (new) — the rule.
  - `e2e/celc_diff_test.cc` (new) — native≡wasm differential.
  - Reconcile: `testing-checklist.md` rows; this doc's status on closeout.

## 14. Risks

  - **Two backends to maintain** — every new codegen arm lands in both the
    wasm and C++ emitters. Ongoing tax; mitigated by the shared IR/kernels
    and the differential test (a missing arm fails native≡wasm immediately).
  - **Buffer sizing** — deep expressions exceed a fixed stack `buf[]`; use
    `layout_pass`'s size to pick stack below a threshold, arena above.
  - **3VL / error fidelity** — covered by differential tests; reuse the
    `cel_3vl` kernels rather than re-deriving the truth tables.
  - **Thread-local base reentrancy** — `Eval` is a leaf; document that
    nested `Eval` on one thread must not interleave buffers (they don't).
  - **Per-arch / no dynamic expressions** — by design; embedders needing
    runtime expressions use the wasm path.

## 15. Independence from m31 / m32 / m33

This backend reuses the **current** runtime kernels unchanged, including
the existing linear-scan map/list kernels. It does not require, and must
not block on, the SwissTable index (m32), static-aggregate materialization
(m31), or constant folding (m33). If those land later, the native backend
benefits transparently (it calls the same kernels) with no work here.

## 16. Future work

  - Inline-native peephole for hot scalar chains (skip the kernel call when
    CEL semantics == C++ and no error is possible).
  - Aggregate/nested proto field monomorphization (phase 2 of §9).
  - Partial-eval / unknown-attribute support for the native path.
  - A shared "emit catalogue" so a new op is declared once and both the wasm
    and C++ emitters consume it (reduces the two-backend tax).
