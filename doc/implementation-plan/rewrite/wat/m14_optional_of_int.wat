;; CEL source:  optional.of(1)                              → optional<int>(1)
;; Decl:        — (no free variables)
;;
;; M14 Slice 0 — locks the OptionalCell layout + the
;; `cel_optional_of_at_v` kernel ABI.  Every other M14 WAT and every
;; production runtime kernel hangs off the layout decision made here.
;;
;; ── OptionalCell layout (32 bytes, 8-byte aligned) ──────────
;;
;; struct OptionalCell {
;;   uint32_t present;        // offset  0 — 0=None, 1=Some
;;   uint32_t _pad;           // offset  4 — alignment for inner
;;   CelValue inner;          // offset  8 — 24 bytes (kind + pad + payload)
;; };
;;
;; A `CelValue{kind=CEL_OPTIONAL, payload.opt=<u32 byte offset>}`
;; points at the start of an OptionalCell in linear memory.  The
;; cell is arena-allocated by every `optional.of()` / `optional.none()`
;; call (no shared sentinel — see §3.1 design discussion in
;; `m14-optionals.md`).  Lifetime: until the next `arena_reset`.
;;
;; Why 32 bytes and not a tighter tag-encoded layout:
;;   - One CelKind value (`CEL_OPTIONAL = 14`) keeps every existing
;;     polymorphic switch (cel_equals_at_vv, cel_log printer,
;;     type() lookup) at one extra arm — not two (tag-encoded
;;     None/Some variant would force every switch to handle both).
;;   - 8 bytes of overhead per optional (4 present + 4 pad) is
;;     cheap relative to the 24-byte inner payload.  Map/list
;;     headers already pay 16 bytes of overhead each; the optional
;;     overhead is in the same ballpark.
;;   - No shared-sentinel None: the kernel writes `present=0` to
;;     its own fresh cell.  Kernels never need to handle the case
;;     "the cell I'm reading might be a shared immutable sentinel
;;     and I must not write through this pointer."  Production
;;     can layer a shared-sentinel optimisation on top later
;;     without changing the ABI.
;;
;; ── OptionalCell immutability contract ─────────────────────
;;
;; **OptionalCells are immutable after construction.**  A kernel
;; that reads an OptionalCell through `opt_slot.payload.opt`
;; may NOT write through that offset.  All output goes through
;; `out_slot` (and possibly a fresh arena_alloc for a new cell).
;;
;; This contract is the precondition for a future shared-static-
;; None optimisation: with no kernel writing through opt_slot
;; pointers, the runtime can publish a single static cell with
;; `present=0` at a fixed offset and have `cel_optional_none_at`
;; return that offset unconditionally — no per-call arena_alloc,
;; no per-kernel "is this the sentinel?" branch.  cel-cpp does
;; this at the value layer
;; (`common/values/optional_value.cc:415-418`); the same
;; optimisation lands ABI-compatibly here whenever the perf
;; pass arrives.
;;
;; Every M14 kernel respects the contract: `_of_at_v` / `_none_at`
;; / `select_optional_field_at_vv` write a fresh cell into the
;; arena AND a CelValue into out_slot; `has_value`, `value`, `or`,
;; `or_value` read cells and write out_slot only.
;;
;; ── New runtime kernel this slice locks ────────────────────
;;
;;   cel.cel_optional_of_at_v(out_slot, v_slot) → ()
;;
;; Contract:
;;   1. arena_alloc(32) — get a fresh OptionalCell at `cell_off`.
;;      OOM ⇒ out_slot = {CEL_ERROR, err=CEL_ERR_OVERFLOW}, return.
;;   2. cell[0..4]  = 1            ;; present=Some
;;   3. cell[4..8]  = 0            ;; explicit pad zero
;;   4. cell[8..32] = *v_slot      ;; memcpy 24-byte inner CelValue
;;   5. out_slot.kind        = CEL_OPTIONAL (=14)
;;   6. out_slot._pad        = 0
;;   7. out_slot.payload.opt = cell_off
;;   8. out_slot remaining payload bytes = 0
;;
;; Mirrors the M3/M4 `cel_*_create` / `_at_*` shape: out_slot first,
;; then operand slots.  No return value; caller already knows the
;; result lives at out_slot.  3VL absorption (UNKNOWN/ERROR
;; operands) is handled by `cel_optional_of_at_v` exactly the same
;; way `cel_int_add_at_vv` does (propagate operand poison into
;; out_slot).
;;
;; Memory layout:
;;   [ 0, 16)   reserved null + arena scaffolding
;;   [16, 40)   rodata: kConst `1`  {kind=CEL_INT, i=1}
;;   [40, 64)   workspace: kCall(`optional.of`) out_slot
;;   [64, mem_size)  bump arena — `cel_optional_of_at_v` allocates
;;                   the OptionalCell here.  Expected layout post-call:
;;                     [64, 68)  cell.present = 1 (Some)
;;                     [68, 72)  cell._pad    = 0
;;                     [72, 96)  cell.inner   = {CEL_INT, i=1}
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_optional_of_at_v"
          (func $cel_optional_of_at_v (param i32 i32)))

  ;; rodata: CelValue{CEL_INT(=2), _pad=0, payload.i=1, union_pad=0}
  ;; Wire layout (LE):
  ;;   u32 kind        = 0x02 0x00 0x00 0x00
  ;;   u32 _pad        = 0x00 0x00 0x00 0x00
  ;;   i64 payload.i   = 0x01 0x00 0x00 0x00 0x00 0x00 0x00 0x00
  ;;   union padding   = 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00
  (data (i32.const 16)
        "\02\00\00\00"
        "\00\00\00\00"
        "\01\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)
    ;; `optional.of(1)`:
    ;;   out_slot = 40, v_slot = 16
    (call $cel_optional_of_at_v (i32.const 40) (i32.const 16))
    (i32.const 40))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
