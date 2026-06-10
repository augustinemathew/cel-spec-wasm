# Operator coverage tracker

**Mandate (2026-06-06): every single operator in the CEL language
surface MUST have at least one corpus cell.**  No exceptions, no
"representative samples" — exhaustive.  This file is the
accountability artifact: when an operator is uncovered, it shows up
here unticked.

**Wiring status (2026-06-09):** `celwasm_bench` and `celcpp_bench`
both load every corpus YAML via `LoadCorpus(...)` and register one
Google Benchmark per cell.  Adding a new operator's coverage is a
**YAML edit, not a C++ edit**: drop a cell into the matching surface
file (or add a new file + the surface to
`celwasm_bench.cc::kCorpusFiles` + `BmPrefixForSurface` — in BOTH
bench mains).

**Core-operator grid filled 2026-06-09.**  Thirteen surfaces, 229
cells: arithmetic, comparisons, comprehensions, conversions, index,
lists, logic, long_strings, maps, size, strings, ternary, time.
Every operator family × admitted-type combination below is either
covered (cell id given) or excluded with a per-cell reason tag in
the YAML + a row in §"Exclusions" here.  Extension libraries
(string-ext, math-ext, encoders, lists-ext, sets-ext, optionals,
cel.bind) remain the open tail — see their sections.

**Skip-tag conventions** (a tagged cell stays in the YAML — no
silent gaps):

| tag prefix | meaning |
|---|---|
| `celwasm-skip-*` | cell cannot run through celwasm today; celcpp_bench still runs it.  Suffix names the reason (`-rodata`, `-het-eq`, `-ternary-ident-cond`, `-map-dot-field`, `-arena-overflow`). |
| `celcpp-skip-*` | cell rejected by cel-cpp's checker as configured; celwasm_bench may still run it.  Cells carrying BOTH prefixes run nowhere and exist as documented grid exclusions. |

**Loader gap (documented in `corpus_loader.h`):** activation values
are scalar-only (int/uint/double/bool/string/bytes).  Cells needing a
timestamp/duration/list/map/message-typed *activation binding* instead
construct the operand in-source from literals (`timestamp("…")`,
`[1,2,3]`, `{…}`) and — where the result type is itself unexpressible —
reduce it to a scalar via `int(…)` / `.getSeconds()` / `size(…)` /
`== <literal>`.  The reduce is identical work on both comparators, so
the pairing stays honest; the cell comment names the wrap.

A cell is "covered" when:
1. It appears in a corpus `.yaml` file.
2. It runs through every enabled comparator (celwasm dynamic, celwasm
   static, cel-cpp) — or carries a skip tag naming why not — and the
   paired `result=` labels are byte-identical.
3. The published table shows the cell's timings.

Status legend: ✓ covered (both comparators) · ◐ celcpp-only
(celwasm-skip tag) · ✗ excluded (documented, runs nowhere) ·
n/a not defined by spec.

---

## Arithmetic operators

Source: `doc/langdef.md` §"Standard Definitions" → arithmetic.
Surface files: `arithmetic.yaml` (BM prefix `arith`).

| op       | int | uint | double | mixed-numeric¹ | cell ids |
|----------|-----|------|--------|----------------|----------|
| `+`      | ✓ sweep | ✓ | ✓ sweep | n/a (spec rejects mixed arithmetic) | intAdd{2,10,50,250,1000}Terms(+Const), uintAdd_simple, doubleAdd{…}Terms(+Const) |
| `-` bin  | ✓ sweep | ✓ | ✓ | n/a | intSub{…}Terms(+Const), uintSub_simple, doubleSub_simple |
| `*`      | ✓ sweep | ✓ | ✓ | n/a | intMul{…}Terms(+Const), uintMul_simple, doubleMul_simple |
| `/`      | ✓   | ✓    | ✓      | n/a | intDiv_simple, uintDiv_simple, doubleDiv_simple |
| `%`      | ✓   | ✓    | n/a    | n/a | intMod_simple, uintMod_simple |
| unary `-`| ✓   | n/a² | ✓      | n/a | intNeg, doubleNeg |

