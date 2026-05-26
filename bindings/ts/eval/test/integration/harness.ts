/**
 * Shared real-wasm e2e harness. NOT a test file (no `.test.` — vitest's
 * integration glob skips it). The e2e is split into themed files
 * (`scalars`, `field-access`, `aggregates`) that all `setup()` this.
 *
 * It owns: the compiled runtime + two `TypeRegistry`s (Customer, HostMsg3),
 * the fixture→source manifest, the eval helpers, and the backing builders.
 * `show(value)` renders a `Value` compactly so each test's title reads
 * `<source expression>  →  <expected output>` — the expression comes from
 * the manifest the compiler wrote, the input from the describe block name.
 */
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join, resolve } from 'node:path';
import { create, type DescMessage } from '@bufbuild/protobuf';
import { Engine } from '../../src/engine.js';
import { Program } from '../../src/program.js';
import { Activation } from '../../src/activation.js';
import { Value } from '../../src/value.js';
import { TypeRegistry } from '../../src/type-registry.js';
import { CelKind } from '../../src/celvalue.js';

const HERE = dirname(fileURLToPath(import.meta.url));
export const TESTDATA = join(HERE, '../testdata');
export const RUNTIME =
  process.env.CEL_RUNTIME_WASM ??
  resolve(
    HERE,
    '../../../../../bazel-bin/runtime/cel_runtime_wasm.wasm',
  );

export const SESSION_TOKEN = new Uint8Array([0xde, 0xad, 0xbe, 0xef]);

// fixture filename → the CEL source expression it was compiled from.
const SOURCES = JSON.parse(
  readFileSync(join(TESTDATA, 'fixtures.json'), 'utf8'),
) as Record<string, string>;

export function src(fixture: string): string {
  return SOURCES[fixture] ?? `(unknown: ${fixture})`;
}

/** Compact one-line rendering of a `Value` for test titles. */
export function show(v: Value): string {
  switch (v.kind) {
    case CelKind.Null:
      return 'null';
    case CelKind.Bool:
      return String(v.bool);
    case CelKind.Int:
      return `${v.int}`;
    case CelKind.Uint:
      return `${v.uint}u`;
    case CelKind.Double:
      return `${v.double}`;
    case CelKind.String:
      return JSON.stringify(v.value);
    case CelKind.Bytes:
      return `0x${[...v.bytes].map((b) => b.toString(16).padStart(2, '0')).join('')}`;
    case CelKind.Message:
      return 'message';
    case CelKind.ListHost:
      return 'list';
    case CelKind.MapHost:
      return 'map';
    case CelKind.Unknown:
      return 'unknown';
    case CelKind.Error:
      return `error(${v.errorCode})`;
  }
}

export interface Harness {
  /** Plan `fixture` and eval it with `act`. */
  run(fixture: string, act: Activation): Promise<Value>;
  /** A single-binding Activation. */
  bind(name: string, value: Value): Activation;
  /** A realistic Customer as a real protobuf-es message. */
  protoCustomer(): Value;
  /** The same data as a plain JS object (JSON-style). */
  objectCustomer(): Value;
  /** A HostMsg3 (enum + repeated element types) proto message. */
  protoHostMsg(): Value;
  /** The Customer registry (for `toMessage` on returned messages). */
  readonly customerReg: TypeRegistry;
  /** A registry-free Engine (proves JSON-object input needs no registry). */
  bareEngine(): Promise<Engine>;
}

let cached: Harness | undefined;

export async function setup(): Promise<Harness> {
  if (cached !== undefined) {
    return cached;
  }
  const customerReg = TypeRegistry.fromDescriptorSet(
    readFileSync(join(TESTDATA, 'customer.fds.bin')),
  );
  const hostMsgReg = TypeRegistry.fromDescriptorSet(
    readFileSync(join(TESTDATA, 'host_msg3.fds.bin')),
  );
  const customerDesc = mustGet(customerReg, 'celwasm.testdata.Customer');
  const hostMsgDesc = mustGet(hostMsgReg, 'celwasm.testdata.HostMsg3');
  const runtimeBytes = readFileSync(RUNTIME);
  const engine = await Engine.create(runtimeBytes, { registry: customerReg });

  cached = {
    customerReg,
    async run(fixture, act) {
      const instance = await engine.plan(
        Program.fromBytes(readFileSync(join(TESTDATA, fixture))),
      );
      return instance.eval(act);
    },
    bind(name, value) {
      return new Activation().bind(name, value);
    },
    protoCustomer() {
      return Value.message(
        customerReg.message(
          create(customerDesc, {
            name: 'Ann',
            age: 30,
            userId: 42n,
            priority: 7,
            balanceCents: 100000n,
            creditScore: 9.5,
            isPremium: true,
            sessionToken: SESSION_TOKEN,
            billingAddress: { city: 'NYC', country: 'US' },
            metadata: { k: 'v' },
            tierQuotas: { 1: 50 },
            tags: ['gold', 'vip'],
          }),
        ),
      );
    },
    objectCustomer() {
      return Value.object({
        name: 'Ann',
        age: 30n,
        user_id: 42n,
        credit_score: 9.5,
        is_premium: true,
        session_token: SESSION_TOKEN,
        billing_address: { city: 'NYC', country: 'US' },
        metadata: new Map([['k', 'v']]),
        tags: ['gold', 'vip'],
      });
    },
    protoHostMsg() {
      return Value.message(
        hostMsgReg.message(
          create(hostMsgDesc, {
            kind: 7, // KIND_SEVEN → CEL int 7
            repI32: [11, 22],
            repB: [true],
            repF64: [1.5],
            repMsg: [{ i32: 99 }],
          }),
        ),
      );
    },
    async bareEngine() {
      return Engine.create(runtimeBytes);
    },
  };
  return cached;
}

function mustGet(reg: TypeRegistry, name: string): DescMessage {
  const d = reg.getMessage(name);
  if (d === undefined) throw new Error(`descriptor ${name} missing`);
  return d;
}
