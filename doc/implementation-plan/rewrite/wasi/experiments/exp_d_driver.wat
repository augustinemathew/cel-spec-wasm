;; Drives the arena: init(64K), alloc(128), alloc(256), reset, alloc(128).
;; Returns the second-round alloc's offset — should equal the FIRST alloc.
(module
  (import "lib" "memory" (memory 2))
  (import "lib" "arena_init"   (func $init   (param i32)))
  (import "lib" "arena_alloc"  (func $alloc  (param i32) (result i32)))
  (import "lib" "arena_reset"  (func $reset))
  (import "lib" "arena_cursor" (func $cursor (result i32)))

  (func (export "scenario") (result i32)
    (local $first i32) (local $second i32) (local $third i32)
    (call $init (i32.const 65536))            ;; 64 KB arena
    (local.set $first  (call $alloc (i32.const 128)))
    (local.set $second (call $alloc (i32.const 256)))
    (call $reset)
    (local.set $third  (call $alloc (i32.const 128)))
    ;; Return 1 if third == first (reset worked), else 0
    (i32.eq (local.get $first) (local.get $third)))
)