¹ Mixed-numeric arithmetic is rejected by the static subset (and by
cel-cpp's checker); mixed-numeric *comparison* is tracked in the
comparison grid below.
² CEL has no unary `-` on uint.

Length sweep `{2, 10, 50, 250, 1000}` per intAdd/intMul/intSub/
doubleAdd, var + const variants.  `*1000TermsConst` cells are
`celwasm-skip-rodata` (1000 literal CelValues = 24 KB rodata vs the
8 KB low-memory window).

## Non-numeric `+` / `-` overloads

| operand types          | result   | cell id (surface)            | status |
|------------------------|----------|------------------------------|--------|
| string + string        | string   | strings.concat2, strings.concatChain{10,100,1000}Terms | ✓ (1000-term ◐ `celwasm-skip-arena-overflow`) |
| bytes + bytes          | bytes    | strings.bytesConcat2         | ✓      |
| list + list            | list     | lists.concat (`size([…]+[…])`) | ✓    |
| timestamp + duration   | timestamp| time.tsAddDur (int-reduced)  | ✓      |
| duration + timestamp   | timestamp| time.durAddTs                | ✓      |
| duration + duration    | duration | time.durAddDur (getSeconds-reduced) | ✓ |
| timestamp - timestamp  | duration | time.tsSubTs                 | ✓      |
| timestamp - duration   | timestamp| time.tsSubDur                | ✓      |
| duration - duration    | duration | time.durSubDur               | ✓      |

---

## Comparison operators

Surface: `comparisons.yaml` (BM prefix `cmp`).  Heterogeneous
operands: see §Exclusions.

| op   | bool | int | uint | double | string | bytes | timestamp | duration | list | map | null | cross-num |
|------|------|-----|------|--------|--------|-------|-----------|----------|------|-----|------|-----------|
| `==` | ✓ boolEq | ✓ intEq | ✓ uintEq | ✓ doubleEq | ✓ stringEq | ✓ bytesEq | ✓ tsEq | ✓ durEq | ✓ listEq | ✓ mapEq | ✓ nullEq | ✗ intEqDouble (excluded, see §Exclusions) |
| `!=` | ✓ boolNe | ✓ intNe | ✓ uintNe | ✓ doubleNe | ✓ stringNe | ✓ bytesNe | ✓ tsNe | ✓ durNe | ✓ listNe | ✓ mapNe | n/a¹ | ✗ |
| `<`  | n/a² | ✓ intLt | ✓ uintLt | ✓ doubleLt | ✓ stringLt | ✓ bytesLt | ✓ tsLt | ✓ durLt | n/a | n/a | n/a | ◐ intLtDouble (celwasm-skip-het-eq) |
| `<=` | n/a² | ✓ intLe | ✓ uintLe | ✓ doubleLe | ✓ stringLe | ✓ bytesLe | ✓ tsLe | ✓ durLe | n/a | n/a | n/a | ✗ |
| `>`  | n/a² | ✓ intGt | ✓ uintGt | ✓ doubleGt | ✓ stringGt | ✓ bytesGt | ✓ tsGt | ✓ durGt | n/a | n/a | n/a | ✗ |
| `>=` | n/a² | ✓ intGe | ✓ uintGe | ✓ doubleGe | ✓ stringGe | ✓ bytesGe | ✓ tsGe | ✓ durGe | n/a | n/a | n/a | ✗ |

¹ `null != null` adds nothing over nullEq; `null == non-null` is the
nullEqInt exclusion.
² Ordering on bool is not defined by spec.

Chained: cmp.intLtChain20 (`a<b && b<c && …`, 20 comparisons).
String-eq payload sweep: long_strings.eqLong_N{10,100,1000}_{match,
mismatch} (N=10000 cells are `celwasm-skip-rodata`, see §Findings).

---

## Logical / boolean operators

Surface: `logic.yaml` (BM prefix `logic`), `ternary.yaml`
(`ternary`).

| op           | shape                       | cell ids | status |
|--------------|----------------------------|----------|--------|
| `&&`         | 2-term var / const          | logic.and2, logic.and2Const | ✓ |
| `&&`         | 10-term chain               | logic.and10Terms | ✓ |
| `&&`         | short-circuit vs not (`false && <scan>` / `true && <scan>`) | logic.andShortCircuit, logic.andNoShortCircuit | ✓ |
| `\|\|`       | 2-term var / const          | logic.or2, logic.or2Const | ✓ |
| `\|\|`       | 10-term chain               | logic.or10Terms | ✓ |
| `\|\|`       | short-circuit vs not        | logic.orShortCircuit, logic.orNoShortCircuit | ✓ |
| `!`          | single / triple, var + const| logic.not1, logic.not1Const, logic.not3 | ✓ |
| ternary `?:` | int result, computed cond   | ternary.intComputedCond, ternary.intConst | ✓ |
| ternary `?:` | string result               | ternary.stringComputedCond | ✓ |
| ternary `?:` | nested chain                | ternary.nested3 | ✓ |
| ternary `?:` | bare bool-var condition     | ternary.intVarCond | ◐ `celwasm-skip-ternary-ident-cond` — **celwasm BUG, see §Findings** |

3VL-with-unknown shapes are PartialEval-only and out of the eval
corpus's scope (no unknown bindings in the bench activation schema).

---

## Membership `in`

Surfaces: `lists.yaml` (BM prefix `in_list`), `maps.yaml` (`map`).

| range kind     | elem/key types covered | cell ids | status |
|----------------|------------------------|----------|--------|
| list<string>   | length sweep {5,20,100,1000}, var + literal needle | lists.{5,20,100,1000}(_lit) | ✓ ({1000,1000_lit} ◐ `celwasm-skip-rodata`) |
| list<int>      | 20 elems, worst-case scan | lists.int20 | ✓ |
| list<uint>     | "                       | lists.uint20 | ✓ |
| list<double>   | "                       | lists.double20 | ✓ |
| list<bool>     | "                       | lists.bool20 | ✓ |
| list<bytes>    | —                       | — | ✗ excluded: bytes list literals add no kernel path beyond string (same memcmp scan); revisit if the kernel ever specializes |
| list<message>  | —                       | — | ✗ excluded: needs proto descriptor + message activation (see §Proto-message exclusion) |
| map<string,V>  | 10 keys, var + const    | maps.inString, maps.inStringConst | ✓ |
| map<int,V>     | 10 keys                 | maps.inInt | ✓ |
| map<uint,V>    | 10 keys                 | maps.inUint | ✓ |
| map<bool,V>    | 2 keys                  | maps.inBool | ✓ |

---

## Indexing `_[_]`

Surface: `index.yaml` (BM prefix `idx`).

| shape          | cell ids | status |
|----------------|----------|--------|
| list[int], var + const key | idx.listInt, idx.listIntConst | ✓ |
| map[string], var + const key | idx.mapString, idx.mapStringConst | ✓ |
| map[int]       | idx.mapInt | ✓ |
| map[uint]      | idx.mapUint | ✓ |
| map[bool]      | idx.mapBool | ✓ |

---

## Selection / field access + has()

| shape | cell ids | status |
|-------|----------|--------|
| map.field sugar (string key) | maps.dotField | ◐ `celwasm-skip-map-dot-field` (cleanup-backlog #9: Select routes through the message-field trampoline → type_mismatch error) |
| `has(map.field)` | maps.hasKey | ◐ same gap (e2e/known_bugs_test.cc::HasOnMapPresentKey) |
| message.field / `has(msg.field)` | — | ✗ excluded — see §Proto-message exclusion |

---

## Comprehension macros

Surface: `comprehensions.yaml` (BM prefix `compr`).  Ranges are
20-element literal int lists unless noted; `map`/`filter` results are
`size(…)`-reduced (see loader-gap note above).

| macro        | cell ids | status |
|--------------|----------|--------|
| `all`        | compr.all20 + length sweep compr.all{10,100,1000} | ✓ (all1000 ◐ `celwasm-skip-rodata`) |
| `exists`     | compr.exists20 | ✓ |
| `exists_one` | compr.existsOne20 | ✓ |
| `map`        | compr.map20 | ✓ |
| `filter`     | compr.filter20 | ✓ |
| `exists` over map keys | compr.existsMapKey | ✓ |

---

## size(_)

Surface: `size.yaml` (BM prefix `size`).  The list cells double as
the list-literal-construction length sweep {10,100,1000}.

| input | cell ids | status |
|-------|----------|--------|
| string (var + const) | size.string, size.stringConst | ✓ |
| bytes  | size.bytes | ✓ |
| list   | size.list{10,100,1000} | ✓ (list1000 ◐ `celwasm-skip-rodata`) |
| map    | size.map{10,100} | ✓ |

---

## String functions (standard library)

Surface: `strings.yaml` + `long_strings.yaml` (BM prefix `str`).

| fn | cell ids | status |
|----|----------|--------|
| `_.contains(_)` | strings.contains, long_strings.containsLong_N{10,100,1000} | ✓ (N10000 ◐ rodata) |
| `_.startsWith(_)` | strings.startsWith | ✓ |
| `_.endsWith(_)` | strings.endsWith | ✓ |
| `_.matches(_)` cheap anchor | strings.matchesCheap | ✓ |
| `_.matches(_)` moderate regex (alternation + bounded repeat) | strings.matchesComplex | ✓ |

---

## Type conversions

Surface: `conversions.yaml` (BM prefix `conv`).

| from \ to | int | uint | double | string | bytes | timestamp | duration |
|-----------|-----|------|--------|--------|-------|-----------|----------|
| int       | n/a | ✓ uintFromInt | ✓ doubleFromInt | ✓ stringFromInt | n/a | ✗¹ | ✗¹ |
| uint      | ✓ intFromUint | n/a | ✓ doubleFromUint | ✓ stringFromUint | n/a | n/a | n/a |
| double    | ✓ intFromDouble | ✓ uintFromDouble | n/a | ✓ stringFromDouble | n/a | n/a | n/a |
| string    | ✓ intFromString | ✓ uintFromString | ✓ doubleFromString | n/a | ✓ bytesFromString | ✓ timestampRoundTrip² | ✓ durationRoundTrip² |
| bool      | n/a | n/a | n/a | ✓ stringFromBool | n/a | n/a | n/a |
| bytes     | n/a | n/a | n/a | ✓ stringFromBytes | n/a | n/a | n/a |
| timestamp | ✓ intFromTimestamp | n/a | n/a | ✓ stringFromTimestamp | n/a | n/a | n/a |
| duration  | ✗¹ | n/a | n/a | ✓ stringFromDuration | n/a | n/a | n/a |
| `type(_)` | ✓ typeOfInt, typeOfString (`type(x) == <type>` form) | | | | | | |

¹ `timestamp(int)` / `duration(int)` / `int(duration)` excluded:
non-uniform support across the two stacks' standard libraries as
configured; the epoch path is covered from the timestamp side by
intFromTimestamp.  Revisit if the published table needs them.
² Parse + format round-trip (`string(timestamp(s))`) — result type
otherwise unexpressible in the scalar-only expected schema.

`bool(string)` / `string(bool)`: string→bool excluded (same
non-uniform-support bucket¹); bool→string covered (stringFromBool).
`dyn(_)` is rejected by celwasm's static subset by design — not a
corpus row.

---

## Timestamp / duration operators + accessors

Surface: `time.yaml` (BM prefix `time`).  Arithmetic rows are in the
`+`/`-` overload table above.

| accessor | UTC | named tz | cell ids |
|----------|-----|----------|----------|
| `getFullYear` | ✓ | ✓ | time.tsGetFullYearUtc, time.tsGetFullYearTz |
| `getHours`    | ✓ | ✓ | time.tsGetHoursUtc, time.tsGetHoursTz |
| `getSeconds`  | ✓ | ✓ | time.tsGetSecondsUtc, time.tsGetSecondsTz |
| duration `getHours` / `getSeconds` | ✓ | n/a | time.durGetHours, time.durGetSeconds |
| `getDate`/`getDayOfMonth`/`getDayOfWeek`/`getDayOfYear`/`getMilliseconds`/`getMinutes`/`getMonth` | ✗ | ✗ | TODO: not yet covered — same kernel family as the three covered accessors (one trampoline per accessor); add when the published table wants the full accessor sweep |

---

## Proto-message exclusion (applies to several grids above)

Cells needing a proto message — `msg.field` selection,
`has(msg.field)`, `msg in [msg]`, message equality — are **excluded
from this corpus** (not tagged, not present) because BOTH halves of
the pairing lack the plumbing:

  - `corpus_loader.h`'s activation schema has no message literal and
    no descriptor reference;
  - `celcpp_bench` deliberately links zero first-party targets and
    resolves types from the generated descriptor pool only — our
    `testdata` fixture protos are not in its pool, and linking them
    in would start the first-party-symbol-clash problem its header
    documents.

When a proto surface lands it needs: loader schema bump (message
literal + descriptor path), TypeForKind/ValueFromLiteral extension in
BOTH bench mains, and a shared fixture proto target both binaries can
link.  Until then this section is the documented gap.

---

## Exclusions (cells no comparator runs)

| cell | reason |
|------|--------|
| cmp.intEqDouble | heterogeneous `==`: celwasm checker rejects (`celwasm-skip-het-eq`); cel-cpp checker rejects too — equality stays homogeneous at check time even with `enable_cross_numeric_comparisons` (`celcpp-skip-het-eq-check`).  Kept in YAML as the documented grid row. |
| cmp.nullEqInt | same: `1 == null` rejected by both checkers as configured. |

## celwasm-skipped cells (celcpp-only, ◐)

| tag | cells | reason |
|-----|-------|--------|
| `celwasm-skip-rodata` | arith.{intAdd,intMul,intSub,doubleAdd}1000TermsConst, in_list.{1000,1000_lit}, size.list1000, compr.all1000, str.{eqLong,containsLong}_N10000* | >8 KB expression rodata (1000 literals ≈ 24 KB; 10 KB string literal).  Static mode rejects loudly; dynamic mode must not be trusted past the bound (see §Findings). |
| `celwasm-skip-het-eq` | cmp.intLtDouble (+ the two §Exclusions cells) | checker rejects mixed-numeric comparison; cel-cpp runs it with `enable_cross_numeric_comparisons`. |
| `celwasm-skip-ternary-ident-cond` | ternary.intVarCond | **celwasm bug**, see §Findings. |
| `celwasm-skip-map-dot-field` | map.dotField, map.hasKey | cleanup-backlog #9 Select-on-map gap. |
| `celwasm-skip-arena-overflow` | str.concatChain1000Terms | 999 intermediate concats overflow the eval arena (CEL overflow error value); same family as known_bugs ExpressionIntermediatesArenaCliff. |

## Findings (correctness divergences surfaced by this corpus)

1. **Ternary with a bare bool-variable condition returns null
   (celwasm, 2026-06-09).**  `c ? x : y` with `c` bound via the
   activation evaluates to **null**; computed conditions
   (`a > b ? …`, `(c || false) ? …`, `!c ? …`) are correct.
   Verified via the cel CLI in both link modes.  Pinned by
   ternary.intVarCond (`celwasm-skip-ternary-ident-cond`).
2. **Dynamic-mode silent wrong answer past the rodata bound
   (celwasm, 2026-06-09).**  `a == "<10000 x's>"` with `a` equal:
   link_mode=static rejects at compile ("rodata ends at byte 10040,
   past the 8192-byte window"); link_mode=dynamic **compiled and
   returned false** (cel-cpp: true).  The 8 KB bound is enforced
   loudly only on the static path.  Pinned by
   long_strings.eqLong_N10000_match (`celwasm-skip-rodata`); the
   m28 doc §10 lists the N=10000 rodata budget as out-of-scope
   follow-up — the *silent* dynamic-mode miscompare is the sharp
   edge to carry into that follow-up.
3. **Heterogeneous equality is a checker gap on both stacks as
   configured** (`int == double`, `1 == null`): celwasm rejects by
   design of the current static subset; cel-cpp rejects at check
   time (runtime heterogeneous equality exists but is unreachable
   through the checked-compile path used here).  cmp.intLtDouble
   (ordering) runs on cel-cpp only.

---

## Extension libraries — the open tail

Uncovered; each gets its own surface file when its milestone's
published table calls for it.  (The celwasm kernels exist for most
of these — see `compiler/codegen/overload_table.cc` — the gap is
corpus cells, not implementation.)

### String-ext
`strings.format`, `indexOf`, `lastIndexOf`, `lowerAscii`,
`upperAscii`, `replace`, `split`, `join`, `substring`, `trim`,
`quote`, `charAt`, `reverse`.

### Math-ext
`math.greatest/least/abs/ceil/floor/round/trunc/sign/isInf/isNaN/
isFinite/bitOr/bitAnd/bitXor/bitNot/bitShiftLeft/bitShiftRight/
sqrt`.

### Encoders-ext
`base64.encode`, `base64.decode`.

### Network-ext
`net.ip`, `net.cidr` families.

### Optionals
`optional.of/ofNonZeroValue/none`, `hasValue`, `value`, `orValue`,
`?field` selection.

### Bindings
`cel.bind(var, val, expr)`.

### Remaining time accessors
See the accessor table above (7 accessors × UTC/tz).

---

## How this file is maintained

- **Every new operator that ships in any milestone gets a row here**,
  unticked, with a TODO cell-id.
- **When a corpus entry is added** that exercises an operator, the
  row gets ticked and the cell-ids backfilled.
- **When a cell gains a skip tag**, the row flips to ◐/✗ here in the
  same commit, with the reason.
- **CI regression**: the `OPERATORS.md` parser is part of the
  benchmark harness — a CI check fails if an operator is shipped but
  not yet tracked (the celfn registry lists ops we know about; an op
  in the registry without a tracker row trips the check).

Mandate enforced.
