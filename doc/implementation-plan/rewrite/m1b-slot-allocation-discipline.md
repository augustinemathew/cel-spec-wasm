# M1b — Slot allocation discipline (post-bug-fix)

Status: shipped 2026-06-04.

The original M1 `SlotAllocator` (`compiler/codegen/slot_allocator.{h,cc}`)
was a bump-only design with `Release` as a no-op. M10 flipped on a LIFO
free list so released cells could be reused, which kept long
arithmetic chains tight but introduced two latent bugs we hit in
production at 2026-06-04:

1. **`wasm trap: unaligned atomic` at `≥ 2000`-term arithmetic.** The
   24-byte CelValue stride from an 8-aligned workspace base produced
   odd-indexed slots at `offset % 16 == 8`. The wasm32-wasi-threads
   runtime helpers eventually emit a `memory.atomic.*` op that fails
   on a misaligned cell. Pinned by
   `e2e/known_bugs_test::LongArith_2000Terms_NoUnalignedAtomicTrap`.
2. **Nested-aggregate slot aliasing.** The free list let an outer
   aggregate's parent slot pull a just-released inner aggregate's
   slot. Codegen emits the outer's `cel_make_message(parent)` /
   `cel_list_create(parent)` first, then descends into the inner
   aggregate which re-initializes that same slot, leaving the
   outer's later `cel_set_field` / `cel_list_append_at` reading from
   the wrong CelValue. Pinned by
   `e2e/m4_test::NestedListOuterRoundTrip` and
   `e2e/m7_test::MapStringToMessageFromLiteral`.

This doc is the singular source of truth for how the slot allocator
and the layout visitors cooperate. The dispatch is **per AST node
kind**; the AST is the closed set in `cel.expr.Expr.expr_kind`
(proto: `cel/expr/syntax.proto`; C++ API: `cel::Expr` in
`third_party/cel-cpp/common/expr.h`):

```
oneof expr_kind {
  Constant      const_expr;
  Ident         ident_expr;
  Select        select_expr;
  Call          call_expr;
  CreateList    list_expr;
  CreateStruct  struct_expr;   // map  in the C++ API when message_name is empty (kMapExpr)
                               // proto-message otherwise               (kStructExpr)
  Comprehension comprehension_expr;
}
```

Every kind below is documented with its **child Expr nodes** (the
sub-trees the visitor traverses into), its **storage assignment
rule**, the **codegen read/write order** of the runtime helper its
lowering emits, and the **slot-lifecycle invariant** that makes the
storage rule correct.

## 0. Pre-reading: stride + base alignment

- `SlotAllocator::kSlotStride = 32` bytes. CelValue itself stays 24
  bytes; the trailing 8 bytes of each cell are pad that no codepath
  reads or writes.
- Workspace base is rounded to 16 bytes by
  `layout_pass.cc::RoundUp16`. Combined with the 32-byte stride,
  every slot the allocator hands out is 16-byte aligned regardless
  of index — required by `memory.atomic.*` ops.
- The reserved low region of `cel.memory` is `[0,
  MemoryLayout::kReservedLowMemoryBytes) = [0, 8192)`. rodata +
  workspace + a 256-byte guard band live here; wasi-libc's static
  data + heap + stack live above 8192. `LayoutPass` refuses to
  compile an expression whose rodata + workspace would extend into
  the guard or past it (see `MemoryLayout::MaxWorkspaceBytes`).

## 1. `kConstant` (`Constant const_expr`)

**Proto fields.** One of `null_value`, `bool_value`, `int64_value`,
`uint64_value`, `double_value`, `string_value`, `bytes_value`,
`duration_value` (deprecated), `timestamp_value` (deprecated).
**Child Expr nodes.** None.

**Storage.** Packed into rodata by `ConstLayoutVisitor` —
`storage.kind = kStaticRodata`. The offset is the byte offset of
the 24-byte CelValue frame inside `layout.rodata`. The kStaticRodata
storage is **immutable** at runtime; no slot is ever acquired.

