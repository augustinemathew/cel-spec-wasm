;; CEL source:  string(ip('192.168.0.1'))
;; Decl:        — (no free variables)
;;
;; M18 Slice-0 WAT-first trace freezing the net.IP value layout + the
;; parse / to-string kernel ABI.  `ip(s)` is a global call →
;; `cel_ip_parse_at_v`; `string(ipVal)` → `cel_ip_to_string_at_v`.
;;
;; NEW runtime kind + representation this trace FREEZES:
;;   - CEL_IP = 18 (next tail value after CEL_LIST_HOST = 17).
;;   - CelValue.payload gets a `uint32_t net_ref` arm — an arena byte
;;     offset to a parsed `NetIp` struct (mirrors arena_list.header_ptr):
;;        NetIp { uint32_t family;   // 4 = IPv4, 6 = IPv6
;;                uint8_t  addr[16]; }  // canonical bytes; v4 in [0,4)
;;     20 bytes, arena-allocated by cel_ip_parse_at_v.
;;   - Equality (`==`) is memcmp over {family, addr} so the v4 form and
;;     the hex v4-mapped form (`::ffff:c0a8:1`) normalise EQUAL — the
;;     parser writes family=4 + the 4 v4 bytes for both (see
;;     m18-ast-probe-findings.md note on form-sensitivity).
;;   - Parse failure → poison(CEL_ERROR, CEL_ERR_INVALID_ARGUMENT).
;;     The corpus's rich message strings are NOT compared by the
;;     conformance harness (runner.cc::CompareEvalError checks
;;     IsError() only), so a numeric code suffices.
;;
;; Kernel ABI (both `_at_v` = out + 1 value):
;;   cel.cel_ip_parse_at_v(out_slot, str_slot)      — i32, i32 → ()
;;   cel.cel_ip_to_string_at_v(out_slot, ip_slot)   — i32, i32 → ()
;;
;; Expected decoded result: {CEL_STRING, "192.168.0.1"}.
;;
;; NOTE: the kernels don't exist yet.  This trace ASSEMBLES (wasm-as)
;; to freeze the layout + ABI; it runs through wat_runner once Slice A
;; lands the kernels.
;;
;; Memory layout:
;;   [ 0,  8)  null sentinel
;;   [ 8, 16)  legacy arena cursor/limit slots (arena now in runtime BSS)
;;   [16, 40)  workspace: input CEL_STRING CelValue (s.ptr=200, len=11)
;;   [40, 64)  workspace: parsed CEL_IP CelValue (out of cel_ip_parse)
;;   [64, 88)  workspace: result CEL_STRING CelValue (out=64)
;;   [200,211) rodata: the input bytes "192.168.0.1"
;;   [88+]   bump arena (heap) — cel_ip_parse_at_v allocs the NetIp.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "cel_ip_parse_at_v" (func $cel_ip_parse_at_v (param i32 i32)))
  (import "cel" "cel_ip_to_string_at_v"
          (func $cel_ip_to_string_at_v (param i32 i32)))

  ;; Input CEL_STRING at slot 16: kind=5, payload.s = {ptr=200, len=11}.
  (data (i32.const 16)
        "\05\00\00\00" "\00\00\00\00"
        "\c8\00\00\00" "\0b\00\00\00" "\00\00\00\00\00\00\00\00")
  ;; String bytes in rodata at 200.
  (data (i32.const 200) "192.168.0.1")

  (func $eval (result i32)
    (call $arena_reset)
    ;; ip('192.168.0.1') → CEL_IP at slot 40.
    (call $cel_ip_parse_at_v (i32.const 40) (i32.const 16))
    ;; string(<that>) → CEL_STRING at slot 64.
    (call $cel_ip_to_string_at_v (i32.const 64) (i32.const 40))
    (i32.const 64))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
