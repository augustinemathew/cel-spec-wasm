// Round-trip tests for the CompileRequest proto encoder: every encoded
// request must decode (through the same schema) back to the fields the
// request carried, and the field numbers must match
// `bindings/c/compiler/compile_request.proto` (pinned byte-level below —
// the schema is the wire contract with `cew_compile`).

import { describe, expect, it } from 'vitest';

import type { CompileRequest } from './backend.js';
import { compileRequestType, encodeCompileRequest } from './compile-request.js';

/** The decoded shape (proto field names, per compile_request.proto). */
interface DecodedRequest {
  readonly source: string;
  readonly variables: readonly { name: string; type: string }[];
  readonly fns: readonly string[];
  readonly container: string;
  readonly optimize_level: number;
  readonly link_mode: number;
  readonly descriptor_set: Uint8Array;
}

function decode(bytes: Uint8Array): DecodedRequest {
  return compileRequestType.decode(bytes) as unknown as DecodedRequest;
}

// Wire values of compile_request.proto's LinkMode (mirrors
// `celwasm.abi.LinkMode`, abi/cel_abi.proto).
const LINK_MODE_DYNAMIC = 0;
const LINK_MODE_STATIC = 1;

// Proto wire tags for CompileRequest's fields: (field_number << 3) | wire
// type (compile_request.proto field numbers; LEN = 2, VARINT = 0).
const TAG_SOURCE = (1 << 3) | 2; // 0x0a
const TAG_VARIABLES = (2 << 3) | 2; // 0x12
const TAG_FNS = (3 << 3) | 2; // 0x1a
const TAG_CONTAINER = (4 << 3) | 2; // 0x22
const TAG_OPTIMIZE_LEVEL = (5 << 3) | 0; // 0x28
const TAG_LINK_MODE = (6 << 3) | 0; // 0x30
const TAG_DESCRIPTOR_SET = (7 << 3) | 2; // 0x3a

