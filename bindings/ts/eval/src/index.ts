// Public surface of the pure-TypeScript cel-wasm evaluator.
//
// The shared wire-format type contracts (`CelValue`, `CelInput`,
// `CelAbi`, the kind / error-code / offset constants, `MessageBacking`,
// `HostFunction`, `Program`) are the single source of truth in
// `./types`; every other package re-exports them from here.  The
// `Engine` / `Instance` runtime assembly lands in WI-1.5.
export * from './types.js';

// Decode a Program's embedded `cel.abi` descriptor (the compiler binding
// and any Program consumer need this to read a Program's variable table).
export { decodeAbi, CelAbiError } from './abi.js';

// The runtime assembly — the canonical §A.5 API.  `Engine.create` →
// `engine.plan(program)` → `instance.eval(activation)` is the pure-TS
// evaluation path: instantiate a static Program, marshal an activation,
// run `$eval`, decode the result.
export { Engine } from './engine.js';
export type { EngineOptions } from './engine.js';
export { CelFnDeclError } from './celfn-decl.js';
export { CelRuntimeLoadError } from './runtime-loader.js';
export { Instance, CelEvalError } from './instance.js';
export type { Activation } from './activation.js';
export { CelMarshalError } from './marshal.js';
