;; CEL source:  {?'k1': optional.of('v1'), ?'k2': optional.none()} → map<string, string>({'k1': 'v1'})
;; Decl:        — (no free variables)
;;
;; Locks the `cel_map_insert_at_if_present` kernel ABI for
;; `{?key: val}` map-literal entries.  Per probe Q3
;; (ast_shape_probe_test.cc), `{?key: val}` sets the per-entry
;; `optional_entry: bool` flag in `CreateStruct.entries[i]`.  The
;; codegen emits a regular eval-into-slot for the value followed by
;; the `_if_present` kernel call (instead of the unconditional
;; `cel_map_insert_at`).
;;
;; The kernel mirrors the existing `cel_map_insert_at_if_bool`
;; (cel_runtime.c:376) but the predicate is an `optional<V>` payload
;; rather than a `CelValue{CEL_BOOL}`:
;;
;;   Some(v) ⇒ unwrap inner and `cel_map_insert_at(map, key, &inner)`
;;   None    ⇒ silent no-op
;;   ERROR / UNKNOWN on opt_slot ⇒ propagate verbatim into map_slot
;;   opt_slot kind != CEL_OPTIONAL ⇒ poison map_slot with
;;     CEL_ERR_TYPE_MISMATCH
;;
;; ── New runtime kernel this slice locks ────────────────────
;;
;;   cel.cel_map_insert_at_if_present(map_slot, key_slot, opt_value_slot)
;;
;;   1. m = cel_value_at(map_slot)
;;   2. If m.kind != CEL_MAP_ARENA: return (already poisoned).
;;   3. opt = cel_value_at(opt_value_slot)
;;   4. If opt.kind == CEL_ERROR || opt.kind == CEL_UNKNOWN:
;;        *map_slot = *opt_slot ;; propagate
;;        return
;;   5. If opt.kind != CEL_OPTIONAL:
;;        poison map_slot with CEL_ERR_TYPE_MISMATCH; return
;;   6. cell = (OptionalCell*)(memory_base + opt.payload.opt)
;;   7. If !cell.present: return ;; silent no-op
;;   8. Otherwise `cel_map_insert_at(map_slot, key_slot, &cell.inner)`
;;      — key 3VL is still performed by `cel_map_insert_at` (the
;;      existing path).
;;
;; ── Memory layout ─────────────────────────────────────────────
;;
;;   [ 0, 16)    reserved null + arena scaffolding
;;   [16, 40)    rodata: key 'k1' CelValue
;;                {kind=CEL_STRING(5), payload.s={ptr=256, len=2}}
;;   [40, 64)    rodata: key 'k2' CelValue
;;                {kind=CEL_STRING(5), payload.s={ptr=258, len=2}}
;;   [64, 88)    rodata: value 'v1' CelValue
;;                {kind=CEL_STRING(5), payload.s={ptr=260, len=2}}
;;   [88, 112)   workspace: map — CEL_MAP_ARENA result
;;   [112, 136)  workspace: optional.of('v1') — Some<string>
;;   [136, 160)  workspace: optional.none() — None
;;   [160, mem_size)  bump arena.  By the end of $eval:
;;                    map header (16 B) + capacity=2 entries run
;;                    (2 × 48 = 96 B) + 2 OptionalCells (2 × 32 B).
;;                    count == 1 — only the Some entry inserted.
;;
;;   String bytes at [256, 262):
;;     [256, 258) = "k1"
;;     [258, 260) = "k2"
;;     [260, 262) = "v1"
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_map_create" (func $cel_map_create (param i32 i32)))
  (import "cel" "cel_optional_of_at_v"
          (func $cel_optional_of_at_v (param i32 i32)))
  (import "cel" "cel_optional_none_at"
          (func $cel_optional_none_at (param i32)))
  (import "cel" "cel_map_insert_at_if_present"
          (func $cel_map_insert_at_if_present (param i32 i32 i32)))

  ;; rodata: key 'k1' CelValue.
  (data (i32.const 16)
        "\05\00\00\00"
        "\00\00\00\00"
        "\00\01\00\00"
        "\02\00\00\00"
        "\00\00\00\00\00\00\00\00")
  ;; rodata: key 'k2' CelValue.
  (data (i32.const 40)
        "\05\00\00\00"
        "\00\00\00\00"
        "\02\01\00\00"
        "\02\00\00\00"
        "\00\00\00\00\00\00\00\00")
  ;; rodata: value 'v1' CelValue.
  (data (i32.const 64)
        "\05\00\00\00"
        "\00\00\00\00"
        "\04\01\00\00"
        "\02\00\00\00"
        "\00\00\00\00\00\00\00\00")
  ;; String bytes at [256, 262): "k1" + "k2" + "v1".
  (data (i32.const 256) "k1k2v1")

  (func $eval (result i32)
    (call $arena_reset)
    ;; step 1: empty arena map at workspace slot 88, capacity 2.
    (call $cel_map_create (i32.const 88) (i32.const 2))
    ;; step 2: optional.of('v1') at slot 112.
    (call $cel_optional_of_at_v (i32.const 112) (i32.const 64))
    ;; step 3: optional.none() at slot 136.
    (call $cel_optional_none_at (i32.const 136))
    ;; step 4: insert (k1, Some('v1')) — entry inserted, count=1.
    (call $cel_map_insert_at_if_present
          (i32.const 88)    ;; map_slot
          (i32.const 16)    ;; key_slot — 'k1'
          (i32.const 112))  ;; opt_value_slot — Some
    ;; step 5: insert (k2, None) — silent no-op, count stays 1.
    (call $cel_map_insert_at_if_present
          (i32.const 88)    ;; map_slot
          (i32.const 40)    ;; key_slot — 'k2'
          (i32.const 136))  ;; opt_value_slot — None
    (i32.const 88))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
