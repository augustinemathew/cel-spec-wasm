// Serialize a {@link CompileRequest} into the proto message the compiler
// wasm consumes: one `celwasm.compile.CompileRequest` buffer is the whole
// `cew_compile` input (source + every compile option), replacing the
// ad-hoc length-prefixed records blob that preceded it.
//
// The schema below is the protobufjs JSON-descriptor mirror of
// `bindings/c/compiler/compile_request.proto` — field names, numbers, and
// types must match it exactly (the wire is field-number-addressed, but a
// drifted name here is how a field silently stops being set).  Encoding
// goes through protobufjs, never hand-rolled bytes.
//
// Because the proto `source` field is length-delimited, an expression
// carrying an embedded NUL byte (a CEL `b'\x00'` byte literal) survives
// the crossing — the records blob's NUL-terminated source could not.

// Default (CJS) import, not named: protobufjs is CommonJS, and plain
// Node ESM (e.g. scripts/gen-dynamic-fixtures.mjs importing the built
// dist) does not synthesize `Root` as a named export the way the
// vitest/tsc interop does.
import protobuf from 'protobufjs';
import type { INamespace, Type } from 'protobufjs';

import type { CompileRequest } from './backend.js';

// `bindings/c/compiler/compile_request.proto` as a protobufjs JSON
// descriptor (embedded so no .proto file ships with the package).
const COMPILE_REQUEST_SCHEMA: INamespace = {
  nested: {
    celwasm: {
      nested: {
        compile: {
          nested: {
            VariableDecl: {
              fields: {
                name: { type: 'string', id: 1 },
                type: { type: 'string', id: 2 },
              },
            },
            // Numbering matches `celwasm.abi.LinkMode` (abi/cel_abi.proto).
            LinkMode: {
              values: { LINK_MODE_DYNAMIC: 0, LINK_MODE_STATIC: 1 },
            },
            CompileRequest: {
              fields: {
                source: { type: 'string', id: 1 },
                variables: { rule: 'repeated', type: 'VariableDecl', id: 2 },
                fns: { rule: 'repeated', type: 'string', id: 3 },
                container: { type: 'string', id: 4 },
                optimize_level: { type: 'uint32', id: 5 },
                link_mode: { type: 'LinkMode', id: 6 },
                descriptor_set: { type: 'bytes', id: 7 },
              },
            },
          },
        },
      },
    },
  },
};

// `link_mode` wire values (compile_request.proto `LinkMode`, which
// mirrors `celwasm.abi.LinkMode` — cel_abi.proto).  Literals rather than
// an import: eval's `LinkMode` is a cross-package `const enum`, not
// importable by value under `isolatedModules`.
const LINK_MODE_DYNAMIC = 0;
const LINK_MODE_STATIC = 1;

/**
 * The resolved `celwasm.compile.CompileRequest` protobufjs type.
 * Exported for the round-trip tests, which decode the encoded bytes
 * through the same schema and compare fields.
 */
export const compileRequestType: Type = protobuf.Root.fromJSON(
  COMPILE_REQUEST_SCHEMA,
).lookupType('celwasm.compile.CompileRequest');

/**
 * Encode a {@link CompileRequest} as serialized
 * `celwasm.compile.CompileRequest` bytes — the single buffer
 * `cew_compile` parses.
 *
 * `link_mode` is always populated explicitly (static unless the request
 * says `'dynamic'`): the proto zero value is DYNAMIC, so the static
 * default the public API documents must be stated on the wire.
 */
export function encodeCompileRequest(request: CompileRequest): Uint8Array {
  const message = compileRequestType.create({
    source: request.source,
    variables: request.vars.map((v) => ({ name: v.name, type: v.type })),
    fns: [...(request.fns ?? [])],
    ...(request.container !== undefined
      ? { container: request.container }
      : {}),
    ...(request.optimizeLevel !== undefined
      ? { optimize_level: request.optimizeLevel }
      : {}),
    link_mode:
      request.linkMode === 'dynamic' ? LINK_MODE_DYNAMIC : LINK_MODE_STATIC,
    ...(request.descriptorSetBytes !== undefined
      ? { descriptor_set: request.descriptorSetBytes }
      : {}),
  });
  return compileRequestType.encode(message).finish();
}
