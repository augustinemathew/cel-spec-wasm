#!/usr/bin/env python3
"""Host harness for the proto-via-serialization foreign-Go probe.

Validates user-guide §8.5: a proto crosses the foreign boundary as protobuf
BINARY bytes. This host stands in for the C++ trampoline:
  - serialize acme.User -> protobuf wire bytes (host side)
  - celfn_realloc(0,0,1,len) -> ptr   (alloc in MODULE memory)
  - write the wire bytes into module memory at ptr (lower)
  - isAdult(ptr,len) -> 0/1            (glue proto.Unmarshal's them)

The critical claim under test: proto.Unmarshal actually LINKS + RUNS inside a
Go wasip1 wasm module (the protobuf runtime uses reflection/init registration).

Wire bytes are built by hand (no host protobuf dep needed) — protobuf binary
is language-agnostic, which is the whole point of §8.5.
Validates: age 20 -> true, age 10 -> false, plus a name+age message.
"""
import sys
from wasmtime import Engine, Store, Module, Linker, WasiConfig

WASM = sys.argv[1] if len(sys.argv) > 1 else "protocase/rules.wasm"


def encode_user(age: int, name: str = "") -> bytes:
    """Hand-encode acme.User{age:int32=1, name:string=2} protobuf wire bytes."""
    out = bytearray()
    if age != 0:  # proto3 omits zero-valued scalars
        out.append(0x08)  # field 1, wiretype 0 (varint)
        v = age
        while True:  # varint encode (age assumed small/non-negative here)
            b = v & 0x7F
            v >>= 7
            if v:
                out.append(b | 0x80)
            else:
                out.append(b)
                break
    if name:
        nb = name.encode("utf-8")
        out.append(0x12)  # field 2, wiretype 2 (length-delimited)
        out.append(len(nb))  # assumes len < 128
        out.extend(nb)
    return bytes(out)


def main():
    engine = Engine()
    store = Store(engine)
    store.set_wasi(WasiConfig())
    linker = Linker(engine)
    linker.define_wasi()
    module = Module.from_file(engine, WASM)
    instance = linker.instantiate(store, module)
    exports = instance.exports(store)
    exports["_initialize"](store)  # Go wasip1 reactor init, once

    memory = exports["memory"]
    realloc = exports["celfn_realloc"]
    is_adult = exports["isAdult"]

    def call_is_adult(age, name=""):
        wire = encode_user(age, name)
        n = len(wire)
        if n == 0:
            return bool(is_adult(store, 0, 0))
        ptr = realloc(store, 0, 0, 1, n)
        memory.write(store, wire, ptr)
        return bool(is_adult(store, ptr, n))

    cases = [
        (20, "", True),
        (10, "", False),
        (18, "Alice", True),
        (17, "Bob", False),
        (0, "Zero", False),
    ]
    ok = True
    for age, name, want in cases:
        got = call_is_adult(age, name)
        status = "PASS" if got == want else "FAIL"
        if got != want:
            ok = False
        label = f"User{{age:{age}" + (f", name:{name!r}" if name else "") + "}"
        print(f"  [{status}] isAdult({label}) = {got}  (want {want})  "
              f"wire={encode_user(age, name).hex()}")
    print("PROTOCASE:", "ALL PASS" if ok else "FAILURES")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
