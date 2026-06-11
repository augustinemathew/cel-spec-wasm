// Public surface of the pure-TypeScript cel-wasm evaluator.
//
// The shared wire-format type contracts (`CelValue`, `CelInput`,
// `CelAbi`, the kind / error-code / offset constants, `MessageBacking`,
// `HostFunction`, `Program`) are the single source of truth in
// `./types`; every other package re-exports them from here.  The
// `Engine` / `Instance` runtime assembly lands in WI-1.5.
export * from './types.js';
