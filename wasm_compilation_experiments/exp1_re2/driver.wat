;; Hand-coded driver that calls into hello.wasm's match() (substring search).
;; Shares lib's linear memory; writes test inputs as rodata into that memory.
(module
  (import "lib" "memory" (memory 1))
  (import "lib" "match"
          (func $match (param i32 i32 i32 i32) (result i32)))

  (data (i32.const 0x1000) "hello world wasi-sdk")  ;; len 20
  (data (i32.const 0x1020) "wasi")                   ;; len 4
  (data (i32.const 0x1030) "xyz")                    ;; len 3
  (data (i32.const 0x1040) "the quick brown fox")    ;; len 19
  (data (i32.const 0x1060) "quick")                  ;; len 5

  ;; case 0: match "wasi" in "hello world wasi-sdk" → 1
  (func (export "case0") (result i32)
    (call $match (i32.const 0x1000) (i32.const 20)
                 (i32.const 0x1020) (i32.const 4)))
  ;; case 1: match "xyz" in "hello world wasi-sdk" → 0
  (func (export "case1") (result i32)
    (call $match (i32.const 0x1000) (i32.const 20)
                 (i32.const 0x1030) (i32.const 3)))
  ;; case 2: match "quick" in "the quick brown fox" → 1
  (func (export "case2") (result i32)
    (call $match (i32.const 0x1040) (i32.const 19)
                 (i32.const 0x1060) (i32.const 5)))
)