**Lifecycle.** None. Constants neither Acquire nor Release.

**Aliasing.** Constants share their cell with every consumer that
reads them — rodata is read-only and read-many, so aliasing is
trivial.

## 2. `kIdentExpr` (`Ident ident_expr`)

**Proto fields.** `name` (string).
**Child Expr nodes.** None.

**Storage.** Assigned by `IdentStorageVisitor` —
`storage.kind = kLocal`, `storage.payload = local_index` where the
local is the wasm `i32` local declared in `$eval`'s preamble. Each
referenced variable also owns a permanent 32-byte cell in the
workspace (allocated by `ReserveVariableSlots`, not by
`SlotAllocator`); the kLocal points the consumer at that fixed
cell.

**Lifecycle.** None at the SlotAllocator level. The variable's
cell is alive for the full `$eval` invocation.

**Aliasing.** Variable cells never alias scratch cells (their
offsets are below `slot_allocator.base_offset`). Distinct variables
get distinct cells; multiple `kIdentExpr` nodes referencing the
same name share one cell.

## 3. `kSelectExpr` (`Select select_expr`)

**Proto fields.** `field` (string), `test_only` (bool).
**Child Expr nodes.** `operand` (1).

**Storage.** Assigned by `SelectStorageVisitor::PostVisitSelect` —
`storage.kind = kWorkspaceSlot`. PostVisit order:

```cpp
PostVisitSelect(expr, sel):
  if (operand.storage.kind == kWorkspaceSlot)
    slots.Release(operand.storage.payload)
  expr.storage = {kWorkspaceSlot, slots.Acquire()}
```

**Codegen read/write order.** Lowers to either
`cel_host.cel_get_field(operand_slot, field_id, out_slot)` (regular
form) or `cel_select_optional_field_at_vv(operand_slot,
key_slot, out_slot)` (optional-operand form). **Both helpers read
`operand_slot` before writing `out_slot`** — they pull the
CelValue into a local copy, resolve the field/key, then write the
result into `out_slot`. Aliasing `out_slot == operand_slot` is
safe by construction.

**Lifecycle.** Release operand first, Acquire result. The LIFO
free list hands the just-vacated operand cell back so chained
`c.a.b.c.d` Selects peak at one workspace slot.

**`test_only` flag.** `has(x.y)` is a kSelect with `test_only=true`
whose codegen calls `cel_host.cel_has_field(operand_slot, field_id,
out_slot)`. Same slot lifecycle as the regular form; aliasing
safe for the same read-before-write reason.

## 4. `kCallExpr` (`Call call_expr`)

**Proto fields.** `function` (string), `target` (optional Expr; the
receiver in `x.f(y)`), `args` (repeated Expr).
**Child Expr nodes.** `target` (0 or 1) + each element of `args` (N).

**Storage.** Assigned by `AggregateStorageVisitor::PostVisitCall`.
PostVisit order:

```cpp
PostVisitCall(expr, call):
  if (call.function == "dyn" and call.args.size == 1 and !call.has_target):
    expr.storage = call.args[0].storage   // dyn() passthrough — no new slot
    return
  if (!IsIndexCall(call) and call.has_target):
    ReleaseIfWorkspaceSlot(call.target)
  for arg in call.args:
    ReleaseIfWorkspaceSlot(arg)
  expr.storage = {kWorkspaceSlot, slots.Acquire()}
```

**Codegen read/write order.** Every runtime helper backing a kCall
reads its operand slots BEFORE writing its result slot. Concretely:

