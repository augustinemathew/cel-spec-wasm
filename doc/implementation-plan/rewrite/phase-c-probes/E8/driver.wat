;; Driver for E8 re2_match.wasm.
(module
  (import "lib" "memory" (memory 1 65536 shared))
  (import "lib" "match"
          (func $match (param i32 i32 i32 i32) (result i32)))
  (import "lib" "__wasm_call_ctors" (func $ctors))

  (data (i32.const 0x180000) "[a-z]+@[a-z]+")
  (data (i32.const 0x180020) "alice@example")
  (data (i32.const 0x180040) "no-match-here")

  (func (export "match_ok") (result i32)
    (call $ctors)
    (call $match (i32.const 0x180000) (i32.const 13)
                 (i32.const 0x180020) (i32.const 13)))

  (func (export "match_fail") (result i32)
    (call $ctors)
    (call $match (i32.const 0x180000) (i32.const 13)
                 (i32.const 0x180040) (i32.const 13)))
)
