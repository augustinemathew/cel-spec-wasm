#!/usr/bin/env python3
"""Validate how Go panics cross the foreign-wasm boundary.

For each failure mode we record:
  - does the call raise (a wasmtime Trap) the host can catch?
  - what is the trap text (proc_exit / unreachable / oob)?
  - is the SAME instance still usable afterward (call `ok` -> 7)?

The last point is the load-bearing one for the trampoline: if a panic poisons
the instance, the engine must rebuild it; if not, kError + carry on is enough.
"""
import sys
from wasmtime import Engine, Store, Module, Linker, WasiConfig, Trap, ExitTrap

WASM = sys.argv[1] if len(sys.argv) > 1 else "paniccase/rules.wasm"


def fresh():
    e = Engine()
    s = Store(e)
    s.set_wasi(WasiConfig())
    l = Linker(e)
    l.define_wasi()
    m = Module.from_file(e, WASM)
    inst = l.instantiate(s, m)
    inst.exports(s)["_initialize"](s)
    return s, inst


def trycall(s, inst, name, *args):
    """Call export, classify outcome."""
    fn = inst.exports(s).get(name)
    if fn is None:
        return ("MISSING", None)
    try:
        r = fn(s, *args)
        return ("RET", r)
    except ExitTrap as t:
        return ("EXIT", t.code if hasattr(t, "code") else str(t).splitlines()[0])
    except Trap as t:
        return ("TRAP", str(t).splitlines()[0])
    except Exception as t:  # noqa
        return (type(t).__name__, str(t).splitlines()[0])


def reuse_check(s, inst):
    """After a failure, is the instance still alive? call ok -> expect 7."""
    return trycall(s, inst, "ok")


def main():
    print("=== sanity: ok() on a fresh instance ===")
    s, inst = fresh()
    print("  ok() ->", trycall(s, inst, "ok"))

    print("\n=== explicit panic() ===")
    s, inst = fresh()
    print("  doPanic() ->", trycall(s, inst, "doPanic"))
    print("  reuse ok() ->", reuse_check(s, inst))

    print("\n=== nil deref ===")
    s, inst = fresh()
    print("  doNilDeref() ->", trycall(s, inst, "doNilDeref"))
    print("  reuse ok() ->", reuse_check(s, inst))

    print("\n=== index out of range ===")
    s, inst = fresh()
    print("  doOOB(5) ->", trycall(s, inst, "doOOB", 5))
    print("  reuse ok() ->", reuse_check(s, inst))

    print("\n=== recovered panic (shim-side recover) ===")
    s, inst = fresh()
    print("  doRecovered(1) ->", trycall(s, inst, "doRecovered", 1))
    print("  doRecovered(0) ->", trycall(s, inst, "doRecovered", 0))
    print("  reuse ok() ->", reuse_check(s, inst))


if __name__ == "__main__":
    main()