| Function shape | Helper | Read-before-write |
| --- | --- | --- |
| `_+_` / `_-_` / `_*_` / `_/_` / `_%_` on ints | `cel_int_add_at_vv(out, lhs, rhs)` etc. | Yes |
| Same on uints / doubles | `cel_uint_*` / `cel_double_*` | Yes |
| `_==_` / `_!=_` / `_<_` / … | `cel_*_eq_at_vv` / `cel_*_lt_at_vv` family | Yes |
| `_in_` | `cel_list_in` / `cel_map_in` | Yes |
| `_[_]` (list index) | `cel_list_at(list, idx, out)` | Yes |
| `_[_]` (map lookup) | `cel_map_lookup_arena(map, key, out)` / `cel_host.cel_map_lookup` | Yes |
| `_&&_` / `_||_` / `!_` | Binaryen `if`/`select` — branch arms write `out` after reading conds/args | Yes |
| `_?_:_` | Binaryen `if` over cond, then/else arms each write `out` after reading their slot | Yes |
| `size` / `int` / `string` / `bytes` / `bool` / `uint` / `double` / `type` / `dyn` (non-passthrough form) | `cel_size_*` / `cel_*_from_*` family | Yes |
| Receiver-form (`s.startsWith`, `s.contains`, `s.matches`, …) | dispatched on receiver type; all read receiver + args before write | Yes |
| User-defined kCelFn / kForeignComponent | trampoline; reads operand slot pointers, writes out_slot last | Yes |

**Lifecycle.** Release target + every arg first, Acquire result.
LIFO reuse fires aggressively, so long arithmetic chains
`((((a+b)+c)+d)…)` peak at 1 workspace slot.

**Aliasing.** Safe — every helper reads its operand inputs before
writing `out_slot`. The dyn-passthrough arm doesn't allocate a
slot at all (it just forwards `args[0].storage`).

## 5. `kListExpr` (`CreateList list_expr`)

**Proto fields.** `elements` (repeated Expr), `optional_indices`
(repeated int32 — element indices where the element is an
`?expr`).
**Child Expr nodes.** Each element of `elements` (N).

**Storage.** Assigned by
`AggregateStorageVisitor::PreVisitExpr` (dispatched on
`kind_case == kListExpr`). PreVisit order:

```cpp
PreVisitExpr(expr):                          # before descending into elements
  if expr.kind_case == kListExpr:
    expr.storage = {kWorkspaceSlot, slots.Acquire()}

PostVisitList(expr, list):                   # after all elements done
  for elem in list.elements:
    ReleaseIfWorkspaceSlot(elem)
  # parent's own slot is released by its consumer, not here
```

**Codegen read/write order.** Lowers to
`cel_list_create(parent_slot, N)` followed by per-element
`cel_list_append_at(parent_slot, elem_slot)` (or
`cel_list_append_at_if_present(parent_slot, opt_elem_slot)` for
optional elements). `cel_list_create` **writes parent_slot
first** (initializes a new `{CEL_LIST_ARENA, header→{capacity=N,
count=0, elements_offset=…}}` CelValue), then each `append_at`
reads `parent_slot` AND `elem_slot` and writes the arena entry.

**Lifecycle.** Acquire at PreVisit, before any descendant Acquire
happens. PostVisit releases element slots so siblings of the
parent list can reuse them. The parent's own slot is released by
the kSelect/kCall consumer that reads the list as its operand.

**Aliasing.** Parent's slot is acquired before every descendant
visit, so it is **never in the free list during its own subtree's
lifetime**. No descendant Acquire can pull it. Safe by
construction — see §10.1 proof.

## 6. `kMapExpr` (`CreateStruct struct_expr` with empty `message_name`)

**Proto fields.** `message_name = ""`, `entries[]` where each
`Entry` has `id`, `map_key:Expr`, `value:Expr`, `optional_entry:
bool`.
**Child Expr nodes.** For each entry: `map_key` and `value` (2N
total).

**Storage.** Assigned by `AggregateStorageVisitor::PreVisitExpr`
dispatched on `kind_case == kMapExpr`. Same PreVisit Acquire as
kListExpr.

