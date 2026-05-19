;; Driver for E5 combined.wasm.  Calls parse_timestamp with a
;; canned RFC3339 string; reads back the seconds value.
(module
  (import "lib" "memory" (memory 1 65536 shared))
  (import "lib" "parse_timestamp"
          (func $parse (param i32 i32 i32 i32) (result i32)))

  (data (i32.const 0x80000) "2026-05-18T10:00:00Z")

  (func (export "run") (result i32)
    (local $err i32)
    (local.set $err
      (call $parse
            (i32.const 0x80000)
            (i32.const 20)
            (i32.const 0x80100)
            (i32.const 0x40110)))
    (if (i32.ne (local.get $err) (i32.const 0))
      (then (return (i32.const -1))))
    (i32.load (i32.const 0x80100)))
)
