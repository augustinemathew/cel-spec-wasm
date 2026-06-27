;; CEL source:  {0:100, 1:101, 2:102, 3:103, 4:104, 5:105, 6:106, 7:107, 8:108}[5]
;; Decl:        — (no free variables)
;;
;; m32.B — compile-time materialization of a constant map literal WITH its
;; baked SwissTable index.  Unlike 73_map_swisstable_index.wat (which BUILDS
;; the 9-entry map at eval time via cel_map_create + 9× cel_map_insert +
;; cel_map_index_build), here the WHOLE map value — ArenaMapHeader + the
;; 48-byte {key,val} entry run + the baked control-byte / slot-array index
;; block + the outer CEL_MAP_ARENA frame — is written into the module's data
;; segment at compile time.  $eval constructs NOTHING: it reads the
;; materialized map directly through cel_map_lookup_arena, which follows
;; hdr->index_offset (!= 0) and resolves m[5] through the BAKED index
;; (cel_map_index_find), exactly as if cel_map_index_build had run.
;;
;; This freezes the byte layout StaticMemoryBuilder::MaterializeMap must
;; reproduce and proves the read kernel cannot tell a materialized,
;; index-baked map from a runtime-built one — the byte-identity gate
;; (StaticMemoryBuilderKeystoneTest pins the bytes against the runtime
;; builders).  9 >= kCelMapIndexThreshold (8), so an index IS baked.
;;
;; The data bytes below are emitted verbatim by MaterializeMap at
;; base_offset 16 (regenerate from the materializer if the layout changes).
;;
;; Memory map (all offsets absolute; CelValue is 24 B, ArenaMapHeader 16 B,
;; entry stride 48 B = {key:24, val:24}; CEL_INT=2, CEL_MAP_ARENA=8):
;;   [   0,  16)  reserved (null sentinel)
;;   [  16,  32)  ArenaMapHeader { count=9, cap=9, entries_offset=32,
;;                                 index_offset=464 }
;;   [  32, 464)  entry run: 9 × 48-B {key:CEL_INT i, val:CEL_INT 100+i}
;;   [ 464, 552)  baked index block: ctrl[16]+clone[7] (kEmpty 0x80 /
;;                7-bit H2), pad to 4, then 16 × u32 slot = entry index
;;   [ 552, 576)  outer frame {CEL_MAP_ARENA, header_ptr=16}  ← the value
;;                              the kMapExpr lowers to an i32.const of
;;   [ 576, 600)  lookup key   {CEL_INT, i=5}
;;   [ 600, 624)  workspace: cel_map_lookup_arena result slot (out=600)
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "cel_map_lookup_arena"
          (func $cel_map_lookup_arena (param i32 i32 i32)))

  ;; ── materialized map [16, 552): header + 48-B run + baked index ──────
  ;; ArenaMapHeader @16: count=9, cap=9, entries_offset=32, index_offset=464
  (data (i32.const 16)
        "\09\00\00\00\09\00\00\00\20\00\00\00\d0\01\00\00")
  ;; entry run @32 (key/val CelValues, 48 B per entry, source order 0..8)
  (data (i32.const 32)
        "\02\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00\02\00\00\00\00\00\00\00"
        "\64\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00"
        "\02\00\00\00\00\00\00\00\01\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00\02\00\00\00\00\00\00\00"
        "\65\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00"
        "\02\00\00\00\00\00\00\00\02\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00\02\00\00\00\00\00\00\00"
        "\66\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00"
        "\02\00\00\00\00\00\00\00\03\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00\02\00\00\00\00\00\00\00"
        "\67\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00"
        "\02\00\00\00\00\00\00\00\04\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00\02\00\00\00\00\00\00\00"
        "\68\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00"
        "\02\00\00\00\00\00\00\00\05\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00\02\00\00\00\00\00\00\00"
        "\69\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00"
        "\02\00\00\00\00\00\00\00\06\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00\02\00\00\00\00\00\00\00"
        "\6a\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00"
        "\02\00\00\00\00\00\00\00\07\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00\02\00\00\00\00\00\00\00"
        "\6b\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00"
        "\02\00\00\00\00\00\00\00\08\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00\02\00\00\00\00\00\00\00"
        "\6c\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00")
  ;; baked index block @464: ctrl[16] + cloned mirror[7] (= first 7 ctrl
  ;; bytes), pad to 4-byte align, then 16 × u32 slot array.
  (data (i32.const 464)
        "\00\80\80\80\80\4a\5a\80\80\41\80\2f\57\4e\36\6d"
        "\00\80\80\80\80\4a\5a\00"            ;; cloned first-7 mirror + pad
        "\06\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00"
        "\00\00\00\00\04\00\00\00\05\00\00\00\00\00\00\00"
        "\00\00\00\00\01\00\00\00\00\00\00\00\00\00\00\00"
        "\07\00\00\00\02\00\00\00\08\00\00\00\03\00\00\00")
  ;; outer frame @552: CEL_MAP_ARENA(8), arena_map.header_ptr = 16
  (data (i32.const 552)
        "\08\00\00\00" "\00\00\00\00"
        "\10\00\00\00" "\00\00\00\00\00\00\00\00\00\00\00\00")

  ;; Lookup key @576: {CEL_INT, i=5}
  (data (i32.const 576)
        "\02\00\00\00" "\00\00\00\00"
        "\05\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    ;; Read materialized map[5] — resolves via the BAKED index, no
    ;; construction, no host trip.  Yields CEL_INT(105) at out=600.
    (call $cel_map_lookup_arena
          (i32.const 600) (i32.const 552) (i32.const 576))
    (i32.const 600))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
