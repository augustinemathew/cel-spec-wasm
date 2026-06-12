// First-party proto surface for sibling binding packages (the conformance
// harness in particular).  The eval binding owns the descriptor-backed
// message decode rules (`messageToObject`, WKT peeling, presence); a sibling
// that must build a *comparable* expected value — e.g. the conformance
// harness materialising an `object_value` matcher's proto into the same
// decoded shape `instance.eval` returns — has to decode through the IDENTICAL
// code path, not re-derive the rules.  This barrel re-exports just that
// decode + descriptor surface so the rules stay single-sourced in
// `proto/backing.ts` / `proto/descriptors.ts`.
//
// This is a first-party-internal entrypoint (the `@cel-wasm/eval/proto`
// subpath), not part of the curated public value API in `index.ts`; an
// external consumer instantiates messages through `Engine.create` +
// activation, never this barrel.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.4.6.

export {
  ProtoMessageBacking,
  coerceObjectToMessage,
  messageToObject,
  isWellKnownWrappable,
} from './proto/backing.js';
export { DescriptorSet } from './proto/descriptors.js';
