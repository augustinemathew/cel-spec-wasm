# Operator coverage tracker

**Mandate (2026-06-06): every single operator in the CEL language
surface MUST have at least one corpus cell.**  No exceptions, no
"representative samples" — exhaustive.  This file is the
accountability artifact: when an operator is uncovered, it shows up
here unticked.

**Lit/var pairing invariant (2026-06-16): every operator carries BOTH
a literal-operand cell and a variable-operand cell.**  The two share
the operator/kernel and differ only in operand binding — the var cell
binds operands through the activation, the literal twin (id +
`Const`) inlines them in `source` with no activation.  The
`(var − const)` delta isolates per-Eval variable-binding cost (each
bound activation variable costs ~13 ns/Eval vs ~0 for a literal),
which is otherwise invisible in a single number.  **Operand types the
loader cannot bind — timestamp, duration, map, message (see "Loader
schema" below) — are literal-only by necessity**; those rows note the
gap and have no `Const` twin (they ARE the literal form).

**Wiring status (2026-06-09):** `celwasm_bench` and `celcpp_bench`
both load every corpus YAML via `LoadCorpus(...)` and register one
Google Benchmark per cell.  Adding a new operator's coverage is a
**YAML edit, not a C++ edit**: drop a cell into the matching surface
file (or add a new file + the surface to
`celwasm_bench.cc::kCorpusFiles` + `BmPrefixForSurface` — in BOTH
bench mains).

**Core-operator grid filled 2026-06-09.**  Sixteen surfaces (was
thirteen until 2026-06-11): arithmetic, comparisons, comprehensions,
conversions, index, lists, **literals**, logic, long_strings, maps,
**policies**, **proto**, size, strings, ternary, time.  The 2026-06-11
expansion absorbed the legacy `bench/` eval benches into the corpus
(bound-list `in` sweeps, IAM-permission lists, the 1000-term mixed
polynomial, the literal-kind floor sweep, `int(string(123))`) and
added the proto-read + composite-policy tiers the new
message-activation loader unlocked.
Every operator family × admitted-type combination below is either
covered (cell id given) or excluded with a per-cell reason tag in
the YAML + a row in §"Exclusions" here.  Extension libraries
(string-ext, math-ext, encoders, lists-ext, sets-ext, optionals,
cel.bind) remain the open tail — see their sections.

**Skip-tag conventions** (a tagged cell stays in the YAML — no
silent gaps):

| tag prefix | meaning |
|---|---|
| `celwasm-skip-*` | cell cannot run through celwasm today; celcpp_bench still runs it.  Suffix names the reason (`-het-eq`, `-ternary-ident-cond`, `-map-dot-field`). |
| `celcpp-skip-*` | cell rejected by cel-cpp's checker as configured; celwasm_bench may still run it.  Cells carrying BOTH prefixes run nowhere and exist as documented grid exclusions. |

**Loader schema (documented in `corpus_loader.h`):** activation
values cover scalars (int/uint/double/bool/string/bytes), `list<T>`
of any scalar elem (explicit `values:` or generated
`gen: {range: N}` / `gen: {template: "…%07d", count: N}`), and proto
messages (`{type: message, message_type, textproto}` — parsed against
the generated descriptor pool by each bench main at registration).
Still missing: map-typed activations and timestamp/duration bindings.
Cells needing those construct the operand in-source from literals
(`timestamp("…")`, `{…}`) and — where the result type is itself
unexpressible in `expected:` (timestamp, duration, message, map) —
reduce it to a scalar via `int(…)` / `.getSeconds()` / `size(…)` /
`.field` / `== <literal>`.  The reduce is identical work on both
comparators, so the pairing stays honest; the cell comment names the
wrap.

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
| `+`      | ✓ sweep | ✓ | ✓ sweep | n/a (spec rejects mixed arithmetic) | intAdd{2,10,50,250,1000}Terms(+Const), uintAdd_simple(+Const), doubleAdd{…}Terms(+Const) |
| `-` bin  | ✓ sweep | ✓ | ✓ | n/a | intSub{…}Terms(+Const), uintSub_simple(+Const), doubleSub_simple(+Const) |
| `*`      | ✓ sweep | ✓ | ✓ | n/a | intMul{…}Terms(+Const), uintMul_simple(+Const), doubleMul_simple(+Const) |
| `/`      | ✓   | ✓    | ✓      | n/a | intDiv_simple(+Const), uintDiv_simple(+Const), doubleDiv_simple(+Const) |
| `%`      | ✓   | ✓    | n/a    | n/a | intMod_simple(+Const), uintMod_simple(+Const) |
| unary `-`| ✓   | n/a² | ✓      | n/a | intNeg(+Const), doubleNeg(+Const) |

Every arithmetic op now carries its literal-operand twin — the
op-coverage var simples gained `…Const` siblings (2026-06-16:
`intDiv_simpleConst`, `intMod_simpleConst`, `intNegConst`,
`uintAdd/Mul/Sub/Div/Mod_simpleConst`, `doubleMul/Sub/Div_simpleConst`,
`doubleNegConst`) to match the length-sweep cells that already had them.

¹ Mixed-numeric arithmetic is rejected by the static subset (and by
cel-cpp's checker); mixed-numeric *comparison* is tracked in the
comparison grid below.
² CEL has no unary `-` on uint.

Length sweep `{2, 10, 50, 250, 1000}` per intAdd/intMul/intSub/
doubleAdd, var + const variants.  `*1000TermsConst` cells (1000
literal CelValues = 24 KB rodata) fit the 256 KiB low-memory window
and run on both comparators.

Composite-shape cells (2026-06-11, absorbed from the legacy
`bench/` tree):

| cell id | expression | question it answers |
|---|---|---|
| arith.polyMix1000Terms | `a*d + b*a + … (1000 terms)` over 10 prime-bound vars | the legacy in_operator_bench long-arith headline: 1000 muls + 999 adds of CSE-resistant mixed terms — what does AOT straight-lining buy on compute-heavy arithmetic? |
| arith.abcAbcShapeVars | `a + b + c + a + b + c` (a=1,b=2,c=3) | activation-marshal cost on a fixed 6-term call graph (var half of the pair) |
| arith.abcAbcShapeLit | `1 + 2 + 3 + 1 + 2 + 3` | const twin of abcAbcShapeVars; the var−lit delta isolates the variable path |

**Rename note:** abcAbcShape{Vars,Lit} were hand-coded
registrations in `celwasm_bench.cc`
(`BM_arith_intAdd_AbcAbcShape_{Vars,Lit}Today`) from before the
corpus could express a var/lit adjacency pair.  They are corpus
cells now — BM names changed to
`BM_arith_abcAbcShape{Vars,Lit}` (corpus naming wins), and
cel-cpp gained the pair for free.

## Non-numeric `+` / `-` overloads

| operand types          | result   | cell id (surface)            | status |
|------------------------|----------|------------------------------|--------|
| string + string        | string   | strings.concat2, strings.concatChain{10,100,1000}Terms | ✓ |
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

**Lit/var twins (2026-06-16):** every scalar-comparison var cell above
(int/uint/double/bool/string/bytes × ==/!=/</<=/>/>=) now has a
literal-operand twin `<cell>Const` (e.g. `intEq`→`intEqConst`,
`stringLt`→`stringLtConst`, `bytesGe`→`bytesGeConst`).  The twin
mirrors its sibling's operator + true result with operands inlined as
literals (small operands: `42`, `"a"`/`"b"`, `b"a"`/`b"b"`), so
`(var − const)` is the per-Eval variable-binding cost on an identical
comparison.  `intLt`/`intLtConst` predate this expansion.  The
**timestamp / duration / list / map / null** rows stay literal-only —
the loader cannot bind those operand types (see "Loader schema"), so
the existing `ts*`/`dur*`/`listEq`/`mapEq`/`nullEq` cells ARE the
literal form and have no separate `Const` twin.  Bytes `…Const` cells
carry `skip-source-check` (the loader's identifier heuristic reads the
`b"…"` prefix as a variable).

Chained: cmp.intLtChain20 (`a<b && b<c && …`, 20 comparisons).
String-eq payload sweep: long_strings.eqLong_N{10,100,1000,10000}_{match,
mismatch} (the N=10000 cells now fit the 256 KiB window; see §Findings).

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
| `!`          | single / triple, var + const| logic.not1, logic.not1Const, logic.not3, logic.not3Const | ✓ |
| ternary `?:` | int result, computed cond   | ternary.intComputedCond, ternary.intConst | ✓ |
| ternary `?:` | string result, var + const  | ternary.stringComputedCond, ternary.stringConst | ✓ |
| ternary `?:` | nested chain                | ternary.nested3 | ✓ |
| ternary `?:` | bare bool-var condition     | ternary.intVarCond | ◐ `celwasm-skip-ternary-ident-cond` — **celwasm BUG, see §Findings** |

3VL-with-unknown shapes are PartialEval-only and out of the eval
corpus's scope (no unknown bindings in the bench activation schema).

---

## Membership `in`

Surfaces: `lists.yaml` (BM prefix `in_list`), `maps.yaml` (`map`).

| range kind     | elem/key types covered | cell ids | status |
|----------------|------------------------|----------|--------|
| list<string>   | length sweep {5,20,100,1000}, var + literal needle | lists.{5,20,100,1000}(_lit) | ✓ |
| list<string> literal, early-exit/miss | 100 elems, first-element + absent needle | lists.100_lit_first, lists.100_lit_miss | ✓ |
| list<int> BOUND (`x in xs`, activation-bound list) | length sweep {100, 1k, 10k, 100k, 1M}, x = N−2 worst case | lists.bound{100,1000,10000,100000,1000000} | ✓ |
| list<int> BOUND, 1M early-exit/miss | x = 0 (first) / x = −1 (absent) | lists.bound1000000_first, lists.bound1000000_miss | ✓ |
| list<string> BOUND, IAM-permission shape | 50-byte generated permissions, {100, 1000}, needle = last | lists.iam{100,1000} | ✓ |
| list<string> BOUND, IAM early-exit/miss | needle = first / absent (both 50 bytes) | lists.iam1000_first, lists.iam1000_miss | ✓ |
| list<int>      | 20 elems, worst-case scan | lists.int20 | ✓ |
| list<uint>     | "                       | lists.uint20 | ✓ |
| list<double>   | "                       | lists.double20 | ✓ |
| list<bool>     | "                       | lists.bool20 | ✓ |
| list<bytes>    | —                       | — | ✗ excluded: bytes list literals add no kernel path beyond string (same memcmp scan); revisit if the kernel ever specializes |
| list<message>  | —                       | — | ✗ excluded: message-ELEMENT lists are still outside the loader schema (list elems are scalar-only); message ACTIVATIONS landed — see §Proto-message reads |
| map<string,V>  | 10 keys, var + const    | maps.inString, maps.inStringConst | ✓ |
| map<int,V>     | 10 keys, var + const    | maps.inInt, maps.inIntConst | ✓ |
| map<uint,V>    | 10 keys, var + const    | maps.inUint, maps.inUintConst | ✓ |
| map<bool,V>    | 2 keys, var + const     | maps.inBool, maps.inBoolConst | ✓ |

The bound cells mirror the legacy `bench/in_operator_bench.cc`
shapes (BM_Eval_In_IntList_Bound_WorstCase, BM_Eval_In_1M_*,
BM_Eval_In_IamPermissions_Bound_Last) — the production "build the
permission set once, query many times" path the literal cells
can't reach (parser 100k-codepoint cap).  The IAM bound sweep
stops at 1000 deliberately: the legacy bench measured an arena
OOM at N=10000 on the bound-string scan (note in lists.yaml).

---

## Indexing `_[_]`

Surface: `index.yaml` (BM prefix `idx`).

| shape          | cell ids | status |
|----------------|----------|--------|
| list[int], var + const key | idx.listInt, idx.listIntConst | ✓ |
| map[string], var + const key | idx.mapString, idx.mapStringConst | ✓ |
| map[int], var + const key | idx.mapInt, idx.mapIntConst | ✓ |
| map[uint], var + const key | idx.mapUint, idx.mapUintConst | ✓ |
| map[bool], var + const key | idx.mapBool, idx.mapBoolConst | ✓ |

---

## Selection / field access + has()

| shape | cell ids | status |
|-------|----------|--------|
| map.field sugar (string key) | maps.dotField | ◐ `celwasm-skip-map-dot-field` (cleanup-backlog #9: Select routes through the message-field trampoline → type_mismatch error) |
| `has(map.field)` | maps.hasKey | ◐ same gap (e2e/known_bugs_test.cc::HasOnMapPresentKey) |
| message.field | proto.read_*, proto.cust_*, proto.select_depth{1,2,4} | ✓ — see §Proto-message reads |
| `has(msg.field)` | — | ✗ TODO: no cell yet; the message-activation plumbing exists now (proto surface), add when the published table wants presence-test numbers |

---

## Comprehension macros

Surface: `comprehensions.yaml` (BM prefix `compr`).  Ranges are
20-element literal int lists unless noted; `map`/`filter` results are
`size(…)`-reduced (see loader-gap note above).

| macro        | cell ids | status |
|--------------|----------|--------|
| `all`        | compr.all20 + length sweep compr.all{10,100,1000} | ✓ |
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
| bytes (var + const) | size.bytes, size.bytesConst | ✓ |
| list   | size.list{10,100,1000} | ✓ |
| map    | size.map{10,100} | ✓ |

---

## String functions (standard library)

Surface: `strings.yaml` + `long_strings.yaml` (BM prefix `str`).

Every string-function var cell now has a literal-receiver twin
(`<cell>Const`, 2026-06-16: the haystack is a string literal, not a
bound variable), plus the `+` concat pairs (`concat2`/`concat2Const`,
`bytesConcat2`/`bytesConcat2Const`).

| fn | cell ids | status |
|----|----------|--------|
| `_.contains(_)` | strings.contains, strings.containsConst, long_strings.containsLong_N{10,100,1000,10000} | ✓ |
| `_.startsWith(_)` | strings.startsWith, strings.startsWithConst | ✓ |
| `_.endsWith(_)` | strings.endsWith, strings.endsWithConst | ✓ |
| `_.matches(_)` cheap anchor | strings.matchesCheap, strings.matchesCheapConst | ✓ |
| `_.matches(_)` moderate regex (alternation + bounded repeat) | strings.matchesComplex, strings.matchesComplexConst | ✓ |

---

## Type conversions

Surface: `conversions.yaml` (BM prefix `conv`).

| from \ to | int | uint | double | string | bytes | timestamp | duration |
|-----------|-----|------|--------|--------|-------|-----------|----------|
| int       | n/a | ✓ uintFromInt | ✓ doubleFromInt | ✓ stringFromInt | n/a | ✗¹ | ✗¹ |
| uint      | ✓ intFromUint | n/a | ✓ doubleFromUint | ✓ stringFromUint | n/a | n/a | n/a |
| double    | ✓ intFromDouble | ✓ uintFromDouble | n/a | ✓ stringFromDouble | n/a | n/a | n/a |
| string    | ✓ intFromString, intFromStringNested³ | ✓ uintFromString | ✓ doubleFromString | n/a | ✓ bytesFromString | ✓ timestampRoundTrip² | ✓ durationRoundTrip² |
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
³ intFromStringNested is the const-only nested form
`int(string(123))` (the legacy bench/pipeline_bench.cc
BM_Eval_IntFromString shape): two chained conversion kernels, no
activation — pairs with the var-bound intFromString to isolate the
back-to-back-conversion cost.

`bool(string)` / `string(bool)`: string→bool excluded (same
non-uniform-support bucket¹); bool→string covered (stringFromBool).
`dyn(_)` is rejected by celwasm's static subset by design — not a
corpus row.

**Lit/var twins (2026-06-16):** every simple scalar conversion var
cell now has a literal-argument twin `<cell>Const` (the convert kernel
runs on a literal instead of a bound activation value):
`intFromUint/Double/StringConst`, `uintFromInt/Double/StringConst`,
`doubleFromInt/Uint/StringConst`,
`stringFromInt/Uint/Double/Bool/BytesConst`, `bytesFromStringConst`.
`intFromStringConst` (`int("42")`) is the direct twin of
`intFromString`; the existing `intFromStringNested` (`int(string(123))`)
remains a separate two-kernel shape, not a lit twin.  The
timestamp/duration round-trips were already literal-only (loader
cannot bind those source types).

---

## Timestamp / duration operators + accessors

Surface: `time.yaml` (BM prefix `time`).  Arithmetic rows are in the
`+`/`-` overload table above.

| accessor | UTC | named tz | cell ids |
|----------|-----|----------|----------|
| `getFullYear` | ✓ (+ year-9999 boundary: tsGetFullYearUtcMax) | ✓ | time.tsGetFullYearUtc, time.tsGetFullYearUtcMax, time.tsGetFullYearTz |
| `getHours`    | ✓ | ✓ | time.tsGetHoursUtc, time.tsGetHoursTz |
| `getSeconds`  | ✓ | ✓ | time.tsGetSecondsUtc, time.tsGetSecondsTz |
| `getDayOfWeek` | ✓ | ✗ | time.tsGetDayOfWeekUtc (2024-06-15 = Saturday → 6; 0-based from Sunday) |
| duration `getHours` / `getSeconds` | ✓ | n/a | time.durGetHours, time.durGetSeconds |
| `getDate`/`getDayOfMonth`/`getDayOfYear`/`getMilliseconds`/`getMinutes`/`getMonth` (+ `getDayOfWeek` tz) | ✗ | ✗ | TODO: not yet covered — same kernel family as the covered accessors (one trampoline per accessor); add when the published table wants the full accessor sweep |

---

## Literal floor sweep

Surface: `literals.yaml` (BM prefix `lit`).  Five const-only cells —
the bare literal per scalar kind (the legacy
`bench/cel_pipeline_bench.cc` input sweep).  Each cell is the floor
of the whole pipeline: $eval dispatch + one CelValue decode and
nothing else; per-kind spread here is decode/marshal cost.

| cell id | expression | question it answers |
|---|---|---|
| lit.int | `42` | int decode floor |
| lit.bool | `true` | bool decode floor |
| lit.double | `3.14` | double decode floor |
| lit.string | `"hello"` | string (rodata + variable-length payload) decode floor |
| lit.null | `null` | null decode floor (`expected: {type: null}`; both mains label it `result=<non-numeric>`) |

---

## Proto-message reads

Surface: `proto.yaml` (BM prefix `proto`).  **The proto-message
exclusion that used to live here is closed (2026-06-11):** the
loader gained the `{type: message, message_type, textproto}`
activation form, both bench mains parse the textproto against the
generated descriptor pool (`//testdata` fixture cc_protos are
linked into BOTH binaries via `kDescriptorsLinked` — celcpp_bench
stays first-party-free; cc_proto targets carry none of the
clashing `cel::` symbols).  Fixtures: `m` =
celwasm.testdata.HostMsg3, `c` = celwasm.testdata.Customer.  Every
cell reduces to a scalar (the parity hook).

| cell id | expression | question it answers |
|---|---|---|
| proto.read_s | `m.s` | string singular read (variable-length payload marshal) |
| proto.read_b | `m.b` | bool singular read |
| proto.read_f64 | `m.f64` | double singular read |
| proto.read_u64 | `m.u64` | uint64 singular read |
| proto.cust_name | `c.name` | Customer-domain string read (e2e-fixture twin of read_s) |
| proto.cust_age | `c.age` | int32→CEL-int read (sign-extending load) |
| proto.cust_is_premium | `c.is_premium` | Customer bool read |
| proto.select_depth1 | `m.i64` | depth-1 baseline for the nesting sweep |
| proto.select_depth2 | `m.inner.i64` | +1 nested-message hop cost |
| proto.select_depth4 | `m.inner.inner.inner.i64` | +3 hops — is per-hop cost linear? |
| proto.select_depth8 | `m.inner…(x7).i64` | deep-nesting tail of the per-hop sweep |
| proto.select_depth16 | `m.inner…(x15).i64` | pathological nesting — per-hop cost still linear at depth 16? |
| proto.rep_i32_at0 | `m.rep_i32[0]` | repeated-scalar index, first element |
| proto.rep_i32_at9 | `m.rep_i32[9]` | repeated-scalar index, last of 10 (index-position sensitivity) |
| proto.rep_msg_at1_s | `m.rep_msg[1].s` | repeated-MESSAGE index + select (list_at returning a message) |
| proto.tags_at2 | `c.tags[2]` | the exact legacy BM_Eval_ListAt_Proto shape (5 tags) |
| proto.map_str_i32 | `m.str_to_i32["b"]` | proto map lookup, string key |
| proto.map_i64_str | `m.i64_to_str[2]` | proto map lookup, int key |
| proto.map_str_msg_i64 | `m.str_to_msg["k"].i64` | map lookup returning a message + select |
| proto.metadata_b | `c.metadata["b"]` | the exact legacy BM_Eval_MapLookup_Proto shape |
| proto.pair_list_arena | `[10, 20, 30, 40, 50][2]` | arena half of the list-index dispatch-crossover pair |
| proto.pair_map_arena | `{"a": 1, "b": 2, "c": 3}["b"]` | arena half of the map-lookup crossover pair |
| proto.construct_name | `celwasm.testdata.Customer{name: "Ada"}.name` | message construction + immediate select (scalar-reduced kStructExpr) |
| proto.reads5 | `m.i32 + m.i64 + m.si32 + m.si64 + m.sfx32` | 5 reads off one bound message — per-read slope |
| proto.reads10 | 10 int reads `+`-combined | 10 reads (6 distinct int fields + 4 repeats; host-call reads are CSE-opaque) |

Still outside the schema: message-ELEMENT lists (`msg in [msg]`,
list<message> activations), map-typed activations, message-typed
`expected:` (cells select-reduce instead), `has(msg.field)` cells
(plumbing exists; not yet written).

---

## Composite policies

Surface: `policies.yaml` (BM prefix `policy`).  Realistic
authz/admission shapes combining the axes the proto surface
measures in isolation; answers "do the per-op costs compose
linearly in a real policy?".  Every cell carries a `# why:` line in
the YAML (DESIGN.md §6.4).

| cell id | expression (abbrev.) | question it answers |
|---|---|---|
| policy.ternary2 | `c.age > 30 ? (c.is_premium ? "gold" : "silver") : "basic"` | 2-deep ternary nesting over proto-read conditions |
| policy.ternary5 | `c.age > 60 ? "a" : … : "f"` (5 arms) | per-arm cost of a ternary cascade re-reading one proto field |
| policy.premium_gate | `c.is_premium ? c.age : 0` | bare proto-bool select as ternary condition (does the ternary-ident-cond bug extend to selects?) |
| policy.str_in_list | `m.s in ["alpha", …]` | proto read feeding an arena literal-list scan (trampoline→wasm handoff) |
| policy.arena_map_gate | `c.age >= {"us": 21, "de": 18}["us"]` | arena map-literal lookup vs proto field (wasm→trampoline handoff) |
| policy.authz_basic | `(c.is_premium && c.age >= 18 && c.name in […]) ? "allow" : "deny"` | the headline 3-read + in-list + ternary allow/deny cost |
| policy.authz_deep | same, depth-2/3 selects on `m.inner…` | what select depth adds to a whole policy |
| policy.mega100 | 25-arm nested ternary, every condition = depth-1..8 select + map lookup + repeated index; 159 proto accesses/Eval | the policy-engine stress number |
| policy.authz_deep8 | authz with every condition behind an 8-deep chain | does (reads x depth) compose, or do same-spine walks amortise? |
| policy.quota_check | `m.str_to_i32["used"] + …["pending"] < …["limit"]` | 3 proto-map lookups + arithmetic + comparison (quota shape) |
| policy.tier_route | `c.balance_cents >= 100000u ? … : (… c.metadata["tier"] …)` | ternary chain across uint compare / bool select / map lookup |
| policy.risk_score | `c.credit_score >= 700.0 && c.balance_cents > 1000u` | mixing payload kinds (double + uint) in one policy |

---

## Exclusions (cells no comparator runs)

| cell | reason |
|------|--------|
| cmp.intEqDouble | heterogeneous `==`: celwasm checker rejects (`celwasm-skip-het-eq`); cel-cpp checker rejects too — equality stays homogeneous at check time even with `enable_cross_numeric_comparisons` (`celcpp-skip-het-eq-check`).  Kept in YAML as the documented grid row. |
| cmp.nullEqInt | same: `1 == null` rejected by both checkers as configured. |

Bench shapes that are NOT corpus-representable (no cell exists, by
design — this list is the accountability record):

| shape | why not representable | where it IS covered |
|-------|----------------------|---------------------|
| error-path evals (div-by-zero, modulo-by-zero, map key miss, list index out of range) | both bench mains `ABSL_CHECK_OK` the Eval result and stamp a value label — the harness requires Ok results, and an error-result `expected:` form doesn't exist | kernel-level only: `//benchmark/kernel` + the runtime/e2e error-path tests pin semantics; eval-corpus rows would need a harness extension (error-kind labels) first |
| 10 000-element LITERAL int list eval | known celwasm fault: compile + Plan succeed but Eval traps in wasmtime (`store.rs:2440 assertion failed: fault.is_none()`, noted in the legacy bench) — a corpus cell would crash celwasm_bench at registration | compile-side cost covered by `//benchmark/compiler:in_operator_compile_bench`; the eval-side BOUND path covers N=10000+ (lists.bound10000…) |
| unknown-merge / partial-eval shapes (3VL with unknowns, comprehension over unknown range) | the corpus activation schema has no unknown-attribute bindings; PartialEval is a different API surface than `Instance::Eval(Activation)` | partial-eval component tests; out of the eval corpus's scope by design |

## celwasm-skipped cells (celcpp-only, ◐)

| tag | cells | reason |
|-----|-------|--------|
| `celwasm-skip-het-eq` | cmp.intLtDouble (+ the two §Exclusions cells) | checker rejects mixed-numeric comparison; cel-cpp runs it with `enable_cross_numeric_comparisons`. |
| `celwasm-skip-ternary-ident-cond` | ternary.intVarCond | **celwasm bug**, see §Findings. |
| `celwasm-skip-map-dot-field` | map.dotField, map.hasKey | cleanup-backlog #9 Select-on-map gap. |

## Findings (correctness divergences surfaced by this corpus)

1. **Ternary with a bare bool-variable condition returns null
   (celwasm, 2026-06-09).**  `c ? x : y` with `c` bound via the
   activation evaluates to **null**; computed conditions
   (`a > b ? …`, `(c || false) ? …`, `!c ? …`) are correct.
   Verified via the cel CLI in both link modes.  Pinned by
   ternary.intVarCond (`celwasm-skip-ternary-ident-cond`).
2. **(RESOLVED) Dynamic-mode silent wrong answer past the rodata
   bound (celwasm, 2026-06-09; closed when the window was raised to
   256 KiB).**  When the window was 8 KB, `a == "<10000 x's>"` with
   `a` equal diverged by link mode: static rejected at compile while
   dynamic **compiled and returned false** (cel-cpp: true).  Both
   halves are now closed — the 256 KiB window admits a 10 KB literal,
   and the compile-time rodata gate (`CheckStaticWindowFits` in
   layout_pass.cc) rejects an over-budget layout loudly in BOTH link
   modes.  long_strings.eqLong_N10000_match now runs on celwasm and
   evaluates true.
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
