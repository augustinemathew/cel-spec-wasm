# WAT traces — expr → target wasm, end-to-end

Companion doc to `design.md` + `m2-ident-select-unknowns.md` + (future)
`m5-comprehensions.md`.  Purpose: for each expression shape, write the
**exact wasm we want `expr_lower` to emit**, in WAT, and keep it
assembled-and-validated in-tree.  Treat each example as an executable
spec for that codegen arm.

Why this exists:

  - The expected wasm shape is the *source of truth* for a codegen
    arm.  Writing it before touching `expr_lower.cc` forces us to
    decide memory layout, ABI, and local-assignment up front — not
    as we go.
  - Comprehensions / selects / has()-traps get their design
    debated at the WAT level before a line of C++ is written.
    Cheap to throw away, expensive to discover mid-implementation.
  - Each WAT assembles with `wasm-as` (binaryen).  Broken shape →
    broken assemble → broken before the codegen arm lands.

Run:

```bash
for f in doc/implementation-plan/rewrite/wat/*.wat; do
  wasm-as "$f" -o "/tmp/$(basename "$f" .wat).wasm"
done
```

All examples in this doc live at `doc/implementation-plan/rewrite/wat/`
as standalone `.wat` files.  Each one is heavily commented inline.

## Memory map (shared across every expression)

```
[ 0,  8)  reserved null sentinel — offset 0 means "absent"
[ 8, 12)  arena cursor  (u32)  — written by cel_reset at entry
[12, 16)  arena limit   (u32)  — written by cel_reset at entry
[16, rodata_base + rodata.size())
          rodata: 24-byte CelValue frames + variable-length payload
          bytes (string bodies, bytes bodies).  Packed by
          StaticMemoryBuilder; written into memory as an active
          data segment at module-load time.
[workspace_base, workspace_base + workspace_bytes)
          24-byte CelValue cells, one per referenced free variable
          (M2) and one per internal-node output slot (M2.C selects,
          M3 calls).  Host writes variable values here before each
          Eval; codegen writes internal-node outputs here.
[arena_base, mem_size)
          bump arena for variable-length payloads produced at eval
          time (string concat results, etc.).  Reset on every Eval
          via cel_reset.
```

Alignments:

  - Every CelValue cell is 24 bytes, 8-aligned.
  - `workspace_base = round_up_8(rodata_base + rodata.size())`.
  - `arena_base = round_up_8(workspace_base + workspace_bytes)`.

Constants in every example:

  - `rodata_base = 16` (fixed).
  - `mem_size = 131072` (two wasm pages, default).

## Ident → local mapping (M2.B → M5 unified)

Every kIdent in the AST — free variable, comprehension iter, or
comprehension accu — lowers to the same wasm:

```
(local.get <local_index>)
```

The differences live at the **set site**, not the read site:

| Kind of ident | Set by | Set how often | Offset value |
|---|---|---|---|
| Free variable (M2.B) | `$eval` prelude | once per Eval | compile-time-known workspace slot |
| Comprehension iter (M5) | loop header | once per iteration | `iter_range_base + N * 24` (pointer into list payload) |
| Comprehension accu (M5) | accu_init + loop_step tail | once + once per iteration | accu slot, or fresh out from loop_step |

Consequence: `expr_lower`'s kIdent arm is **one line**
(`BinaryenLocalGet(payload, i32)`) and is identical across every
milestone that adds a new ident-like binding.

---

## 1. Literal — `42`

*M1 baseline.*  No variables, no workspace, rodata holds the one
CelValue.

```wat
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))

  ;; CelValue{kind=CEL_INT(2), _pad=0, payload.i=42, pad8=0}
  (data (i32.const 16)
        "\02\00\00\00"
        "\00\00\00\00"
        "\2a\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $cel_reset (i32.const 40) (i32.const 131072))
    (i32.const 16))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
```

Layout:

  - rodata = 24 bytes at [16, 40).
  - workspace_bytes = 0.
  - arena_base = 40.

