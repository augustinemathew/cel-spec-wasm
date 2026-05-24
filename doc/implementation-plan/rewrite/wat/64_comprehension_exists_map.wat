;; CEL source:  {1: "a", 2: "b"}.exists(k, k > 1)   → true
;; Decl:        — (no free variables)
;;
;; M5 slice (Slice E — map-key iteration).  Locks the wasm shape
;; for comprehensions over a `map(K, V)` source, single iter_var
;; binding to the map's *keys*.  Per m5-comprehensions-design.md
;; §3.5 / §7.4 we choose Option β (in-place key iteration; no
;; keys materialisation) because materialising a fresh key list
;; per comprehension is O(N) allocation for the same O(N) work.
;;
;; cel-cpp macro → kComprehensionExpr (single-iter-var on map):
;;   iter_range  = {1: "a", 2: "b"}             (kCreateMap)
;;   iter_var    = k                            (binds key only)
;;   accu_var    = @result
;;   accu_init   = false
;;   loop_cond   = @not_strictly_false(!@result)
;;   loop_step   = @result || (k > 1)
;;   result      = @result
;;
;; ── Key design claims ───────────────────────────────────────
;;
;;   1. **Three NEW runtime helpers shape the iter — `cel_map_iter_init`,
;;      `cel_map_iter_next`, `cel_map_iter_key_at`.**  Iteration
;;      state lives in a single i32 handle (a wasm local), no
;;      allocation per comprehension.  Internally
;;      `cel_map_iter_init` returns the entries-run base address;
;;      `cel_map_iter_next` increments by `kCelMapEntryStride = 48`
;;      and returns 0 when past the count×48 end.
;;
;;   2. **Key materialised into a workspace slot per iter.**
;;      Unlike list iteration (where `iter_off` IS the in-place
;;      pointer into the element run), map keys may be polymorphic
;;      (CEL_INT, CEL_STRING, CEL_UINT, CEL_BOOL) so the iter_var's
;;      "value" needs a stable slot the kIdent arm can resolve to.
;;      `cel_map_iter_key_at(key_slot, handle)` writes the current
;;      key (24-byte memcpy out of the entries run) into that slot.
;;      `k` then lowers to `(i32.const <key_slot>)` per the
;;      uniform kIdent rule.
;;
;;   3. **Iter handle is opaque to codegen.**  Codegen knows only
;;      "init returns an i32, next returns 0 or 1, key_at consumes
;;      the same i32".  The internal layout (entries pointer vs.
;;      bucket cursor) is a runtime implementation detail and can
;;      change without affecting the WAT shape.
;;
;; ── DEPENDS ON Slice E ──────────────────────────────────────
;;
;; The runtime exports `cel_map_iter_init`, `cel_map_iter_next`,
;; and `cel_map_iter_key_at` do NOT exist yet.  This WAT will
;; FAIL to instantiate via `wat_runner` until Slice E ships
;; their bodies in `cel_map.c` and adds the names to
;; `kRuntimeExports`.  Tagged `manual` until then.
;;
;; Memory layout:
;;   [ 0, 16)   reserved (null sentinel; arena state lives in runtime BSS)
;;   [16, 40)   rodata: map-key kConst {CEL_INT, i=1}
;;   [40, 64)   rodata: map-key kConst {CEL_INT, i=2}
;;   [64, 88)   rodata: map-val kConst {CEL_STRING, s={ptr=200,len=1}}
;;   [88,112)   rodata: map-val kConst {CEL_STRING, s={ptr=201,len=1}}
;;   [112,136)  rodata: accu_init = {CEL_BOOL, false}
;;   [136,160)  rodata: rhs of `k > 1` = {CEL_INT, i=1}
;;   [160,184)  workspace: iter_range map slot (kCreateMap result)
;;   [184,208)  workspace: accu_slot
;;   [208,232)  workspace: key_slot (iter_var binding, written by
;;                                   cel_map_iter_key_at each iter)
;;   [232,256)  workspace: step_out scratch
;;   [256+]  bump arena (malloc'd in heap)
;;   (String payload bytes "ab" at offset 200 inside the reserved
;;    rodata region preceding the workspace.)
;;
;; ── Runtime helpers ─────────────────────────────────────────
;;   cel.cel_map_create        (M3.F)
;;   cel.cel_map_insert        (M3.F)
;;   cel.cel_int_gt_at_vv      (M5.B)
;;   cel.cel_or                (M5.G)
;;   cel.cel_map_iter_init     (Slice E NEW)  — (map_slot) → handle
;;   cel.cel_map_iter_next     (Slice E NEW)  — (handle)   → 0|1
;;   cel.cel_map_iter_key_at   (Slice E NEW)  — (out, handle) → ()
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_map_create" (func $cel_map_create (param i32 i32)))
  (import "cel" "cel_map_insert" (func $cel_map_insert (param i32 i32 i32)))
  (import "cel" "cel_int_gt_at_vv"
          (func $cel_int_gt_at_vv (param i32 i32 i32)))
  (import "cel" "cel_or" (func $cel_or (param i32 i32 i32)))
  ;; DEPENDS ON Slice E.
  (import "cel" "cel_map_iter_init"
          (func $cel_map_iter_init (param i32) (result i32)))
  (import "cel" "cel_map_iter_next"
          (func $cel_map_iter_next (param i32) (result i32)))
  (import "cel" "cel_map_iter_key_at"
          (func $cel_map_iter_key_at (param i32 i32)))

  ;; Map keys {1, 2}.
  (data (i32.const 16)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  ;; Map values {"a", "b"} — payload bytes packed at offset 200.
  (data (i32.const 64)
        "\05\00\00\00" "\00\00\00\00"
        "\c8\00\00\00" "\01\00\00\00"
        "\00\00\00\00" "\00\00\00\00")
  (data (i32.const 88)
        "\05\00\00\00" "\00\00\00\00"
        "\c9\00\00\00" "\01\00\00\00"
        "\00\00\00\00" "\00\00\00\00")
  ;; accu_init = {CEL_BOOL, false}.
  (data (i32.const 112)
        "\01\00\00\00" "\00\00\00\00"
        "\00\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  ;; rhs of `k > 1` = {CEL_INT, i=1}.
  (data (i32.const 136)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  ;; String payload bytes "ab" at [200, 202).
  (data (i32.const 200) "ab")

  (func $eval (result i32)
    (local $iter_handle i32)

    (call $arena_reset)

    ;; iter_range = {1: "a", 2: "b"} at slot 160.
    (call $cel_map_create (i32.const 160) (i32.const 2))
    (call $cel_map_insert (i32.const 160) (i32.const 16) (i32.const 64))
    (call $cel_map_insert (i32.const 160) (i32.const 40) (i32.const 88))

    ;; accu_slot at 184 ← rodata false at 112.
    (i32.store offset=0  (i32.const 184) (i32.load offset=0  (i32.const 112)))
    (i32.store offset=4  (i32.const 184) (i32.load offset=4  (i32.const 112)))
    (i32.store offset=8  (i32.const 184) (i32.load offset=8  (i32.const 112)))
    (i32.store offset=12 (i32.const 184) (i32.load offset=12 (i32.const 112)))
    (i32.store offset=16 (i32.const 184) (i32.load offset=16 (i32.const 112)))
    (i32.store offset=20 (i32.const 184) (i32.load offset=20 (i32.const 112)))

    ;; Initialise the iter handle from the map.
    (local.set $iter_handle
               (call $cel_map_iter_init (i32.const 160)))

    (block $exit
      (loop $continue
        ;; Exit when the iterator says we're done (next returns 0).
        (br_if $exit
               (i32.eqz
                 (call $cel_map_iter_next (local.get $iter_handle))))

        ;; Materialise the current key into the iter_var workspace slot.
        (call $cel_map_iter_key_at
              (i32.const 208) (local.get $iter_handle))

        ;; Exit when `exists` peephole says accu is already true.
        (br_if $exit (i32.load offset=8 (i32.const 184)))

        ;; loop_step: @result || (k > 1)
        (call $cel_int_gt_at_vv
              (i32.const 232)
              (i32.const 208)             ;; k IS key_slot — kIdent uniform
              (i32.const 136))
        (call $cel_or
              (i32.const 184) (i32.const 184) (i32.const 232))

        (br $continue)))

    (i32.const 184))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
