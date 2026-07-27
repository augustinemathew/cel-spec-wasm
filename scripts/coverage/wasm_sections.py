#!/usr/bin/env python3
"""Dump imports, exports, and function types of a wasm core module.

Usage: wasm_sections.py <module.wasm>

Zero-dependency import/export-section parser.  The m38 probe used it to
enumerate the env::llvm_* gcov imports of an instrumented
cel_runtime.wasm and to check whether __indirect_function_table is
exported; generally useful for eyeballing any ABI surface without
building Binaryen."""
import sys, struct

data = open(sys.argv[1], 'rb').read()
assert data[:4] == b'\0asm', 'not a wasm module'
pos = 8

def leb(p):
    r = s = 0
    while True:
        b = data[p]; p += 1
        r |= (b & 0x7f) << s; s += 7
        if not b & 0x80: return r, p

def name(p):
    n, p = leb(p)
    return data[p:p+n].decode('utf-8', 'replace'), p+n

types = []
while pos < len(data):
    sid = data[pos]; pos += 1
    size, pos = leb(pos)
    end = pos + size
    p = pos
    if sid == 1:  # type section
        cnt, p = leb(p)
        for _ in range(cnt):
            assert data[p] == 0x60; p += 1
            np_, p = leb(p); params = [hex(data[p+i]) for i in range(np_)]; p += np_
            nr, p = leb(p); rets = [hex(data[p+i]) for i in range(nr)]; p += nr
            types.append((params, rets))
    elif sid == 2:  # import section
        cnt, p = leb(p)
        print(f'== IMPORTS ({cnt}) ==')
        for _ in range(cnt):
            mod, p = name(p); nm, p = name(p)
            kind = data[p]; p += 1
            if kind == 0:
                ti, p = leb(p)
                print(f'  func {mod}::{nm}  params={types[ti][0]} results={types[ti][1]}')
            elif kind == 1:
                p += 1; lim = data[p]; p += 1
                mn, p = leb(p)
                if lim & 1: mx, p = leb(p)
                print(f'  table {mod}::{nm}')
            elif kind == 2:
                lim = data[p]; p += 1
                mn, p = leb(p)
                if lim & 1: mx, p = leb(p)
                print(f'  memory {mod}::{nm}')
            elif kind == 3:
                p += 2
                print(f'  global {mod}::{nm}')
    elif sid == 7:  # export section
        cnt, p = leb(p)
        kinds = {0: 'func', 1: 'table', 2: 'mem', 3: 'global'}
        tables = []; mems = []; others = 0
        for _ in range(cnt):
            nm, p = name(p)
            kind = data[p]; p += 1
            idx, p = leb(p)
            if kind == 1: tables.append(nm)
            elif kind == 2: mems.append(nm)
            elif nm.startswith(('llvm', '__gcov', '__llvm', '__wasm')): print(f'  EXPORT {kinds[kind]} {nm}')
            else: others += 1
        print(f'== EXPORTS: {cnt} total; tables={tables} mems={mems} other-funcs={others} ==')
    pos = end
