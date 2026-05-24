;; CEL source:  TestAllTypes{?single_int32: optional.of(5), ?single_string: optional.none()}
;; Decl:        — (no free variables)
;;
;; Locks the `cel_set_field_at_if_present` kernel ABI for
;; `Foo{?field: opt_value}` proto-literal entries.  Per cel-cpp
;; wire format (see m14-optionals.md §1.2 + probe Q3 analogue for
;; StructExprField), the per-entry `optional` flag in
;; `StructExprField` is set; codegen emits one regular
;; `cel_make_message` + per-entry either `cel_set_field` (regular)
;; or `cel_set_field_at_if_present` (this kernel).
;;
;; The kernel completes the trio of optional-payload predicate
;; insert/append helpers — symmetric to
;; `cel_map_insert_at_if_present` (M14.8) and
;; `cel_list_append_at_if_present` (M14.7) — but the inner
;; "actually set the field" step is a host trampoline
;; (`cel_host.cel_set_field`) rather than a pure-wasm kernel.
;;
;; ── New runtime kernel this slice locks ────────────────────
;;
;;   cel.cel_set_field_at_if_present(msg_slot, field_ref_id,
;;                                   opt_value_slot)
;;
;;   1. m = cel_value_at(msg_slot)
;;   2. If m.kind != CEL_MESSAGE: return (msg already poisoned by
;;      an upstream 3VL absorption; preserve the poison).
;;   3. opt = cel_value_at(opt_value_slot)
;;   4. If opt.kind == CEL_ERROR || opt.kind == CEL_UNKNOWN:
;;        *msg_slot = *opt  ;; propagate; aborts the proto literal
;;        return
;;   5. If opt.kind != CEL_OPTIONAL:
;;        poison msg_slot with CEL_ERR_TYPE_MISMATCH; return
;;   6. cell = (OptionalCell*)(memory_base + opt.payload.opt)
;;   7. If !cell.present: return ;; silent no-op — field stays
;;      unset in the proto.  This matches proto semantics: a
;;      field that's never set + cel-cpp's `has()` returns false.
;;   8. Otherwise: cel_host.cel_set_field(msg_slot, field_ref_id,
;;                                        &cell.inner)
;;      The host trampoline (Layer-2 ProtoBacking) walks the
;;      FieldDescriptor and writes the unwrapped inner CelValue
;;      into the proto message via Reflection::Set*.  The slot
;;      pointing at cell.inner is byte-equivalent to staging
;;      cell.inner into a workspace slot first — the offset is
;;      stable until the next arena_reset.
;;
;; ── Why the kernel itself is pure wasm ────────────────────────
;;
;; The optional unwrap (`cell->present`, `cell->inner`) is just
;; memory reads — no proto reflection needed.  Only the inner
;; `cel_set_field` call needs the host.  This is the same shape
;; as the map/list `_if_present` kernels: wasm-side predicate
;; check, then delegate the actual mutation to whichever helper
;; performs it.  For map/list that helper is itself pure-wasm
;; (`cel_map_insert_at` / `cel_list_append_at`); for proto it's
;; a host trampoline.  The wrapper is identical.
;;
;; Cross-reference: this design pull-in is documented in
;; m14-optionals.md §0 "Scope pull-in 2026-05-22" and worked-
;; example 1 in `doc/implementation-plan/design-pressure-test-prompt.md`.
;;
;; ── Memory layout ─────────────────────────────────────────────
;;
;;   [ 0, 16)   reserved null + arena scaffolding
;;   [16, 40)   rodata: inner CelValue {CEL_INT, i=5}
;;   [40, 64)   workspace: CelMessage{kind=CEL_MESSAGE,
;;              payload.msg_slot=1} (pre-populated by harness;
;;              kernel reads kind only — the host stub records
;;              its arg slots into wat_runner-managed state, so
;;              the message contents don't matter for the WAT)
;;   [64, 88)   workspace: optional.of(5) — Some<int>
;;   [88, 112)  workspace: optional.none() — None
;;   [112, mem_size)  bump arena.  By the end of $eval: two
;;              OptionalCells (2 × 32 B).
;;
;; ── Host stub contract ────────────────────────────────────────
;;
;; The wat_runner test installs a `cel_host_cel_set_field_stub`
;; that records (msg_slot, field_ref_id, value_slot) on each
;; invocation.  Test expectations:
;;
;;   - Some-path call: stub invoked once with field_ref_id=42
;;     (sentinel ID; field names aren't dereferenced for the
;;     ABI lock — the value_slot points 8 bytes into the
;;     Some-cell's OptionalCell (past `present` + `_pad`)).
;;   - None-path call: stub NOT invoked.  This is the load-
;;     bearing assertion that the wrapper short-circuits before
;;     the host call — equivalent to the
;;     `cel_list_append_at_if_present` None test (M14.7) that
;;     proves the list count stays at 0.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_optional_of_at_v"
          (func $cel_optional_of_at_v (param i32 i32)))
  (import "cel" "cel_optional_none_at"
          (func $cel_optional_none_at (param i32)))
  (import "cel" "cel_set_field_at_if_present"
          (func $cel_set_field_at_if_present (param i32 i32 i32)))

  ;; rodata: CelValue{CEL_INT, i=5} at offset 16.
  (data (i32.const 16)
        "\02\00\00\00"
        "\00\00\00\00"
        "\05\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")
  ;; workspace pre-write: CelMessage{kind=CEL_MESSAGE(10),
  ;; payload.msg_slot=1} at offset 40.  Static rodata data segment
  ;; is fine — the test never observes the message's payload, only
  ;; passes msg_slot=40 through to the kernel.
  (data (i32.const 40)
        "\0a\00\00\00"
        "\00\00\00\00"
        "\01\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)
    ;; step 1: optional.of(5) at slot 64.
    (call $cel_optional_of_at_v (i32.const 64) (i32.const 16))
    ;; step 2: optional.none() at slot 88.
    (call $cel_optional_none_at (i32.const 88))
    ;; step 3: set field (sentinel field_ref_id=42) with Some(5).
    ;; The kernel unwraps and calls host stub with the inner offset.
    (call $cel_set_field_at_if_present
          (i32.const 40)   ;; msg_slot
          (i32.const 42)   ;; field_ref_id (sentinel)
          (i32.const 64))  ;; opt_value_slot — Some
    ;; step 4: set field with None.  Kernel short-circuits; host
    ;; stub NOT invoked.
    (call $cel_set_field_at_if_present
          (i32.const 40)   ;; msg_slot
          (i32.const 43)   ;; field_ref_id (different sentinel —
                            ;; lets the test prove which call
                            ;; reached the host if accounting drifts)
          (i32.const 88))  ;; opt_value_slot — None
    (i32.const 40))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
