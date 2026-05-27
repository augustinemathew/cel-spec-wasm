;; CEL source (the .celfn library + caller):
;;   int @native.double(int x) = x * 2 ;
;;   int @native.quad(int x)   = double(double(x)) ;
;;   caller expression:  quad(21)            → 84
;;
;; MODEL A — single-module inlining.  Each `@native` body lowers to a
;; LOCAL (func) in the SAME module as `$eval` — not an import, not a
;; separate module.  Each body is just another CEL expression run through
;; the same ResolvePass → LayoutPass → expr_lower passes, placed at a
;; DISJOINT static band in the shared `[16, 8192)` region via
;; LayoutPass's `rodata_base_override`.  Contrast Model B
;; (m13_celfn_double_{caller,lib}.wat): separate lib module + `__memory_base`
;; PIC relocation + Engine::Plan registration.  Model A needs none of that.
;;
;; The call ABI is the universal slot convention shared by every helper,
;; builtin, and `@host` import: `(i32 out_slot, i32 arg0, ...) -> ()`,
;; absolute byte offsets into the shared `cel.memory`.  A `@native` body
;; is reached by a plain LOCAL `call $<overload_id>` (routing target
;; `kLocal`), NOT an import.
;;
;; ── What this trace proves ──────────────────────────────────────────
;;   (1) A `@native` body lowers to a local func on the slot ABI and runs
;;       end-to-end through the runtime kernels over the shared memory.
;;   (2) A body can call ANOTHER native (quad → double) via `kLocal`.
;;   (3) SINGLE-STATIC-BAND REUSE is sound: `double`'s ONE scratch cell is
;;       reused by BOTH the inner and outer calls.  This is safe ONLY
;;       because each call COPIES its result OUT to the caller's out_slot
;;       (copy-out ABI) before the next call clobbers the band — so the
;;       inner result is parked in `quad`'s slot (112) before the outer
;;       call re-enters `double`.  No per-call frame; eval is
;;       single-threaded and recursion is rejected, so one static band per
;;       body suffices for arbitrary nesting/repetition.
;;
;; ── Memory layout (all bands in the expr's [16, 8192) region) ────────
;;   [  0,  16)  reserved (null sentinel; arena state in runtime BSS)
;;   $eval band:
;;   [ 16,  40)  rodata: CelValue{INT, 21}      (the argument literal)
;;   [ 40,  64)  workspace: result slot for quad(21)   (top-level out_slot)
;;   double band  (disjoint — its own LayoutPass via rodata_base_override):
;;   [ 64,  88)  rodata: CelValue{INT, 2}       (double's literal `2`)
;;   [ 88, 112)  workspace: double's private scratch  (the reused band cell)
;;   quad band:
;;   [112, 136)  workspace: inner double(...) result   (quad's intermediate)
;;
;; Disjointness: $eval writes only [16,64), double only {88 + the
;; caller-supplied out_slot}, quad only {112 + the out_slots it forwards}.
;; No body writes a bare offset outside its band except the out_slot it
;; was handed — so the bands cannot clobber each other.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  ;; Runtime kernels the bodies reach — same surface as the expr module.
  (import "cel" "cel_int_mul_at_vv"
          (func $cel_int_mul_at_vv (param i32 i32 i32)))
  (import "cel" "cel_copy_slot"
          (func $cel_copy_slot (param i32 i32)))

  ;; rodata: CelValue{kind=CEL_INT(2), payload.i=21} at 16 (the arg literal)
  (data (i32.const 16)
        "\02\00\00\00" "\00\00\00\00"
        "\15\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  ;; rodata: CelValue{kind=CEL_INT(2), payload.i=2} at 64 (double's literal)
  (data (i32.const 64)
        "\02\00\00\00" "\00\00\00\00"
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  ;; @native double(x) = x * 2.  Local func; ABI (out_slot, arg0_slot)->().
  ;; Body band: rodata `2` at 64, private scratch at 88.
  (func $double_int (param $out_slot i32) (param $arg0_slot i32)
    ;; double.scratch(88) = arg0 * 2
    (call $cel_int_mul_at_vv
          (i32.const 88)
          (local.get $arg0_slot)
          (i32.const 64))
    ;; COPY-OUT: write the caller's out_slot from the private scratch.
    ;; This is the invariant that makes the single scratch cell reusable.
    (call $cel_copy_slot (local.get $out_slot) (i32.const 88)))

  ;; @native quad(x) = double(double(x)).  Calls double twice via kLocal.
  ;; Body band: intermediate slot for the inner result at 112.
  (func $quad_int (param $out_slot i32) (param $arg0_slot i32)
    ;; inner double(x): result into quad's intermediate slot 112
    (call $double_int (i32.const 112) (local.get $arg0_slot))
    ;; outer double(inner): result straight into quad's out_slot
    ;; (copy elision of the body-root call — it targets out_slot directly)
    (call $double_int (local.get $out_slot) (i32.const 112)))

  (func $eval (result i32)
    (call $arena_reset)
    ;; quad(21): out = workspace 40, arg = rodata 16.
    (call $quad_int (i32.const 40) (i32.const 16))
    ;; result CelValue lives at 40.
    (i32.const 40))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