```cpp
PostVisitMap(expr, m):
  for entry in m.entries:
    ReleaseIfWorkspaceSlot(entry.key)
    ReleaseIfWorkspaceSlot(entry.value)
```

**Codegen read/write order.** Lowers to
`cel_map_create(parent_slot)` (or `cel_map_create_with_capacity`)
followed by per-entry `cel_map_insert(parent_slot, key_slot,
val_slot)`. Optional entries dispatch through
`cel_map_insert_if_present`. Parent's slot is **written first** by
the create call.

**Lifecycle.** PreVisit Acquire, PostVisit Release operands.
Identical pattern to kListExpr; same aliasing-safety argument.

## 7. `kStructExpr` (`CreateStruct struct_expr` with non-empty `message_name`)

**Proto fields.** `message_name` (string, fully-qualified proto
type), `entries[]` where each `Entry` has `id`, `field_key:string`
(NOT `map_key`), `value:Expr`, `optional_entry:bool`.
**Child Expr nodes.** For each entry: `value` only — the field key
is a static string, not an Expr (N total).

**Storage.** Assigned by `AggregateStorageVisitor::PreVisitExpr`
dispatched on `kind_case == kStructExpr`. Same PreVisit Acquire.

```cpp
PostVisitStruct(expr, s):
  for f in s.fields:
    ReleaseIfWorkspaceSlot(f.value)
```

**Codegen read/write order.** Lowers to
`cel_host.cel_make_message(type_id, parent_slot)` followed by
per-field `cel_host.cel_set_field(parent_slot, field_ref_id,
value_slot)`. Optional fields dispatch through
`cel_set_field_at_if_present`. `cel_make_message` **writes
parent_slot first** (allocates a default-constructed proto on
the host arena, interns it, and stamps the CEL_MESSAGE CelValue).

**Lifecycle.** Identical to kMapExpr / kListExpr.

**Aliasing.** Identical; PreVisit Acquire makes the parent's slot
unreachable to descendants.

## 8. `kComprehensionExpr` (`Comprehension comprehension_expr`)

This is the kind that was glossed over before. Comprehensions
have **five Expr children** and **three string-typed names**:

**Proto fields.**
- `iter_var: string` — name of the first per-iteration variable.
- `iter_var2: string` — name of the second per-iter variable (v2
  macros: `map.exists(k, v, …)`, etc.); empty for v1 forms.
- `accu_var: string` — name of the accumulator.
- `iter_range: Expr` — the collection being iterated.
- `accu_init: Expr` — initial accumulator value (runs once before
  the loop).
- `loop_condition: Expr` — bool predicate checked each iteration.
- `loop_step: Expr` — produces the new accu_var each iteration.
- `result: Expr` — final value (typically reads accu_var).

**Child Expr nodes traversed by every visitor.** All five: 
`iter_range`, `accu_init`, `loop_condition`, `loop_step`, `result`.
The default `cel::AstTraverse` walks into each one in turn, so
`SelectStorageVisitor` and `AggregateStorageVisitor` see them like
any other Expr. Slots are allocated and released **as if those
subtrees were free-standing expressions**.

**The kComprehensionExpr node itself.** Has NO `PreVisit*` /
`PostVisit*` override in either storage visitor — its own
`storage.kind` stays `kNone`. The comprehension's "result" at
codegen time is read from the accu_var's workspace cell, NOT from
a slot on the comp node itself. See
`expr_lower_comprehension.cc:222` —
`return accu_v->slot_offset`.

**Variables associated with the comprehension.**
- `accu_var` is declared in `variables[]` as
  `ResolvedVariableKind::kComprehensionAccu`. `ReserveVariableSlots`
  reserves a permanent 32-byte cell for it; that cell is the
  comp's effective result location.
