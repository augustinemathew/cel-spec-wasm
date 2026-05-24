;; CEL source:  base64.decode('aGVsbG8')
;; Decl:        — (no free variables)
;;
;; M17 Slice 0 — locks the slot-out ABI for the `encoders` extension's
;; decode kernel.  `base64.decode(string) -> bytes` is self-hosted in
;; cel_runtime.wasm (same import-from-"cel" shape as encode / concat).
;;
;; The chosen input — 'aGVsbG8' with NO trailing '=' padding — is the
;; load-bearing conformance case (encoders_ext.textproto::decode/
;; hello_without_padding, m17-encoders-ext.md §7 risk).  absl::Base64-
;; Unescape accepts missing padding, so the kernel needs no manual
;; re-pad step; this WAT pins that behaviour into the trace so a future
;; absl tightening surfaces here, not in conformance.
;;
;; Memory layout:
;;   [ 0, 16)  reserved (null sentinel; arena state lives in runtime BSS)
;;   [16, 40)  rodata: kConst "aGVsbG8"
;;                     {kind=CEL_STRING(5), span.ptr=40, span.len=7}
;;   [40, 47)  rodata: string body "aGVsbG8" (7 bytes)
;;   [47, 48)  padding to 8-align the workspace slot
;;   [48, 72)  workspace: kCall(`base64.decode`) result slot (out=48)
;;   [72+]  bump arena (malloc'd in heap) — the decoded bytes live here
;;
;; New import this milestone:
;;   cel.cel_base64_decode_at_v(out_slot, str_slot) — i32×2 → ()
;;
;; cel_base64_decode_at_v contract (cel-cpp parity:
;;   third_party/cel-cpp/extensions/encoders.cc::Base64Decode →
;;   absl::Base64Unescape):
;;     - reads `str_slot` as CEL_STRING.  Wrong kind → out_slot =
;;       {CEL_ERROR, err=CEL_ERR_TYPE_MISMATCH}.
;;     - 3VL absorption — CEL_UNKNOWN/CEL_ERROR in str_slot pass
;;       through verbatim.
;;     - happy path: `absl::Base64Unescape(view, &out)` returns true;
;;       allocates out.size() bytes in the arena, copies the decoded
;;       bytes, writes out_slot = {CEL_BYTES, payload.s={ptr=<new>, len=N}}.
;;       Output is CEL_BYTES (may be non-UTF-8), NOT CEL_STRING.
;;     - invalid input: `absl::Base64Unescape` returns false → out_slot =
;;       {CEL_ERROR, err=CEL_ERR_INVALID_ARGUMENT}.  The error message
;;       string mirrors cel-cpp verbatim: "invalid base64 data"
;;       (encoders.cc:51).
;;     - OOM during arena_alloc → {CEL_ERROR, CEL_ERR_OVERFLOW}.
;;
;; "aGVsbG8" → b'hello' (68 65 6c 6c 6f), 5 bytes.  Decoded payload lives
;; in the bump arena until the next arena_reset.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_base64_decode_at_v"
          (func $cel_base64_decode_at_v (param i32 i32)))

  ;; rodata @ 16: kind=CEL_STRING(5), _pad, span.ptr=40, span.len=7, pad8.
  (data (i32.const 16)
        "\05\00\00\00" "\00\00\00\00"
        "\28\00\00\00" "\07\00\00\00"
        "\00\00\00\00\00\00\00\00")
  ;; rodata @ 40: the 7-byte body (unpadded base64).
  (data (i32.const 40) "aGVsbG8")

  (func $eval (result i32)
    (call $arena_reset)

    ;; out_slot=48, str=16.  Helper Base64Unescapes the 7 input bytes,
    ;; allocates 5 bytes in the arena, copies "hello" into them, and
    ;; writes {CEL_BYTES, payload.s={ptr=<new>, len=5}} to slot 48.
    (call $cel_base64_decode_at_v
          (i32.const 48) (i32.const 16))

    (i32.const 48))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
