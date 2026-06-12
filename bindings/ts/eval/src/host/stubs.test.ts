import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

import { CelErrorCode } from '../types.js';

import {
  CEL_ENV_MODULE,
  CelTzAccessorKind,
  computeCivilField,
  makeStubImports,
  makeTimestampTzAccessor,
  WASI_IMPORT_NAMES,
  WASI_MODULE,
  type CelLogRecord,
  type TzAccessorCodec,
} from './stubs.js';

// Resolve a golden fixture relative to this module (vitest runs from
// the package root; the fixtures live under eval/fixtures).
function fixturePath(name: string): string {
  return fileURLToPath(new URL(`../../fixtures/${name}`, import.meta.url));
}

function loadFixture(name: string): Uint8Array {
  return new Uint8Array(readFileSync(fixturePath(name)));
}

// Every fixture imports these three modules (empirical, build-fixtures).
const FIXTURES = [
  'int_add.wasm',
  'var_int_add.wasm',
  'string_concat.wasm',
  'list_exists.wasm',
  'map_index.wasm',
  'divide_by_zero.wasm',
] as const;

// A throwing cel_host group: test scaffolding only.  Instantiation links
// imports but does NOT call them, so trap-stubs are enough to reach a
// successful instantiate without owning the cel_host trampolines (other
// WIs).  Built from the fixture's own import list so the arity/name set
// always matches whatever the Program declares.
function trapCelHostFor(mod: WebAssembly.Module): WebAssembly.ModuleImports {
  const group: Record<string, WebAssembly.ImportValue> = {};
  for (const imp of WebAssembly.Module.imports(mod)) {
    if (imp.module === 'cel_host' && imp.kind === 'function') {
      group[imp.name] = (): never => {
        throw new Error(`cel_host.${imp.name} called (test trap)`);
      };
    }
  }
  return group;
}

describe('fixture import surface (empirical)', () => {
  it('every fixture imports exactly cel_env + cel_host + wasi', () => {
    for (const name of FIXTURES) {
      const mod = new WebAssembly.Module(loadFixture(name));
      const modules = new Set(
        WebAssembly.Module.imports(mod).map((i) => i.module),
      );
      expect([...modules].sort()).toEqual(['cel_env', 'cel_host', WASI_MODULE]);
    }
  });

  it('cel_env exports exactly cel_log', () => {
    const mod = new WebAssembly.Module(loadFixture('int_add.wasm'));
    const names = WebAssembly.Module.imports(mod)
      .filter((i) => i.module === CEL_ENV_MODULE)
      .map((i) => i.name);
    expect(names).toEqual(['cel_log']);
  });

  it('the WASI imports a fixture needs are covered by WASI_IMPORT_NAMES', () => {
    const mod = new WebAssembly.Module(loadFixture('int_add.wasm'));
    const needed = WebAssembly.Module.imports(mod)
      .filter((i) => i.module === WASI_MODULE)
      .map((i) => i.name);
    for (const name of needed) {
      expect(WASI_IMPORT_NAMES).toContain(name);
    }
  });
});

describe('makeStubImports', () => {
  it('covers the cel_env + WASI groups (every WASI name present)', () => {
    const imports = makeStubImports();
    const env = imports[CEL_ENV_MODULE];
    const wasi = imports[WASI_MODULE];
    expect(typeof env?.cel_log).toBe('function');
    for (const name of WASI_IMPORT_NAMES) {
      expect(typeof wasi?.[name]).toBe('function');
    }
  });

  it('WASI stubs return errno 0 (success)', () => {
    const wasi = makeStubImports()[WASI_MODULE] as Record<
      string,
      (...args: number[]) => number
    >;
    for (const name of WASI_IMPORT_NAMES) {
      if (name === 'proc_exit') continue;
      expect(wasi[name]?.(0, 0, 0, 0)).toBe(0);
    }
  });

  it('proc_exit throws rather than silently swallowing a guest exit', () => {
    const wasi = makeStubImports()[WASI_MODULE] as Record<
      string,
      (code: number) => never
    >;
    expect(() => {
      wasi.proc_exit?.(1);
    }).toThrow(/proc_exit/);
  });

  it.each(FIXTURES)(
    'lets the static Program %s INSTANTIATE (cel_env + wasi + trap cel_host)',
    async (name) => {
      const bytes = loadFixture(name);
      const mod = new WebAssembly.Module(bytes);
      const imports: WebAssembly.Imports = {
        ...makeStubImports(),
        cel_host: trapCelHostFor(mod),
      };
      const instance = await WebAssembly.instantiate(mod, imports);
      // The Program exports `$eval` (or `eval`) and `memory` (§A.4.4);
      // a successful instantiate is the deliverable's done-when.
      expect(instance).toBeInstanceOf(WebAssembly.Instance);
      expect(instance.exports.memory).toBeInstanceOf(WebAssembly.Memory);
    },
  );
});