describe('encodeCompileRequest', () => {
  it('round-trips a minimal request (source only) with the static default', () => {
    const bytes = encodeCompileRequest({ source: '1 + 2', vars: [] });
    const decoded = decode(bytes);
    expect(decoded.source).toBe('1 + 2');
    expect(decoded.variables).toEqual([]);
    expect(decoded.fns).toEqual([]);
    expect(decoded.container).toBe('');
    expect(decoded.optimize_level).toBe(0);
    // The public API's default is static, stated explicitly on the wire
    // (the proto zero value is dynamic).
    expect(decoded.link_mode).toBe(LINK_MODE_STATIC);
    expect(bytes.includes(TAG_LINK_MODE)).toBe(true);
  });

  it('round-trips every option present', () => {
    const fds = Uint8Array.of(0x0a, 0x00, 0xff, 0x10);
    const request: CompileRequest = {
      source: 'x + y',
      vars: [
        { name: 'x', type: 'int' },
        { name: 'y', type: 'list<int>' },
      ],
      fns: ['string @host.upper(this string s);', 'int @host.f(int a);'],
      container: 'com.example',
      optimizeLevel: 3,
      linkMode: 'static',
      descriptorSetBytes: fds,
    };
    const decoded = decode(encodeCompileRequest(request));
    expect(decoded.source).toBe('x + y');
    expect(
      decoded.variables.map((v) => ({ name: v.name, type: v.type })),
    ).toEqual([
      { name: 'x', type: 'int' },
      { name: 'y', type: 'list<int>' },
    ]);
    expect([...decoded.fns]).toEqual([
      'string @host.upper(this string s);',
      'int @host.f(int a);',
    ]);
    expect(decoded.container).toBe('com.example');
    expect(decoded.optimize_level).toBe(3);
    expect(decoded.link_mode).toBe(LINK_MODE_STATIC);
    expect(new Uint8Array(decoded.descriptor_set)).toEqual(fds);
  });

  it('encodes dynamic link mode as the proto zero value', () => {
    const bytes = encodeCompileRequest({
      source: '1',
      vars: [],
      linkMode: 'dynamic',
    });
    const decoded = decode(bytes);
    expect(decoded.link_mode).toBe(LINK_MODE_DYNAMIC);
    // The encoder always states link_mode (protobufjs serializes a set
    // own-property even at the zero value): the trailing record is
    // [tag, 0].
    expect(bytes[bytes.length - 2]).toBe(TAG_LINK_MODE);
    expect(bytes[bytes.length - 1]).toBe(LINK_MODE_DYNAMIC);
  });

  it("preserves an embedded NUL byte in the source (b'\\x00' literal)", () => {
    // Built via fromCharCode so the test file carries no literal NUL.
    const nul = String.fromCharCode(0);
    const source = `b'${nul}' < b'${String.fromCharCode(1)}'`;
    const bytes = encodeCompileRequest({ source, vars: [] });
    expect(decode(bytes).source).toBe(source);
    // The length-delimited source field carries the raw NUL byte.
    expect(bytes.includes(0)).toBe(true);
  });

  it('preserves multi-byte UTF-8 in the source', () => {
    const source = "'héllo' + '🎉'";
    const bytes = encodeCompileRequest({ source, vars: [] });
    expect(decode(bytes).source).toBe(source);
  });

  it('round-trips descriptor-set bytes spanning the full byte range', () => {
    const fds = new Uint8Array(256);
    for (let i = 0; i < 256; i++) fds[i] = i;
    const decoded = decode(
      encodeCompileRequest({ source: '1', vars: [], descriptorSetBytes: fds }),
    );
    expect(new Uint8Array(decoded.descriptor_set)).toEqual(fds);
  });

  it('omits absent optional fields from the wire', () => {
    const bytes = encodeCompileRequest({
      source: '1',
      vars: [],
      linkMode: 'dynamic',
    });
    expect(bytes.includes(TAG_VARIABLES)).toBe(false);
    expect(bytes.includes(TAG_FNS)).toBe(false);
    expect(bytes.includes(TAG_CONTAINER)).toBe(false);
    expect(bytes.includes(TAG_OPTIMIZE_LEVEL)).toBe(false);
    expect(bytes.includes(TAG_DESCRIPTOR_SET)).toBe(false);
  });

  it('pins the wire field numbers to compile_request.proto', () => {
    // protobufjs writes set fields in field-number order, and the encoder
    // always sets `source` and `link_mode` — so with an empty source the
    // record at byte 2 (after the 2-byte empty-source record `[0x0a, 0]`)
    // is the field under test.
    const sourceOnly = encodeCompileRequest({
      source: 'z',
      vars: [],
      linkMode: 'dynamic',
    });
    expect(sourceOnly[0]).toBe(TAG_SOURCE);

    const withVar = encodeCompileRequest({
      source: '',
      vars: [{ name: 'a', type: 'int' }],
      linkMode: 'dynamic',
    });
    expect(withVar[2]).toBe(TAG_VARIABLES);

    const withFn = encodeCompileRequest({
      source: '',
      vars: [],
      fns: ['f'],
      linkMode: 'dynamic',
    });
    expect(withFn[2]).toBe(TAG_FNS);

    const withContainer = encodeCompileRequest({
      source: '',
      vars: [],
      container: 'c',
      linkMode: 'dynamic',
    });
    expect(withContainer[2]).toBe(TAG_CONTAINER);

    const withOptimize = encodeCompileRequest({
      source: '',
      vars: [],
      optimizeLevel: 2,
      linkMode: 'dynamic',
    });
    expect(withOptimize[2]).toBe(TAG_OPTIMIZE_LEVEL);

    const withLink = encodeCompileRequest({ source: '', vars: [] });
    expect(withLink[2]).toBe(TAG_LINK_MODE);

    // descriptor_set (id 7) follows link_mode (id 6) on the wire, so it
    // sits after the empty-source record AND the `[0x30, 0]` link record.
    const withFds = encodeCompileRequest({
      source: '',
      vars: [],
      descriptorSetBytes: Uint8Array.of(1),
      linkMode: 'dynamic',
    });
    expect(withFds[4]).toBe(TAG_DESCRIPTOR_SET);
  });
});
