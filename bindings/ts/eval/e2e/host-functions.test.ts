// e2e host-function (`@host`) behaviors.
//
// Ported intent from the C++ `e2e/m21_*` host-call-adapter suites: a
// registered JS host function should round-trip CelValue arguments and
// returns through the `cel_fn.*` trampoline inside a compiled Program.
//
// BLOCKER (verified): these e2e cases compile a `@host`-calling expression
// WITHOUT supplying the matching `@host` function declaration, so the
// reference fails type-check (`undeclared reference to 'myFn'`).  Wiring the
// `CompileOptions.fns` (`@host` `.celfn`) declarations through these cases —
// so a compiled `@host` Program is reachable end-to-end — is a focused
// follow-up gated on a host-fn fixture, per `m29-typescript-bindings.md` §A.7.
// The `cel_fn` trampoline contract (decode arg slots → call the JS impl →
// encode the result) is unit-pinned in `eval/src/instance.test.ts`
// (`buildCelFnImports — host-fn round-trip`); these e2e cases carry the
// assertions they WILL make once the fixture lands.

import { compile } from '@cel-wasm/compiler';
import { describe, expect, it } from 'vitest';

import { Engine } from '@cel-wasm/eval';
import type { CelValue } from '@cel-wasm/eval';

describe('@host function compile path', () => {
  it('referencing an undeclared function fails type-check (the blocker)', async () => {
    await expect(compile('myFn(1, 2)')).rejects.toThrow(/undeclared reference/);
  });

  it.skip('a registered int->int host fn round-trips through cel_fn.* (needs a @host fixture)', async () => {
    const program = await compile('addOne(41)' /* @host addOne(int): int */);
    const engine = await Engine.create();
    engine.defineFunction(
      'addOne(int): int',
      (...args: CelValue[]): CelValue => {
        const [x] = args;
        return (x as bigint) + 1n;
      },
    );
    const instance = await engine.plan(program);
    expect(instance.eval({})).toBe(42n);
  });

  it.skip('a host fn returning a CelError propagates the error value (needs a @host fixture)', async () => {
    const program = await compile('boom()' /* @host boom(): int */);
    const engine = await Engine.create();
    engine.defineFunction(
      'boom(): int',
      (): CelValue => ({
        kind: 'error',
        code: 41,
        message: 'host adapter error',
      }),
    );
    const instance = await engine.plan(program);
    const out = instance.eval({});
    expect(out).toMatchObject({ kind: 'error', code: 41 });
  });
});
