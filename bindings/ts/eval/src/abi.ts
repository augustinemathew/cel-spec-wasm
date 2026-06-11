// Decode the `cel.abi` custom section of a compiled CEL Program.
//
// A compiled Program is "just wasm + a `cel.abi` descriptor": the
// compiler embeds one `CelAbi` protobuf message into a wasm custom
// section named `cel.abi`, and the host decodes it at load time to
// build its marshal table (which variable goes in which workspace
// slot, with which encoder).  This module is the TS port of the C++
// decoder in `eval/internal/abi_decode.cc:46-163` — it walks the wasm
// section table to locate `cel.abi`, then parses the protobuf payload
// field-by-field into the {@link CelAbi} contract from `./types.js`.
//
// The `attributes` table (partial-eval / unknowns) is intentionally
// skipped: unknowns are out of scope for the binding (§A.3), so the
// proto field is consumed-and-discarded by the section walk.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.4.3;
//       wire schema `abi/cel_abi.proto`; C++ reference `abi_decode.cc`.

import { Reader } from 'protobufjs';

import type { CelAbi, FieldEntry, TypeEntry, VariableEntry } from './types.js';
import { LinkMode } from './types.js';

/**
 * Thrown when the supplied bytes are not a valid wasm module or lack a
 * `cel.abi` custom section.  Carries a stable `.code` so callers can
 * distinguish the two failure modes without string-matching.
 */
export class CelAbiError extends Error {
  override readonly name = 'CelAbiError';
  readonly code: 'NOT_WASM' | 'NO_ABI_SECTION' | 'BAD_PAYLOAD';

  constructor(
    code: 'NOT_WASM' | 'NO_ABI_SECTION' | 'BAD_PAYLOAD',
    message: string,
  ) {
    super(message);
    this.code = code;
  }
}

// Wasm module header: 4-byte magic + 4-byte version (`abi_decode.cc:18-20`).
const WASM_MAGIC: readonly [number, number, number, number] = [
  0x00, 0x61, 0x73, 0x6d,
];
const WASM_VERSION = 1;

// Custom sections have section_id = 0 (`abi_decode.cc:23`).
const CUSTOM_SECTION_ID = 0;

// Upper bound on an unsigned LEB128-encoded u32 (5 × 7 bits;
// `abi_decode.cc:26`).
const MAX_U32_LEB_BYTES = 5;

// Protobuf wire type for a length-delimited field (`bytes`, `string`,
// embedded message) — the only non-varint wire type the CelAbi schema
// uses.  Unknown fields are skipped via `Reader.skipType`.
const WIRE_TYPE_LENGTH_DELIMITED = 2;

const CEL_ABI_SECTION_NAME = 'cel.abi';

/** A cursor into a byte stream for the manual LEB128 / section walk. */
interface Cursor {
  readonly bytes: Uint8Array;
  pos: number;
}

/**
 * Decode an unsigned LEB128 u32 at `cursor.pos`, advancing the cursor.
 * Mirrors `DecodeLeb128U32` in `abi_decode.cc:28-44`.
 */
function decodeLeb128U32(cursor: Cursor): number {
  let result = 0;
  let shift = 0;
  for (let i = 0; i < MAX_U32_LEB_BYTES; i++) {
    if (cursor.pos >= cursor.bytes.length) {
      throw new CelAbiError('NOT_WASM', 'cel.abi: truncated LEB128 u32');
    }
    const b = cursor.bytes[cursor.pos] ?? 0;
    cursor.pos++;
    // `>>> 0` keeps the running value an unsigned 32-bit integer.
    result = (result | ((b & 0x7f) << shift)) >>> 0;
    if ((b & 0x80) === 0) {
      return result;
    }
    shift += 7;
  }
  throw new CelAbiError('NOT_WASM', 'cel.abi: LEB128 u32 exceeds five bytes');
}

/**
 * Validate the 8-byte wasm header (magic + version) and position the
 * cursor at the first section.  Mirrors `CheckWasmHeader`
 * (`abi_decode.cc:46-63`).
 */