describe('cel_env.cel_log sink', () => {
  // Builds a memory holding a file/fn/fmt string layout and drives the
  // nine-i32 cel_log import directly, mirroring the wire contract
  // (runtime/cel_log.h:46-50).
  function memoryWith(bytes: Uint8Array): {
    memory: WebAssembly.Memory;
    write: (off: number, data: Uint8Array) => void;
  } {
    const memory = new WebAssembly.Memory({ initial: 1 });
    new Uint8Array(memory.buffer).set(bytes);
    return {
      memory,
      write: (off, data) => {
        new Uint8Array(memory.buffer).set(data, off);
      },
    };
  }

  function celLogOf(imports: WebAssembly.Imports): (...a: number[]) => void {
    const fn = imports[CEL_ENV_MODULE]?.cel_log;
    if (typeof fn !== 'function') throw new Error('cel_log missing');
    return fn as (...a: number[]) => void;
  }

  it('is a no-op when no logger is supplied (does not throw)', () => {
    const memory = new WebAssembly.Memory({ initial: 1 });
    const log = celLogOf(makeStubImports({ memory: () => memory }));
    expect(() => {
      log(0, 0, 0, 0, 1, 0, 0, 0, 0);
    }).not.toThrow();
  });

  it('is a no-op when a logger is set but no memory resolves', () => {
    let calls = 0;
    const log = celLogOf(makeStubImports({ log: () => void calls++ }));
    log(0, 0, 0, 0, 1, 0, 0, 0, 0);
    expect(calls).toBe(0);
  });

  it('delivers a formatted message to the optional logger', () => {
    // Layout: file at 0, fn at 16, fmt at 32, no argv.
    const enc = new TextEncoder();
    const { memory } = memoryWith(new Uint8Array(0));
    const mem = new Uint8Array(memory.buffer);
    mem.set(enc.encode('expr.cel'), 0); // file_ptr=0  file_len=8
    mem.set(enc.encode('eval'), 16); // fn_ptr=16   fn_len=4
    mem.set(enc.encode('hello world'), 32); // fmt_ptr=32 fmt_len=11

    const records: CelLogRecord[] = [];
    const log = celLogOf(
      makeStubImports({ log: (r) => records.push(r), memory: () => memory }),
    );
    log(0, 8, 16, 4, 42, 32, 11, 0, 0);

    expect(records).toHaveLength(1);
    expect(records[0]).toEqual({
      file: 'expr.cel',
      fn: 'eval',
      line: 42,
      message: 'hello world',
    });
  });

  it('formats %s / %d / %u / %b directives from argv slots', () => {
    const enc = new TextEncoder();
    const memory = new WebAssembly.Memory({ initial: 1 });
    const mem = new Uint8Array(memory.buffer);
    const view = new DataView(memory.buffer);
    // fmt at 0; the %s string payload at 64; argv at 128.
    const fmt = 'n=%d u=%u b=%b s=%s';
    mem.set(enc.encode(fmt), 0);
    mem.set(enc.encode('hi'), 64);

    // Four argv slots of 16 bytes: tag at +0, payload at +8.
    const argvPtr = 128;
    const writeSlot = (i: number, tag: number, payload: bigint): void => {
      const base = argvPtr + i * 16;
      view.setUint32(base, tag, true);
      view.setBigUint64(base + 8, payload, true);
    };
    writeSlot(0, 2 /* INT */, BigInt.asUintN(64, -7n));
    writeSlot(1, 3 /* UINT */, 9n);
    writeSlot(2, 5 /* BOOL */, 1n);
    // STR slot: payload = ptr(64) | len(2)<<32
    writeSlot(3, 1 /* STR */, 64n | (2n << 32n));

    const records: CelLogRecord[] = [];
    const log = celLogOf(
      makeStubImports({ log: (r) => records.push(r), memory: () => memory }),
    );
    log(0, 0, 0, 0, 0, 0, fmt.length, argvPtr, 4);

    expect(records[0]?.message).toBe('n=-7 u=9 b=true s=hi');
  });
});

