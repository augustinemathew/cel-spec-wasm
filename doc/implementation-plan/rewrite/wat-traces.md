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
  accu_var    = @result
  accu_init   = false
  loop_cond   = @not_strictly_false(!@result)
  loop_step   = @result || (x > 0)
  result      = @result
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
    (local $accu_off  i32)  ;; @result — set by init + loop_step
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

## 50. Duration + timestamp arithmetic — `dur + dur` and `ts - ts` (M7B.B)

Pure-wasm arithmetic kernels.  Per `m7b-duration-timestamp.md` §4.3
(Option C — split runtime/host), the 6 arithmetic helpers
(`cel_dur_add_at_vv`, `cel_dur_sub_at_vv`, `cel_ts_dur_add_at_vv`,
`cel_dur_ts_add_at_vv`, `cel_ts_dur_sub_at_vv`, `cel_ts_ts_sub_at_vv`)
plus the 8 ordering helpers stay in pure wasm — all they need is
`__builtin_add_overflow` on the seconds + nanos-carry, no library
dependency.  The slot-out ABI is byte-for-byte identical to
`cel_int_add_at_vv` (see §16).

Two kernels are traced together to lock both the (dur, dur) → dur
and (ts, ts) → dur signatures — the two "interesting" arithmetic
shapes.  The other 4 helpers are structurally identical with the
kind tag swapped between CEL_DURATION(12) and CEL_TIMESTAMP(13).

| Region | Offset | Contents |
|---|---|---|
| rodata | `[16, 40)` | `CelValue{CEL_DURATION, dur={3600, 0}}` — `duration("3600s")` |
| rodata | `[40, 64)` | `CelValue{CEL_DURATION, dur={60, 0}}` — `duration("60s")` |
| workspace | `[64, 88)` | `cel_dur_add_at_vv` out_slot → `{CEL_DURATION, dur={3660, 0}}` |
| rodata | `[88, 112)` | `CelValue{CEL_TIMESTAMP, ts={1234567890, 0}}` — `"2009-02-13T23:31:30Z"` |
| rodata | `[112, 136)` | `CelValue{CEL_TIMESTAMP, ts={1234567889, 0}}` — `"2009-02-13T23:31:29Z"` |
| workspace | `[136, 160)` | `cel_ts_ts_sub_at_vv` out_slot → `{CEL_DURATION, dur={1, 0}}` |
| arena | `[160, ...)` | bump |

```wat
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_dur_add_at_vv"
          (func $cel_dur_add_at_vv (param i32 i32 i32)))
  (import "cel" "cel_ts_ts_sub_at_vv"
          (func $cel_ts_ts_sub_at_vv (param i32 i32 i32)))
  ;; rodata at 16/40/88/112 elided for brevity — see the file.
  (func $eval (result i32)
    (call $cel_reset (i32.const 160) (i32.const 131072))
    (call $cel_dur_add_at_vv  (i32.const 64) (i32.const 16) (i32.const 40))
    (call $cel_ts_ts_sub_at_vv (i32.const 136) (i32.const 88) (i32.const 112))
    (i32.const 64))
  (export "eval" (func $eval))
  (export "memory" (memory 0)))
```

Invariants the shape locks:

  - Slot-out ABI `(i32 out_slot, i32 a_slot, i32 b_slot) -> ()` is
    byte-identical to the M5.B `cel_int_add_at_vv` shape — codegen
    can reuse `expr_lower.cc::EmitGeneralCall` unchanged once
    `add_duration_duration` / `subtract_timestamp_timestamp`
    graduate from `kExplicitlyUnimplementedIds`.
  - Both helpers write a 24-byte CelValue at out_slot.  The
    `_pad` field after `nanos` in `CelDurTs` must stay zero —
    `cel_equals_at_vv` will compare CelDurTs as a 16-byte unit
    once the M7B.A CEL_DURATION arm lands.
  - Overflow → `{CEL_ERROR, CEL_ERR_OVERFLOW}` per langdef
    §"Timestamps and Durations" (NOT wrap).
  - 3VL absorption mirrors the existing v1 M4 Slice A semantics.

Codegen call-site: `compiler_v2/codegen/expr_lower.cc::EmitGeneralCall`
(M7B.B work), table-driven from the OverloadTable.  Reuses the
M5.B slot-out lowering machinery verbatim.

Authored alongside the WAT file at
`doc/implementation-plan/rewrite/wat/50_duration_arithmetic.wat`.

---

## 51. Timestamp UTC accessor — `ts.getFullYear()` (M7B.C)

The pure-wasm half of the Option-C split.  Per
`m7b-duration-timestamp.md` §4.9, all 10 UTC accessor overloads
project a field of the shared `CelCivil` struct that
`cel_civil_from_seconds` produces — Probe A in §10.1 confirmed
Hinnant's `civil_from_days` algorithm is bit-identical to
`absl::ToCivilSecond(UTCTimeZone())` across the §6.4 quirk grid
(Y2K leap-divisible-400, century-not-leap, langdef Y0001 lower
bound, Y9999 upper bound).  No host trampoline; the runtime kernel
stays descriptor-free per `design.md` §4.7.6.

This trace pins the 2-arg `(out_slot, v_slot) -> ()` shape for all
14 accessor helpers — the 10 timestamp UTC accessors AND the 4
duration accessors (`cel_dur_hours`, `cel_dur_minutes`, `cel_dur_seconds`,
`cel_dur_milliseconds`).  Only the field projection or division
ladder differs per-helper.

| Region | Offset | Contents |
|---|---|---|
| workspace | `[16, 40)` | bound `ts` slot → `{CEL_TIMESTAMP, ts={1234567890, 0}}` |
| workspace | `[40, 64)` | `cel_ts_year_utc` out_slot → `{CEL_INT, i=2009}` |
| arena | `[64, ...)` | bump |

```wat
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_ts_year_utc"
          (func $cel_ts_year_utc (param i32 i32)))
  (func $eval (result i32)
    (local $ts_off i32)
    (local.set $ts_off (i32.const 16))
    (call $cel_reset (i32.const 64) (i32.const 131072))
    (call $cel_ts_year_utc
          (i32.const 40)
          (local.get $ts_off))
    (i32.const 40))
  (export "eval" (func $eval))
  (export "memory" (memory 0)))
```

`cel_ts_year_utc` arg layout:

  - `out_slot` — 24B CelValue cell receiving `{CEL_INT(2), i = year}`
    where `year ∈ [1, 9999]` per langdef §"Timestamps and Durations".
  - `ts_slot` — 24B CelValue with `kind = CEL_TIMESTAMP(13)`.  Other
    kinds route to `CEL_ERROR(TYPE_MISMATCH)`; UNKNOWN/ERROR pass
    through verbatim.

Invariants the shape locks:

  - 2-arg `(out, v)` slot-out form; nanos field is read but ignored
    by the year accessor (resolves to year-level only).
  - `cel_civil_from_seconds` is a shared internal helper, NOT a
    public ABI export — the 10 timestamp UTC accessors are the
    public surface.
  - The companion two-arg form `ts.getFullYear('America/Los_Angeles')`
    goes through a different (host trampoline) ABI surface — see §54.

Codegen call-site: `compiler_v2/codegen/expr_lower.cc::EmitGeneralCall`
(M7B.C work).  The 9 sibling timestamp UTC accessors AND the 4
duration accessors use this same lowering shape with a different
helper name.

Authored alongside the WAT file at
`doc/implementation-plan/rewrite/wat/51_timestamp_year_utc.wat`.

---

## 52. Timestamp parse — `timestamp("2009-02-13T23:31:30Z")` (M7B.D)

Per `m7b-duration-timestamp.md` §4.3 (Option C — split runtime/host),
constructors and formatters that genuinely need a library trampoline
to the host.  RFC3339 parsing is one of those — Probe B in
`m7b-duration-timestamp.md` §10.2 confirmed `absl::ParseTime` admits
inputs CEL rejects (lowercase `z`, year > 9999, leap-second `23:59:60`,
two-digit year), so the Layer-2 impl post-validates after absl.

| Region | Offset | Contents |
|---|---|---|
| rodata | `[16, 40)` | `CelValue{CEL_STRING, span={ptr=40, len=20}}` |
| rodata | `[40, 60)` | `"2009-02-13T23:31:30Z"` |
| workspace | `[64, 88)` | `cel_timestamp_parse` out_slot → `{CEL_TIMESTAMP, ts={1234567890, 0}}` |
| arena | `[88, ...)` | bump |

