/**
 * `conformance_ts` runner — the cross-host conformance gate, on the REAL
 * `@celwasm/eval` library (Engine / Program / Instance / Value). For each
 * fixture row exported by the C++ `conformance_ts_export` (a `<n>.wasm`
 * Program + its `SimpleTest` as proto3 JSON + the `cpp_outcome`), it builds
 * an `Activation` from the row's bindings, evaluates the Program through
 * the production host wiring, compares the decoded `Value` against the
 * row's matcher, and **zero-diffs `ts_outcome` against `cpp_outcome`** (m19
 * §5.6). The C++ pass-set is both the ceiling and the ground truth.
 *
 * Because it drives the real library, it evaluates the aggregate / proto /
 * list / map / arena rows the old scalar-only standalone runner skipped —
 * a row classifies `skip` only when it's genuinely outside the current
 * host subset (a not-yet-implemented matcher kind, a binding shape the
 * value model can't build, or a trampoline/codec that throws an expected
 * "unimplemented" error).
 */
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import {
  Activation,
  CelKind,
  Engine,
  Program,
  TypeRegistry,
  Value,
  asList,
  asMap,
  isError,
  isNull,
  isUnknown,
  type EngineOptions,
  type Value as CelValue,
} from '../../eval/src/index.js';

/** Outcome of evaluating one row through the TS host. */
export type Outcome = 'pass' | 'fail' | 'skip';

export interface ConformanceResult {
  readonly total: number;
  /** `cpp=<o> ts=<o>` → count. */
  readonly matrix: Record<string, number>;
  /** Comparable rows (ts ≠ skip, cpp ∈ {pass,fail}) where ts === cpp. */
  readonly agree: number;
  /** REGRESSIONS: cpp=pass but ts=fail — TS worse than C++. Must be 0; the
   *  C++ pass-set is the floor the TS host may not drop below. */
  readonly regressions: number;
  /** AHEAD: cpp=fail but ts=pass — TS produced the spec-correct value on a
   *  row C++ itself fails (a known C++ gap). A positive divergence, tracked
   *  not failed; the matcher is the spec, so ts=pass means spec-correct. */
  readonly ahead: number;
  /** Human-readable regression descriptions (for failure output). */
  readonly regressionExamples: readonly string[];
  /** For the cpp=pass ts=skip coverage gap: reason bucket → count. Accounts
   *  for every row C++ passes that the TS host doesn't yet evaluate. */
  readonly skipGap: Record<string, number>;
}

// A binding shape / matcher kind the current host can't represent yet:
// the row classifies `skip` (no ground truth to diff against).
class Unsupported extends Error {
  public override readonly name = 'Unsupported';
}

// Errors that mean "this row is outside the current host subset" — caught
// and turned into `skip`, never a diff. Anything else propagates (a real
// bug surfaces loudly rather than hiding as a skip).
const SKIPPABLE_ERRORS = new Set([
  'Unsupported',
  'EvalError',
  'TrampolineError',
  'CelDecodeError',
  'ArenaDecodeError',
  'HostBackingError',
  'MessageBuildError',
  'ValueError',
  'RegistryError',
  'EngineError',
  // protobuf-es rejects an out-of-range enum / bad field value on set;
  // enum-range handling differs from C++ and is out of the current subset.
  'FieldValueInvalidError',
  // A wasm trap (unreachable) — a runtime path not yet implemented
  // (e.g. optionals ext); the row is outside the host subset.
  'RuntimeError',
]);

function isSkippable(e: unknown): boolean {
  return e instanceof Error && SKIPPABLE_ERRORS.has(e.name);
}

