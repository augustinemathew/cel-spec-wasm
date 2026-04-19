# M7 — Standard library completeness

Status: **planned.**  Blocked on M6 — the host-import mechanism for
stdlib overloads that delegate out of the module (regex, message
equality) reuses M6's custom-function path.

## Scope

Cover every function / overload listed in `../langdef.md` §stdlib and
in `../extensions/` that the static subset allows.  Up to M6 we've
implemented a handful opportunistically (arithmetic, comparisons,
size, string concat / startsWith / endsWith / contains); M7 is where
we close the gap and stop deferring.

Post-M7, every entry in:

  - `../langdef.md` §stdlib (plain CEL stdlib)
  - `../extensions/strings.md` (string extension)
  - `../extensions/math.md` (math extension) — if added
  - `../extensions/lists.md` (list extension) — if added
  - `../extensions/protos.md` (proto extension) — if added
  - `../extensions/sets.md` (set extension) — if added

that's valid in the static subset has a codegen path + at least one
positive and one negative e2e test.

## Deliverables (by sub-area)

### Temporal

- [ ] `timestamp(string) → timestamp` + inverse.  Parse RFC3339 at
      compile time where the argument is a literal; otherwise a host
      import.
- [ ] `duration(string) → duration` + inverse.
- [ ] `getDate`, `getDayOfMonth`, `getDayOfWeek`, `getDayOfYear`,
      `getFullYear`, `getHours`, `getMilliseconds`, `getMinutes`,
      `getMonth`, `getSeconds`.  All take an optional timezone arg.
- [ ] Arithmetic on timestamps / durations (`_+_`, `_-_`).
- [ ] Comparisons.

### Bytes

- [ ] `bytes(string)` / `string(bytes)` conversion (size-preserving,
      UTF-8 validated when going bytes → string).
- [ ] `size(bytes)`, `bytes + bytes` (concat), equality, indexing.

### Regex

- [ ] `matches(string, pattern)` — the pattern is a constant in a
      well-formed static expression; pre-compile at compile time into
      a `pattern_id` interned in `cel.abi.patterns`; the host provides
      `cel_host.string_matches(s, pattern_id) → i32`.
- [ ] Pattern literal extraction pass in `expr_lower` — walks the
      AST pre-codegen, pulls every regex literal into the intern
      table, rewrites the call to take a `pattern_id`.
- [ ] Negative test: a non-literal pattern (`matches(s, pattern_var)`)
      is rejected with an "expected constant regex" diagnostic.

### Math extension (if admitted to static subset)

- [ ] `abs`, `ceil`, `floor`, `round`, `sign`, `trunc`, `min`, `max`,
      `isNaN`, `isInf`, `sqrt`, `log`, `exp`, `pow`.  Most are direct
      WASM instructions (`f64.abs`, `f64.sqrt`, …); a few
      (`pow`, `log`, `exp`) need libm — host import.

### String extension

- [ ] `charAt`, `indexOf`, `lastIndexOf`, `lowerAscii`, `upperAscii`,
      `replace`, `split`, `trim` and friends per
      `../extensions/strings.md`.  Most lower to a runtime helper;
      `replace` + `split` require allocation so route through the bump
      arena.

### Format directives

- [ ] `"%.2f".format(3.14)` — pre-format-compile literal format
      strings into a tiny internal opcode stream interned in
      `cel.abi.patterns` (shared table with regex).  The runtime
      helper `cel_fmt_apply` consumes the opcode stream + args.
- [ ] Spec conformance test for every %-directive the spec defines.

### Proto extension

- [ ] `proto.getExt` / `proto.hasExt` — extension field access.
      Needs `cel_host.get_extension(externref, i32 ext_id)`.

## Testing obligations

Per-function e2e coverage in `compiler/e2e/stdlib_test.cc`:

- [ ] Every function gets **two positive cases** (canonical + edge
      case) and **one negative case** (wrong-type argument at
      runtime → ERROR, or wrong-type argument at compile time →
      checker rejection).
- [ ] Conformance fixture: every `simple.textproto` file under
      `tests/simple/testdata/` that exercises a stdlib function is
      admitted to the M8 run.  M7 must not regress any fixture that
      the current scalar slice already passes.

## Open design questions

1. **Pre-compiled regex portability.** The host-side regex engine
   needs to match the spec's RE2-ish semantics.  If we pre-compile
   to a DFA host-side, each host binding carries a DFA loader.  A
   tempting alternative: pre-compile to a portable opcode stream
   that a tiny WASM-side matcher consumes.  Perf unclear — defer
   until M7 real use.
2. **Format string syntax check.** Spec has a full grammar for `%`
   directives; parsing it with cel-cpp's parser or a hand-rolled
   scanner is an open question.  Leaning hand-rolled for
   simplicity; it's <100 LOC.
