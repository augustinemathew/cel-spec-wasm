;; Driver for exp_e_absl_parsetime.wasm.  Imports the lib's memory
;; and `parse` export, supplies a hand-coded RFC3339 timestamp,
;; calls parse, reads back the seconds value.
;;
;; Expected: timestamp "2026-05-18T10:00:00Z" → 1779184800 unix seconds.

(module
  (import "lib" "memory" (memory 1 65536 shared))
  (import "lib" "parse"
          (func $parse (param i32 i32 i32 i32) (result i32)))

  ;; Pre-init the input string + scratch slots at offsets above
  ;; the lib's static-data + stack region.  The lib's __heap_base
  ;; is around 0x12000; pick 0x14000 to land safely above.
  (data (i32.const 0x14000) "2026-05-18T10:00:00Z")

  ;; Out: parse(input, 20, &out_seconds, &out_nanos)
  ;; Returns the parsed Unix seconds as i64 (low 32 bits returned
  ;; here as i32 for simplicity in the wat_runner harness).
  (func (export "run") (result i32)
    (local $err i32)
    (local.set $err
      (call $parse
            (i32.const 0x14000)      ;; buf
            (i32.const 20)           ;; len
            (i32.const 0x14100)      ;; out_seconds (i64 = 8 bytes)
            (i32.const 0x14110)))    ;; out_nanos (i32 = 4 bytes)
    (if (i32.ne (local.get $err) (i32.const 0))
      (then (return (i32.const -1))))
    ;; Return the low 32 bits of out_seconds (sufficient for
    ;; verifying the parse worked; full i64 not needed for the
    ;; smoke test).
    (i32.load (i32.const 0x14100)))
)