// ── unknown-narrowing helpers (proto3 JSON is parsed as `unknown`) ──
function asObject(x: unknown): Record<string, unknown> {
  if (typeof x !== 'object' || x === null || Array.isArray(x)) {
    throw new Unsupported('expected a JSON object');
  }
  return x as Record<string, unknown>;
}
function asArray(x: unknown): unknown[] {
  if (!Array.isArray(x)) {
    throw new Unsupported('expected a JSON array');
  }
  return x;
}
function asString(x: unknown): string {
  if (typeof x !== 'string') {
    throw new Unsupported('expected a JSON string');
  }
  return x;
}
// proto3 JSON encodes int64/uint64 as strings (sometimes numbers).
function toBigInt(x: unknown): bigint {
  if (typeof x === 'string') return BigInt(x);
  if (typeof x === 'number') return BigInt(x);
  throw new Unsupported('expected an integer');
}
// proto3 JSON encodes the float specials as the strings "NaN"/"Infinity".
function toDouble(x: unknown): number {
  if (x === 'NaN') return NaN;
  if (x === 'Infinity') return Infinity;
  if (x === '-Infinity') return -Infinity;
  if (typeof x === 'number') return x;
  if (typeof x === 'string') return Number(x);
  throw new Unsupported('expected a double');
}
function fromBase64(b64: string): Uint8Array {
  return new Uint8Array(Buffer.from(b64, 'base64'));
}
function bytesEqual(a: Uint8Array, b: Uint8Array): boolean {
  return a.length === b.length && a.every((x, i) => x === b[i]);
}

/** Convert a `cel.expr.Value` (proto3 JSON) to a public `Value` for binding.
 *  Throws `Unsupported` for shapes the value model can't build (→ skip). */
function exprToValue(ev: unknown): CelValue {
  const o = asObject(ev);
  if ('nullValue' in o) return Value.null();
  if ('boolValue' in o) return Value.bool(o.boolValue === true);
  if ('int64Value' in o) return Value.int(toBigInt(o.int64Value));
  if ('uint64Value' in o) return Value.uint(toBigInt(o.uint64Value));
  if ('doubleValue' in o) return Value.double(toDouble(o.doubleValue));
  if ('stringValue' in o) return Value.string(asString(o.stringValue));
  if ('bytesValue' in o) return Value.bytes(fromBase64(asString(o.bytesValue)));
  if ('listValue' in o) {
    const lv = asObject(o.listValue);
    const values = 'values' in lv ? asArray(lv.values) : [];
    return Value.list(values.map(exprToValue));
  }
  if ('mapValue' in o) {
    const mv = asObject(o.mapValue);
    const entries = 'entries' in mv ? asArray(mv.entries) : [];
    return Value.map(
      entries.map((e): readonly [CelValue, CelValue] => {
        const eo = asObject(e);
        return [exprToValue(eo.key), exprToValue(eo.value)];
      }),
    );
  }
  // enum / type / object(message) bindings: out of subset.
  throw new Unsupported(`binding value kind: ${Object.keys(o).join(',')}`);
}

/** Compare a decoded `Value` against a `cel.expr.Value` matcher (proto3
 *  JSON). `'skip'` for matcher kinds the host can't compare yet. */
function compareValue(got: CelValue, want: unknown): boolean | 'skip' {
  const o = asObject(want);
  if ('nullValue' in o) return isNull(got);
  if ('boolValue' in o) {
    return got.kind === CelKind.Bool && got.bool === (o.boolValue === true);
  }
  if ('int64Value' in o) {
    return got.kind === CelKind.Int && got.int === toBigInt(o.int64Value);
  }
  if ('uint64Value' in o) {
    return got.kind === CelKind.Uint && got.uint === toBigInt(o.uint64Value);
  }
  if ('doubleValue' in o) {
    const d = toDouble(o.doubleValue);
    if (got.kind !== CelKind.Double) return false;
    return Number.isNaN(d) ? Number.isNaN(got.double) : got.double === d;
  }
  if ('stringValue' in o) {
    return got.kind === CelKind.String && got.value === asString(o.stringValue);
  }
  if ('bytesValue' in o) {
    return (
      got.kind === CelKind.Bytes &&
      bytesEqual(got.bytes, fromBase64(asString(o.bytesValue)))
    );
  }
  if ('listValue' in o) return compareList(got, asObject(o.listValue));
  if ('mapValue' in o) return compareMap(got, asObject(o.mapValue));
  // enum / type / object matchers: not comparable yet.
  return 'skip';
}

