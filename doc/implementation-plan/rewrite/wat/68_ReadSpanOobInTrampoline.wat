;; Hand-authored adversarial fixture for the host-fn trampoline
;; bounds-check path — closes cleanup-backlog #42 / coverage gap 1.
;;
;; INVARIANT under test: every host-fn trampoline that lifts a
;; wasm-supplied (ptr, len) pair into an `absl::string_view` MUST
;; route the lift through `WasmtimeMemoryView::ReadSpan`, which
;; bounds-checks against `MemoryView::Size()` (see
;; `eval/internal/cel_host_wasmtime.h:127`).  A future trampoline
;; that bypasses `MemoryView` — e.g. reads directly via
;; `wasmtime_sharedmemory_data + ptr` without the IsInBounds
;; guard — would dereference 4 GiB past the wasm memory base,
;; SIGSEGV'ing on the guard page in the lucky case and exfiltrating
;; host bytes adjacent to the wasm reservation in the unlucky
;; case (the audit case from cleanup-backlog #36).
;;
;; A naturally compiled CEL expression cannot synthesise the
;; adversarial input: codegen + the activation marshal both go
;; through `WasmtimeArenaAllocator`, which only hands out in-bounds
;; offsets.  So we have to hand-author the wasm module that stages
;; the bad CelValue in a workspace slot and then calls the host
;; trampoline with that slot.
;;
;; ── Memory layout (expr region [0, 8192)) ───────────────────────
;;   [ 0, 16)  reserved (null sentinel + legacy arena slots)
;;   [16, 40)  arg0 CelValue — the ADVERSARIAL string:
;;               kind          = CEL_STRING (5)
;;               _pad          = 0
;;               payload.s.ptr = 0xFFFFFFFF   (max u32, OOB)
;;               payload.s.len = 64           (would copy 64 bytes)
;;               payload tail  = 0
;;   [40, 64)  out_slot — receives the CelValue the host writes
;;               (this fixture's callback writes
;;                {CEL_INT, payload.i = lifted_len} so the test
;;                can read the lifted length back via `Eval()`'s
;;                returned Value and assert it is 0 — proving
;;                `ReadSpan` returned the empty string_view).

(module
  ;; Same shared-memory + arena import surface as a normally compiled
  ;; expr module.  Engine::Plan binds these.
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))

  ;; The host fn under test.  Bound at Plan time by the test via
  ;; `Engine::AddFunction("probe_string", 2, callback)`.  The
  ;; callback's `HostCallContext::ArgString(0)` invokes
  ;; `WasmtimeMemoryView::ReadSpan(ptr=0xFFFFFFFF, len=64)`.
  (import "cel_fn" "probe_string"
    (func $probe_string (param i32 i32)))

  ;; arg0 CelValue — the adversarial string ptr/len.
  ;; Wire layout (LE), 24 bytes:
  ;;   u32 kind          = 0x05 0x00 0x00 0x00   (CEL_STRING)
  ;;   u32 _pad          = 0x00 0x00 0x00 0x00
  ;;   u32 payload.s.ptr = 0xFF 0xFF 0xFF 0xFF   (0xFFFFFFFF — the audit ptr)
  ;;   u32 payload.s.len = 0x40 0x00 0x00 0x00   (64)
  ;;   u64 union tail    = 0x00 ... 0x00
  (data (i32.const 16)
        "\05\00\00\00"
        "\00\00\00\00"
        "\ff\ff\ff\ff"
        "\40\00\00\00"
        "\00\00\00\00"
        "\00\00\00\00")

  ;; `eval` — invoke probe_string(out_slot=40, arg0_slot=16),
  ;; return offset 40 so `Instance::Eval()` decodes the CelValue
  ;; the callback wrote.  Resetting the arena up-front matches
  ;; the codegen prelude.
  (func $eval (result i32)
    (call $arena_reset)
    (call $probe_string
      (i32.const 40)    ;; out_slot
      (i32.const 16))   ;; arg0_slot — adversarial CelValue
    (i32.const 40))     ;; returned offset → decoded by Instance::Eval

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