---

## 2. Ident — `x` where `x : int`

*M2.B.*  Single free variable, zero literals.  Rodata is empty; the
variable's 24-byte slot sits right at `rodata_base`.

```wat
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))

  (func $eval (result i32)
    (local $x_off i32)  ;; one local per referenced variable

    ;; PRELUDE
    (local.set $x_off (i32.const 16))

    ;; RESET
    (call $cel_reset (i32.const 40) (i32.const 131072))

    ;; BODY — kIdent lowering
    (local.get $x_off))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
```

Layout:

  - rodata = empty.
  - workspace = [16, 40) — one slot for `x`.
  - arena_base = 40.

Host responsibility per Eval: encode the `Value::Int(v)` passed via
`Activation::Bind("x", …)` into the 24-byte cell at offset 16 before
calling `$eval`.

---

## 3. Two idents — `x + y` where both are `int`

*M2.B with M3 kCall stubbed.*  Two locals, two prelude `local.set`s.
The kCall arm (arithmetic) isn't implemented until M3 — the body stubs
to `unreachable` so the module still validates.

```wat
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))

  (func $eval (result i32)
    (local $x_off i32)  ;; local 0
    (local $y_off i32)  ;; local 1

    (local.set $x_off (i32.const 16))
    (local.set $y_off (i32.const 40))

    (call $cel_reset (i32.const 64) (i32.const 131072))

    ;; M3 body:
    ;;   (call $cel_int_add_at_vv
    ;;         (i32.const <out>)
    ;;         (local.get $x_off)
    ;;         (local.get $y_off))
    ;;   (i32.const <out>)
    unreachable)

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
```

Layout:

  - workspace = [16, 64) — two variable slots back-to-back.
  - arena_base = 64.

---

## 4. Select — `c.name` where `c : Customer`

*M2.C forward-look.*  Introduces the `cel_host.cel_get_field`
trampoline — four-arg slot-out signature
`(out_slot, msg_slot, field_ref_id, attribute_id)`.

```wat
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (import "cel_host" "cel_get_field"
          (func $cel_get_field (param i32 i32 i32 i32)))
  (import "cel_host" "cel_has_field"
          (func $cel_has_field (param i32 i32 i32 i32)))

  (func $eval (result i32)
    (local $c_off i32)

    (local.set $c_off (i32.const 16))

    (call $cel_reset (i32.const 64) (i32.const 131072))

    ;; kSelect → cel_host.cel_get_field
    (call $cel_get_field
          (i32.const 40)       ;; out_slot
          (local.get $c_off)   ;; msg_slot
          (i32.const 1)        ;; field_ref_id ("Customer.name")
          (i32.const 1))       ;; attribute_id ("c.name")

    (i32.const 40))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
```

Layout:

  - workspace = [16, 64)
      - [16, 40) = variable slot for `c`
      - [40, 64) = output slot for the select
  - arena_base = 64.

Note on slot assignment: LayoutPass reserves a fresh workspace slot
per internal node that produces a CelValue output (selects, calls,
hases).  Fresh-slot-per-node keeps the pass simple — later milestones
can add slot reuse (peak-count optimisation) without changing the
codegen arm.

ABI tables:

  - `cel.abi.fields[1] = {field_number: 1, name: "name", owner_fqn:
    "celwasm.testdata.Customer"}`.
  - `cel.abi.attributes[1] = {variable: "c", qualifiers: ["name"]}`.

Both decoded once by `Engine::Plan`; the runtime trampoline reads
`fields[field_ref_id]` to resolve the descriptor and reads
`attributes[attribute_id]` to match against unknown patterns under
`PartialEval`.

---

## 5. Comprehension — `[1, 2, 3].exists(x, x > 0)`

*M5 forward-look.*  The big payoff of the uniform `kLocal` ident
lowering: iter and accu are just more locals set by the loop
header, read by the body's kIdent arm — identical to free-variable
codegen at the read site.

