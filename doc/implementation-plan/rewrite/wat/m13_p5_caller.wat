;; M13 Probe 5 — expression-side caller for a host-backed custom fn.
;;
;; Mirrors what celwasmc would emit for `name.length()` where
;;
;;     int @host.length(this string s);
;;
;; is declared in the .celfn file (overload-id `length_string`,
;; wasm import module `cel_fn`).
;;
;; Memory layout:
;;
;;   [ 0,  8)   reserved null sentinel
;;   [ 8, 16)   reserved (arena cursor / limit slot — unused here)
;;   [16, 40)   args[0] — string CelValue
;;                kind = CEL_STRING (5)
;;                payload.s.ptr = 80
;;                payload.s.len = 11    ("hello world")
;;   [40, 64)   out_slot — receives the int CelValue the host wrote
;;   [80, 91)   "hello world" bytes

(module
  (import "cel" "memory" (memory 2))

  ;; The host-backed custom fn — bound at runtime by the engine's
  ;; host-callback registry (see m13_p5_host_test.cc).
  (import "cel_fn" "length_string"
    (func $host_length (param i32 i32)))

  ;; args[0] = string CelValue at [16, 40)
  ;;   kind = CEL_STRING = 5
  ;;   _pad = 0
  ;;   payload.s = { ptr=80, len=11 }
  (data (i32.const 16)
        "\05\00\00\00"    ;; kind = 5
        "\00\00\00\00"    ;; _pad
        "\50\00\00\00"    ;; payload.s.ptr = 80
        "\0b\00\00\00"    ;; payload.s.len = 11
        "\00\00\00\00"    ;; payload tail
        "\00\00\00\00")

  ;; The "hello world" bytes at offset 80.
  (data (i32.const 80) "hello world")

  ;; eval() — call host_length(out=40, s=16), return out_slot offset.
  (func $eval (result i32)
    (call $host_length
      (i32.const 40)    ;; out_slot
      (i32.const 16))   ;; s_slot
    (i32.const 40))     ;; return out_slot

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
