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

## 16. Arithmetic — `1 + 2` (M5.B slot-out helper ABI)

`wat/16_arith_int_add.wat`.  First WAT for the M5 general kCall
arm.  Locks the uniform helper ABI shape every M5 helper uses:

```
(i32 out_slot, i32 arg0, ..., i32 argN-1) -> void
```

Helpers read operand CelValues out of `arg*_slot`s, compute, write
the result CelValue into `out_slot`.  No return value — the caller
already knows the result lives at `out_slot`.

`cel_int_add_at_vv(out_slot, a_slot, b_slot)`:

  - reads a / b as CEL_INT (any other kind on either operand →
    `out_slot = {CEL_ERROR, err = CEL_ERR_TYPE_MISMATCH}`);
  - 3VL absorption — UNKNOWN/ERROR on either operand propagates
    verbatim into `out_slot`;
  - signed overflow → `CEL_ERR_OVERFLOW` per langdef §"Numeric
    values" (NOT wrap).  `__builtin_add_overflow` detects;
  - happy path: `out_slot = {CEL_INT, _pad=0, i = a+b}`.

Memory map:
  - rodata [16, 40) + [40, 64) — two CEL_INT kConsts.
  - workspace [64, 88) — kCall(`_+_`) result slot.
  - arena [88, …).

Mul on int64 is the load-bearing implementation detail: the
runtime helper avoids `__builtin_mul_overflow` because clang
lowers it through `__multi3` (compiler-rt 128-bit multiply, not
linked into the wasm32 freestanding build).  Instead the runtime
splits operands into 32-bit halves and assembles the 128-bit
product manually using only 32×32→64 partial multiplies that the
wasm32 backend lowers natively as `i64.mul`.  See
`cel_runtime.c::uint64_mul_overflows`.

## 17. Comparison — `1 == 2` (M5.B slot-out compare ABI)

`wat/17_compare_int_eq.wat`.  Companion to 16 — same slot-out
shape, only the result kind differs (CEL_BOOL instead of CEL_INT).
Locks that comparison and arithmetic share the helper ABI.

`cel_int_eq_at_vv(out_slot, a_slot, b_slot)`:

  - reads a / b as CEL_INT (cross-type numeric equality
    `1 == 1u` / `1 == 1.0` routes through a separate
    `cel_numeric_*` ladder added in M5.B step 2);
  - 3VL absorption matches the arith helpers;
  - happy path: `out_slot = {CEL_BOOL, b = (a==b ? 1 : 0)}`.

Same memory map as 16.  Runtime exports cover the full per-kind
matrix (eq/ne/lt/le/gt/ge × int/uint/double + bool eq/ne + null
eq); the WAT exercises one representative.

## 18. String concat — `"ab" + "cd"` (M5.C arena-alloc helper ABI)

`wat/18_string_concat.wat`.  M5.C's WAT representative.  Locks
the slot-out shape for helpers that allocate output bytes in the
arena: same `(out_slot, args…) → ()` signature as M5.B, but the
helper's effect is to extend a CelSpan to point at fresh arena
bytes rather than rewrite a fixed-size scalar payload.

`cel_string_concat_at_vv(out_slot, a_slot, b_slot)`:

  - reads a / b as CEL_STRING (other kind on either operand →
    CEL_ERR_TYPE_MISMATCH);
  - 3VL absorption matches the arith / compare envelope;
  - allocates `a.len + b.len` bytes via `cel_alloc`.  OOM →
    `out_slot = {CEL_ERROR, err = CEL_ERR_OVERFLOW}`;
  - copies a.bytes then b.bytes into the new buffer;
  - writes `out_slot = {CEL_STRING, payload.s = {ptr=<new>, len}}`.

Memory map:
  - rodata [16, 40) + [40, 64) — two CEL_STRING kConsts pointing
    into payload bytes at [64, 68).
  - workspace [72, 96) — kCall(`_+_`) result slot.
  - arena [96, …) — concat target lives here.

The new payload is owned by the arena `cel_reset` rewinds at the
top of the next $eval, mirroring M1's `cel_make_string` lifetime
contract exactly.

The other M5.C string helpers (size, eq, lt, contains,
startsWith, endsWith) and bytes helpers (concat, size, eq, lt)
share the same slot-out shape but don't allocate — size writes
CEL_INT, the predicates write CEL_BOOL.  Only concat exercises
the arena-alloc path the WAT locks here.

## 21. List size — `size([1, 2, 3])` (M5.D step 1 kArena)

`wat/21_size_list.wat`.  First WAT for the M5.D aggregate-op
kArena fast path.  Same slot-out shape as the M3/M4 dispatchers,
but the helper now reports a derived value (count) instead of
returning a slice of operand state.

`cel_list_size_arena(out_slot, list_slot) — i32×2 → ()`:

  - reads l as CEL_LIST_ARENA (other kind → CEL_ERR_TYPE_MISMATCH);
  - 3VL absorption — UNKNOWN/ERROR propagates;
  - happy path: writes `{CEL_INT, i = ArenaListHeader.count}`.