cel-cpp's macro expander already rewrites `exists` to the explicit
`kComprehensionExpr` form:

```
kComprehensionExpr {
  iter_range  = [1, 2, 3]                      // kCreateList
  iter_var    = x
  accu_var    = __result__
  accu_init   = false
  loop_cond   = @not_strictly_false(!__result__)
  loop_step   = __result__ || (x > 0)
  result      = __result__
}
```

Target WAT (imports for the M3/M6 arithmetic helpers are declared but
we don't evaluate this module yet — it serves as the design spec for
the comprehension codegen arm):

```wat
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (import "cel" "cel_int_gt_at_vv"
          (func $cel_int_gt_at_vv (param i32 i32 i32)))
  (import "cel" "cel_or_at_vv"
          (func $cel_or_at_vv (param i32 i32 i32)))

  ;; Three int CelValues at [16, 88).
  (data (i32.const 16)
        "\02\00\00\00\00\00\00\00"
        "\01\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00"
        "\02\00\00\00\00\00\00\00"
        "\02\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00"
        "\02\00\00\00\00\00\00\00"
        "\03\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  ;; accu_init = false at [88, 112).
  (data (i32.const 88)
        "\01\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  ;; rhs of `x > 0` at [112, 136).
  (data (i32.const 112)
        "\02\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (local $iter_off  i32)  ;; x's local — set by loop header
    (local $accu_off  i32)  ;; __result__ — set by init + loop_step
    (local $end_off   i32)
    (local $step_out  i32)

    (call $cel_reset (i32.const 208) (i32.const 131072))

    ;; Comprehension setup.
    (local.set $iter_off (i32.const 16))   ;; &list[0]
    (local.set $end_off  (i32.const 88))   ;; &list[3]  (1-past-end)
    (local.set $accu_off (i32.const 88))   ;; accu <- rodata-false
    (local.set $step_out (i32.const 184))  ;; scratch for `(x > 0)`

    (block $exit
      (loop $continue
        ;; Exit when iter >= end.
        (br_if $exit
               (i32.ge_u (local.get $iter_off) (local.get $end_off)))

        ;; Exit when accu is already true.  CelValue.payload.b
        ;; lives 8 bytes into the cell (skipping kind + _pad).
        (br_if $exit (i32.load offset=8 (local.get $accu_off)))

        ;; loop_step: accu = accu || (x > 0)
        (call $cel_int_gt_at_vv
              (local.get $step_out)
              (local.get $iter_off)    ;; x IS iter_off — same kIdent arm
              (i32.const 112))
        (call $cel_or_at_vv
              (i32.const 160)
              (local.get $accu_off)
              (local.get $step_out))
        (local.set $accu_off (i32.const 160))

        ;; Advance iter by sizeof(CelValue) = 24.
        (local.set $iter_off
                   (i32.add (local.get $iter_off) (i32.const 24)))
        (br $continue)))

    (local.get $accu_off))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
```

Layout:

  - rodata [16, 136) — list elements + accu_init + zero-literal.
  - workspace [136, 208) — list header slot, accu slot, step_out slot.
  - arena_base = 208.

Invariants this WAT locks:

  1. **No per-iteration memcpy of iter_var.**  `x` IS
     `iter_off` — we advance a pointer, never copy the 24 bytes.
     `kLocal` storage for the iter_var is load-bearing here; the
     `kWorkspaceSlot` alternative would require a memcpy per
     iteration.
  2. **Same kIdent arm across free vars and comprehension vars.**
     The `x` inside `x > 0` lowers to `(local.get $iter_off)` —
     bit-for-bit what we'd emit for a free variable.
  3. **Loop structure is two nested wasm blocks** — `block $exit`
     around `loop $continue` — with `br_if $exit` / `br $continue`.
     Short-circuit `exists` exits as soon as accu becomes true.
  4. **No stack growth per iteration.**  Every slot used inside the
     loop is pre-assigned at LayoutPass.  `cel_alloc` only fires
     for variable-length payloads the helpers build (string
     concat, etc.) — not for the comprehension frame.