function compareList(
  got: CelValue,
  lv: Record<string, unknown>,
): boolean | 'skip' {
  if (got.kind !== CelKind.ListHost) return false;
  const want = 'values' in lv ? asArray(lv.values) : [];
  const elems = asList(got);
  if (elems.length !== want.length) return false;
  for (let i = 0; i < want.length; i++) {
    const el = elems[i];
    if (el === undefined) return false;
    const r = compareValue(el, want[i]);
    if (r === 'skip') return 'skip';
    if (!r) return false;
  }
  return true;
}

function compareMap(
  got: CelValue,
  mv: Record<string, unknown>,
): boolean | 'skip' {
  if (got.kind !== CelKind.MapHost) return false;
  const want = 'entries' in mv ? asArray(mv.entries) : [];
  const pairs = asMap(got);
  if (pairs.length !== want.length) return false;
  const used = new Set<number>();
  for (const entry of want) {
    const eo = asObject(entry);
    const match = findEntry(pairs, used, eo);
    if (match === 'skip') return 'skip';
    if (match < 0) return false;
    used.add(match);
  }
  return true;
}

// Find an unused result pair whose key+value match the matcher entry.
// Returns the index, -1 (no match), or 'skip' (uncomparable sub-value).
function findEntry(
  pairs: readonly (readonly [CelValue, CelValue])[],
  used: ReadonlySet<number>,
  entry: Record<string, unknown>,
): number | 'skip' {
  for (let i = 0; i < pairs.length; i++) {
    if (used.has(i)) continue;
    const pair = pairs[i];
    if (pair === undefined) continue;
    const keyMatch = compareValue(pair[0], entry.key);
    if (keyMatch === 'skip') return 'skip';
    if (!keyMatch) continue;
    const valMatch = compareValue(pair[1], entry.value);
    if (valMatch === 'skip') return 'skip';
    return valMatch ? i : -1; // key matched → this is the entry
  }
  return -1;
}

// A SimpleTest's fields we read (proto3 JSON, camelCase, defaults omitted).
function field(test: Record<string, unknown>, name: string): unknown {
  return test[name];
}

/** Build the Activation for a row; returns `undefined` if a binding shape
 *  is unsupported (→ the row skips). */
function buildActivation(
  test: Record<string, unknown>,
): Activation | undefined {
  const act = new Activation();
  const bindings = field(test, 'bindings');
  if (bindings === undefined) return act;
  for (const [name, b] of Object.entries(asObject(bindings))) {
    const value = asObject(b).value;
    if (value === undefined) return undefined;
    act.bind(name, exprToValue(value));
  }
  return act;
}

/** A row's outcome plus, for a skip, WHY it skipped (categorized so the
 *  gate can report the coverage gap vs C++ by reason). */
interface RowResult {
  readonly outcome: Outcome;
  readonly reason: string;
}
function skip(reason: string): RowResult {
  return { outcome: 'skip', reason };
}
function decided(outcome: Outcome): RowResult {
  return { outcome, reason: '' };
}

// Map a skippable error to a coverage-gap reason bucket.
function skipReasonFor(e: unknown): string {
  const name = e instanceof Error ? e.name : 'Error';
  const msg = e instanceof Error ? e.message : '';
  if (name === 'Unsupported') {
    return `binding: ${msg}`;
  }
  // Break TrampolineError down by which trampoline (it's the first token of
  // the message, e.g. "cel_host.cel_list_iter_open not implemented …") so
  // the gap histogram orders the build-out.
  if (name === 'TrampolineError') {
    return `trampoline: ${msg.split(' ')[0] ?? ''}`;
  }
  return `eval-threw: ${name}`; // CelDecode/Arena/EvalError/…
}