function checkWasmHeader(cursor: Cursor): void {
  const { bytes } = cursor;
  if (bytes.length < 8) {
    throw new CelAbiError(
      'NOT_WASM',
      'cel.abi: byte stream shorter than 8-byte wasm header',
    );
  }
  for (let i = 0; i < 4; i++) {
    if (bytes[i] !== WASM_MAGIC[i]) {
      throw new CelAbiError(
        'NOT_WASM',
        'cel.abi: wasm magic bytes do not match \\0asm',
      );
    }
  }
  // Version is a little-endian u32 at offset 4.
  const version =
    ((bytes[4] ?? 0) |
      ((bytes[5] ?? 0) << 8) |
      ((bytes[6] ?? 0) << 16) |
      ((bytes[7] ?? 0) << 24)) >>>
    0;
  if (version !== WASM_VERSION) {
    throw new CelAbiError(
      'NOT_WASM',
      `cel.abi: unsupported wasm version ${String(version)} (expected 1)`,
    );
  }
  cursor.pos = 8;
}

/**
 * Walk the wasm section table and return the payload bytes of the
 * custom section named {@link CEL_ABI_SECTION_NAME}.  Mirrors
 * `FindCustomSection` (`abi_decode.cc:65-106`).
 */
function findAbiSection(bytes: Uint8Array): Uint8Array {
  const cursor: Cursor = { bytes, pos: 0 };
  checkWasmHeader(cursor);

  while (cursor.pos < bytes.length) {
    const sectionId = bytes[cursor.pos] ?? 0;
    cursor.pos++;
    const sectionSize = decodeLeb128U32(cursor);
    if (cursor.pos + sectionSize > bytes.length) {
      throw new CelAbiError(
        'NOT_WASM',
        'cel.abi: section size runs past end of module',
      );
    }
    const sectionEnd = cursor.pos + sectionSize;

    if (sectionId !== CUSTOM_SECTION_ID) {
      cursor.pos = sectionEnd;
      continue;
    }

    // Custom section payload begins with a length-prefixed name.
    const nameLen = decodeLeb128U32(cursor);
    if (cursor.pos + nameLen > sectionEnd) {
      throw new CelAbiError(
        'NOT_WASM',
        'cel.abi: custom section name length runs past section end',
      );
    }
    const name = new TextDecoder('utf-8').decode(
      bytes.subarray(cursor.pos, cursor.pos + nameLen),
    );
    cursor.pos += nameLen;

    if (name === CEL_ABI_SECTION_NAME) {
      return bytes.subarray(cursor.pos, sectionEnd);
    }
    cursor.pos = sectionEnd;
  }

  throw new CelAbiError(
    'NO_ABI_SECTION',
    `cel.abi: custom section \`${CEL_ABI_SECTION_NAME}\` not found in wasm byte stream`,
  );
}

// ───────────────────────────────────────────────────────────────────
// Protobuf payload parsing.  Field tags are read directly from
// `abi/cel_abi.proto`; each `decode*` consumes exactly one
// length-delimited submessage via a sub-`Reader`.
// ───────────────────────────────────────────────────────────────────

/** Parse one `VariableEntry` submessage (`cel_abi.proto:57-63`). */
function decodeVariableEntry(reader: Reader, end: number): VariableEntry {
  let name = '';
  let localIndex = 0;
  let slotOffset = 0;
  let repr = 0;
  while (reader.pos < end) {
    const tag = reader.uint32();
    switch (tag >>> 3) {
      case 1: // name = 1
        name = reader.string();
        break;
      case 2: // local_index = 2
        localIndex = reader.uint32();
        break;
      case 3: // slot_offset = 3
        slotOffset = reader.uint32();
        break;
      case 4: // repr = 4
        repr = reader.uint32();
        break;
      default:
        reader.skipType(tag & 7);
        break;
    }
  }
  return { name, localIndex, slotOffset, repr };
}

