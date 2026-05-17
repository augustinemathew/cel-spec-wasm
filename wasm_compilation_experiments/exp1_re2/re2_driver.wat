;; Driver for re2_lib.wasm — imports its memory and match() export.
;; Pre-populates pattern and text strings, invokes match.
(module
  (import "lib" "memory" (memory 1 65536 shared))
  (import "lib" "match"
          (func $match (param i32 i32 i32 i32) (result i32)))

  ;; Use the top of memory — well above RE2's static data
  (data (i32.const 0x24000) "[a-z]+@[a-z]+")     ;; len 13
  (data (i32.const 0x24020) "alice@example")      ;; len 13
  (data (i32.const 0x24040) "\\d{4}-\\d{2}-\\d{2}")  ;; len 18 (date pattern)
  (data (i32.const 0x24060) "today: 2026-05-17")  ;; len 17

  (func (export "case0") (result i32)
    (call $match (i32.const 0x24000) (i32.const 13)
                 (i32.const 0x24020) (i32.const 13)))
  (func (export "case1") (result i32)
    (call $match (i32.const 0x24040) (i32.const 17)
                 (i32.const 0x24060) (i32.const 17)))
)
