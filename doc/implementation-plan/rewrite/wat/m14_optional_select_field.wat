;; CEL source:  optional.of({'c': 'v'}).c                       → optional<string>('v')
;; Decl:        — (no free variables)
;;
;; M14 Slice 0 — locks the `cel_select_optional_field` kernel ABI.
;; This is the load-bearing decision of Slice 0: per probe Q11
;; (ast_shape_probe_test.cc), BOTH of these expressions converge on
;; the same runtime kernel:
;;
;;   1. `.?field` over a non-optional source:
;;        {'c': 'v'}.?c
;;      Parses as `Call("_?._", [map, "c"])` with overload_id
;;      `select_optional_field` — a regular kCall arm.
;;
;;   2. `.field` over an optional-typed source:
;;        optional.of({'c': 'v'}).c
;;      The cel-cpp checker leaves this as `kSelectExpr` and
;;      promotes the type to `optional<string>`.  Codegen detects
;;      the optional-typed operand (from `WasmAnnotations`) and
;;      routes the kSelectExpr through the same kernel that `.?`
;;      uses (`LowerSelect` new branch — m14-optionals.md §1.7).
;;
;; This WAT exercises path (2) because it stresses the kernel's
;; "I see an OptionalCell as my source — unwrap before doing the
;; lookup" branch.  Path (1)'s map-source / list-source variants
;; share the same kernel shape and ride this one's ABI freeze.
;;
;; ── New runtime kernel this slice locks ────────────────────
;;
;;   cel.cel_select_optional_field_at_vv(out_slot, src_slot, key_slot)
;;
;; Where:
;;   - src_slot  : a CelValue.  May be CEL_OPTIONAL (Select-on-
;;                 optional path) OR any indexable kind
;;                 (CEL_MAP_ARENA / CEL_MAP_HOST / CEL_MESSAGE)
;;                 for the `.?field` path.
;;   - key_slot  : a CelValue holding the field name as a
;;                 CEL_STRING.  Constant lifted to rodata at
;;                 compile time.
;;
;; Contract:
;;   1. Read src = *src_slot.
;;   2. Polymorphic dispatch on src.kind:
;;        CEL_OPTIONAL:
;;          cell = cel_value_at(src.payload.opt)
;;          if !cell.present:
;;            out_slot = optional.none() ;; (None-propagation; see WAT 4)
;;            return
;;          // Unwrap, then recurse on the inner kind.  The kernel
;;          // re-enters its own switch with src = cell.inner.
;;          src = cell.inner
;;        (fallthrough)
;;        CEL_MAP_ARENA / CEL_MAP_HOST:
;;          // Call the existing map-lookup primitive — for arena
;;          // maps this is `cel_map_lookup_arena(scratch, src,
;;          // key)`; for host maps it's the kHost trampoline
;;          // `cel_host.cel_map_lookup`.  `scratch` is a temp slot
;;          // (the kernel reuses the inner-CelValue area of the
;;          // allocated OptionalCell, see step 3).
;;          //
;;          // Absent-key contract: on miss, the existing
;;          // `cel_map_lookup_arena` writes
;;          // `{CEL_ERROR, err=CEL_ERR_NO_SUCH_KEY}` to scratch
;;          // (per cel_runtime.c::cel_map_lookup_arena, line 195).
;;          // The optional kernel reinterprets THAT specific error
;;          // code as "absent ⇒ optional.none()"; any OTHER error
;;          // (TYPE_MISMATCH, UNKNOWN on key, etc.) propagates
;;          // unchanged into out_slot.  Single special case, one
;;          // kernel — no second map-lookup primitive needed.
;;        CEL_LIST_ARENA / CEL_LIST_HOST:
;;          // Similar — call `cel_list_at_arena` / `cel_host.cel_list_at`.
;;          // Absent index (out-of-bounds) ⇒ `cel_list_at_arena`
;;          // writes `{CEL_ERROR, err=CEL_ERR_INDEX_OUT_OF_BOUNDS}`.
;;          // Reinterpret IDX_OOB as None; other errors propagate.
;;        CEL_MESSAGE:   (path (1) only — defer to Slice B; uses
;;                       cel_host.cel_get_field with the existing
;;                       FIELD_NOT_FOUND poison, reinterpreted as
;;                       None by symmetry with the map/list cases.)
;;        anything else: out_slot = {CEL_ERROR, err=CEL_ERR_TYPE_MISMATCH}.
;;   3. Wrap or none:
;;        If the inner lookup produced an absent-error (NO_SUCH_KEY /
;;        INDEX_OUT_OF_BOUNDS / FIELD_NOT_FOUND): write a fresh None
;;        OptionalCell + out_slot = {CEL_OPTIONAL, opt=cell_off}.
;;        If found: arena_alloc(32) a Some-cell, memcpy the inner
;;        result, write out_slot = {CEL_OPTIONAL, opt=cell_off}.
;;        If a non-absent error: copy as-is to out_slot.
;;   4. 3VL absorption: CEL_UNKNOWN / CEL_ERROR on src or key
;;      propagates into out_slot directly without invoking the
;;      lookup.
;;
;; Why a single kernel for both paths:
;;   - The "absent key → optional.none()" semantic IS what both
;;     paths want.  Path (2) just adds an outer "is the source
;;     itself present?" check.
;;   - One kernel = one ABI to lock, one cel-cpp parity surface to
;;     test against, one OverloadTable seed (well, technically two
;;     overload IDs — `select_optional_field` and the chained map/
;;     list variants — but they all route to one runtime export).
;;   - LowerSelect in expr_lower.cc gets a 4-line branch ("if
;;     operand-annotated optional, call the same kernel") instead
;;     of needing its own sibling kernel.
;;
;; Memory layout:
;;   [ 0, 16)   reserved null + arena scaffolding
;;   [16, 40)   rodata: map key 'c' as CelValue
;;               {kind=CEL_STRING(5), payload.s={ptr=256, len=1}}
;;   [40, 64)   rodata: map value 'v' as CelValue
;;               {kind=CEL_STRING(5), payload.s={ptr=257, len=1}}
;;   [64, 88)   workspace: map {'c': 'v'} — CEL_MAP_ARENA result
;;   [88, 112)  workspace: optional.of(map) — CEL_OPTIONAL result
;;   [112, 136) workspace: `.c` select-result — optional<string> result
;;   [136, mem_size)  bump arena.  By the end of $eval:
;;                    map header (16 B) + entries run (1 × 48 B) +
;;                    OptionalCell (32 B) + result OptionalCell (32 B).
;;
;;   String bytes:
;;     [256, 257) = "c"
;;     [257, 258) = "v"
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_map_create" (func $cel_map_create (param i32 i32)))
  (import "cel" "cel_map_insert" (func $cel_map_insert (param i32 i32 i32)))
  (import "cel" "cel_optional_of_at_v"
          (func $cel_optional_of_at_v (param i32 i32)))
  (import "cel" "cel_select_optional_field_at_vv"
          (func $cel_select_optional_field_at_vv (param i32 i32 i32)))

  ;; rodata: key 'c' CelValue.
  ;;   kind=CEL_STRING(5), _pad=0, payload.s={ptr=256(0x100), len=1}
  (data (i32.const 16)
        "\05\00\00\00"
        "\00\00\00\00"
        "\00\01\00\00"
        "\01\00\00\00"
        "\00\00\00\00\00\00\00\00")
  ;; rodata: value 'v' CelValue.
  ;;   kind=CEL_STRING(5), _pad=0, payload.s={ptr=257(0x101), len=1}
  (data (i32.const 40)
        "\05\00\00\00"
        "\00\00\00\00"
        "\01\01\00\00"
        "\01\00\00\00"
        "\00\00\00\00\00\00\00\00")
  ;; String bytes: "cv" at [256, 258).
  (data (i32.const 256) "cv")

  (func $eval (result i32)
    (call $arena_reset)
    ;; step 1: build map {'c': 'v'} at workspace slot 64.
    (call $cel_map_create (i32.const 64) (i32.const 1))
    (call $cel_map_insert
          (i32.const 64)   ;; map slot
          (i32.const 16)   ;; key slot ('c')
          (i32.const 40))  ;; value slot ('v')
    ;; step 2: wrap map in optional → workspace slot 88.
    (call $cel_optional_of_at_v (i32.const 88) (i32.const 64))
    ;; step 3: `.c` on the optional<map>.  LowerSelect routes to
    ;; the optional-aware kernel because the operand is annotated
    ;; optional-typed.  Result lands at workspace slot 112 as
    ;; optional<string>('v').
    (call $cel_select_optional_field_at_vv
          (i32.const 112)  ;; out_slot
          (i32.const 88)   ;; src_slot — OPTIONAL<MAP>
          (i32.const 16))  ;; key_slot — string 'c'
    (i32.const 112))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
