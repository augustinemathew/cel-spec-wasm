;; M13 Probe 1 — expression-side caller for a foreign-wasm custom fn.
;;
;; Simulates what celwasmc would emit for the CEL expression
;; `userId.allow("/admin")` where `allow` is declared as
;;
;;     bool rules.allow(this string userId, string resource);
;;
;; The signature uses only types the v1 cross-foreign-boundary
;; constraint permits (§4.5.1 of m13-custom-fns.md): primitives +
;; strings/bytes + Duration/Timestamp + non-proto-bearing list/map.
;; Proto messages are explicitly out of scope for the foreign-wasm
;; backend in v1 — they enter the expression's wasm as externrefs
;; from the host adapter and have no representation that survives
;; the cross-foreign-module hop.  See §4.5.1 for the constraint's
;; motivation + the documented future-relaxation paths.
;;
;; The .celfn ⇒ wasm import wiring this probe is locking in:
;;
;;     bool rules.allow(this string userId, string resource);
;;
;; becomes wasm import:
;;
;;     (import "rules" "allow_string_string"
;;       (func (param i32 i32 i32)))
;;
;; with call shape `(out_slot, arg0_slot, arg1_slot)`.
;;
;; Memory layout (host-owned 2-page memory, shared with the foreign
;; module — both modules see the same `cel.memory`):
;;
;;   [ 0,  8)   reserved null sentinel
;;   [ 8, 16)   reserved (legacy cursor/limit slot — arena state now in BSS)
;;   [16, 40)   args[0] — userId CelValue
;;                kind = CEL_STRING (5)
;;                payload.s.ptr = 80
;;                payload.s.len = 4      ("alex")
;;   [40, 64)   args[1] — resource CelValue
;;                kind = CEL_STRING (5)
;;                payload.s.ptr = 88
;;                payload.s.len = 6      ("/admin")
;;   [64, 88)   out_slot — receives the bool CelValue the foreign fn writes
;;   [80, 84)   "alex" bytes (referenced by args[0].payload.s)
;;   [88, 94)   "/admin" bytes (referenced by args[1].payload.s)
;;
;; What the probe proves:
;;   * Two user wasms can share a host-owned linear memory.
;;   * The caller can pre-stage CelValue args and an out_slot in memory,
;;     pass slot offsets to an import, and decode the result by reading
;;     the out_slot bytes after the call returns.
;;   * The wasmtime Linker correctly wires `rules.allow_message_acme_User_string`
;;     (the alias-qualified import) to the foreign module's export of
;;     the same name.

(module
  ;; Shared 2-page memory.  Producer-side choice — see §4.5.2 of
  ;; m13-custom-fns.md.  In Probe 1 the foreign module imports
  ;; memory too (hand-rolled stub).  In Probe 2 the foreign module
  ;; (TinyGo) exports memory, and the engine binds THAT as
  ;; `cel.memory` for this caller.  Either way, this `(import "cel"
  ;; "memory" …)` resolves correctly.
  (import "cel" "memory" (memory 2 1024 shared))

  ;; The foreign custom fn.  In the real system this is wired up at
  ;; Engine::Plan via RuntimeBindings::AddModule("rules", instance).
  (import "rules" "allow_string_string"
    (func $rules_allow (param i32 i32 i32)))

  ;; Pre-staged args + bytes.  In the real system, the args get
  ;; written by the expression's earlier codegen (loading from
  ;; activation slots, lowering string literals into rodata, etc.).
  ;; For the probe we hand-stuff them as a `(data …)` initializer.

  ;; args[0] = userId string CelValue at [16, 40)
  ;;   kind = CEL_STRING = 5 = 0x05
  ;;   _pad = 0
  ;;   payload.s = { ptr=80, len=4 }   ("alex")
  (data (i32.const 16)
        "\05\00\00\00"    ;; kind = 5
        "\00\00\00\00"    ;; _pad
        "\50\00\00\00"    ;; payload.s.ptr = 80
        "\04\00\00\00"    ;; payload.s.len = 4
        "\00\00\00\00"    ;; payload tail
        "\00\00\00\00")

  ;; args[1] = resource string CelValue at [40, 64)
  ;;   kind = CEL_STRING = 5
  ;;   _pad = 0
  ;;   payload.s = { ptr=88, len=6 }   ("/admin")
  (data (i32.const 40)
        "\05\00\00\00"    ;; kind = 5
        "\00\00\00\00"    ;; _pad
        "\58\00\00\00"    ;; payload.s.ptr = 88
        "\06\00\00\00"    ;; payload.s.len = 6
        "\00\00\00\00"    ;; payload tail
        "\00\00\00\00")

  ;; String bytes the args point at.
  (data (i32.const 80) "alex")
  (data (i32.const 88) "/admin")

  ;; eval() — the entry point a real expr module would expose.
  ;; Returns the out_slot offset (64) so the host can decode the
  ;; bool CelValue the foreign fn wrote.
  (func $eval (result i32)
    ;; Call rules.allow(out=64, userId=16, resource=40).
    (call $rules_allow
      (i32.const 64)    ;; out_slot
      (i32.const 16)    ;; userId_slot
      (i32.const 40))   ;; resource_slot
    (i32.const 64))     ;; return out_slot offset

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