describe('computeCivilField (cel_timestamp_tz_accessor math)', () => {
  // 2009-02-13T23:31:30Z = epoch 1234567890.  In UTC and in a +zone the
  // civil fields differ; assert both to pin the zone projection.
  const T = 1234567890n;

  it('projects civil fields in UTC', () => {
    const f = (k: CelTzAccessorKind): number => {
      const r = computeCivilField(T, 0, 'UTC', k);
      if (!r.ok) throw new Error('unexpected error');
      return r.value;
    };
    expect(f(CelTzAccessorKind.YEAR)).toBe(2009);
    expect(f(CelTzAccessorKind.MONTH)).toBe(1); // 0-based: February
    expect(f(CelTzAccessorKind.DAY_OF_MONTH_1)).toBe(13);
    expect(f(CelTzAccessorKind.DAY_OF_MONTH)).toBe(12); // 0-based
    expect(f(CelTzAccessorKind.HOURS)).toBe(23);
    expect(f(CelTzAccessorKind.MINUTES)).toBe(31);
    expect(f(CelTzAccessorKind.SECONDS)).toBe(30);
    expect(f(CelTzAccessorKind.DAY_OF_WEEK)).toBe(5); // Friday
  });

  it('shifts fields into a named IANA zone (America/Los_Angeles)', () => {
    // PST is UTC-8 in February → 15:31 local, still the 13th.
    const hours = computeCivilField(
      T,
      0,
      'America/Los_Angeles',
      CelTzAccessorKind.HOURS,
    );
    expect(hours).toEqual({ ok: true, value: 15 });
    const day = computeCivilField(
      T,
      0,
      'America/Los_Angeles',
      CelTzAccessorKind.DAY_OF_MONTH_1,
    );
    expect(day).toEqual({ ok: true, value: 13 });
  });

  it('honors DST via Intl (Europe/London summer = UTC+1)', () => {
    // 2021-07-01T12:00:00Z, London is BST (UTC+1) → 13:00 local.
    const summer = 1625140800n;
    const hours = computeCivilField(
      summer,
      0,
      'Europe/London',
      CelTzAccessorKind.HOURS,
    );
    expect(hours).toEqual({ ok: true, value: 13 });
  });

  it('returns milliseconds from sub-second nanos', () => {
    const r = computeCivilField(
      T,
      123_000_000,
      'UTC',
      CelTzAccessorKind.MILLISECONDS,
    );
    expect(r).toEqual({ ok: true, value: 123 });
  });

  it('day-of-year is 0-based (Jan 1 UTC → 0)', () => {
    // 2021-01-01T00:00:00Z = epoch 1609459200.
    const r = computeCivilField(
      1609459200n,
      0,
      'UTC',
      CelTzAccessorKind.DAY_OF_YEAR,
    );
    expect(r).toEqual({ ok: true, value: 0 });
  });

  it('rejects an unknown IANA zone with INVALID_ARGUMENT', () => {
    const r = computeCivilField(T, 0, 'Not/AZone', CelTzAccessorKind.YEAR);
    expect(r).toEqual({ ok: false, code: CelErrorCode.INVALID_ARGUMENT });
  });

  // Fixed-offset zones (doc/langdef.md §"Timezones": explicit hour/minute
  // offsets from UTC) are arithmetic, mirroring the C++ host's
  // `ResolveTimeZone` (eval/internal/cel_host.cc): sign-optional `HH:MM`
  // (the unsigned form is `+`, per cel-cpp's time_functions.cc), `Z`,
  // and `UTC`.
  describe('fixed-offset zones', () => {
    const f = (zone: string, k: CelTzAccessorKind): number => {
      const r = computeCivilField(T, 0, zone, k);
      if (!r.ok) throw new Error(`unexpected error for zone '${zone}'`);
      return r.value;
    };

    it("accepts the unsigned form '02:00' as +02:00 (conformance timestamps/timezones)", () => {
      // 23:31:30Z + 02:00 = 01:31:30 the next day.
      expect(f('02:00', CelTzAccessorKind.HOURS)).toBe(1);
      expect(f('02:00', CelTzAccessorKind.DAY_OF_MONTH_1)).toBe(14);
      expect(f('02:00', CelTzAccessorKind.DAY_OF_WEEK)).toBe(6); // Saturday
    });

    it("treats 'Z' and '-00:00' as UTC", () => {
      expect(f('Z', CelTzAccessorKind.HOURS)).toBe(23);
      expect(f('-00:00', CelTzAccessorKind.HOURS)).toBe(23);
    });

    it('handles a negative half-hour offset (-02:30)', () => {
      // 23:31:30Z - 02:30 = 21:01:30 same day.
      expect(f('-02:30', CelTzAccessorKind.HOURS)).toBe(21);
      expect(f('-02:30', CelTzAccessorKind.MINUTES)).toBe(1);
      expect(f('-02:30', CelTzAccessorKind.DAY_OF_MONTH_1)).toBe(13);
    });

    it('handles a positive 45-minute offset (+05:45, Kathmandu-shaped)', () => {
      // 23:31:30Z + 05:45 = 05:16:30 the next day.
      expect(f('+05:45', CelTzAccessorKind.HOURS)).toBe(5);
      expect(f('+05:45', CelTzAccessorKind.MINUTES)).toBe(16);
      expect(f('+05:45', CelTzAccessorKind.DAY_OF_MONTH_1)).toBe(14);
    });

    it('rejects out-of-range and malformed offsets with INVALID_ARGUMENT', () => {
      for (const zone of [
        '24:00',
        '02:60',
        '2:00',
        '+2:00',
        '02:0',
        '++02:00',
      ]) {
        expect(computeCivilField(T, 0, zone, CelTzAccessorKind.HOURS)).toEqual({
          ok: false,
          code: CelErrorCode.INVALID_ARGUMENT,
        });
      }
    });
  });

  it('projects across a DST transition in an IANA zone (America/Los_Angeles)', () => {
    // The US spring-forward instant: 2023-03-12 02:00 PST → 03:00 PDT
    // (10:00:00Z).  One second before, local civil time is 01:59:59
    // (UTC-8); at the instant it is 03:00:00 (UTC-7).
    const before = 1678615199n; // 2023-03-12T09:59:59Z
    const after = 1678615200n; // 2023-03-12T10:00:00Z
    const zone = 'America/Los_Angeles';
    expect(computeCivilField(before, 0, zone, CelTzAccessorKind.HOURS)).toEqual(
      { ok: true, value: 1 },
    );
    expect(computeCivilField(after, 0, zone, CelTzAccessorKind.HOURS)).toEqual({
      ok: true,
      value: 3,
    });
  });
});