---

## 6. Array index — `arr[0]` where `arr : list<int>`

*M6 forward-look.*  The `_[_]` operator is a kCall whose semantics
the runtime `cel_list_index_at_vv` helper provides.  For
`list<int>`, the helper reads `arr`'s CelArray header to find the
payload base, computes `payload + index * 24`, and copies (or
aliases) the CelValue into the output slot.

```wat
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (import "cel" "cel_list_index_at_vv"
          (func $cel_list_index_at_vv (param i32 i32 i32)))

  ;; rodata: int const {0} at [16, 40).
  (data (i32.const 16)
        "\02\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (local $arr_off i32)

    (local.set $arr_off (i32.const 40))   ;; arr's variable slot

    (call $cel_reset (i32.const 88) (i32.const 131072))

    ;; `_[_]` → cel_list_index_at_vv(out_slot, list, index)
    (call $cel_list_index_at_vv
          (i32.const 64)       ;; out_slot
          (local.get $arr_off) ;; list
          (i32.const 16))      ;; index const 0

    (i32.const 64))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
```

Layout:

  - rodata [16, 40) — the literal `0`.
  - workspace [40, 88) — arr's slot, then out slot.
  - arena_base = 88.

Key observation: at the ident level nothing is special here.
`arr` is just another free variable; its CelValue lives in a
workspace slot and the kIdent arm emits `(local.get $arr_off)`.
The index operator is a kCall — its slot-out ABI is the same as
every other kCall.

Same story for `c.arr[10 + c.id]` (the "root in its own index"
shape): two kIdent `c` nodes share one slot, two kCalls (`_+_`
and `_[_]`) each take their own output slot.

---

## 6. Map literal — `{1: 10}` (M3.F kArena)

`wat/06_map_literal.wat`.  First map example: kCreateMap lowering.
Two rodata kConst frames for the key and value, one workspace slot
for the kMapExpr result.  Body is `cel_map_create(out, capacity)`
followed by one `cel_map_insert(out, key, value)` per entry, then
returns the map's slot offset.

Memory:

  - rodata [16, 40) + [40, 64) — the `1` and `10` kConsts.
  - workspace [64, 88) — kMapExpr result slot.  After the create,
    holds `{kind=CEL_MAP_ARENA, payload.arena_map.header_ptr=88}`.
  - arena [88, …) — `cel_map_create` allocates 16 B for the
    `ArenaMapHeader`, plus `capacity*48` B for the entries run.
    `cel_map_insert` writes (key, value) into the next free slot.

ABI surface introduced:

  - `cel.cel_map_create(out_slot, capacity)` — i32, i32 → ()
  - `cel.cel_map_insert(map_slot, key_slot, val_slot)` — i32×3 → ()

Both come from `cel_runtime.wasm`; codegen always links the
runtime fully (no AST-shape gating, per CLAUDE.md).

## 7. Map literal indexed — `{1: 10}[1]` (M3.F kArena fast path)

`wat/07_map_index_arena.wat`.  Builds the same map as 06, then
indexes it.  Because `ResolvePass::MapOriginVisitor` proved
`map_origin = kArena` on the kMapExpr, codegen routes the lookup
through `cel.cel_map_lookup_arena` — the pure-wasm fast path —
bypassing the kDynamic dispatcher and the kHost trampoline.

Memory:

  - rodata [16, 40) + [40, 64) — insert key + value.
  - rodata [64, 88) — lookup-key kConst (no dedup at M3, so the
    literal `1` appears twice in rodata).
  - workspace [88, 112) — kMapExpr result slot.
  - workspace [112, 136) — kCallExpr lookup-result slot.

The lookup helper does a linear scan of the entries run for a key
that `StructurallyEquals` the lookup key, copies the matching
value CelValue into out_slot, or writes
`{CEL_ERROR, payload.err=CEL_ERR_NO_SUCH_KEY}` on miss.

