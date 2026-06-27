;; CEL source:  {0:100, 1:101, 2:102, 3:103, 4:104, 5:105, 6:106, 7:107, 8:108}[5]
;; Decl:        — (no free variables)
;;
;; m32.A — freezes the runtime-built SwissTable index construction
;; sequence.  A 9-entry int-keyed arena map literal is built with
;; cel_map_create + 9× cel_map_insert; then cel_map_index_build is
;; emitted as the TERMINAL map-construction step (after the last
;; insert, before the map is consumed).  9 >= kCelMapIndexThreshold
;; (8), so the build actually allocates the index block and sets
;; hdr->index_offset != 0.  The lookup then resolves m[5] through
;; cel_map_index_find (the indexed path) rather than a linear scan.
;;
;; Invariant this WAT locks: the codegen call shape is unchanged —
;; cel_map_index_build is a single `(call $cel.cel_map_index_build
;; (i32.const map_slot))`, no new ABI surface.  It is a PURE
;; ACCELERATOR: m[5] resolves to CEL_INT(105) whether or not the
;; index was built (07_map_index_arena.wat is the linear-path
;; sibling; this one exercises the >=8 indexed path).
;;
;; Memory layout (each kConst CelValue is 24 bytes: kind:u32 @0,
;; _pad:u32 @4, payload.i:i64 @8, trailing pad to 24):
;;   [   0,  16)  null sentinel
;;   [  16, 16+24*9)  rodata: 9 keys   {CEL_INT, i=0..8}      (16..232)
;;   [ 232, 232+24*9) rodata: 9 values {CEL_INT, i=100..108}  (232..448)
;;   [ 448, 472)  rodata: lookup key   {CEL_INT, i=5}
;;   [ 472, 496)  workspace: kMapExpr result slot (out=472)
;;   [ 496, 520)  workspace: kCallExpr lookup result slot (out=496)
;;   [ 520+]  bump arena (entries run + index block)
;;
;; CEL_INT kind word = 2.  Each (data) is 24 bytes:
;;   "\02\00\00\00" (kind) "\00\00\00\00" (pad)
;;   "<i64 little-endian>" (payload.i, 8 bytes) "\00..\00" (8 trailing pad)
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_map_create" (func $cel_map_create (param i32 i32)))
  (import "cel" "cel_map_insert" (func $cel_map_insert (param i32 i32 i32)))
  (import "cel" "cel_map_index_build"
          (func $cel_map_index_build (param i32)))
  (import "cel" "cel_map_lookup_arena"
          (func $cel_map_lookup_arena (param i32 i32 i32)))

  ;; Keys 0..8 (24-byte stride) at 16, 40, 64, 88, 112, 136, 160, 184, 208.
  (data (i32.const 16)
        "\02\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\02\00\00\00\00\00\00\00\01\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  (data (i32.const 64)
        "\02\00\00\00\00\00\00\00\02\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  (data (i32.const 88)
        "\02\00\00\00\00\00\00\00\03\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  (data (i32.const 112)
        "\02\00\00\00\00\00\00\00\04\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  (data (i32.const 136)
        "\02\00\00\00\00\00\00\00\05\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  (data (i32.const 160)
        "\02\00\00\00\00\00\00\00\06\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  (data (i32.const 184)
        "\02\00\00\00\00\00\00\00\07\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  (data (i32.const 208)
        "\02\00\00\00\00\00\00\00\08\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")

  ;; Values 100..108 (0x64..0x6c) at 232, 256, 280, 304, 328, 352, 376, 400, 424.
  (data (i32.const 232)
        "\02\00\00\00\00\00\00\00\64\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  (data (i32.const 256)
        "\02\00\00\00\00\00\00\00\65\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  (data (i32.const 280)
        "\02\00\00\00\00\00\00\00\66\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  (data (i32.const 304)
        "\02\00\00\00\00\00\00\00\67\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  (data (i32.const 328)
        "\02\00\00\00\00\00\00\00\68\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  (data (i32.const 352)
        "\02\00\00\00\00\00\00\00\69\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  (data (i32.const 376)
        "\02\00\00\00\00\00\00\00\6a\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  (data (i32.const 400)
        "\02\00\00\00\00\00\00\00\6b\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  (data (i32.const 424)
        "\02\00\00\00\00\00\00\00\6c\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")

  ;; Lookup key = 5 at 448.
  (data (i32.const 448)
        "\02\00\00\00\00\00\00\00\05\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)

    ;; Build the 9-entry map at slot 472.
    (call $cel_map_create (i32.const 472) (i32.const 9))
    (call $cel_map_insert (i32.const 472) (i32.const 16)  (i32.const 232))
    (call $cel_map_insert (i32.const 472) (i32.const 40)  (i32.const 256))
    (call $cel_map_insert (i32.const 472) (i32.const 64)  (i32.const 280))
    (call $cel_map_insert (i32.const 472) (i32.const 88)  (i32.const 304))
    (call $cel_map_insert (i32.const 472) (i32.const 112) (i32.const 328))
    (call $cel_map_insert (i32.const 472) (i32.const 136) (i32.const 352))
    (call $cel_map_insert (i32.const 472) (i32.const 160) (i32.const 376))
    (call $cel_map_insert (i32.const 472) (i32.const 184) (i32.const 400))
    (call $cel_map_insert (i32.const 472) (i32.const 208) (i32.const 424))

    ;; TERMINAL construction step: build the SwissTable index.
    ;; count = 9 >= kCelMapIndexThreshold (8), so this sets
    ;; hdr->index_offset != 0 and activates the indexed lookup path.
    (call $cel_map_index_build (i32.const 472))

    ;; Look up key 5 (at 448) → should resolve via the index to
    ;; CEL_INT(105), written to slot 496.
    (call $cel_map_lookup_arena
          (i32.const 496) (i32.const 472) (i32.const 448))

    (i32.const 496))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