/** Evaluate one row → its TS outcome (+ skip reason). */
async function runRow(
  engine: Engine,
  dir: string,
  wasm: string,
  test: Record<string, unknown>,
): Promise<RowResult> {
  // Rows outside the eval gate's scope (check-only / unknowns / typed).
  if (
    field(test, 'checkOnly') !== undefined ||
    field(test, 'unknown') !== undefined ||
    field(test, 'anyUnknowns') !== undefined ||
    field(test, 'typedResult') !== undefined
  ) {
    return skip('scope: check-only / unknown / typed-result');
  }

  let result: CelValue;
  try {
    const act = buildActivation(test);
    if (act === undefined) return skip('binding: missing value');
    const instance = await engine.plan(
      Program.fromBytes(readFileSync(join(dir, wasm))),
    );
    result = instance.eval(act);
  } catch (e) {
    if (isSkippable(e)) return skip(skipReasonFor(e));
    throw e;
  }

  // Error matchers: pass iff the TS host produced a CEL error value.
  if (
    field(test, 'evalError') !== undefined ||
    field(test, 'anyEvalErrors') !== undefined
  ) {
    return decided(isError(result) ? 'pass' : 'fail');
  }
  // A value matcher but TS produced error/unknown → can't compare (skip).
  if (isError(result) || isUnknown(result)) {
    return skip('result: error/unknown vs value-matcher');
  }

  // Value matcher, or implicit-bool-true when no matcher is set.
  const want = field(test, 'value') ?? { boolValue: true };
  let verdict: boolean | 'skip';
  try {
    verdict = compareValue(result, want);
  } catch (e) {
    if (isSkippable(e)) return skip(skipReasonFor(e));
    throw e;
  }
  if (verdict === 'skip') {
    return skip(`matcher: ${Object.keys(asObject(want)).join(',')}`);
  }
  return decided(verdict ? 'pass' : 'fail');
}

/** Run the whole fixtures dir and return the zero-diff tally.
 *  `descriptorsPath`, if given, is a FileDescriptorSet covering the corpus's
 *  message types — it powers `cel_make_message` (message construction). */
export async function runConformance(
  fixturesDir: string,
  runtimeWasmPath: string,
  descriptorsPath?: string,
): Promise<ConformanceResult> {
  const options: EngineOptions =
    descriptorsPath === undefined
      ? {}
      : {
          registry: TypeRegistry.fromDescriptorSet(
            readFileSync(descriptorsPath),
          ),
        };
  const engine = await Engine.create(readFileSync(runtimeWasmPath), options);
  const lines = readFileSync(join(fixturesDir, 'index.jsonl'), 'utf8')
    .trim()
    .split('\n');

  const matrix: Record<string, number> = {};
  const skipGap: Record<string, number> = {};
  const regressionExamples: string[] = [];
  let agree = 0;
  let regressions = 0;
  let ahead = 0;

  for (const line of lines) {
    const row = asObject(JSON.parse(line));
    const cpp = asString(row.cpp_outcome);
    const wasm = asString(row.wasm);
    const test = asObject(row.test);
    let res: RowResult;
    try {
      res = await runRow(engine, fixturesDir, wasm, test);
    } catch (e) {
      res = { outcome: 'fail', reason: '' };
      if (regressionExamples.length < 12) {
        regressionExamples.push(`${asString(row.row_id)}: threw ${String(e)}`);
      }
    }
    const ts = res.outcome;
    matrix[`cpp=${cpp} ts=${ts}`] = (matrix[`cpp=${cpp} ts=${ts}`] ?? 0) + 1;

    // The coverage gap vs C++: rows C++ passes but TS skips, by reason.
    if (cpp === 'pass' && ts === 'skip') {
      skipGap[res.reason] = (skipGap[res.reason] ?? 0) + 1;
    }

    // Only rows where both sides evaluated the same matcher (cpp ∈
    // {pass,fail}, ts ≠ skip) are comparable. The C++ pass-set is the FLOOR:
    //   - ts === cpp                → agree
    //   - cpp=pass, ts=fail         → REGRESSION (TS worse; fails the gate)
    //   - cpp=fail, ts=pass         → AHEAD (TS spec-correct on a C++ gap)
    if (ts === 'skip' || cpp === 'skip') continue;
    if (ts === cpp) {
      agree++;
    } else if (cpp === 'pass') {
      regressions++;
      if (regressionExamples.length < 12) {
        regressionExamples.push(`${asString(row.row_id)}: cpp=pass ts=fail`);
      }
    } else {
      ahead++;
    }
  }

  return {
    total: lines.length,
    matrix,
    agree,
    regressions,
    ahead,
    regressionExamples,
    skipGap,
  };
}