## 8. Bound map indexed — `m["k"]` (M3.F kHost path)

`wat/08_map_index_host.wat`.  `m` is declared `map<string, int>`;
the host binds a `HostMapBacking` (vector-backed via
`Activation::Bind`, or `ProtoMap` if from a proto map field) into
the per-Instance `ExternrefTable`.  The map CelValue at `m`'s slot
carries `{CEL_MAP_HOST, payload.ref_slot=<n>}`.
`MapOriginVisitor` stamped `map_origin = kHost` on the kIdent
(because its declared type is `map<…>`), so codegen routes the
index call directly through `cel_host.cel_map_lookup` — no
runtime dispatcher trip, no arena helpers.

The body is one extern call:
```
(call $cel_map_lookup out_slot m_slot key_slot)
```

The Layer-3 wasmtime trampoline (in
`api/internal/cel_host_wasmtime.cc`) reads the ref_slot, looks up
the backing via `ExternrefTable::LookupMap`, decodes the key
CelValue, calls `HostMapBacking::Get`, encodes the result back
into out_slot via `EncodeFieldResult`.

## 9. Dynamic-origin map index — runtime dispatcher (M3.C)

`wat/09_map_index_dynamic.wat`.  When `ResolvePass` cannot prove
a single origin (mixed `?:` arms, future kCall return), the
kCallExpr arm emits `call $cel.cel_map_lookup` — the runtime
dispatcher in `cel_runtime.wasm`.  The dispatcher tail-calls into
the arena fast path or the host trampoline based on the
operand's runtime CelKind:

  - `CEL_MAP_ARENA` → `return_call cel_map_lookup_arena`
  - `CEL_MAP_HOST` → `return_call cel_host.cel_map_lookup`
  - anything else (UNKNOWN / ERROR / type mismatch) → poison
    out_slot or absorb 3VL.

The musttail discipline is enforced via `__attribute__((musttail))`
in `cel_runtime.c`; the wasm tail-call feature must be on at the
engine level (mirrored in `api/engine.cc`).

This trace authors the call site; the dispatcher BODY lives in
`cel_runtime.wasm`.  At M4 the only origin that flows through
`kDynamic` from the frontend is the future `?:` codegen (M5);
production paths cover the dispatcher arms via
`m3_test::EnvelopeBoundaryE2ETest` and `instance_test`.  The
`wat_runner_test` for this WAT documents a wasmtime c-api panic
on the tail-call → host-import path and SKIPs the end-to-end
run.

## 10. Proto map field — `c.metadata["k"]` (M3.G chained kSelect + kCall)

`wat/10_proto_map_field.wat`.  Two host trampoline calls in
sequence:

  1. `cel_host.cel_get_field` reads c's `metadata` field.
     `ProtoBacking::ReadField` on a MAP field returns
     `Value::HostMap(ProtoMap{owner, field})`; the trampoline
     interns it via `InternMap` and writes
     `{CEL_MAP_HOST, ref_slot=<n>}` into the kSelect's output
     slot.
  2. `cel_host.cel_map_lookup` indexes that HostMap by `"k"` —
     the operand at the call site is exactly the kSelect's
     output slot from step 1.

Locks the kSelect → kCall(`_[_]`) chaining shape.  The
`MapOriginVisitor` stamps `map_origin = kHost` on the kSelect
because its result type is `map<…>` — driving codegen to emit
the host-arm import name at step 2.

## 11. List literal — `[1, 2, 3]` (M4.F kArena)

`wat/11_list_literal.wat`.  Mirror of 06 for lists.  Three rodata
kConst frames for the elements, one workspace slot for the
kListExpr result.  Body is `cel_list_create(out, count)` followed
by one `cel_list_set(out, index, elem)` per element.

**Plan-vs-execution delta** from `m4-list-literals.md`: the runtime
API is `create(out, count)` + `set(list, index, elem)`, NOT the
planned `create / append / grow` triple.  Codegen always knows the
element count at lowering time, so a fixed-length API is simpler.
Past-count `set` poisons with `CEL_ERR_OVERFLOW`.