```wat
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel_host" "cel_timestamp_parse"
          (func $cel_timestamp_parse (param i32 i32)))

  (data (i32.const 16)
        "\05\00\00\00\00\00\00\00\28\00\00\00\14\00\00\00\00\00\00\00\00\00\00\00")
  (data (i32.const 40) "2009-02-13T23:31:30Z")

  (func $eval (result i32)
    (call $cel_reset (i32.const 88) (i32.const 131072))
    (call $cel_timestamp_parse (i32.const 64) (i32.const 16))
    (i32.const 64))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
```

`cel_timestamp_parse` arg layout:

  - `out_slot` — 24B CelValue cell that will receive the parsed
    `{CEL_TIMESTAMP(13), payload.ts = CelDurTs{seconds, nanos, _pad}}`.
    On parse failure the Layer-2 impl writes `{CEL_ERROR,
    CEL_ERR_INVALID_ARG}` here instead — no trap.
  - `str_slot` — 24B CelValue with `kind = CEL_STRING(5)` and
    span pointing to the source body.  Any non-string in this slot
    routes to `{CEL_ERROR, CEL_ERR_TYPE_MISMATCH}`.

Invariants the shape locks:

  - 2-arg slot-out form, module `cel_host` (not `cel`) — locks the
    host-vs-runtime split.
  - Post-absl validation runs in Layer-2 against the CEL admit-set
    identified by Probe B; the trampoline never traps.
  - The companion overloads `cel_host.cel_duration_parse`,
    `cel_host.cel_timestamp_format` use the same 2-arg shape.

Codegen call-site: `compiler_v2/codegen/expr_lower.cc::EmitGeneralCall`
once `string_to_timestamp` / `timestamp_to_timestamp` graduate from
`kExplicitlyUnimplementedIds`.

Authored alongside the WAT file at
`doc/implementation-plan/rewrite/wat/52_timestamp_parse.wat`.

---

## 53. Duration format — `string(duration("3600s"))` (M7B.D)

The partner of §52.  Format-side of the Option-C split: the proto
Duration canonical text format ("3600s", "0.001s", "0.000000001s")
requires the same fractional-zero-suppression logic cel-cpp
delegates to `internal::EncodeDurationToJson`.  Layer-2 `*Impl`
in `cel_host.cc` will own that body; the wasm side just emits the
trampoline call.

| Region | Offset | Contents |
|---|---|---|
| rodata | `[16, 40)` | `CelValue{CEL_DURATION, dur={3600, 0}}` |
| workspace | `[40, 64)` | `cel_duration_format` out_slot → `{CEL_STRING, span={arena_off, 5}}` for `"3600s"` |
| arena | `[64, ...)` | bump (Layer-2 cel_allocs the formatted body here) |

```wat
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (import "cel_host" "cel_duration_format"
          (func $cel_duration_format (param i32 i32)))
  ;; rodata @ 16: CEL_DURATION{3600, 0} — elided.
  (func $eval (result i32)
    (call $cel_reset (i32.const 64) (i32.const 131072))
    (call $cel_duration_format
          (i32.const 40)        ;; out_slot
          (i32.const 16))       ;; dur_slot
    (i32.const 40))
  (export "eval" (func $eval))
  (export "memory" (memory 0)))
```

`cel_duration_format` arg layout:

  - `out_slot` — receives `{CEL_STRING(5), span={ptr=<arena_off>,
    len=<formatted bytes>}}`.  The formatted bytes are cel_alloc'd
    into the per-Eval arena by the Layer-2 trampoline body.
  - `dur_slot` — read as CEL_DURATION; any other kind → CEL_ERROR.

Invariants the shape locks:

  - Layer-2 trampoline owns arena writes via `cel_alloc(len)`.
    The wasm caller never sees the formatted bytes pre-cel-alloc.
  - Trailing-zero suppression follows proto canonical form
    ("3600s" not "3600.000s") — cel-cpp parity (Probe E).
  - Same 2-arg slot-out shape as §52 — Layer-2 dispatch tells the
    parse and format apart by name only.

Codegen call-site: `expr_lower.cc::EmitGeneralCall` once
`duration_to_string` graduates from `kExplicitlyUnimplementedIds`.

Authored alongside the WAT file at
`doc/implementation-plan/rewrite/wat/53_duration_format.wat`.

---

## 54. Timestamp accessor with IANA TZ — `ts.getFullYear("America/Los_Angeles")` (M7B.E)

The 10-into-1 dispatch trampoline for with-TZ accessors.  Per
`m7b-duration-timestamp.md` §4.3, with-TZ accessors fold to ONE
host import (`cel_host.cel_timestamp_tz_accessor`) parameterised by
an `accessor_kind` u32 enum — keeps the cel_host ABI surface
bounded.  The IANA tzdata database lives on the host
(`absl::TimeZone::Load`); pure wasm has no way to evaluate
"America/Los_Angeles" without bundling tzdata.

`accessor_kind` enum (matches plan §4.3 / §5 M7B.E):

| Value | Kind | Source method |
|---:|---|---|
| 0 | `kYear` | `getFullYear()` |
| 1 | `kMonth` | `getMonth()` (0-based) |
| 2 | `kDate` | `getDate()` (1-based) |
| 3 | `kDayOfMonth` | `getDayOfMonth()` (0-based) |
| 4 | `kDayOfYear` | `getDayOfYear()` (0-based) |
| 5 | `kDayOfWeek` | `getDayOfWeek()` (0=Sunday) |
| 6 | `kHours` | `getHours()` |
| 7 | `kMinutes` | `getMinutes()` |
| 8 | `kSeconds` | `getSeconds()` |
| 9 | `kMilliseconds` | `getMilliseconds()` |

This trace exercises `accessor_kind=0` (kYear) with an IANA name.
The fixed-offset shape through the same trampoline is in §55.

| Region | Offset | Contents |
|---|---|---|
| rodata | `[16, 40)` | `CelValue{CEL_TIMESTAMP, ts={1234567890, 0}}` |
| rodata | `[40, 64)` | `CelValue{CEL_STRING, span={ptr=64, len=19}}` |
| rodata | `[64, 83)` | `"America/Los_Angeles"` |
| workspace | `[88, 112)` | out_slot → `{CEL_INT, i=2009}` |
| arena | `[112, ...)` | bump |

```wat
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel_host" "cel_timestamp_tz_accessor"
          (func $cel_timestamp_tz_accessor (param i32 i32 i32 i32)))
  ;; rodata @ 16/40/64 elided.
  (func $eval (result i32)
    (call $cel_reset (i32.const 112) (i32.const 131072))
    (call $cel_timestamp_tz_accessor
          (i32.const 88)        ;; out_slot
          (i32.const 16)        ;; ts_slot
          (i32.const 40)        ;; tz_slot
          (i32.const 0))        ;; accessor_kind = kYear
    (i32.const 88))
  (export "eval" (func $eval))
  (export "memory" (memory 0)))
```

Invariants the shape locks:

  - 4-arg slot-out form `(out, ts, tz, kind)` — same wire shape as
    the M2 `cel_host.cel_get_field` trampoline.
  - 10 with-TZ overloads share one host import; `accessor_kind` is
    an OverloadTable-supplied u32 constant per call site, not a
    runtime input.
  - tz string is a regular CEL_STRING — no special encoding.
  - tz load failure (unknown IANA, malformed offset) → CEL_ERROR
    at out_slot; the trampoline never traps.

Codegen call-site: `expr_lower.cc::EmitGeneralCall`; the
OverloadTable carries the per-overload `accessor_kind` constant.

Authored alongside the WAT file at
`doc/implementation-plan/rewrite/wat/54_timestamp_year_with_tz.wat`.

---

## 55. Timestamp accessor with fixed-offset TZ — `ts.getHours("+02:00")` (M7B.E)

Fixed-offset flavour of §54.  Same host trampoline, different
`accessor_kind` (6 = kHours) and tz body ("+02:00" vs IANA name).
`absl::TimeZone::Load("+02:00")` parses the offset directly without
touching tzdata, but the trampoline reads it through the same
`absl::TimeZone` handle — a single Layer-2 impl covers both
flavours.  Pinning both flavours in WATs forces the trampoline to
exercise both code paths in the cohort tests.

23:31:30Z + 02:00 = 01:31:30 next day → `getHours = 1`.  This is
the dateline-cross edge that motivates the with-TZ form at all —
the no-TZ UTC accessor on the same timestamp gives `getHours = 23`.

| Region | Offset | Contents |
|---|---|---|
| rodata | `[16, 40)` | `CelValue{CEL_TIMESTAMP, ts={1234567890, 0}}` |
| rodata | `[40, 64)` | `CelValue{CEL_STRING, span={ptr=64, len=6}}` |
| rodata | `[64, 70)` | `"+02:00"` |
| workspace | `[72, 96)` | out_slot → `{CEL_INT, i=1}` |
| arena | `[96, ...)` | bump |