describe('makeTimestampTzAccessor trampoline', () => {
  it('reads ts + zone, computes, and writes an INT via the codec', () => {
    let written: { slot: number; value: number } | undefined;
    const codec: TzAccessorCodec = {
      readTimestamp: () => ({ epochSeconds: 1234567890n, nanos: 0 }),
      readZone: () => 'UTC',
      writeInt: (slot, value) => void (written = { slot, value }),
      writeError: () => {
        throw new Error('unexpected error write');
      },
    };
    const tz = makeTimestampTzAccessor(codec);
    tz(100 /* out */, 8 /* ts */, 16 /* tz */, CelTzAccessorKind.YEAR);
    expect(written).toEqual({ slot: 100, value: 2009 });
  });

  it('writes INVALID_ARGUMENT for an unknown zone', () => {
    let errCode: number | undefined;
    const codec: TzAccessorCodec = {
      readTimestamp: () => ({ epochSeconds: 0n, nanos: 0 }),
      readZone: () => 'Bogus/Zone',
      writeInt: () => {
        throw new Error('unexpected int write');
      },
      writeError: (_slot, code) => void (errCode = code),
    };
    const tz = makeTimestampTzAccessor(codec);
    tz(0, 0, 0, CelTzAccessorKind.YEAR);
    expect(errCode).toBe(CelErrorCode.INVALID_ARGUMENT);
  });
});
