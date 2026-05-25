;; CEL source:  cidr('192.168.0.0/24').containsIP(ip('192.168.0.1'))
;; Decl:        — (no free variables)
;;
;; M18 Slice-0 WAT-first trace freezing the net.CIDR value layout + the
;; receiver-method `containsIP` kernel ABI.  Per the checker probe,
;; `<cidr>.containsIP(<ip>)` reaches as a receiver kCallExpr
;; (has_target=true) — codegen flattens target→arg0, same as string_ext
;; receivers — so the runtime kernel is a plain 3-slot call.
;;
;; NEW runtime kind + representation this trace FREEZES:
;;   - CEL_CIDR = 19 (after CEL_IP = 18).
;;   - payload.net_ref → arena `NetCidr` struct:
;;        NetCidr { uint32_t family;   // 4 / 6
;;                  uint32_t prefix;   // mask length (bits)
;;                  uint8_t  addr[16]; }  // network address bytes
;;     24 bytes, arena-allocated by cel_cidr_parse_at_v.
;;   - containsIP: family must match; compare the first `prefix` bits of
;;     the IP's addr against the CIDR's network addr.  A cross-family
;;     pair (v4 cidr, v6 ip) is `false`, not an error (corpus row
;;     `cidr('2001:db8::/32').containsIP(ip('192.168.1.1'))` → false).
;;   - `containsIP` is overloaded (net.IP arg AND string arg); this
;;     trace covers the net.IP-arg form (`net_cidr_containsIP_ip`).
;;
;; Kernel ABI:
;;   cel.cel_cidr_parse_at_v(out_slot, str_slot)               — i32×2 → ()
;;   cel.cel_ip_parse_at_v(out_slot, str_slot)                 — i32×2 → ()
;;   cel.cel_cidr_contains_ip_at_vv(out_slot, cidr_slot, ip_slot) — i32×3 → ()
;;
;; Expected decoded result: {CEL_BOOL, true}.
;;
;; NOTE: kernels not yet implemented — ASSEMBLES to freeze layout/ABI;
;; runs through wat_runner once Slice C lands the kernels.
;;
;; Memory layout:
;;   [ 0,  8)  null sentinel
;;   [ 8, 16)  legacy arena cursor/limit slots
;;   [16, 40)  CEL_STRING "192.168.0.0/24"  (ptr=200, len=14)
;;   [40, 64)  CEL_STRING "192.168.0.1"     (ptr=220, len=11)
;;   [64, 88)  parsed CEL_CIDR  (out of cel_cidr_parse)
;;   [88,112)  parsed CEL_IP    (out of cel_ip_parse)
;;   [112,136) result CEL_BOOL  (out=112)
;;   [200,214) rodata "192.168.0.0/24"
;;   [220,231) rodata "192.168.0.1"
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "cel_cidr_parse_at_v"
          (func $cel_cidr_parse_at_v (param i32 i32)))
  (import "cel" "cel_ip_parse_at_v" (func $cel_ip_parse_at_v (param i32 i32)))
  (import "cel" "cel_cidr_contains_ip_at_vv"
          (func $cel_cidr_contains_ip_at_vv (param i32 i32 i32)))

  ;; CEL_STRING "192.168.0.0/14"? no — "192.168.0.0/24" (14 bytes) @16.
  (data (i32.const 16)
        "\05\00\00\00" "\00\00\00\00"
        "\c8\00\00\00" "\0e\00\00\00" "\00\00\00\00\00\00\00\00")
  ;; CEL_STRING "192.168.0.1" (11 bytes) @40.
  (data (i32.const 40)
        "\05\00\00\00" "\00\00\00\00"
        "\dc\00\00\00" "\0b\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 200) "192.168.0.0/24")
  (data (i32.const 220) "192.168.0.1")

  (func $eval (result i32)
    (call $arena_reset)
    ;; cidr('192.168.0.0/24') → CEL_CIDR at 64.
    (call $cel_cidr_parse_at_v (i32.const 64) (i32.const 16))
    ;; ip('192.168.0.1') → CEL_IP at 88.
    (call $cel_ip_parse_at_v (i32.const 88) (i32.const 40))
    ;; <cidr>.containsIP(<ip>) → CEL_BOOL at 112.
    (call $cel_cidr_contains_ip_at_vv
          (i32.const 112) (i32.const 64) (i32.const 88))
    (i32.const 112))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