```wat
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel_host" "cel_timestamp_tz_accessor"
          (func $cel_timestamp_tz_accessor (param i32 i32 i32 i32)))
  ;; rodata elided.
  (func $eval (result i32)
    (call $cel_reset (i32.const 96) (i32.const 131072))
    (call $cel_timestamp_tz_accessor
          (i32.const 72)        ;; out_slot
          (i32.const 16)        ;; ts_slot
          (i32.const 40)        ;; tz_slot ("+02:00")
          (i32.const 6))        ;; accessor_kind = kHours
    (i32.const 72))
  (export "eval" (func $eval))
  (export "memory" (memory 0)))
```

Invariants the shape locks:

  - Same trampoline as §54 — fixed-offset tz strings flow through
    the identical wire shape.
  - `accessor_kind` constant differs per overload; codegen reads it
    from the OverloadTable, not from operand-level runtime data.
  - "+02:00" form goes through `absl::TimeZone::Load` but bypasses
    the tzdata lookup — Layer-2 should not branch on tz format,
    absl handles both.

Codegen call-site: same as §54 (`expr_lower.cc::EmitGeneralCall`);
this WAT just locks the second tz-string shape so the trampoline
is exercised on both absl branches.

Authored alongside the WAT file at
`doc/implementation-plan/rewrite/wat/55_timestamp_hours_fixed_offset.wat`.

---

## 56 — google.protobuf.Int32Value{value: 5} (M8.C wrapper tail-unwrap)

`m8-wrapper-types.md` §4 Arm C: wrapper-FQN struct literals lower as
the M7.A/B `cel_make_message` + `cel_set_field` pair, then a
**tail-call** to a new host trampoline `cel_wkt_unwrap_wrapper` that
peels the freshly-constructed wrapper message back to its inner
scalar.  Direct clone of m7b's `cel_wkt_unwrap_time` shape (see §51)
with one extra arg.

```wat
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel_host" "cel_make_message"
          (func $cel_make_message (param i32 i32)))
  (import "cel_host" "cel_set_field"
          (func $cel_set_field (param i32 i32 i32)))
  (import "cel_host" "cel_wkt_unwrap_wrapper"
          (func $cel_wkt_unwrap_wrapper (param i32 i32 i32)))

  (data (i32.const 40)
        "\02\00\00\00\00\00\00\00\05\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $cel_reset (i32.const 64) (i32.const 131072))
    (call $cel_make_message (i32.const 1) (i32.const 16))
    (call $cel_set_field (i32.const 16) (i32.const 1) (i32.const 40))
    (call $cel_wkt_unwrap_wrapper (i32.const 16) (i32.const 16) (i32.const 2))
    (i32.const 16))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
```

Memory layout:

  - `[ 0, 16)` reserved + arena cursor/limit
  - `[16, 40)` workspace slot for the kStructExpr — written THREE
    times: cel_make_message stamps `{CEL_MESSAGE, externref}`,
    cel_set_field mutates the proto behind the externref (slot
    bytes unchanged), cel_wkt_unwrap_wrapper overwrites the slot
    with the peeled scalar `{CEL_INT, payload.i=5}`.
  - `[40, 64)` rodata: literal `5` CelValue (operand to set_field).
  - `[64, mem_size)` bump arena — untouched (the peeled scalar is
    by-value in the CelValue payload; the wrapper proto lived in
    the ExternrefTable, not in linear memory).

Why this is the **tail** of kStructExpr, not a separate AST node:
`compiler_v2/ir/typed_ast.cc:56` maps every `wrapper(XX)` type to
`Repr::kXX`, so every consumer of a wrapper-typed expression
(equality, arithmetic-guard, list-element-assignment) already
expects a scalar slot.  Without the tail-unwrap, kStructExpr would
leave a `CEL_MESSAGE` slot at `out_slot` and the next pass would
either CHECK-fail or silently miscompile.  See
`m8-wrapper-types.md` §4.2 for the full trace of
`Int32Value{value: 1} == 1`.

`cel_wkt_unwrap_wrapper` arg layout:

  - `out_slot`     — offset of the 24B CelValue cell to overwrite
    with the peeled scalar.  In the tail-call shape this is the
    same offset as `msg_slot` (matches m7b's
    `MaybeEmitWktUnwrapTailCall` argument pattern).
  - `msg_slot`     — offset of the just-constructed wrapper's
    `{CEL_MESSAGE, externref}` CelValue.
  - `wrapper_kind` — i32 CelKind tag (BOOL=1, INT=2, UINT=3,
    DOUBLE=4, STRING=5, BYTES=6) — codegen knows the kind
    statically from the wrapper FQN, so the Layer-2 impl avoids
    a per-call descriptor walk.  Int32/Int64 both collapse onto
    `CEL_INT=2`; UInt32/UInt64 onto `CEL_UINT=3`; Float/Double
    onto `CEL_DOUBLE=4`, matching CEL's value algebra.

Layer-2 trampoline (`CelWktUnwrapWrapperImpl`, to land in
`compiler_v2/api/internal/cel_host.cc` alongside
`CelWktUnwrapTimeImpl`):

  1. Read CelValue at `msg_slot`.  3VL absorption — if CEL_ERROR
     or CEL_UNKNOWN, propagate to `out_slot` and return.
  2. Expect CEL_MESSAGE; mismatch → `{CEL_ERROR,
     CEL_ERR_TYPE_MISMATCH}` (defence in depth; codegen only
     emits the call when the FQN is a wrapper).
  3. Look up the externref → `HostMessageBacking::message()`.
  4. Cross-check `descriptor()->full_name()` matches the FQN
     implied by `wrapper_kind` (one switch); mismatch → ERROR.
  5. Reflection-read field number 1 (`value`); write the matching
     scalar `CelValue` (`CEL_INT(5)` for the Int32Value case) to
     `out_slot`.

Cross-ref to the m7b analog (§51 + `expr_lower.h:72-84`): the
codegen seam is identical (`MaybeEmitWktUnwrapTailCall` in
`compiler_v2/codegen/expr_lower.cc:439-451`), extended to dispatch
on wrapper FQNs in addition to Timestamp/Duration and to thread the
`wrapper_kind` enum through the 3rd call arg.

---

## 60. Comprehension exists over list literal — `[1, 2, 3].exists(v, v > 0)` (M5 Slices A–C)

The milestone-final WAT for `exists` over a list source.  Supersedes
the M5-prototype `05_comprehension_exists.wat` (which predated this
milestone and locked the design-claim doctrine — "uniform `kIdent`
load: `v` IS `iter_off`, no per-iter memcpy").  WAT 60 keeps that
doctrine and pins it to real exported runtime symbols
(`cel_list_create` / `cel_list_set` for the literal,
`cel_int_gt_at_vv` for the predicate, `cel_or` for the accu
combine).  See `m5-comprehensions-design.md` §5 (Shape A) and §6
(macro 2 recipe), `m5-comprehensions-followon.md` §3.1 (canonical
lowering shape).

Layout: rodata [16, 136) for the three list-elem CelValues, the
accu_init `false`, and the predicate rhs `0`.  Workspace
[136, 208) for the iter_range list slot, accu slot, and per-iter
step_out scratch.  arena_base = 208.

Invariants:
  - **Loop-cond peephole**: read `accu.payload.b` directly at
    `accu_slot + 8` to drive `br_if $exit`.  Valid because type
    analysis proves accu can only hold a CEL_BOOL.
  - **Same-slot aliasing into `cel_or`** is well-defined per
    `cel_3vl.h`'s contract (the helper never reads `a`/`b` after
    writing `out`).  Codegen exploits this — no separate
    "accu_next" slot is allocated.
  - **Iter pointer reads the list payload from the `ArenaListHeader`'s
    `elements_offset` field**, not a fresh slot — bit-identical to
    the strategy `cel_list_at_arena` uses.

**Runnable today.**

---

## 61. Comprehension all over list literal — `[1, 2, 3].all(v, v > 0)` (M5 Slices A–C)

Sibling to 60.  The codegen arm is *generic* — only `accu_init`'s
constant bytes and `loop_step`'s helper choice differ between
`exists` and `all`.  Reading 60 and 61 side-by-side is the
clearest demonstration that the comprehension shape is one arm,
not two.

Differences from 60:
  - `accu_init` rodata is `{CEL_BOOL, true}` instead of false.
  - `loop_cond` peephole is `br_if $exit (i32.eqz (accu.b))` —
    `all` exits when accu becomes strictly false, not strictly true.
  - `loop_step` calls `cel_and` (M5.G) instead of `cel_or`.

