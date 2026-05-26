#!/usr/bin/env python3
"""Host harness for the string-case foreign-Go probe (`bool isValidName(string)`).

Stands in for the C++ `cel_call_foreign` trampoline (§5.2): instantiates the
Go wasm reactor with a WASI context, calls `_initialize`, then per CEL call:
  - celfn_realloc(0,0,1,len)  -> ptr   (allocate arg bytes in MODULE memory)
  - write the UTF-8 string bytes into the module's exported memory at ptr (lower)
  - isValidName(ptr,len)      -> 0/1   (the fixed-ABI export)
  - read the bool result (lift)

Validates: "Alice"->true, "bob"->false, ""->false.
"""
import sys
from wasmtime import Engine, Store, Module, Linker, WasiConfig

WASM = sys.argv[1] if len(sys.argv) > 1 else "strcase/rules.wasm"


def make_instance():
    engine = Engine()
    store = Store(engine)
    # Go wasip1 imports wasi_snapshot_preview1.* — provide a WASI context.
    store.set_wasi(WasiConfig())
    linker = Linker(engine)
    linker.define_wasi()
    module = Module.from_file(engine, WASM)
    instance = linker.instantiate(store, module)
    return store, instance


def call_is_valid_name(store, instance, s: str) -> bool:
    exports = instance.exports(store)
    memory = exports["memory"]
    realloc = exports["celfn_realloc"]
    is_valid = exports["isValidName"]
    init = exports.get("_initialize")
    if init is not None:
        init(store)  # reactor init (idempotent enough for the probe; once per instance)

    data = s.encode("utf-8")
    n = len(data)
    if n == 0:
        # zero-length: pass ptr=0,len=0; realloc returns 0 for newLen==0.
        return bool(is_valid(store, 0, 0))
    ptr = realloc(store, 0, 0, 1, n)
    memory.write(store, data, ptr)  # lower: copy bytes into module memory
    return bool(is_valid(store, ptr, n))


def main():
    store, instance = make_instance()
    # _initialize must run exactly once per instance; call here.
    instance.exports(store)["_initialize"](store)

    cases = [("Alice", True), ("bob", False), ("", False), ("Zoe", True), ("9x", False)]
    ok = True
    for s, want in cases:
        # fresh instance per call would also work; reuse here (realloc leaks, fine).
        got = call_is_valid_name_no_init(store, instance, s)
        status = "PASS" if got == want else "FAIL"
        if got != want:
            ok = False
        print(f"  [{status}] isValidName({s!r}) = {got}  (want {want})")
    print("STRCASE:", "ALL PASS" if ok else "FAILURES")
    sys.exit(0 if ok else 1)


def call_is_valid_name_no_init(store, instance, s: str) -> bool:
    exports = instance.exports(store)
    memory = exports["memory"]
    realloc = exports["celfn_realloc"]
    is_valid = exports["isValidName"]
    data = s.encode("utf-8")
    n = len(data)
    if n == 0:
        return bool(is_valid(store, 0, 0))
    ptr = realloc(store, 0, 0, 1, n)
    memory.write(store, data, ptr)
    return bool(is_valid(store, ptr, n))


if __name__ == "__main__":
    main()