The kHost / kDynamic siblings land in M5.D step 2 (the kHost
trampoline goes through `cel_host.cel_list_size`; the kDynamic
dispatcher uses `__attribute__((musttail))` arms exactly like
`cel_list_at`).  Codegen routes via `list_origin` annotation per
`map-list-dispatch.md §6`.

## 22. List `in` — `2 in [1, 2, 3]` (M5.D step 1 kArena)

`wat/22_in_list.wat`.  Two-operand companion to 21.  Locks the
`(out_slot, value_slot, list_slot)` shape.

`cel_list_in_arena(out_slot, value_slot, list_slot) — i32×3 → ()`:

  - reads l as CEL_LIST_ARENA;
  - 3VL absorption on BOTH operands;
  - element comparison via `cel_value_eq` — the shared scalar
    matcher used by both `list_in_arena` and `list_eq_arena`.
    Reuses `map_keys_equal`'s int↔uint cross-type ladder, plus
    same-kind double / bytes / null branches that map keys
    don't need;
  - happy path: writes `{CEL_BOOL, b = (needle ∈ list ? 1 : 0)}`.

The 7 M5.D step 1 helpers (`cel_{list,map}_{size,in,eq}_arena` +
`cel_list_concat_arena`) all share this shape — slot-out, scalar
result, kArena-only.  WATs 21 / 22 lock the two operand-count
shapes (1-operand `size`, 2-operand `in`); the rest is mechanical
mirror.

---

## 30. Logical AND — `true && false` (M5.G eager-eval helper)

`wat/30_logical_and.wat`.  Locks the slot-out shape for `_&&_`.
Both operands are eagerly evaluated into their own slots; the
helper then runs the 3VL truth table and writes the result into
`out_slot`.  No short-circuit branching at the wasm level —
non-strict semantics force full eval (`false && (1/0)` must
succeed at `false`, not propagate the divide-by-zero), and the
truth table itself runs entirely inside `cel_and`.

`cel_and(out_slot, a_slot, b_slot) — i32×3 → ()` (parity with
cel-cpp `runtime/standard/logical_functions.cc::LogicalAnd`):

  - OK(false) on EITHER side absorbs everything (including a
    non-3VL other operand) → `false && X = false`.
  - Past the absorber: any non-3VL operand → CEL_ERROR with
    code `CEL_ERR_TYPE_MISMATCH`.
  - OK(true) && X = X (with X ∈ {bool, error, unknown}).
  - ERROR > UNKNOWN dominance.
  - Both UNKNOWN → sorted-deduplicated union of attribute-id
    sets via `cel_unknown_merge`.

## 31. Logical OR — `false || true` (M5.G eager-eval helper)

`wat/31_logical_or.wat`.  Symmetric companion to 30.  Mirrors
`cel_and` with the OK(false) / OK(true) absorbers swapped:
`true || X = true (any X)`; `false || X = X` (with the same
downstream type-check + ERROR/UNKNOWN dominance).

## 32. Logical NOT — `!true` (M5.G unary helper)

`wat/32_logical_not.wat`.  Unary slot-out helper.  ABI mirrors
the unary arithmetic helpers (`cel_int_neg_at_v`, etc.):
`(out_slot, v_slot) → ()`.

`cel_not(out_slot, v_slot)`:

  - bool true  → bool false
  - bool false → bool true
  - ERROR / UNKNOWN → propagate verbatim (24-byte copy).
  - Any other kind → CEL_ERROR with `CEL_ERR_TYPE_MISMATCH`.

## 33. Conditional — `true ? 1 : 2` (M5.G inline branching)

`wat/33_conditional.wat`.  Unlike the eager-eval helpers for
`_&&_` / `_||_`, ternary lowers to **inline branching wasm**:
only the selected arm is evaluated, side effects on the dropped
arm are skipped, and the result is materialised into `out_slot`
via `cel_copy_slot`.  This is correct under langdef
§"Conditional expression": "If c is an error or unknown, the
result is c.  Otherwise, only the chosen branch is evaluated."

Branch shape (codegen target):

```
if (cond.kind == CEL_BOOL) {
  if (cond.payload.b != 0) {
    <eval then-branch into then_slot>
    cel_copy_slot(out, then_slot);
  } else {
    <eval else-branch into else_slot>
    cel_copy_slot(out, else_slot);
  }
} else {
  // ERROR / UNKNOWN propagate verbatim.
  cel_copy_slot(out, cond_slot);
}
```

`cel_copy_slot(dst_slot, src_slot)` is a tiny memcpy helper —
24-byte CelValue copy.  The ternary lowering is the only caller
today; future helpers (e.g. an explicit `as`-cast slot move)
could reuse it without growing the runtime surface.  Codegen
could inline a `BinaryenMemoryCopy` instead, but a runtime-side
helper keeps `expr_lower` lean and the WAT shape regular.

---

## 40. Proto message construction — `HostMsg3{}` (M7.A empty literal)

