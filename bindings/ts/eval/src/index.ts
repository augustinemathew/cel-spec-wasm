/**
 * `@celwasm/eval` public surface — mirrors `doc/user-guide.md`'s
 * `Engine` / `Program` / `Instance` / `Activation` / `Value`. There is no
 * `Compiler` here: compilation stays in C++; this package consumes
 * `Program` bytes (the §2 split boundary).
 */
export { Engine, EngineError } from './engine.js';
export type { EngineOptions } from './engine.js';
export { Instance, EvalError } from './instance.js';
export type { RuntimeHandles } from './instance.js';
export { Program } from './program.js';
export { Activation } from './activation.js';
export {
  Value,
  ValueError,
  asBool,
  asInt,
  asUint,
  asDouble,
  asString,
  asBytes,
  asMessage,
  asList,
  asMap,
  isNull,
  isError,
  isUnknown,
} from './value.js';
export {
  HostBackingError,
  scalarValue,
  messageValue,
  listValue,
  mapValue,
} from './host/backing.js';
export type {
  HostValue,
  MessageBacking,
  ListBacking,
  MapBacking,
} from './host/backing.js';
export {
  ObjectMessageBacking,
  ObjectListBacking,
  ObjectMapBacking,
  jsToHost,
} from './host/object-backing.js';
export {
  ProtoMessageBacking,
  ProtoListBacking,
  ProtoMapBacking,
} from './host/proto-backing.js';
export {
  ValueListBacking,
  ValueMapBacking,
  valueToHost,
  hostToValue,
} from './host/value-backing.js';
export { TypeRegistry, RegistryError } from './type-registry.js';
export {
  decodeValueAt,
  ArenaListBacking,
  ArenaMapBacking,
  ArenaDecodeError,
} from './host/arena-backing.js';
export type { DecodeContext } from './host/arena-backing.js';
export {
  CELVALUE_SIZE,
  CelKind,
  CelDecodeError,
  decodeCelValue,
  encodeInlineScalar,
} from './celvalue.js';
export type { CelValue } from './celvalue.js';
export {
  Repr,
  AbiDecodeError,
  findCustomSection,
  decodeCelAbi,
  variablesByName,
} from './abi.js';
export type { CelAbi, VariableEntry, FieldEntry, TypeEntry } from './abi.js';
export { ExternrefTable } from './externref.js';
export { WasiError, createWasiPreview1 } from './wasi-shim.js';
export type { WasiFn, WasiPreview1Imports } from './wasi-shim.js';
