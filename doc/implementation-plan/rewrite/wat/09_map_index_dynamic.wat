;; CEL source:  m[1]    (where m is whatever happens to flow in —
;;                       e.g. ?: between a literal and a bound map.
;;                       At M4 the ?: codegen isn't lowered yet, so
;;                       this trace stands as the dispatcher's
;;                       executable spec for when M5 lights it up.)
;; Decl:        m : map<int, int>     (operand origin treated as kDynamic)
;;
;; M3.C slice — runtime-side kDynamic dispatcher.  When ResolvePass
;; cannot prove a single origin (mixed ?: arms, future kCall return,
;; …) the kCallExpr arm emits `call $cel.cel_map_lookup`, which
;; tail-calls into either the arena fast path or the host
;; trampoline based on the operand's runtime CelKind:
;;
;;   CEL_MAP_ARENA → return_call cel_map_lookup_arena
;;   CEL_MAP_HOST  → return_call cel_host.cel_map_lookup
;;   anything else (CEL_UNKNOWN / CEL_ERROR / type mismatch) →
;;                  poison out_slot or absorb 3VL.
;;
;; The musttail discipline is enforced in cel_runtime.c via
;; `__attribute__((musttail))`; if the wasm tail-call feature is
;; disabled at the engine level, the runtime won't even
;; instantiate.
;;
;; This trace authors the dispatcher CALL SITE (the wasm the
;; codegen emits when `m_origin == kDynamic`); the dispatcher's
;; BODY lives in cel_runtime.wasm.  The harness binds the dispatcher
;; from the runtime instance via BindExport; the test pre-writes a
;; CEL_MAP_ARENA-shaped CelValue into m's slot to exercise the
;; arena arm of the dispatcher.
;;
;; Memory layout — same as 08 (host arm) since the call-site
;; lowering only differs in target name.
;;
;;   [ 0, 16)  reserved (null sentinel; arena state lives in runtime BSS)
;;   [16, 40)  workspace: m's slot
;;   [40, 64)  rodata: lookup-key {CEL_INT, i=1}
;;   [64, 88)  workspace: kCallExpr result slot
;;   [88+]  bump arena (malloc'd in heap)
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  ;; The dispatcher itself — no `_arena` / `_host` suffix.  Bound
  ;; from cel_runtime.wasm at instantiate time; the dispatcher
  ;; tail-calls into one of the two arms.
  (import "cel" "cel_map_lookup"
          (func $cel_map_lookup (param i32 i32 i32)))

  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (local $m_off i32)

    (local.set $m_off (i32.const 16))
    (call $arena_reset)

    ;; kDynamic arm of `_[_]`: the runtime dispatcher decides
    ;; arena vs. host at runtime by reading m's CelKind.
    (call $cel_map_lookup
          (i32.const 64)        ;; out_slot
          (local.get $m_off)    ;; map_slot — kind tag drives dispatch
          (i32.const 40))       ;; key_slot

    (i32.const 64))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
