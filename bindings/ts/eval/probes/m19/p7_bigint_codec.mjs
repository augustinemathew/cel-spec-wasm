// M19 Probe P-7 — bigint ⟷ wire i64/u64 round-trip at the extremes.
//
// CEL `int` is signed 64-bit, `uint` unsigned 64-bit. JS `number` loses
// precision past 2^53, so the CelValue codec MUST use DataView bigint
// (getBigInt64/setBigUint64) for the int/uint payload arms. This probe
// pins that the round-trip is exact at the boundaries `number` would
// corrupt. Graduates into celvalue.test.ts boundary cases.
//
// Throwaway probe — delete at M19 closeout.

const LE = true;
const buf = new ArrayBuffer(24); // one CelValue
const view = new DataView(buf);

// payload `i` (int) / `u` (uint) live at +8 (cel_data.h:109).
function roundTripInt(v) {
  view.setBigInt64(8, v, LE);
  return view.getBigInt64(8, LE);
}
function roundTripUint(v) {
  view.setBigUint64(8, v, LE);
  return view.getBigUint64(8, LE);
}

const INT64_MIN = -(2n ** 63n);
const INT64_MAX = 2n ** 63n - 1n;
const UINT64_MAX = 2n ** 64n - 1n;

const cases = [
  ["int  INT64_MIN", roundTripInt, INT64_MIN],
  ["int  INT64_MAX", roundTripInt, INT64_MAX],
  ["int  0", roundTripInt, 0n],
  ["int  -1", roundTripInt, -1n],
  ["uint UINT64_MAX", roundTripUint, UINT64_MAX],
  ["uint 0", roundTripUint, 0n],
  ["uint 2^63 (past Number)", roundTripUint, 2n ** 63n],
];

let ok = true;
for (const [label, fn, v] of cases) {
  const got = fn(v);
  const pass = got === v;
  ok &&= pass;
  console.log(`  ${pass ? "ok " : "FAIL"}  ${label}: ${v} -> ${got}`);
}

// Sanity: prove `number` WOULD have corrupted these, justifying bigint.
const corrupt = Number(INT64_MAX) !== 2 ** 63 - 1 || !Number.isSafeInteger(Number(UINT64_MAX));
console.log(`  (number would corrupt these: ${corrupt})`);

console.log(ok ? "P-7 PASS ✅  bigint codec exact at i64/u64 extremes" : "P-7 FAILED ❌");
process.exit(ok ? 0 : 1);
