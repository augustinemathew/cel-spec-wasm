// e2e timestamp / duration behaviors — construction, accessors,
// arithmetic, and comparison.
//
// Ported from the C++ `e2e/m7b_test.cc` (timestamp / duration surfaces).
// Grounded in `doc/langdef.md` §"Timestamp" / §"Duration":
//   - `timestamp(string)` parses an RFC-3339 instant; `duration(string)`
//     parses a Go-style duration (`1h30m`, `90m`, `60s`).
//   - Accessors (`getFullYear`, `getMonth` [0-based], `getHours`,
//     `getDayOfWeek` [0=Sunday], …) read UTC components.
//   - Arithmetic: ts+dur→ts, ts−dur→ts, ts−ts→dur, dur+dur→dur.
//
// A decoded timestamp is `{ kind: 'timestamp', epochSeconds, nanos }`; a
// decoded duration is `{ kind: 'duration', seconds, nanos }`.

import { describe, expect, it } from 'vitest';

import { evalCel } from './helpers.js';

import type { CelDuration, CelTimestamp } from '@cel-wasm/eval';

const KNOWN = "timestamp('2009-02-13T23:31:30Z')"; // epoch 1234567890

describe('construction', () => {
  it('timestamp parses an RFC-3339 instant to epoch seconds', async () => {
    expect(await evalCel(KNOWN)).toEqual({
      kind: 'timestamp',
      epochSeconds: 1234567890n,
      nanos: 0,
    } satisfies CelTimestamp);
  });
  it.each([
    ["duration('1h')", 3600n],
    ["duration('90m')", 5400n],
    ["duration('1h30m')", 5400n],
    ["duration('60s')", 60n],
  ])('%s → %s seconds', async (src, seconds) => {
    expect(await evalCel(src)).toEqual({
      kind: 'duration',
      seconds,
      nanos: 0,
    } satisfies CelDuration);
  });
});

describe('timestamp accessors (UTC components)', () => {
  it.each([
    [`${KNOWN}.getFullYear()`, 2009n],
    [`${KNOWN}.getMonth()`, 1n], // 0-based: February
    [`${KNOWN}.getDate()`, 13n],
    [`${KNOWN}.getHours()`, 23n],
    [`${KNOWN}.getMinutes()`, 31n],
    [`${KNOWN}.getSeconds()`, 30n],
    [`${KNOWN}.getDayOfWeek()`, 5n], // 0=Sunday → Friday
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
});

describe('duration accessors', () => {
  it.each([
    ["duration('1h').getHours()", 1n],
    ["duration('90m').getMinutes()", 90n],
    ["duration('1h30m').getSeconds()", 5400n],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
});

describe('arithmetic (langdef §Timestamp/Duration)', () => {
  it('timestamp + duration → timestamp', async () => {
    expect(
      await evalCel("timestamp('1970-01-01T00:00:00Z') + duration('60s')"),
    ).toEqual({ kind: 'timestamp', epochSeconds: 60n, nanos: 0 });
  });
  it('timestamp - duration → timestamp', async () => {
    expect(
      await evalCel("timestamp('1970-01-01T00:01:00Z') - duration('60s')"),
    ).toEqual({ kind: 'timestamp', epochSeconds: 0n, nanos: 0 });
  });
  it('timestamp - timestamp → duration', async () => {
    expect(
      await evalCel(
        "timestamp('1970-01-01T00:01:00Z') - timestamp('1970-01-01T00:00:00Z')",
      ),
    ).toEqual({ kind: 'duration', seconds: 60n, nanos: 0 });
  });
  it('duration + duration → duration', async () => {
    expect(await evalCel("duration('1h') + duration('30m')")).toEqual({
      kind: 'duration',
      seconds: 5400n,
      nanos: 0,
    });
  });
});

describe('comparison & equality', () => {
  it.each([
    [`${KNOWN} > timestamp('2000-01-01T00:00:00Z')`, true],
    [`${KNOWN} == ${KNOWN}`, true],
    ["duration('1h') < duration('2h')", true],
    ["duration('1h') == duration('60m')", true],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
});
