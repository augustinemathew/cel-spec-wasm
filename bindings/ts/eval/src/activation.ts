// The activation — the bag of bound variable values an Eval reads.
//
// An activation is a plain `Record<string, CelInput>`: variable name →
// JS-natural bound value.  The marshal (`./marshal.js`) walks the
// Program's declared variables and writes each bound value into its
// workspace slot before `$eval`.  This module owns the activation's
// public shape + the normalization the Instance applies before marshal:
// a missing / `undefined` activation is the empty activation (a
// variable-free Program evaluates with no bindings).
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.5.

import type { CelInput } from './types.js';

/**
 * A set of variable bindings for one Eval — variable name → JS-natural
 * bound value.  Passed to {@link Instance.eval}; the declared `repr` of
 * each Program variable decides how its bound value is interpreted
 * (§A.4.6).
 */
export type Activation = Record<string, CelInput>;

/**
 * Normalize an optional activation to a concrete record.  An omitted
 * activation (the nullary `instance.eval()`) is the empty activation —
 * valid for a Program that declares no variables, and surfaced as an
 * unbound-variable error by the marshal otherwise.
 */
export function normalizeActivation(activation?: Activation): Activation {
  return activation ?? {};
}
