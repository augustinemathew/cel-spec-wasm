#!/usr/bin/env python3
"""Host harness for the layered (3-file) foreign-Go glue probe.

Validates the packed scalar-return ABI from celfn_exports.go:
  return is an i64: low 32 bits = bool value, bit 32 = CELFN_ERR.
The host masks bit 32 -> kError; else lifts the bool. This proves the error
channel distinguishes a genuine `false` from a foreign failure (panic/decode).

Cases:
  isValidName_string: "Alice"->true, "bob"->false, ""->false
  isAdult_message_acme_User: User{age:20}->true, age:10->false,
                             and a CORRUPT proto -> CELFN_ERR (not false).
"""
import sys
from wasmtime import Engine, Store, Module, Linker, WasiConfig

WASM = sys.argv[1] if len(sys.argv) > 1 else "layered/rules.wasm"
CELFN_ERR = 1 << 32


def encode_user(age, name=""):
    out = bytearray()
    if age != 0:
        out.append(0x08); v = age
        while True:
            b = v & 0x7F; v >>= 7
            out.append(b | 0x80 if v else b)
            if not v:
                break
    if name:
        nb = name.encode(); out += bytes([0x12, len(nb)]) + nb
    return bytes(out)


def main():
    e = Engine(); s = Store(e); s.set_wasi(WasiConfig())
    l = Linker(e); l.define_wasi()
    inst = l.instantiate(s, Module.from_file(e, WASM))
    ex = inst.exports(s); ex["_initialize"](s)
    mem, realloc = ex["memory"], ex["celfn_realloc"]
    is_valid = ex["isValidName_string"]
    is_adult = ex["isAdult_message_acme_User"]

    def write(b):
        n = len(b)
        if n == 0:
            return 0, 0
        p = realloc(s, 0, 0, 1, n); mem.write(s, b, p); return p, n

    def unpack(raw):
        raw &= 0xFFFFFFFFFFFFFFFF  # wasmtime gives signed i64
        return ("ERR", None) if (raw & CELFN_ERR) else ("OK", bool(raw & 1))

    ok = True

    def check(label, raw, want):
        nonlocal ok
        kind, val = unpack(raw)
        got = (kind, val)
        passed = got == want
        ok = ok and passed
        print(f"  [{'PASS' if passed else 'FAIL'}] {label} -> {got}  (want {want})")

    print("=== isValidName_string ===")
    for s_, want in [("Alice", ("OK", True)), ("bob", ("OK", False)), ("", ("OK", False))]:
        p, n = write(s_.encode())
        check(f"isValidName({s_!r})", is_valid(s, p, n), want)

    print("=== isAdult_message_acme_User ===")
    for age, want in [(20, ("OK", True)), (10, ("OK", False))]:
        p, n = write(encode_user(age))
        check(f"isAdult(age={age})", is_adult(s, p, n), want)

    # Corrupt proto: a bogus tag/length must surface as CELFN_ERR, NOT false.
    p, n = write(b"\xff\xff\xff\xff")
    check("isAdult(<corrupt bytes>)", is_adult(s, p, n), ("ERR", None))

    print("LAYERED:", "ALL PASS" if ok else "FAILURES")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