The `i32.eqz` wrapper is the only structural difference; everything
else is bit-identical, including the same-slot aliasing trick into
`cel_and`.  See `m5-comprehensions-design.md` §9.4 for the
error-short-circuit subtlety: peephole-on-bool-payload is correct
only when type analysis proves accu can hold only CEL_BOOL.  The
general path evaluates the full `@not_strictly_false` expression
when accu's static type is wider (e.g. dyn-typed accu).

**Runnable today.**

---

## 62. Comprehension map over list literal — `[1, 2, 3].map(v, v * 2)` (M5 Slice D)

`map(v, t)` produces a new list — the per-iter accumulator grows
by one element.  cel-cpp's macro emits `accu + [t]` for
`loop_step`; a naive lowering would compile this to
`cel_list_concat(accu, accu, [t])` — O(N²) total work over N
iterations.  Slice D introduces `cel_list_append_at(list_slot,
value_slot)` with geometric (2×) growth → amortised O(N), and
codegen pattern-matches the `kCall(_+_, accu_ref,
kCreateList([single_elem]))` IR shape to emit the append-at
directly.

The empty-list `accu_init` is `cel_list_create(accu_slot, 0)`,
which allocates a bare `ArenaListHeader` with `count = capacity =
elements_offset = 0`.  The first `cel_list_append_at` call grows
the elements run on demand.  Codegen could pre-size to
`cel_list_size(iter_range)` for the unconditional-map case, but
that specialises per-macro and was rejected in
`m5-comprehensions-followon.md` §3.6.

**Depends on Slice D** — `cel_list_append_at` does not yet exist as
a `cel_runtime.wasm` export.  `wasm-as` validates the WAT today;
`wat_runner_test` skips this fixture (tag = manual) until Slice D
ships the runtime helper and adds the name to `kRuntimeExports`.

---

## 63. Comprehension filter over list literal — `[1, 2, 3].filter(v, v != 2)` (M5 Slice D)

`filter(v, p)` is `map`'s sibling: the accumulator is a list, but
the append is *conditional*.  cel-cpp's macro emits a ternary
`p ? accu + [v] : accu`; codegen pattern-detects this shape and
emits a wasm `if`-block around the `cel_list_append_at` call,
bypassing the general ternary lowering.

Key shape distinction vs. WAT 62: `filter` appends `v` itself (the
iter element, in-place pointer) — no `step_out` workspace for the
appended value is needed because we never transform `v`.  The only
per-iter workspace is the predicate result slot, which drives the
`if`.

Same dependency as 62: `cel_list_append_at` lands in Slice D.
Tagged `manual` until then.

---

## 64. Comprehension exists over map literal — `{1: "a", 2: "b"}.exists(k, k > 1)` (M5 Slice E)

The first WAT with **map source**.  cel-cpp's evaluator iterates
the map's keys in insertion order via in-place iteration (no keys
list materialised — see `m5-comprehensions-design.md` §7.4).  We
follow Option β: three new runtime helpers shape the iter —
`cel_map_iter_init(map_slot) → handle`, `cel_map_iter_next(handle) →
0|1`, `cel_map_iter_key_at(out, handle)`.

Per-iter, codegen calls `cel_map_iter_next` to advance / check;
on `1` it calls `cel_map_iter_key_at` to materialise the current
key into the iter_var workspace slot (24-byte copy from the
entries run).  `k` then resolves via the uniform kIdent arm to
`(i32.const <key_slot>)`.

**Key contrast with list iteration**: map keys are polymorphic
(may be CEL_INT / CEL_UINT / CEL_BOOL / CEL_STRING), so the
in-place "moving pointer" trick that works for list iteration
(WAT 60) doesn't apply — we need a stable workspace slot the
kIdent arm can resolve to.  The runtime helper does the memcpy.

**Depends on Slice E** — three runtime helpers (`cel_map_iter_init`,
`cel_map_iter_next`, `cel_map_iter_key_at`) plus the Slice F-overlap
helper `cel_map_iter_value_at` need to ship in `cel_map.c` and be
added to `kRuntimeExports`.  Tagged `manual` until then.

---

## 65. cel.bind degenerate — `cel.bind(x, 5, x + 1)` (M5 Slice I)

The Shape-C codegen optimisation.  `cel.bind` expands (via cel-cpp's
`bindings_ext`) into a `kComprehensionExpr` with `iter_range = []`
and `loop_cond = false` — a comprehension whose loop body never
runs.  Generic Shape A would produce the correct answer (loop
runs zero times, accu retains its init, result evaluates with
accu_var bound to accu_init), but it pays a ~5-wasm-op prologue
per bind.  Shape C detects the degenerate shape at codegen entry
and emits the eval directly with no loop framing.

Codegen detection criteria (`m5-comprehensions-design.md` §5,
Shape C):
  - `iter_range` is an empty-list literal.
  - `loop_cond` is the constant `false`.
  - `iter_var` is the cel-cpp sentinel name (typically `"#unused"`).

When matched: push a scope binding `accu_var → accu_init's slot`,
evaluate `result`, pop.  No `block`/`loop`/`br_if`.  Per cel-cpp
benchmarks this is ~30% faster on bind-heavy programs.
**Correctness-wise Shape A is a strict superset** — the gate is
purely a perf optimisation, no runtime surface change.

No new runtime helpers.  **Runnable today** (assuming Slice I
ships the parser registration so the macro reaches codegen at
all — until then `cel.bind` won't parse and the source expression
fails before lowering).

---

## 66. Nested comprehension with shadowing — `[1].exists(y, [0].exists(y, y == 0))` (M5 Slices A–C)

Structural test for **nested scopes** and **name shadowing**.  Two
stacked `kComprehensionExpr`s, both binding the same name `y` (and
both using `@result` for accu_var).  The inner binding wins for
the duration of the inner body; the outer binding becomes visible
again after the inner exits.

Invariants this WAT locks:

  1. **Independent wasm locals per nesting level.**  `$iter_off_o`
     and `$iter_off_i` are different locals; LayoutPass allocates
     them disjointly per `ComprehensionFrame` with a free-cursor
     snapshot per frame.
  2. **Independent accu / step_out / list-header workspace slots.**
     The outer comprehension's accu lives at one slot, the inner
     at another.  After the inner pops, references to outer
     `@result` resolve to the outer slot.
  3. **kIdent for `y` inside the inner body resolves to the inner
     iter_off local.**  ScopeResolver walks the stack
     inner-to-outer; the shadowing is automatic, no error
     reported (matches cel-cpp's behaviour per
     `m5-comprehensions-design.md` §3.7).
  4. **Inner comprehension's result is just "the inner accu slot
     offset"**, which the outer body's `cel_or` consumes via a
     `step_out` memcpy.  Codegen treats the inner comprehension
     as a regular sub-expression whose result lives at a known
     slot.

No new runtime helpers.  **Runnable today** (once Slices A–C land
the ResolvePass / LayoutPass / expr_lower body).

---

## 67. Three-arg list exists — `[10, 20, 30].exists(i, v, v == 20 && i == 1)` (M5 Slice F)

The first WAT for the **two-iter-var** comprehension shape.
cel-cpp's `kComprehensionExpr` natively carries `iter_var` AND
`iter_var2`; the evaluator dispatches Evaluate1 vs Evaluate2 at
runtime based on whether `iter_var2` is set.  Binding semantics
(per `m5-comprehensions-followon.md` §3.8 and cel-cpp's
`comprehension_step.cc::Evaluate2`):

  - **List source, two iter_vars**: `iter_var = i` (an int
    counter, current index), `iter_var2 = v` (the per-iter list
    element, same in-place pointer as the single-var case).
  - **Map source, two iter_vars**: `iter_var = k` (the key),
    `iter_var2 = v` (the value).  Covered by a follow-up WAT
    (Slice F + Slice E overlap; not in this initial M5 cut).

Codegen surface for the list two-var case:

  - **Index counter** is a wasm local (`$index`) bumped by 1 per
    iter.  Once per iter, before lowering `loop_cond` /
    `loop_step`, we WRITE the int into a workspace CelValue
    (`{kind=CEL_INT, payload.i=<index>}`).  The kIdent arm for
    `i` lowers to `(i32.const <index_slot>)`.
  - **`v` IS `iter_off`** — same uniform load as single-var (WAT 60).
  - **No new runtime helper.**  The index-as-CelValue write is six
    inline wasm instructions; no helper call.

**Runnable today** (once Slice F's `iter_var2` plumbing in
ResolvePass / LayoutPass / expr_lower lands).

---

