import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

import { CelCompileError } from '../errors.js';

import {
  buildCompileArgs,
  CliBackend,
  resolveCelCli,
  type CompileRequest,
} from './cli-backend.js';

// The wasm preamble: `\0asm` magic + version 1 (little-endian u32).
const WASM_PREAMBLE = new Uint8Array([0x00, 0x61, 0x73, 0x6d, 1, 0, 0, 0]);

const FIXTURES_DIR = fileURLToPath(
  new URL('../../../eval/fixtures/', import.meta.url),
);
const GOLDEN_VAR_INT_ADD = new Uint8Array(
  readFileSync(`${FIXTURES_DIR}var_int_add.wasm`),
);

const CLI_PATH = resolveCelCli();
const describeWithCli = CLI_PATH === undefined ? describe.skip : describe;

describe('buildCompileArgs', () => {
  it('wraps the source in parens to neutralize a leading dash', () => {
    const req: CompileRequest = {
      source: '-x',
      vars: [{ name: 'x', type: 'int' }],
    };
    const args = buildCompileArgs(req, '/tmp/o.wasm');
    expect(args[0]).toBe('compile');
    expect(args[1]).toBe('(-x)');
  });

  it('emits one --var name:Type per declaration in order', () => {
    const req: CompileRequest = {
      source: 'x + y',
      vars: [
        { name: 'x', type: 'int' },
        { name: 'y', type: 'string' },
      ],
    };
    const args = buildCompileArgs(req, '/tmp/o.wasm');
    expect(args).toEqual([
      'compile',
      '(x + y)',
      '--var',
      'x:int',
      '--var',
      'y:string',
      '--output',
      '/tmp/o.wasm',
    ]);
  });

  it('forwards container and optimize level when present', () => {
    const req: CompileRequest = {
      source: '1 + 2',
      vars: [],
      container: 'foo.bar',
      optimizeLevel: 2,
    };
    const args = buildCompileArgs(req, '/tmp/o.wasm');
    expect(args).toContain('--container');
    expect(args).toContain('foo.bar');
    expect(args).toContain('--O');
    expect(args).toContain('2');
  });

  it('omits container/--O when not requested', () => {
    const args = buildCompileArgs({ source: '1', vars: [] }, '/tmp/o.wasm');
    expect(args).not.toContain('--container');
    expect(args).not.toContain('--O');
  });
});

describe('CliBackend constructor', () => {
  it('throws a build-instruction error when no CLI is found', () => {
    expect(() => new CliBackend('/no/such/cel/binary')).toThrow(
      /bazel build \/\/tools\/cel:cel/,
    );
  });
});

describeWithCli('CliBackend.compile (real cel CLI)', () => {
  const backend = new CliBackend(CLI_PATH);

  it('compiles a constant expression to wasm with the right preamble', async () => {
    const wasm = await backend.compile({ source: '1 + 2', vars: [] });
    expect(wasm.subarray(0, WASM_PREAMBLE.length)).toEqual(WASM_PREAMBLE);
  });

  it('compiles a variable expression to the committed golden bytes', async () => {
    const wasm = await backend.compile({
      source: 'x + y',
      vars: [
        { name: 'x', type: 'int' },
        { name: 'y', type: 'int' },
      ],
    });
    expect(wasm).toEqual(GOLDEN_VAR_INT_ADD);
  });

  it('rejects a syntactically invalid expression with a CelCompileError', async () => {
    await expect(
      backend.compile({ source: '1 +', vars: [] }),
    ).rejects.toBeInstanceOf(CelCompileError);
  });

  it('rejects a type-checking failure with a diagnostic', async () => {
    await expect(
      backend.compile({ source: '1 + "a"', vars: [] }),
    ).rejects.toThrow(CelCompileError);
  });
});
