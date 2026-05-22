;; M13 Probe 1 — foreign-module stand-in.
;;
;; Stands in for what TinyGo / Rust / AS produce for a `Foreign rules`
;; declaration block (or, in the current grammar, for any decl shaped
;; `bool rules.<fn>(...)`).  Hand-rolled so the probe can validate
;; the wasm-link contract without dragging in a cross-language
;; toolchain yet.
;;
;; This file is what Probe 2 will replace with a TinyGo-built
;; `rules.wasm`.  The export signature here IS the cross-language
;; ABI contract the Go side will have to honor.
;;
;; Contract (mirrors m13_p1_caller.wat):
;;
;;   (export "allow_string_string"
;;     (func (param i32 i32 i32)))   ;; (out_slot, userId_slot, resource_slot)
;;
;; The fn reads CelValues at userId_slot + resource_slot, makes a
;; decision, and writes a CelValue to out_slot.  The probe stub
;; ignores its inputs and always writes `true` — Probe 2 will
;; replace this with a real decision.

(module
  ;; Same host-owned memory the caller imports.  This is the
  ;; pillar of the shared-memory ABI — both modules see the same
  ;; bytes.
  (import "cel" "memory" (memory 2))

  ;; The foreign export.  Name shape matches what the IDL grammar's
  ;; overload-id synthesis produces from
  ;;
  ;;     bool rules.allow(this string userId, string r);
  ;;
  ;; i.e. `<fnname>_<argkind>_<argkind>…` with the receiver included.
  (func $allow (export "allow_string_string")
    (param $out i32) (param $user i32) (param $resource i32)

    ;; Write `true` as a bool CelValue into *out_slot.
    ;;   kind          = CEL_BOOL = 1
    ;;   _pad          = 0
    ;;   payload.b     = 1   (i32 at payload offset 0, i.e. slot+8)
    ;;   payload tail  = 0   (irrelevant for bool kind)
    (i32.store          (local.get $out) (i32.const 1))    ;; kind = 1
    (i32.store offset=4 (local.get $out) (i32.const 0))    ;; _pad
    (i32.store offset=8 (local.get $out) (i32.const 1))    ;; payload.b = 1
    (i32.store offset=12 (local.get $out) (i32.const 0))   ;; payload tail
    (i32.store offset=16 (local.get $out) (i32.const 0))
    (i32.store offset=20 (local.get $out) (i32.const 0))))