## M13 P1 — Foreign-wasm custom fn link contract (`m13_p1_caller.wat` + `m13_p1_rules_stub.wat`) (M13 Probe 1, 2026-05-21)

Probe-stage WAT pair validating the cross-module wasm-link
contract for `Foreign`-aliased custom CEL functions.  Companion
to [m13-probes.md §"Probe 1"](m13-probes.md).

**CEL source modelled**

```
user.allow("/admin")    where  bool rules.allow(this proto(acme.User) u, string r);
```

**Two WAT files, one shared memory**

  - `m13_p1_caller.wat` — what celwasmc emits for the expression.
    Imports `(cel memory)` + `(rules allow_message_acme_User_string)`.
    Pre-stages a CelMessage at [16,40), a CelString at [40,64), the
    `"/admin"` raw bytes at [88,94), and an out_slot at [64,88).
    Calls the import passing `(out=64, user=16, resource=40)` and
    returns 64 so the host can decode the bool.
  - `m13_p1_rules_stub.wat` — stand-in for what TinyGo / Rust /
    AssemblyScript would produce.  Imports `(cel memory)`; exports
    `allow_message_acme_User_string (param i32 i32 i32)`.  Always
    writes a `CEL_BOOL = true` CelValue to *out_slot.

**Memory layout (caller)**

```
[ 0,  8)   reserved null sentinel
[ 8, 16)   reserved (arena cursor/limit slots — unused; no allocs here)
[16, 40)   args[0]  CelValue{ kind=CEL_MESSAGE(10), payload.msg_slot=1 }
[40, 64)   args[1]  CelValue{ kind=CEL_STRING(5),   payload.s={88,6} }
[64, 88)   out_slot — foreign fn writes its CelValue result here
[88, 94)   "/admin" raw bytes
```

**Link-time invariant**

The wasmtime Linker resolves the caller's `rules.allow_message_acme_User_string`
import against the stub's export of the same name.  No cel-cpp,
no codegen — pure wasmtime wiring.  Host harness in
`compiler_v2/probes/m13_custom_fns/m13_p1_test.cc`:

  1. allocate `cel.memory` (2 pages, host-owned)
  2. instantiate stub with `cel.memory` bound
  3. extract stub's `allow_*` export
  4. instantiate caller with `cel.memory` + `rules.allow_*` bound
  5. call `eval()`, get the out_slot offset
  6. memcpy 24 bytes from memory; assert kind=CEL_BOOL, payload.b=1

**Why both WATs share `cel.memory` from the host**

The simplest model that supports cross-language modules.  Both
modules import the memory; the host owns it.  When Probe 2 swaps
the stub for TinyGo-built wasm, the TinyGo side imports the same
`cel.memory` — drop-in.  Component-model isolation is the future
story but not what TinyGo / AS support cleanly today (§10.5 of
m13-custom-fns.md).

**Runnable today**

```
bazel test //compiler_v2/probes/m13_custom_fns:m13_p1_test --test_output=all
```

Passed in 6ms on first commit.

---

## M16.1 — Variadic min over list — `math.least([3, 1, 2])` (M16 Slice 0)

File: `wat/m16_math_min_list.wat`.  Status: assembles (298 B); runs
through `wat_runner` once `cel_math_min_list_at_v` lands (Slice C).

The `math.least` / `math.greatest` parser macros collapse a list
literal — and any 3+ scalar args — into a single `kListExpr` arg, so
the runtime list form is one kernel:

```
cel.cel_math_min_list_at_v(out_slot, list_slot)   — i32, i32 → ()
```

Memory map: rodata CelValues for the three int elements at 16/40/64;
`cel_list_create(88, 3)` reserves the `ArenaListHeader` (16 B) + 3×24 B
elements run in the bump arena and writes the `CEL_LIST_ARENA` value at
slot 88; three `cel_list_append_at` calls populate it; the kernel folds
to slot 112.

ABI frozen by this trace:

  - Input: one `CEL_LIST_ARENA` (kind=7) CelValue at `list_slot`;
    `payload.arena_list.header_ptr` → `{count, capacity,
    elements_offset}`; elements are a contiguous `count*24 B` CelValue
    run.
  - Output: one CelValue at `out_slot` = the min element.
  - The kernel reads each element's `kind` (offset 0) and folds with
    the cross-type numeric compare ladder, so a mixed int/uint/double
    list yields a dyn-typed result carrying the winning element's
    runtime kind.  Same-kind int list used here for a layout-clear
    baseline; cross-type fold is a kernel unit-test concern, not an
    ABI-shape one.
  - Empty list cannot reach the kernel (macro rejects empty list
    literals at parse time).

`cel_math_max_list_at_v` is byte-identical in shape; only the fold
direction differs.

## M16.2 — Bit shift — `math.bitShiftLeft(1, 2)` (M16 Slice 0)

File: `wat/m16_math_bit_shift.wat`.  Status: assembles (201 B); runs
through `wat_runner` once `cel_math_bit_shift_left_at_vv` lands
(Slice B).

`bitShiftLeft` / `bitShiftRight` are plain global `math.<name>` calls
(no macro); checker resolves `math_bitShiftLeft_{int,uint}_int`.
Runtime surface:

```
cel.cel_math_bit_shift_left_at_vv(out_slot, x_slot, n_slot)
    — i32, i32, i32 → ()
```

Memory map: `x = {CEL_INT, 1}` at 16, `n = {CEL_INT, 2}` at 40, result
at 64.  Decoded result `{CEL_INT, 4}`.