`m7-proto-literals.md` §4.1 + §4.4: `kStructExpr` lowers to a
`cel_host.cel_make_message(type_id, out_slot)` call.  Empty
construction (no entries) is M7.A's baseline; per-entry
`cel_set_field` calls layer in at M7.B, between the make-message
call and the trailing `i32.const out_slot`.

```wat
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (import "cel_host" "cel_make_message"
          (func $cel_make_message (param i32 i32)))

  (func $eval (result i32)
    (call $cel_reset (i32.const 40) (i32.const 131072))
    (call $cel_make_message (i32.const 1) (i32.const 16))
    (i32.const 16))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
```

Memory layout (empty-construction case):

  - `[ 0, 16)` reserved + arena cursor/limit
  - `[16, 40)` workspace slot for the kStructExpr — out_slot
  - `[40, mem_size)` bump arena (untouched for empty literal)

Post-call, the workspace cell at offset 16 holds:
`{ kind = CEL_MESSAGE, payload.msg_slot = <ExternrefTable index> }`.
The externref points at an `OwnedProtoBacking` owning the
default-constructed proto.

ABI table addition: `cel.abi.types[]` — one row per distinct
message FQN constructed by the program.  Each row is
`{ id: u32, fully_qualified_name: string }`.  No descriptor
handle on the wire — `Engine::Plan` resolves FQN against
`DescriptorPool::generated_pool()` (or the embedder-supplied
pool) at load time.  Mirrors `cel.abi.fields[]`'s
`owner_fqn`-resolved-at-Plan-time discipline; descriptor
duplication into the wire would just be a drift hazard.

`cel_make_message` host primitive:

  1. Resolve `type_id` against the per-Instance type table
     (populated from `cel.abi.types[]` at Plan time) → `Descriptor*`.
  2. `MessageFactory::generated_factory()->GetPrototype(desc)->New()`.
  3. Wrap in `OwnedProtoBacking(unique_ptr<Message>)` — owning so
     the externref-table cleanup at `Reset()` frees the message.
  4. `ExternrefTable::Intern(shared_ptr<OwnedProtoBacking>)` →
     `slot`.
  5. Write `{ CEL_MESSAGE, msg_slot = slot }` to `out_slot`.

`OwnedProtoBacking` composes a `ProtoBacking` over its owned
message for the read-side `ReadField` / `HasField` overrides — no
duplicated reflection code; M7.A reuses the M2.C kSelect read
path verbatim against M7-constructed messages.

---

## 41. Proto field set — `HostMsg3{i32: 7}` (M7.B scalar entry)

Layered on top of M7.A's `cel_make_message`: each entry of a
non-empty literal lowers to a `cel_host.cel_set_field(msg_slot,
field_ref_id, value_slot)` call after the make-message call.  The
emit shape is regular — N entries layer N set-field calls between
the make-message and the trailing `(i32.const out_slot)`.

```wat
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (import "cel_host" "cel_make_message"
          (func $cel_make_message (param i32 i32)))
  (import "cel_host" "cel_set_field"
          (func $cel_set_field (param i32 i32 i32)))

  (data (i32.const 64)
        "\02\00\00\00\00\00\00\00\07\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $cel_reset (i32.const 88) (i32.const 131072))
    (call $cel_make_message (i32.const 1) (i32.const 16))
    (call $cel_set_field (i32.const 16) (i32.const 1) (i32.const 64))
    (i32.const 16))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
```

`cel_set_field` arg layout:

  - `msg_slot`     — offset of the 24B CelValue holding the
    `{CEL_MESSAGE, msg_slot}` ref to the OwnedProtoBacking; the
    backing exposes a `mutable_message()` for `Reflection::Set...`
    in M7.B (M7.A only used the read-side accessor).
  - `field_ref_id` — dense index into `cel.abi.fields[]` (the
    same intern table M2.C populates for read-side kSelect).
    M7.B reuses the `FieldRefRow` shape verbatim — the read and
    write paths share the (`field_number`, `name`, `owner_fqn`)
    row.  M7.B emits `field_number=0` so the host resolves the
    FieldDescriptor by name on the bound message at trampoline
    call time, matching the existing `ResolveFieldDescriptor`
    fallback ProtoBacking already uses.
  - `value_slot`   — offset of the 24B CelValue holding the
    new field value.  The trampoline dispatches on the resolved
    FieldDescriptor's `cpp_type` to pick the Reflection setter:
    BOOL → `SetBool`, INT32/INT64 → `SetInt32`/`SetInt64`,
    UINT32/UINT64 → `SetUInt32`/`SetUInt64`, FLOAT/DOUBLE →
    `SetFloat`/`SetDouble`, STRING → `SetString` (TYPE_BYTES vs
    TYPE_STRING distinguishes the encoding the value carries),
    ENUM → `SetEnumValue` (CEL_INT-source per langdef
    §"Enumerated Types").

Repeated, map, and singular-message field shapes are M7.C/E and
the trampoline returns a non-OK Status (wasm trap) until those
slices ship — conformance rows that reach this stub fail the
single row cleanly, not the run.

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
