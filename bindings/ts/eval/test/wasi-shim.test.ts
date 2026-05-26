import { describe, it, expect, vi } from 'vitest';
import { WasiError, createWasiPreview1 } from '../src/wasi-shim.js';

describe('createWasiPreview1', () => {
  it('random_get fills the requested span and returns success (0)', () => {
    const buf = new Uint8Array(32);
    const wasi = createWasiPreview1(() => buf);
    const spy = vi.spyOn(crypto, 'getRandomValues');
    const rc: number = wasi.random_get(4, 8);
    expect(rc).toBe(0);
    // It must hand crypto exactly the [ptr, ptr+len) sub-span.
    expect(spy).toHaveBeenCalledOnce();
    const arg = spy.mock.calls[0]?.[0];
    expect(arg).toBeInstanceOf(Uint8Array);
    expect((arg as Uint8Array).length).toBe(8);
    expect((arg as Uint8Array).byteOffset).toBe(4);
    spy.mockRestore();
  });

  it('proc_exit throws WasiError (guest abort)', () => {
    const wasi = createWasiPreview1(() => new Uint8Array(0));
    expect(() => wasi.proc_exit(1)).toThrow(WasiError);
    expect(() => wasi.proc_exit(1)).toThrow(/proc_exit\(1\)/);
  });

  it('clock_time_get writes the current wall-clock (u64 ns) and returns 0', () => {
    const buf = new Uint8Array(16);
    const wasi = createWasiPreview1(() => buf);
    const beforeNs = BigInt(Date.now()) * 1_000_000n;
    const rc = wasi.clock_time_get(0, 1000n, 4);
    expect(rc).toBe(0);
    const got = new DataView(buf.buffer).getBigUint64(4, true);
    // Within a generous window around "now" (ms-resolution source).
    expect(got).toBeGreaterThanOrEqual(beforeNs);
    expect(got).toBeLessThan(beforeNs + 60_000_000_000n); // +60s
  });

  it('declares exactly the 14 preview1 imports the runtime needs', () => {
    const wasi = createWasiPreview1(() => new Uint8Array(0));
    expect(Object.keys(wasi).sort()).toEqual(
      [
        'clock_time_get',
        'environ_get',
        'environ_sizes_get',
        'fd_close',
        'fd_fdstat_get',
        'fd_prestat_dir_name',
        'fd_prestat_get',
        'fd_read',
        'fd_seek',
        'fd_write',
        'poll_oneoff',
        'proc_exit',
        'random_get',
        'sched_yield',
      ].sort(),
    );
  });

  it('a declared-but-unsupported import throws if actually called', () => {
    const wasi = createWasiPreview1(() => new Uint8Array(0));
    expect(() => wasi.fd_close(3)).toThrow(WasiError);
    expect(() => wasi.fd_write(1, 0, 0, 0)).toThrow(
      /fd_write: unexpected call/,
    );
  });
});