ABI frozen by this trace:

  - `x_slot` value kind = int or uint (result keeps x's kind);
    `n_slot` always CEL_INT.
  - Output CelValue at `out_slot`, same kind as `x_slot`.
  - Shift semantics are spec-defined (math_ext.textproto), NOT C UB:
    negative count and count ≥ 64 are explicit kernel cases (unit-test
    matrix).  This trace exercises the nominal in-range case to lock
    the slot/kind shape.

`bitShiftRight` / `bitAnd` / `bitOr` / `bitXor` share this 3-arg
`_at_vv` shape; `bitNot` is the unary `_at_v` sibling.
## M14 — CEL optionals ABI traces

Six WAT files lock the runtime ABI for the M14 optionals work
(`doc/implementation-plan/rewrite/m14-optionals.md`).  Named with
the `m14_` prefix (no numeric ordinal) because they are authored
as a coherent batch, not a per-slice continuation of the 0-67
line.  Every byte these traces lock must round-trip against the
production kernels; a codegen arm that diverges from these WATs
is a regression by definition.

All six assemble cleanly via `wasm-as`.  `wat_runner_test.cc`
runs six `WatRunnerM14Test.*` cases — **byte-exact**, not smoke.
Each test instantiates `cel_runtime.wasm` (sharing its
shared-memory export with the expr module via
`PreprocessWatMemoryImport`), calls `arena_init`, runs `$eval`,
then decodes the post-eval workspace bytes as a `CelValue` and
(for optional-valued results) dereferences `payload.opt` into a
local `WatRunnerOptionalCell` mirror.  Each test asserts the
exact CelValue / cell contents the WAT header documents (kind,
inner kind, payload bytes, `present` flag, span lengths, etc.).

The harness changes from Slice A that make this work:
`RegisterPendingM14Imports` was deleted; the eight
`cel_optional_*` kernels are now real production exports in
`kRuntimeExports`; the expr module imports the runtime's
exported shared memory rather than a host-allocated `cel.memory`
(matched by a text-substitution rewrite of
`(import "cel" "memory" (memory N))` in
`PreprocessWatMemoryImport`); and the linker now binds the
`wasi_snapshot_preview1` surface that the runtime's
abseil+cctz transitive deps keep alive.  These changes are
discussed in m14-optionals.md §4 Slice A "Plan-vs-execution
delta 2."

The slice locks these layout decisions (the alternatives in
m14-optionals.md §3 were evaluated and rejected — see each WAT's
header for rationale):

  - **OptionalCell is 32 bytes**, arena-allocated per
    `optional.of` / `optional.none`.  One `CelKind` value
    (`CEL_OPTIONAL = 14`); no tag-encoded `SOME` / `NONE`
    variant, no shared-static-None sentinel.
  - **`cel_select_optional_field_at_vv` is a single kernel
    serving both `.?field` and Select-on-optional**.  Per probe
    Q11 the checker leaves Select-on-optional as `kSelectExpr`
    with the result type promoted to `optional<T>`; codegen
    routes both AST shapes through the same runtime export.
  - **Receiver-form member overloads flatten at codegen time**.
    `hasValue` / `value` / `or` / `orValue` ride the existing
    `EmitGeneralCall` receiver-flatten in M5.F; their kernel
    signatures are post-flatten `(out_slot, opt_slot, ...)`.

---

## M14.1.  `optional.of(1)` — OptionalCell layout lock

File: `doc/implementation-plan/rewrite/wat/m14_optional_of_int.wat`

The simplest possible optional construction.  Locks the
OptionalCell layout (32 bytes: `u32 present`, `u32 _pad`,
24-byte `CelValue inner`) and the `cel_optional_of_at_v(out, v)`
kernel ABI.

Memory layout (`mem_size = 131072`):

```
[ 0, 16)   reserved null + arena scaffolding
[16, 40)   rodata: CelValue{CEL_INT, i=1}
[40, 64)   workspace: out_slot — receives the CEL_OPTIONAL
[64, ...)  bump arena.  arena_alloc(32) lands the OptionalCell:
             [64, 68)  cell.present = 1 (Some)
             [68, 72)  cell._pad    = 0
             [72, 96)  cell.inner   = {CEL_INT, i=1}
```

Slice A target — expected post-eval CelValue at offset 40:
`{kind=CEL_OPTIONAL(14), payload.opt=64}`.

Why no tag-encoded `SOME = 15` / `NONE = 16` variant
(rejected from §3.1):
  - Every polymorphic switch (`cel_equals_at_vv`, `cel_log`
    pretty-printer, `type()` resolution, future codegen
    dispatch) would need TWO arms per polymorphic check
    instead of one.  Two arms × ~12 switches = 24 extra
    edges in a sprawl pattern that's easy to forget one of.
  - Saves ~8 B per cell, but the 24-byte inner CelValue
    dominates the footprint — relative overhead is small.
  - Tag-encoded values can't carry per-instance "present"
    state separate from kind, so `optional.or(...)` /
    `orValue(...)` would need a more complex per-kernel
    handshake.  One-cell-one-kind keeps every kernel's
    `kind != CEL_OPTIONAL ⇒ TYPE_MISMATCH` check uniform.

Why no shared-static-None sentinel **yet**:
  - The optimisation saves an arena_alloc per
    `optional.none()` call.  cel-cpp itself does this at the
    value layer
    (`common/values/optional_value.cc:415-418`) — every None
    is the same static object.
  - Slice 0 doesn't ship the optimisation, but it DOES lock
    the **OptionalCell immutability contract** that lets the
    optimisation layer in cleanly later (see the WAT 1 header
    section "OptionalCell immutability contract"): kernels
    that read a cell via `opt_slot.payload.opt` MUST NOT write
    through that offset; all writes go through `out_slot` and
    fresh arena_alloc'd cells.
  - With that contract in place, a future runtime-init shim
    can publish a single static `OptionalCell{present=0}` at
    a fixed offset in the reserved region and have
    `cel_optional_none_at` return that offset unconditionally
    — zero per-kernel branches, zero per-call arena_alloc for
    None.  The ABI is forward-compatible.

**Instantiates today, doesn't compute today** — the kernel
doesn't exist in `cel_runtime.wasm` yet; `RegisterPendingM14Imports`
in `wat_runner.cc` binds a no-op trampoline so the WAT's import
resolves and `$eval` runs without trapping.  The trampoline
writes nothing, so the spec values above are not produced —
they're the assertions Slice A's tests will add once the real
kernel lands.

---

## M14.2.  `optional.of(1).hasValue()` — receiver-form kCall ABI lock

File: `doc/implementation-plan/rewrite/wat/m14_optional_has_value.wat`

Locks two surfaces:
  1. The present-flag read (a 4-byte u32 load from cell offset 0,
     reinterpreted as a CEL_BOOL).
  2. The receiver-form kCall codegen pattern.  Per probe Q12, the
     checker emits `kCallExpr{function="hasValue", target=opt,
     args=[]}` — receiver in `target`, NOT in `args[0]`.  Codegen's
     `EmitGeneralCall` flattens target → arg slot 0 before the
     wasm call; the runtime kernel signature is the post-flatten
     1-arg shape `(out_slot, opt_slot)`.

Memory layout:

```
[ 0, 16)   reserved
[16, 40)   rodata: CelValue{CEL_INT, i=1}
[40, 64)   workspace: optional.of(1) — CEL_OPTIONAL
[64, 88)   workspace: hasValue result — CEL_BOOL
[88, ...)  bump arena.  Step 1's optional.of allocates a 32-byte
           OptionalCell here at [88, 120).
```

Slice A target — expected post-eval CelValue at offset 64:
`{kind=CEL_BOOL(1), payload.b=1}` (true — the optional is Some).

The companion `.value()` accessor (overload `optional_value`)
shares the WAT's receiver-flatten shape — the kernel just reads
24 bytes of `cell.inner` instead of 4 bytes of `cell.present`,
and produces `CEL_ERROR{INVALID_ARGUMENT}` on None.  No separate
WAT needed for `value` because the ABI is the same.

---

## M14.3.  `optional.of({'c': 'v'}).c` — shared select-field kernel

File: `doc/implementation-plan/rewrite/wat/m14_optional_select_field.wat`

Locks the `cel_select_optional_field_at_vv` kernel ABI and the
**single-kernel-for-both-paths** decision: `.?field` (Call(`_?._`))
and Select-on-optional-operand (kSelectExpr promoted to
optional<T>) both route to the same runtime export.  Per probe
Q11, the checker does NOT rewrite Select-on-optional into
`_?._` — it leaves the kSelectExpr alone and promotes the
result type.  Codegen's `LowerSelect` gets a new branch:
when the operand's annotation says optional-typed, route to
the same kernel `.?` uses.  Two codegen entry points
converging on one runtime ABI — cheaper than two parallel
kernels with subtly-different implementations.

Kernel ABI:
```
cel.cel_select_optional_field_at_vv(out_slot, src_slot, key_slot)
```
where `src_slot` may be CEL_OPTIONAL (this WAT's path) OR
CEL_MAP_ARENA / CEL_MAP_HOST / CEL_MESSAGE / list types (the
`.?field` and `.?[key]` paths, exercised by Slice B's overload
seeds + e2e tests).

Memory layout:

```
[ 0, 16)   reserved
[16, 40)   rodata: key 'c' CelValue (CEL_STRING, ptr=256, len=1)
[40, 64)   rodata: value 'v' CelValue (CEL_STRING, ptr=257, len=1)
[64, 88)   workspace: map {'c': 'v'} — CEL_MAP_ARENA
[88, 112)  workspace: optional.of(map) — CEL_OPTIONAL<map>
[112, 136) workspace: select(.c) result — CEL_OPTIONAL<string>
[256, 258) string bytes "cv"
[136, ...) bump arena: map header + entries + 2 OptionalCells.
```

Slice A target — expected post-eval CelValue at offset 112:
`{kind=CEL_OPTIONAL, payload.opt=<cell offset>}` with the
cell's `present=1` and `cell.inner={CEL_STRING, ptr=257, len=1}`.

Why one kernel and not two:
  - The "absent key → optional.none()" semantic is identical for
    both paths.  Path (2) just adds an outer "is the source's
    own present flag 1?" precheck.
  - One ABI freeze, one cel-cpp parity surface to test, one
    `OverloadTable::kBuiltinSeeds` row per surface overload ID
    (`select_optional_field`, `map_optindex_optional_value`,
    `optional_map_optindex_optional_value`, …) all routing to
    one runtime export.
  - `LowerSelect`'s new branch is 4 lines (detect annotation,
    emit one call to the same kernel `.?` uses).  A sibling
    kernel for the Select-on-optional path alone would either
    duplicate the absent-key logic or chain through the kernel
    we already have — neither earns its own ABI surface.

---

## M14.4.  `{'k': 1}.?missing.orValue('default')` — None propagation + unwrap

File: `doc/implementation-plan/rewrite/wat/m14_optional_chain_or_value.wat`

End-to-end None propagation through two kernels:
  - `.?missing` on a map that doesn't contain `'missing'` → the
    absent-key branch of `cel_select_optional_field_at_vv` writes
    a fresh OptionalCell with `present=0`.
  - `.orValue('default')` — `cel_optional_or_value_at_vv` reads
    `cell.present`, sees 0, and memcpys the default CelValue
    into out_slot.  Unwraps: the result is the bare string, NOT
    `optional<string>`.

Kernel ABI:
```
cel.cel_optional_or_value_at_vv(out_slot, opt_slot, default_slot)
```
Receiver-form like hasValue (M14.2) — codegen flattens
`opt.orValue(default)` from `kCallExpr{target=opt, args=[default]}`
to the 3-arg ABI above.

Memory layout:

```
[ 0, 16)   reserved
[16, 40)   rodata: key 'k'        CelValue {CEL_STRING, ptr=256, len=1}
[40, 64)   rodata: value 1        CelValue {CEL_INT, i=1}
[64, 88)   rodata: key 'missing'  CelValue {CEL_STRING, ptr=257, len=7}
[88, 112)  rodata: 'default'      CelValue {CEL_STRING, ptr=264, len=7}
[112, 136) workspace: map {'k': 1} — CEL_MAP_ARENA
[136, 160) workspace: .?missing result — CEL_OPTIONAL(None)
[160, 184) workspace: .orValue result  — CEL_STRING("default")
[256, 271) string bytes: "k" + "missing" + "default"
[184, ...) bump arena: map header + entries + None OptionalCell.
```

Slice A target — expected post-eval CelValue at offset 160:
`{kind=CEL_STRING, payload.s={ptr=264, len=7}}` — the bytes
`"default"` from rodata, surfaced verbatim by the unwrap branch
of orValue.

The `.or(other_opt)` overload (which preserves optional-ness:
`optional.none().or(optional.of(7))` → `optional.of(7)`) has the
same 3-arg ABI shape; the kernel reads the LHS cell, and on
None copies the RHS *cell* into out (preserving the OptionalCell
indirection), on Some copies the LHS *cell* into out.  Output
kind is always `CEL_OPTIONAL`.  In `orValue`'s present branch
the kernel instead copies `cell.inner` (24-byte memcpy of the
wrapped CelValue) — output kind is the inner kind.  Both kernels
fit the same `(out, opt, alt)` shape; only the present-branch
payload-write line differs.

### Short-circuit codegen requirement

cel-cpp implements `or` / `orValue` with a jump step
(`third_party/cel-cpp/eval/eval/optional_or_step.cc:60-105`).
The RHS is **not** evaluated when the LHS is Some.  Our kernel
ABI is eager — both operands evaluated into slots before the
call — which is observationally identical for pure RHS (literal,
ident, kConst) but DIFFERS for RHS that can produce errors or
unknowns:
  - `optional.of(1).orValue(1/0)` under the eager ABI evaluates
    `1/0` into a workspace slot (producing
    `{CEL_ERROR, DIVIDE_BY_ZERO}`), then the kernel sees `Some`
    on LHS and unwraps to `1`.  The error CelValue lingers in
    the workspace slot but is never returned.
  - cel-cpp under jump-step semantics never evaluates `1/0` at
    all — no error CelValue is ever constructed.

For conformance rows that observe only the final result, both
strategies agree.  For partial-eval rows that observe attribute
absorption (or future "no spurious error" assertions), they
diverge.

**Slice B (codegen) must annotate RHS impurity and emit a
short-circuit branch on impure RHS**, falling back to the eager
kernel call on pure RHS.  This is a codegen decision, not a
kernel decision — the kernel ABI stays as-locked above.  The
short-circuit branch is a separate WAT in Slice B (not landed
here).

---

---

## M14.5.  `optional.none()` — distinct 0-input None constructor

File: `doc/implementation-plan/rewrite/wat/m14_optional_none.wat`

Per cel-cpp overload table, `optional.none()` is its own
overload (`optional_none`) with a 0-input signature — NOT a
special case of `optional.of()` with a magic argument.  Slice 0
adds this WAT to lock the distinct ABI surface; the
independent review on 2026-05-21 (P1) flagged its omission as
the same "covered by symmetry" pattern that let M2 ship 29
silent GTEST_SKIPs.

Kernel ABI:
```
cel.cel_optional_none_at(out_slot) → ()
```

Suffix `_at` (no `_v`) is the 0-input cousin of `_at_v` /
`_at_vv` in the M14 family.

Memory layout:

```
[ 0, 16)   reserved
[16, 40)   workspace: out_slot — receives CEL_OPTIONAL(None)
[40, ...)  bump arena.  arena_alloc(32) → cell at [40, 72):
             [40, 44)  cell.present = 0  (None)
             [44, 48)  cell._pad    = 0
             [48, 72)  cell.inner   = zero (never read while
                                            present=0; pinned
                                            zero for cell-equality
                                            memcmp-friendliness)
```

Slice A target — expected post-eval CelValue at offset 16:
`{kind=CEL_OPTIONAL, payload.opt=40}`, with the cell at offset
40 holding `{present=0, _pad=0, inner=zeroed}`.

The OptionalCell immutability contract (WAT 1's "OptionalCell
immutability contract" section) lets a future runtime-init
shim publish a single static None cell at a fixed reserved
offset and have this kernel return that offset unconditionally
— ABI-compatible perf win.

---

## M14.6.  `optional.ofNonZeroValue(0)` — zero-predicate matrix

File: `doc/implementation-plan/rewrite/wat/m14_optional_of_non_zero.wat`

`ofNonZeroValue` wraps `optional.of` with a per-inner-kind
zero-predicate check.  Per probe Q6 + cel-cpp
`runtime/optional_types.cc:58-67`: if `v.IsZeroValue()` return
`optional.none()`, else `optional.of(v)`.

The kernel ABI shape (`(out_slot, v_slot) → ()`) is the same
as `cel_optional_of_at_v` — what this WAT locks is the
**per-kind zero-predicate matrix** (the closed set of
"what counts as zero" rules per CelKind), which IS a
new ABI surface.

Kernel ABI:
```
cel.cel_optional_of_non_zero_at_v(out_slot, v_slot) → ()
```

The matrix is documented in the WAT header (covering all 16
inner kinds including CEL_MESSAGE — which an earlier draft of
m14-optionals.md §3.4 wrongly said cel-cpp errors on;
corrected per `parsed_message_value.cc:78-86`).

Memory layout for the WAT (exercises the CEL_INT zero case):

```
[ 0, 16)   reserved
[16, 40)   rodata: CelValue{CEL_INT, i=0}
[40, 64)   workspace: out_slot — receives CEL_OPTIONAL(None)
                      because 0 is the CEL_INT zero value.
[64, ...)  bump arena: 32-byte None OptionalCell from the
                       tail-called cel_optional_none_at.
```

Slice A target — expected post-eval CelValue at offset 40:
`{kind=CEL_OPTIONAL, payload.opt=64}` with cell `{present=0, ...}`.

Slice A implements `cel_optional_of_non_zero_at_v` as a switch
on `v.kind` dispatching the matrix predicates, then tail-calling
either `cel_optional_none_at` (zero) or `cel_optional_of_at_v`
(non-zero).  Host trampolines for CEL_MAP_HOST / CEL_LIST_HOST /
CEL_MESSAGE need separate kernel additions (host_map_size,
host_list_size — already exported; host message zero-predicate is
new) — design deferred to Slice A pre-flight.

---

## M14.7.  `[?optional.of(10), ?optional.none()]` — list append-if-present

File: `doc/implementation-plan/rewrite/wat/m14_list_append_if_present.wat`

Locks the `cel_list_append_at_if_present` kernel ABI for the
`[?elem]` list-literal entry shape established by probe Q4
(`ast_shape_probe_test.cc`).  The kernel is the optional-payload
analogue of the existing predicate-gated
`cel_list_append_at_if_bool` (cel_runtime.c:345) that M5.B uses
for `filter(v, p)` lowering.

ABI:

```
cel.cel_list_append_at_if_present(list_slot, opt_value_slot) → ()
```

Semantics (matches the `_if_bool` pattern):

  - `l.kind != CEL_LIST_ARENA` → no-op (list already poisoned by
    an earlier 3VL absorption; preserve the poison).
  - `opt.kind` ∈ {CEL_ERROR, CEL_UNKNOWN} → propagate verbatim into
    `list_slot` (aborts the literal per langdef 3VL).
  - `opt.kind != CEL_OPTIONAL` → poison `list_slot` with
    `CEL_ERR_TYPE_MISMATCH`.
  - `opt = Some(v)` → `cel_list_append_at(list_slot, &v)` where the
    inner CelValue is read directly out of the OptionalCell — the
    inner's byte offset within the cell is stable until the next
    `arena_reset`, so passing that offset is byte-equivalent to
    staging the inner into a workspace scratch slot first.
  - `opt = None` → silent no-op.

Memory layout (`mem_size = 131072`):

```
[ 0, 16)   reserved null + arena scaffolding
[16, 40)   rodata: CelValue{CEL_INT, i=10}
[40, 64)   workspace: list — CEL_LIST_ARENA result
[64, 88)   workspace: optional.of(10) — Some<int>
[88, 112)  workspace: optional.none() — None
[112, ...) bump arena.  By end-of-eval: list header (16 B) +
           capacity=2 elements run (2 × 24 = 48 B) + 2 OptionalCells
           (2 × 32 B).  Header.count == 1 — only the Some appended.
```

The wat_runner test decodes the post-eval `ArenaListHeader` and
asserts `count == 1` plus `elements[0] = {CEL_INT, i=10}`.  Locks
the byte-exact relationship between the kernel ABI and the
production codegen path in `EmitKListExpr`'s optional-element
branch (Slice C codegen, §M14.7's companion).

The `_if_bool` / `_if_present` pair is the canonical "two
predicate forms, one append/insert primitive" pattern that
unifies M5.B's comprehension lowering with M14's literal
lowering.  Both predicates' 3VL surface is identical — the
only diff is the kind of slot they read (`CEL_BOOL` vs the
optional's `present` flag).

---

## M14.8.  `{?'k1': optional.of('v1'), ?'k2': optional.none()}` — map insert-if-present

File: `doc/implementation-plan/rewrite/wat/m14_map_insert_if_present.wat`

Locks `cel_map_insert_at_if_present` — the symmetric counterpart
to §M14.7 for `{?key: val}` map-literal entries (probe Q3).
Same predicate surface, same 3VL absorption, key-3VL handled by
the existing `cel_map_insert_at` on the Some path.

ABI:

```
cel.cel_map_insert_at_if_present(map_slot, key_slot, opt_value_slot) → ()
```

Memory layout (`mem_size = 131072`):

```
[ 0, 16)    reserved null + arena scaffolding
[16, 40)    rodata: key 'k1' CelValue
[40, 64)    rodata: key 'k2' CelValue
[64, 88)    rodata: value 'v1' CelValue
[88, 112)   workspace: map — CEL_MAP_ARENA result
[112, 136)  workspace: optional.of('v1') — Some<string>
[136, 160)  workspace: optional.none() — None
[160, ...)  bump arena (map header 16 B + entries run 2 × 48 B +
            2 OptionalCells 2 × 32 B).  Header.count == 1 — only
            the Some entry inserted.

String bytes at [256, 262): "k1" + "k2" + "v1".
```

The wat_runner test decodes the post-eval `ArenaMapHeader`,
asserts `count == 1`, then walks `entries[0]` to confirm the
preserved entry is `(k1, v1)`.

---

## M14.9.  `Foo{?field: opt_v}` — proto-field set-if-present

File: `doc/implementation-plan/rewrite/wat/m14_proto_set_field_if_present.wat`

Locks `cel_set_field_at_if_present` — completes the trio of
optional-payload predicate kernels with the proto-field variant.
Structurally identical to §M14.7 / §M14.8 except the inner
"actually set" step delegates to a host trampoline
(`cel_host.cel_set_field`) rather than a pure-wasm
`cel_map_insert_at` / `cel_list_append_at`.

ABI:

```
cel.cel_set_field_at_if_present(msg_slot, field_ref_id, opt_value_slot) → ()
```

Semantics:

  - `m.kind != CEL_MESSAGE` → no-op (msg already poisoned).
  - `opt.kind` ∈ {CEL_ERROR, CEL_UNKNOWN} → propagate into
    `msg_slot`.
  - `opt.kind != CEL_OPTIONAL` → poison `msg_slot` with
    `CEL_ERR_TYPE_MISMATCH`.
  - `opt = Some(v)` → `cel_host.cel_set_field(msg_slot,
    field_ref_id, &v)`.  The value slot points 8 bytes into the
    OptionalCell (past `present` + `_pad`), giving the host
    trampoline a stable 24-byte view of `cell.inner`.
  - `opt = None` → silent no-op; the proto field stays unset
    (matches proto semantics — `has(msg.field)` returns false).

Why the kernel is pure-wasm even though the inner step needs
the host: the optional unwrap (`cell->present`, `cell->inner`)
is plain memory reads.  Only the final reflection call requires
the host.  Same shape as `_if_present` for map/list — wasm-side
gate, delegate the mutation.  Design pull-in rationale in
`m14-optionals.md` §0 "Scope pull-in 2026-05-22".

Memory layout (`mem_size = 131072`):

```
[ 0, 16)   reserved null + arena scaffolding
[16, 40)   rodata: CelValue{CEL_INT, i=5}
[40, 64)   workspace: CelMessage{CEL_MESSAGE, msg_slot=1}
[64, 88)   workspace: optional.of(5) — Some<int>
[88, 112)  workspace: optional.none() — None
[112, ...) bump arena.  By end-of-eval: 2 OptionalCells
           (2 × 32 B).  Host stub records every invocation; the
           test asserts the stub fired exactly once (Some path)
           AND that the recorded args are
           (msg_slot=40, field_ref_id=42, value_slot=72) where
           value_slot is `opt_64.payload.opt + offsetof(
           OptionalCell, inner) = 40 + 8 = 48`. (The arena_alloc
           order makes the Some cell land at the first arena
           offset; the inner offset is +8 into that cell.)
```

The wat_runner test installs a `cel_host_cel_set_field_stub`
(new field on `WatRunInput`) that captures `(msg_slot,
field_ref_id, value_slot)` on each invocation.  Assertions:

  1. Stub invoked exactly once (Some path).
  2. Recorded args: `msg_slot == 40`, `field_ref_id == 42`,
     `value_slot == <inner-of-Some-cell>`.
  3. The None-path call did NOT reach the stub (proves the
     wasm-side short-circuit works).

This is the **load-bearing correctness test** for the
short-circuit no-op: a stub that runs the host call anyway
would silently violate the proto-write semantics (field gets
unset → set to zero value → `has()` returns true).  Caught at
the WAT level rather than waiting for an integration test.

---

## M17 — `encoders` extension ABI traces

Two self-hosted runtime kernels for the cel-cpp `encoders`
extension library.  Both follow the unary slot-out shape
established by `cel_string_concat_at_vv` (example 18): a
`(out_slot, arg_slot)` helper imported from the `"cel"` module
(NOT a `cel_host` trampoline — these are pure operate-on-CelValue
kernels with no descriptor-pool or externref needs), allocating
their output in the per-Eval bump arena.  Semantics, overload
ids, and the decode error message were confirmed by reading
`third_party/cel-cpp/extensions/encoders.cc` directly (see
`m17-encoders-ext.md` §2).  Both WATs assemble clean under
`wasm-as --enable-threads --enable-bulk-memory`; the end-to-end
`wat_runner` run against `cel_runtime.wasm` lands once Slice A
builds the kernels (the exports don't exist yet).

## M17.1.  `base64.encode(b'hello')` — bytes→string encode

`doc/.../wat/m17_base64_encode.wat`.  Input is a `CEL_BYTES(6)`
literal `b'hello'` (5-byte body at offset 40).  The kernel
`cel_base64_encode_at_v(out=48, bytes=16)` runs
`absl::Base64Escape` over the 5 input bytes, arena-allocates the
8-byte result, and writes `{CEL_STRING(5), span={ptr=<arena>,
len=8}}` — `"aGVsbG8="` ("hel"→`aGVs`, "lo"→`bG8=`).  Output is
ASCII base64 text so the result kind is `CEL_STRING`, not
`CEL_BYTES`, regardless of whether the input bytes were valid
UTF-8.  Wrong-kind input → `{CEL_ERROR, CEL_ERR_TYPE_MISMATCH}`;
ERROR/UNKNOWN absorbed verbatim; arena OOM →
`{CEL_ERROR, CEL_ERR_OVERFLOW}`.

## M17.2.  `base64.decode('aGVsbG8')` — string→bytes decode (unpadded)

`doc/.../wat/m17_base64_decode.wat`.  The input is deliberately
**unpadded** — `'aGVsbG8'` with no trailing `=` — because that is
the load-bearing conformance row
(`encoders_ext.textproto::decode/hello_without_padding`).
`absl::Base64Unescape` accepts missing padding, so the kernel
needs no manual re-pad; this WAT pins that into the trace so a
future absl tightening surfaces here rather than in conformance.
The kernel `cel_base64_decode_at_v(out=48, str=16)` decodes the
7-byte input, arena-allocates the 5-byte result, and writes
`{CEL_BYTES(6), span={ptr=<arena>, len=5}}` — `b'hello'`.  Output
is `CEL_BYTES` (may be non-UTF-8), read via `AsBytes()` on the
public surface.  Invalid input (`Base64Unescape` returns `false`)
→ `{CEL_ERROR, CEL_ERR_INVALID_ARGUMENT}` with message
`"invalid base64 data"` (cel-cpp `encoders.cc:51`, verbatim);
wrong-kind → `CEL_ERR_TYPE_MISMATCH`; ERROR/UNKNOWN absorbed.

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
