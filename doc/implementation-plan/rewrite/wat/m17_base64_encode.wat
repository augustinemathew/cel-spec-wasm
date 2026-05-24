;; CEL source:  base64.encode(b'hello')
;; Decl:        — (no free variables)
;;
;; M17 Slice 0 — locks the slot-out ABI for the `encoders` extension's
;; encode kernel.  `base64.encode(bytes) -> string` is self-hosted in
;; cel_runtime.wasm (NOT a host trampoline) — same shape class as
;; `cel_string_concat_at_vv` (WAT 18): a unary `(out_slot, arg_slot)`
;; helper that allocates its output bytes in the arena.  The only
;; difference vs concat is arity (one value arg, hence `_at_v`).
;;
;; Memory layout:
;;   [ 0, 16)  reserved (null sentinel; arena state lives in runtime BSS)
;;   [16, 40)  rodata: kConst b'hello'
;;                     {kind=CEL_BYTES(6), span.ptr=40, span.len=5}
;;   [40, 45)  rodata: byte body "hello" (5 bytes)
;;   [45, 48)  padding to 8-align the workspace slot
;;   [48, 72)  workspace: kCall(`base64.encode`) result slot (out=48)
;;   [72+]  bump arena (malloc'd in heap) — the encoded string lives here
;;
;; New import this milestone:
;;   cel.cel_base64_encode_at_v(out_slot, bytes_slot) — i32×2 → ()
;;
;; cel_base64_encode_at_v contract (cel-cpp parity:
;;   third_party/cel-cpp/extensions/encoders.cc::Base64Encode →
;;   absl::Base64Escape):
;;     - reads `bytes_slot` as CEL_BYTES.  Wrong kind → out_slot =
;;       {CEL_ERROR, err=CEL_ERR_TYPE_MISMATCH}.
;;     - 3VL absorption — CEL_UNKNOWN/CEL_ERROR in bytes_slot pass
;;       through verbatim.
;;     - happy path: `absl::Base64Escape(span)` produces the standard
;;       RFC 4648 alphabet, WITH padding.  Allocates the escaped length
;;       in the arena via arena_alloc; OOM → {CEL_ERROR, CEL_ERR_OVERFLOW}.
;;     - writes out_slot = {CEL_STRING, payload.s={ptr=<new>, len=N}}.
;;       Raw (non-UTF-8) input bytes encode fine — output is always
;;       ASCII base64 text, hence CEL_STRING not CEL_BYTES.
;;
;; b'hello' (68 65 6c 6c 6f) → "aGVsbG8=" (8 bytes: "hel"→aGVs, "lo"→bG8=).
;; The encoded payload lives in the bump arena, surviving until the next
;; arena_reset (top of next $eval) — same lifetime contract as concat.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_base64_encode_at_v"
          (func $cel_base64_encode_at_v (param i32 i32)))

  ;; rodata @ 16: kind=CEL_BYTES(6), _pad, span.ptr=40, span.len=5, pad8.
  (data (i32.const 16)
        "\06\00\00\00" "\00\00\00\00"
        "\28\00\00\00" "\05\00\00\00"
        "\00\00\00\00\00\00\00\00")
  ;; rodata @ 40: the 5-byte body.
  (data (i32.const 40) "hello")

  (func $eval (result i32)
    (call $arena_reset)

    ;; out_slot=48, bytes=16.  Helper Base64Escapes the 5 input bytes,
    ;; allocates 8 bytes in the arena, copies "aGVsbG8=" into them, and
    ;; writes {CEL_STRING, payload.s={ptr=<new>, len=8}} to slot 48.
    (call $cel_base64_encode_at_v
          (i32.const 48) (i32.const 16))

    (i32.const 48))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