/** Parse one `FieldEntry` submessage (`cel_abi.proto:86-91`). */
function decodeFieldEntry(reader: Reader, end: number): FieldEntry {
  let id = 0;
  let fieldNumber = 0;
  let name = '';
  let ownerFqn = '';
  while (reader.pos < end) {
    const tag = reader.uint32();
    switch (tag >>> 3) {
      case 1: // id = 1
        id = reader.uint32();
        break;
      case 2: // field_number = 2
        fieldNumber = reader.uint32();
        break;
      case 3: // name = 3
        name = reader.string();
        break;
      case 4: // owner_fqn = 4
        ownerFqn = reader.string();
        break;
      default:
        reader.skipType(tag & 7);
        break;
    }
  }
  return { id, fieldNumber, name, ownerFqn };
}

/** Parse one `TypeEntry` submessage (`cel_abi.proto:189-192`). */
function decodeTypeEntry(reader: Reader, end: number): TypeEntry {
  let id = 0;
  let fullyQualifiedName = '';
  while (reader.pos < end) {
    const tag = reader.uint32();
    switch (tag >>> 3) {
      case 1: // id = 1
        id = reader.uint32();
        break;
      case 2: // fully_qualified_name = 2
        fullyQualifiedName = reader.string();
        break;
      default:
        reader.skipType(tag & 7);
        break;
    }
  }
  return { id, fullyQualifiedName };
}

/**
 * Read a length-delimited submessage's exclusive end offset from
 * `reader`, advancing `reader.pos` past the length prefix.
 */
function subMessageEnd(reader: Reader): number {
  const len = reader.uint32();
  return reader.pos + len;
}

/**
 * Decode the `cel.abi` descriptor from a compiled CEL Program's wasm
 * bytes.  Locates the `cel.abi` custom section (validating the wasm
 * magic + version first), then parses the `CelAbi` protobuf payload
 * (`cel_abi.proto:216-275`) into the {@link CelAbi} contract.
 *
 * The `attributes` table (field 4) is skipped — unknowns are out of
 * scope (§A.3).
 *
 * @throws {CelAbiError} if `wasm` is not a valid wasm module
 *   (`code: 'NOT_WASM'`), lacks a `cel.abi` section
 *   (`code: 'NO_ABI_SECTION'`), or carries a malformed payload
 *   (`code: 'BAD_PAYLOAD'`).
 */
export function decodeAbi(wasm: Uint8Array): CelAbi {
  const payload = findAbiSection(wasm);

  const reader = Reader.create(payload);
  const end = reader.len;

  let version = 0;
  const variables: VariableEntry[] = [];
  const fields: FieldEntry[] = [];
  const types: TypeEntry[] = [];
  let runtimeAbiVersion = 0;
  let linkMode: LinkMode = LinkMode.DYNAMIC;

  try {
    while (reader.pos < end) {
      const tag = reader.uint32();
      switch (tag >>> 3) {
        case 1: // version = 1
          version = reader.uint32();
          break;
        case 2: // variables = 2 (repeated VariableEntry)
          variables.push(decodeVariableEntry(reader, subMessageEnd(reader)));
          break;
        case 3: // fields = 3 (repeated FieldEntry)
          fields.push(decodeFieldEntry(reader, subMessageEnd(reader)));
          break;
        case 4: // attributes = 4 — OUT OF SCOPE (unknowns); skip.
          reader.skipType(WIRE_TYPE_LENGTH_DELIMITED);
          break;
        case 5: // types = 5 (repeated TypeEntry)
          types.push(decodeTypeEntry(reader, subMessageEnd(reader)));
          break;
        case 6: // runtime_abi_version = 6
          runtimeAbiVersion = reader.uint32();
          break;
        case 7: // link_mode = 7 (enum, varint)
          linkMode =
            (reader.int32() as LinkMode) === LinkMode.STATIC
              ? LinkMode.STATIC
              : LinkMode.DYNAMIC;
          break;
        default:
          reader.skipType(tag & 7);
          break;
      }
    }
  } catch (err) {
    if (err instanceof CelAbiError) {
      throw err;
    }
    const detail = err instanceof Error ? err.message : String(err);
    throw new CelAbiError(
      'BAD_PAYLOAD',
      `cel.abi: malformed CelAbi payload — ${detail}`,
    );
  }

  return { version, variables, fields, types, runtimeAbiVersion, linkMode };
}