- `iter_var` is declared as
  `ResolvedVariableKind::kComprehensionIter`. NO workspace cell
  (`slot_offset = 0` sentinel). Its wasm `i32` local holds a
  moving pointer into `iter_range`'s element run (list iteration)
  or the result of `cel_map_iter_key_at` (map iteration).
- `iter_var2` for v2 comprehensions is declared as
  `ResolvedVariableKind::kComprehensionAccu` (yes — its per-iter
  CelValue lives in a workspace cell, written each iter by
  codegen, even though semantically it's an iteration variable).
  Sample: in `[1,2].map(i, v, …)` the `i` (the index) is
  `kComprehensionAccu` and gets a slot; the `v` (the value) is
  `kComprehensionIter` and uses the moving pointer.

**Per-comprehension aux locals.** `ComprehensionLocalsVisitor`
allocates `comp_aux_local_base` + 3 consecutive wasm `i32` locals
per kComprehensionExpr (for `end_off`, `cursor`,
`source_addr` — list/map iteration state). These are wasm locals,
not workspace cells; they cost zero bytes of cel.memory.

**Per-child slot lifecycle.** Each of the five Expr children
participates in the normal SelectStorageVisitor /
AggregateStorageVisitor protocol. The total static workspace cost
of a comprehension is therefore:

  workspace(comp) =
      sum_{c ∈ {iter_range, accu_init}} workspace(c)     # transient pre-loop
    + max_{c ∈ {loop_condition, loop_step, result}} workspace(c)
                                                           # statically allocated,
                                                           # reused every iteration
    + 1                                                   # accu_var cell

The pre-loop subtrees (`iter_range`, `accu_init`) release their
slots back to the free list before the loop body begins, so their
cells are available for the body's scratch — the `max` term is
not a sum.

The loop body's slots are STATICALLY allocated by LayoutPass and
RE-USED at runtime each iteration — the slot offsets are
constants in the emitted wasm, the contents change per iter.

**Aliasing in comprehensions.**
- The accu_var's cell is allocated in `variables[]` at a fixed
  offset reserved before any scratch slot. It can never collide
  with a SlotAllocator scratch because the SlotAllocator's
  `base_offset` is past the variables region.
- The loop body's scratch slots are released after the body's
  PostVisit, so they're available for siblings of the comp (e.g.
  in `[…].filter(…) + [a,b,c]`, the list-literal `[a,b,c]`'s
  acquires can reuse the filter body's cells).
- The comp node itself has no slot, so an ancestor kCall /
  kSelect's `ReleaseIfWorkspaceSlot(comp.id())` is a no-op.

**Worked example.** `[a, b, c].filter(v, v > 0)`:

| Sub-expr | Role | Storage |
| --- | --- | --- |
| `[a, b, c]` | iter_range (kListExpr) | scratch slot S0 (PreVisit Acquire) |
| `v` | iter_var | kComprehensionIter — no slot |
| `[]` (synthetic) | accu_init (kListExpr) | scratch slot S1 (PreVisit Acquire) |
| `@result` (synthetic) | accu_var | fixed cell in `variables[]`, say V0 |
| `v > 0` | loop_condition (kCallExpr) | scratch slot S2 (Release-then-Acquire) |
| body (`@result = @result + [v]` shape) | loop_step | scratch slot S3 |
| `@result` | result (kIdentExpr) | kLocal — no scratch slot |

Peak workspace slots after release pattern: ≤ 3 scratch slots
coexist (S0 dies before loop, S1 dies before loop, body alternates
S2/S3). Plus V0 in variables[]. workspace_bytes ≤ 4 × 32 = 128 B.

## 9. The two-rule dispatch table

| Expr kind | Acquire when | Release operand slots when |
| --- | --- | --- |
| `kConstant` | n/a (rodata) | n/a |
| `kIdentExpr` | n/a (variable cell in `variables[]`) | n/a |
| `kSelectExpr` | PostVisit, after Release | PostVisit, before Acquire |
| `kCallExpr` (generic, control flow, indexing, type ops, receiver form) | PostVisit, after Release | PostVisit, before Acquire |
| `kCallExpr` (`dyn(scalar)`) | n/a — forwards arg's storage | n/a |
| `kListExpr` | **PreVisit** | PostVisit |
| `kMapExpr` | **PreVisit** | PostVisit |
| `kStructExpr` | **PreVisit** | PostVisit |
| `kComprehensionExpr` | n/a — children handled per their own kind; accu_var owns a `variables[]` cell | n/a |

## 10. Correctness arguments

### 10.1 Aggregates never alias descendants (proof for §5–§7)

Let `P` be an aggregate node (`kListExpr` / `kMapExpr` /
`kStructExpr`). Let `slot(P)` be the workspace offset its PreVisit
Acquires.

Let `Q` be any descendant of `P` that obtains a workspace slot —
one of: `kSelectExpr`, `kCallExpr` (non-dyn), `kListExpr`,
`kMapExpr`, `kStructExpr`. Q's slot is Acquired during a visit
that occurs strictly between P's PreVisit and P's PostVisit
(post-order traversal: every descendant is fully visited before
the parent's PostVisit fires).

The free list can hand `slot(P)` to a later Acquire only if some
intervening Release pushed `slot(P)`. The places that could push
it:
- P's own PostVisit. Does NOT release `slot(P)`; only operand
  slots.
- P's *consumer*'s PostVisit (an ancestor kSelect / kCall that
  reads P as operand). Happens AFTER P's PostVisit — after every
  descendant of P is done being visited.

Therefore `slot(P)` is never in the free list during any of Q's
Acquire calls. ∎

### 10.2 kSelect / kCall aliasing is safe (proof for §3–§4)

Every runtime helper backing a kSelect or kCall is **read-before-
write**: it reads every operand slot into a local copy, computes
the result, then writes `out_slot`. Aliasing `out_slot ==
operand_i_slot` produces an in-place op — the helper has already
captured `operand_i`'s value by the time it writes `out_slot`.

The Release-then-Acquire ordering at PostVisit means the LIFO
free list returns the just-vacated operand cell as the parent's
result — exactly the in-place pattern the helpers support.

### 10.3 Comprehensions are correct (proof for §8)

The kComprehensionExpr node has no slot of its own; its children
are visited as ordinary Exprs and assigned slots by the matching
kind's rules above. The accu_var is a `variables[]` cell — its
offset is below `slot_allocator.base_offset`, so SlotAllocator
can never hand it back. The aux locals are wasm `i32` locals,
disjoint from cel.memory. There is no extra invariant to prove
beyond §10.1 + §10.2 — the comp slice is "two independent
applications of the rules to disjoint sub-expressions."

## 11. Workspace budget and slot-exhaustion gate

`cel.memory` reserves `[0, MemoryLayout::kReservedLowMemoryBytes)
= [0, 8192)` for the expr module. Above 8192 is wasi-libc's static
data + heap + stack — corruption there means a delayed-death
failure inside `malloc` or libc with no wasm trap to point at.
`LayoutPass` refuses to compile any expression whose rodata +
workspace + a 256-byte guard band would extend at or past 8192;
the constant lives in `compiler/memory_layout.h` and the gate
returns `absl::ResourceExhaustedError` with prefix
`kSlotExhaustedMessagePrefix`.

For a typical CEL expression (tens of internal nodes, a few
constant strings), workspace is under 1 KiB and rodata is under
1 KiB; the gate is well out of reach. The exhaustion case fires
only for pathological codegen-emitted expressions (many declared
variables, very-deep balanced trees, or many independent
max-depth subtrees as siblings).

## 12. Test discipline

Unit tests (slot allocator):
- `slot_allocator_test::LeftAssocAdditionChainAfterReleaseFix` (N
  = 2, 10, 100, 1000, 2000, 10 000) — long-arith chain peaks at
  1 workspace slot.
- `slot_allocator_test::BalancedAdditionTreeAfterReleaseFix`
  (depth = 1, 2, 3, 4, 8, 10) — balanced `+`-tree peaks at the
  tree depth.
- `slot_allocator_test::EverySlotIsSixteenByteAligned` — every
  slot offset is 16-byte aligned across 1024 slots × 5 bases.

Unit tests (layout pass):
- `layout_pass_test::LayoutPassSelectTest::SelectsGetContiguousWorkspaceSlotsAfterVariables`
  — chained Selects share a single scratch cell.
- `layout_pass_test::LayoutPassMapTest::MapLiteralIndexingReusesSingleSlot`
  — kMapExpr + kCallExpr(`_[_]`) share one cell post-release.
- `layout_pass_test::LayoutPassMapTest::MultipleMapNodesShareSlotsViaReuse`
  — multi-aggregate program peaks at one scratch cell.
- `layout_pass_test::LayoutPassListTest::ListLiteralIndexingReusesSingleSlot`
  — kListExpr + index call same as kMapExpr.
- `layout_pass_test::LayoutPassControlFlowTest::*GetsOneCallSlot`
  — `_&&_` / `_||_` / `_?_:_` / `!_` each peak at one scratch
  cell.
- `layout_pass_test::LayoutPassComprehensionTest::IterVarHasNoWorkspaceSlot`
  — kComprehensionIter sentinel.
- `layout_pass_test::LayoutPassComprehensionTest::AccuVarHasWorkspaceSlot`
  — kComprehensionAccu placement.
- `layout_pass_test::LayoutPassComprehensionTest::WorkspaceBytesSkipsIterVars`
  — workspace_bytes excludes iter vars.
- `layout_pass_test::LayoutPassComprehensionTest::FreeVarsCoexistWithCompScope`
  — free var slot ≠ accu var slot.
- `layout_pass_test::LayoutPassComprehensionTest::TwoIterListIndexUsesAccuKind`
  — v2 two-iter list index lives in accu workspace cell.
- `memory_layout_test::*` — pins the constants in
  `compiler/memory_layout.h`.

E2E correctness battery — `e2e/slot_aliasing_test.cc`. Bucketed
by AST kind (each row asserts a boolean eval result):

- **Constants** — every literal kind (null, bool, int, uint,
  double, string, bytes) round-tripped through `==`.
- **kCallExpr variants** — every operator (`+`, `-`, `*`, `/`,
  `%`, `==`, `!=`, `<`, `<=`, `>`, `>=`, `in`, `&&`, `||`, `!`,
  `_[_]`), every type conversion (`int`, `uint`, `double`,
  `string`, `bytes`, `bool`, `type`), receiver-form helpers
  (`size`, `startsWith`, `endsWith`, `contains`, `matches`),
  `dyn` passthrough, ternary nested two-deep.
- **Aggregate edges** — single-element literal, 20-element
  literal (LIFO reuse during appends), aggregate as call arg,
  two aggregates in a binary call.
- **Comprehensions** — every macro (see §13 below for the
  enumeration).
- **Aliasing shapes S1–S20** from the research catalog —
  read-before-write parent↔operand, chained arithmetic, sibling
  release, nested aggregates, chained select, receiver chain,
  map-with-list-field, map-with-map-field, large literal,
  chained indexing, list-of-list-of-list indexed, ternary
  aggregate arms, two-aggregate binary, 4-kind 4-deep stress,
  5-level mixed.

## 13. Comprehension test enumeration

Every CEL comprehension macro is exercised end-to-end:

| Macro | Form | E2E test row | Layout-test row |
| --- | --- | --- | --- |
| `has` | `has(x.y)` — kSelect with test_only=true, not a kComprehensionExpr | m7 `has(c.field)` rows | — |
| `all` | `xs.all(v, p)` | `comp_all_true`, `comp_all_false`, `comp_nested_all_in_all`, `comp_all_over_map_keys_nonempty` | `AccuVarHasWorkspaceSlot` |
| `exists` | `xs.exists(v, p)` | `comp_exists_present`, `comp_exists_absent`, `comp_nested_exists_in_exists`, `comp_exists_over_map_keys` | shares fixture with all |
| `exists_one` | `xs.exists_one(v, p)` | `comp_exists_one_unique`, `comp_exists_one_multiple_false` | shares fixture |
| `filter` | `xs.filter(v, p)` | `comp_filter_then_size`, `comp_filter_then_index`, `comp_size_in_arith` | shares fixture |
| `map` | `xs.map(v, f)` | `comp_map_pure`, `comp_map_summed_via_index` | shares fixture |
| `map(filter)` | `xs.map(v, p, f)` | `comp_map_with_filter` | shares fixture |
| v2 `all` / `exists` over map | `m.all(k, v, p)` etc. | (covered by `comp_*_over_map_keys` set) | `TwoIterListIndexUsesAccuKind` |
| v2 `exists_one` two-iter list | `xs.exists_one(i, v, p)` | covered in m5b | `TwoIterListIndexUsesAccuKind` |
| `cel.bind` | `cel.bind(name, init, body)` | covered in m5b and m13 | — |

When a new comprehension macro is added, the row goes in
`e2e/slot_aliasing_test.cc::kComprehensionKindCases` AND in
`compiler/codegen/layout_pass_test.cc::LayoutPassComprehensionTest`,
AND a row is added to the table above.

## 14. Trade-offs considered

Three designs were compared on 2026-06-04:

### A. Bump-only (Release is a no-op)

Provably correct (no aliasing possible) but peak workspace grows
linearly with internal node count. At 2 000-term arithmetic the
workspace would be ~64 KB, well above the 8 KiB reserved low
region — would corrupt wasi-libc.

**Rejected.** The 8 KiB reserved region makes this unusable for
realistic CEL.

### B. Uniform free-list reuse with "aggregate visitors skip Release on operands"

Aggregate parents don't release operand slots → those operand
slots stay live forever from the allocator's view → no descendant
can alias an ancestor's slot. Works but adds a *non-local* rule
that every future maintainer of the visitors has to remember.

**Rejected.** The coupling is fragile — a new aggregate-style
kind that forgets the rule reintroduces the bug silently.

### C. Uniform free-list reuse for all kinds (the pre-fix M10 design)

The bug. Breaks nested aggregates (§1).

### D. PreVisit Acquire for aggregates, PostVisit Release-then-Acquire for kCall/kSelect

**Shipped.** §10 proves correctness; §3–§7 show the per-kind
behavior. The two-rule split is mechanically dispatched by
`PreVisitExpr` switching on `kind_case`.

## 15. Future work (out of scope of this slice)

- **Sethi–Ullman labeling for non-commutative ops.** Right now
  the post-order traversal evaluates children left-first.
  Sethi-Ullman would visit the higher-labeled child first to
  minimize peak workspace. Safe only for operators whose
  evaluation order is non-observable (the pure arithmetic and
  comparison set); UNSAFE for `&&` / `||` / `?:` (short-circuit
  semantics) and for any user-defined function (purity unknown).
  Marginal gain for our workspace sizes; deferred.
- **Property-based testing with `fuzztest`.** The S1–S20 catalog
  is hand-curated; an AST-grammar-driven generator paired with
  the cel-cpp oracle would extend coverage to thousands of
  expressions automatically.
- **Slot-aliasing static assertion in expr_lower.** A
  compile-time DCHECK on the emitted Binaryen IR that asserts
  `out_slot ≠ operand_slot` for write-before-read helpers (and
  vice versa for the bare-aggregate-init shape) would turn a
  future contract violation into a noisy failure at the point of
  regression.