Memory:

  - rodata [16, 40) + [40, 64) + [64, 88) — the three int kConsts.
  - workspace [88, 112) — kListExpr result slot.  After
    `cel_list_create`, holds `{CEL_LIST_ARENA,
    payload.arena_list.header_ptr=112}`.
  - arena [112, …) — `cel_list_create` allocates 16 B for the
    `ArenaListHeader` plus `count*24` B for the elements run.

## 12. List literal indexed — `[1, 2, 3][1]` (M4.F kArena fast path)

`wat/12_list_index_arena.wat`.  Mirror of 07 for lists.  Indexes
through `cel.cel_list_at_arena` (no host trip).  The index slot
must be `CEL_INT`; non-int → `CEL_ERR_TYPE_MISMATCH`.  Negative
indices and `>= count` → `CEL_ERR_INDEX_OUT_OF_BOUNDS` per langdef
("list indices are int and negative indices are an error, not
Python-style wrap-around").

## 13. Bound list indexed — `xs[0]` (M4.F kHost path)

`wat/13_list_index_host.wat`.  Mirror of 08.  `xs` is declared
`list<int>`; the host binds a `HostListBacking` (vector-backed or
`ProtoList`).  The list CelValue carries
`{CEL_LIST_HOST, payload.ref_slot=<n>}`.  `ListOriginVisitor`
stamps `list_origin = kHost` on the kIdent, so codegen emits a
direct `cel_host.cel_list_at` call — no runtime trip.

## 14. Dynamic-origin list index — runtime dispatcher (M4.C)

`wat/14_list_index_dynamic.wat`.  Mirror of 09 for lists.  Same
dispatcher pattern; same ABI shape; same musttail discipline.
Production paths cover the dispatcher arms via `instance_test` /
`m4_test`; the `wat_runner_test` for this WAT documents the same
wasmtime c-api panic as 09 and SKIPs the end-to-end run.

## 15. Proto repeated field — `c.tags[2]` (M4.G chained kSelect + kCall)

`wat/15_proto_repeated_field.wat`.  Mirror of 10 for lists.  Two
host trampoline calls: `cel_get_field` reads c's `tags` REPEATED
field (`ProtoBacking::ReadField` returns
`Value::HostList(ProtoList{owner, field})`, interned via
`InternList`), then `cel_host.cel_list_at` indexes it.

The `Customer.tags = repeated string` field was added at M4.J
for the e2e suite (`m4_test::ProtoRepeatedE2ETest`).

---

## Future entries (stubs)

  - `has(c.field)` — M2.D, `cel_host.cel_has_field` returns bool
    directly into an out_slot.
  - `c.a.b.c` — M2.C nested select chain; each hop is a separate
    `cel_get_field` call threading msg_slot through intermediate
    output slots.
  - `c.name` under `PartialEval` with unknowns — M2.E; runtime
    trampoline matches `attribute_id` against the pattern set and
    writes `{CEL_UNKNOWN, attribute_id}` to out_slot instead of
    descending the proto.
  - `x + y` — M5 kCall arithmetic; same shape as example 3 with
    the `unreachable` replaced by `cel_int_add_at_vv`.
  - `[1, 2, 3].map(x, x * 2)` — M5; extends example 5 with a new
    list built by the loop_step using `cel_list_create` +
    per-iteration `cel_list_set` over a pre-sized accumulator.
  - `cond ? [1, 2] : xs` — M5 ternary lowering with
    `list_origin = kDynamic`; the `_[_]` arm emits the dispatcher
    (`cel.cel_list_at`) since the operand origin can't be proven
    statically.
  - `x in [1, 2, 3]` / `size([1, 2, 3])` — M5 kCall built-in
    overload set; reuses M3/M4's three-path origin dispatch.

Each future entry follows the same pattern: write the target WAT,
assemble it, discuss the memory layout, land the C++ codegen to
match.
