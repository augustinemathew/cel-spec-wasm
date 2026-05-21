# M12 — `string_ext` extension (self-hosted in runtime)

Status: **in progress — Slices A + B + C + multi-TU split + D landed 2026-05-20.**  Scope
is the cel-cpp `strings` extension library: 13 string functions
+ a printf-style `format` directive parser, all self-hosted
inside `cel_runtime.wasm` against vendored absl.  Conformance
ceiling: **+94 PASS** (the 94 `string_ext.textproto::ext_unimpl`
rows; the 44 `disable_check` rows stay out-of-scope by design,
and the 78 `envelope` rows wait on harness-side matcher
unification).  Net `string_ext.textproto` 0/216 → 94/216.

## 1. Why M12

The cel-cpp `strings` extension is the single most-used CEL
extension library across real-world policies (charAt /
indexOf / replace / split / trim / format).  It currently
fails open at the checker stage with an `ext_unimpl`
classification — every embedder who needs even one of these
functions has to bolt on a custom `cel_host`-trampoline today.
Self-hosting the kernel set in `cel_runtime.wasm` (the Phase C
pattern that landed `matches` and the timestamp/duration
parse + format families) eliminates the workaround surface
for the most common ext-lib request.

Conformance-wise it's also the **second-largest single
ext_unimpl bucket** (94 rows, after `math_ext` at 116) — and
unlike `math_ext`, it lights up the `format` directive parser
that downstream features (audit logging, error formatting in
custom evaluators, structured policy diagnostics) routinely
need.

## 2. Scope

In scope — 13 functions × 19 overloads + the format parser:

| Function | Overloads | Notes |
|---|---|---|
| `charAt(s, i)` | `string_char_at_int` | Code-point at index `i`; out-of-range → empty string. |
| `indexOf(s, sub)` / `indexOf(s, sub, i)` | `string_index_of_string`, `string_index_of_string_int` | Byte-level search; returns code-point index or -1. |
| `lastIndexOf(s, sub)` / `lastIndexOf(s, sub, i)` | `string_last_index_of_string`, `string_last_index_of_string_int` | Symmetric to `indexOf`. |
| `s.lowerAscii()` / `s.upperAscii()` | `string_lower_ascii`, `string_upper_ascii` | ASCII-only case fold per spec (non-ASCII bytes pass through). |
| `s.replace(old, new)` / `s.replace(old, new, n)` | `string_replace_string_string`, `string_replace_string_string_int` | First `n` occurrences, or all if `n` omitted / negative. |
| `s.split(sep)` / `s.split(sep, n)` | `string_split_string`, `string_split_string_int` | Returns list-of-string; empty `sep` splits per code point. |
| `s.substring(i)` / `s.substring(i, j)` | `string_substring_int`, `string_substring_int_int` | Code-point indexed; out-of-range → error. |
| `s.trim()` | `string_trim` | Unicode whitespace per cel-cpp's `extensions/strings.cc` whitespace table. |
| `list.join()` / `list.join(sep)` | `list_join`, `list_join_string` | Consumes a list of strings, returns a single string. |
| `strings.quote(s)` | `strings_quote` | Wraps in `"..."`, escapes per CEL string-literal rules. |
| `s.format(args)` | `string_format` | Printf-style with type-checked directives.  See §4.3. |
| `s.reverse()` | `string_reverse` | Code-point reversal (NOT byte reversal). |

Out of scope (deliberately):

  - **`format_errors` + `type_errors` corpus sections** (44 rows)
    — both carry `disable_check: true`, so they exercise
    parse-only eval which our pipeline doesn't support
    (CLAUDE.md "What not to do" — `RejectDyn` plus the
    checker-passed-only contract).  Stays SKIP.
  - **`value_errors` envelope unification** (9 rows) — the
    matchers use `eval_error { errors { message: "..." } }`
    shapes the harness doesn't compare today.  Lands in a
    harness-side slice; conformance-unlock-plan §Slice 3 +
    the envelope-unification bullet in the M12 follow-up
    section.  Stays SKIP at M12 close.
  - **`format` precision beyond cel-cpp's
    `StringsExtensionFormatOptions::max_precision` default
    (1000)**.  Match cel-cpp's bound; document the cap.
  - **Locale-aware case folding / collation.**  ASCII only,
    per spec.
  - **Unicode normalization** (NFC/NFD).  Spec doesn't require;
    don't add.
  - **`%w` (binary), `%v` (verbose Go-style), or any
    cel-cpp-extension directive beyond the documented set
    (`%d %f %s %e %b %o %x %X`).**  If cel-cpp adds new
    directives, M12 absorbs them; today's spec is the freeze.

## 3. Why self-hosted in runtime (not host trampolines)

Same trade as Phase C's matches kernel:

  - **One round trip per call instead of one round trip per
    Eval.**  String ops fire on every comparison / template
    expansion / policy evaluation; the wasm→host boundary is
    the wrong place to spend cycles per call.  Self-hosted in
    `cel_runtime.wasm` they're a single `(call $string_X)`
    op against linear memory.
  - **No host-side state means no per-Instance caching
    headaches.**  Format strings are typically literals known
    at codegen time; we can pre-parse directive sequences and
    cache the parsed shape in a module-static slot (mirrors
    `cel_matches`'s single-slot most-recent-pattern cache).
  - **Vendored absl is already in the wasm runtime** (Phase C
    C1).  `absl::StrSplit` / `absl::StrReplaceAll` /
    `absl::StrFormat` are free to call.  `absl::StrFormat` is
    NOT the cel-cpp format spec, but `formatting.cc` in
    cel-cpp re-uses `absl::FormatArg` + a hand-rolled
    directive parser — we vendor that pattern.
  - **No new descriptor-pool reads.**  Pure operate-on-CelValue
    kernels — no need to reach into the host's protobuf descriptor
    pool, no externref roundtrip.  That's why string_ext is a
    cleaner self-host target than (say) `proto2_ext` or
    `network_ext`'s IP-range parsing.

## 4. Final file structure

### 4.1 New runtime files

> **Plan-vs-execution delta (added 2026-05-20 during Slice B
> review).**  The original §4.1 sketch placed every non-format
> kernel in a single `cel_string_ext.cc`.  After Slice A + B
> landed (~761 LOC), the projected end-state for Slices A-D
> would push that single TU past 1500 LOC — well over the
> readable-per-file budget.  The TU is now split per-topic
> (mirrors the `cel-runtime-c-split-plan.md` pattern): a single
> public header + a private shared header + one TU per slice.
>
> Tests get the matching split: one test TU per kernel TU, with
> a shared `string_ext_test_helpers.h` fixture (the helper was
> always called out in §5.3; the test split formalises it).

**Public header.**  Unchanged shape — one file, all kernels.

| File | ~LOC | Contents |
|---|---|---|
| `cel_string_ext.h` | ~150 | Public ABI: every `cel_string_ext_*_at_v*` declaration (Slices A-D).  This is the only file other TUs `#include` for the kernel ABI — the multi-TU split is invisible to call sites. |

**Private shared header + impl.**  New, sits between the public
header and the per-topic TUs.

| File | ~LOC | Contents |
|---|---|---|
| `cel_string_ext_internal.h` | ~120 | `static inline` helpers shared by every kernel TU: `Poison`, `Absorb3vlUnary` / `_Binary`, `BorrowSpan`, `WriteStringFromBytes` / `_Span` / `WriteSubspan` / `WriteInt`, and the `Utf8Decode` / `Utf8DecodeMulti` / `PrevCodepoint` / `IsUnicodeWhitespace` UTF-8 helpers.  Per `cel-runtime-c-split-plan.md` §2, keeping these `static inline` preserves cross-TU inlining without forcing extern definitions. |
| `cel_string_ext_internal.cc` | ~30 | Non-inline bodies for the rare helpers too big to inline (currently empty — every helper inlines cleanly; left as a placeholder TU for the `BuildReplaced` / format-directive renderer that Slice E may want to factor out). |

**Per-topic kernel TUs.**  One per slice; each builds on
`cel_string_ext_internal.h`.

| File | ~LOC | Contents |
|---|---|---|
| `cel_string_ext_codepoint.cc` | ~250 | Slice A: `charAt`, `lowerAscii`, `upperAscii`, `trim`, `reverse`.  Plus the `AsciiFoldInto` helper shared by the two case-fold kernels. |
| `cel_string_ext_search.cc` | ~280 | Slice B: `indexOf` × 2, `lastIndexOf` × 2, `substring` × 2, `replace` × 2.  Shared `IndexOfImpl` / `LastIndexOfImpl` / `CodepointToByteOffset` / `ValidatePos` / `BuildReplaced` / `DoReplace` helpers in this TU's anonymous namespace. |
| `cel_string_ext_list.cc` | ~200 | Slice C: `split` (×2), `join` (×2).  Bridges to `cel_list_*` arena-list constructors. |
| `cel_string_ext_quote.cc` | ~80 | Slice D quote: `strings.quote`.  Escape-sequence matrix mirrors cel-cpp's `StringValue::Quote`. |

**Format TU pair (Slices D parser + E renderer).**  Already
separate from `cel_string_ext` per the original §4.1.

| File | ~LOC | Contents |
|---|---|---|
| `cel_string_format.h` | ~80 | Public ABI: `cel_string_format_at_vv(out, fmt, args_list)`.  `FormatDirective` enum mirroring cel-cpp's `extensions/formatting.cc`. |
| `cel_string_format.cc` | ~500 | Directive parser + per-(directive × CelKind) renderer.  Per-Instance single-slot most-recent-format cache (parsed directive sequence keyed by raw format-string bytes, same cache shape as `cel_matches`). |

**BUILD wiring (single `:cel_string_ext` cc_library).**  Mirrors
the existing `:cel_runtime` aggregator pattern — one library
target absorbing every TU; call sites depend on
`:cel_string_ext` and get the full ABI.  Per-TU bazel targets
would add granularity at the cost of cross-TU inlining (the
`-O3 -flto` knobs in `:cel_runtime` rely on the single-library
shape).

```python
cc_library(
    name = "cel_string_ext",
    srcs = [
        "cel_string_ext_codepoint.cc",
        "cel_string_ext_internal.cc",
        "cel_string_ext_internal.h",
        "cel_string_ext_list.cc",
        "cel_string_ext_quote.cc",
        "cel_string_ext_search.cc",
    ],
    hdrs = ["cel_string_ext.h"],
    deps = [":cel_runtime", "@com_google_absl//absl/strings"],
)
```

### 4.2 Codegen change summary

`compiler_v2/codegen/overload_table.cc` — seed 19 overload IDs.
Each points at one of the new runtime exports.  All 19 leave
`kExplicitlyUnimplementedIds` (today's classification).  Seed
count 158 → 177.

| Overload ID | Runtime export |
|---|---|
| `string_char_at_int` | `cel_string_char_at_at_vv` |
| `string_index_of_string` | `cel_string_index_of_at_vv` |
| `string_index_of_string_int` | `cel_string_index_of_at_vvv` |
| `string_last_index_of_string` | `cel_string_last_index_of_at_vv` |
| `string_last_index_of_string_int` | `cel_string_last_index_of_at_vvv` |
| `string_lower_ascii` | `cel_string_lower_ascii_at_v` |
| `string_upper_ascii` | `cel_string_upper_ascii_at_v` |
| `string_replace_string_string` | `cel_string_replace_at_vvv` |
| `string_replace_string_string_int` | `cel_string_replace_n_at_vvvv` |
| `string_split_string` | `cel_string_split_at_vv` |
| `string_split_string_int` | `cel_string_split_n_at_vvv` |
| `string_substring_int` | `cel_string_substring_at_vv` |
| `string_substring_int_int` | `cel_string_substring_range_at_vvv` |
| `string_trim` | `cel_string_trim_at_v` |
| `list_join` | `cel_string_join_at_v` |
| `list_join_string` | `cel_string_join_sep_at_vv` |
| `strings_quote` | `cel_string_quote_at_v` |
| `string_format` | `cel_string_format_at_vv` |
| `string_reverse` | `cel_string_reverse_at_v` |

No `expr_lower.cc` changes — every overload routes through the
existing `EmitGeneralCall` arm (the same path arithmetic /
matches / startsWith use today).

### 4.3 Format directive language

Per the cel-cpp spec (`extensions/formatting.{cc,h}`), the
format string is a sequence of literal characters interspersed
with directives of the form `%[.<precision>]<type>` where
`<type>` is one of:

| Directive | Accepts CelKind | Renders as |
|---|---|---|
| `%d` | INT, UINT | decimal integer |
| `%f` | DOUBLE, INT, UINT | fixed-point with precision (default 6) |
| `%e` | DOUBLE, INT, UINT | exponential with precision (default 6) |
| `%s` | STRING, BYTES (UTF-8 view), BOOL, TIMESTAMP, DURATION, LIST, MAP, NULL_VALUE, TYPE, INT, UINT, DOUBLE | spec-defined canonical string form |
| `%b` | INT, UINT | binary integer |
| `%o` | INT, UINT | octal integer |
| `%x` / `%X` | INT, UINT, STRING, BYTES | lowercase/uppercase hex |

Errors:

  - Mismatch between arg-list length and directive count →
    `CEL_ERROR(kInvalidArgument, "format: N directives but
    M args")`.
  - Directive type incompatible with arg kind → `CEL_ERROR(
    kInvalidArgument, "format: %<dir> expects <kinds>, got <kind>")`.
  - Precision > `max_precision` (1000) → error.
  - Malformed directive (e.g. `%` at end-of-string, `%.5` with
    no type) → error.

Implementation note: the directive parser walks the format
string once, producing a small `std::vector<DirectiveOp>` where
each op is either a literal-byte range or a directive entry.
Cached per format-string (single-slot cache keyed on raw
bytes); the renderer walks the directive list against the args.

## 5. Test coverage strategy

The central deliverable.  M12 adds ~165 new tests across 4 new
test files + targeted entries in existing e2e files.

### 5.1 Per-TU test matrix

> **Plan-vs-execution delta (2026-05-20).**  Original §5.1 had
> one `cel_string_ext_test.cc` per kernel TU.  Splitting the
> kernel TU per topic (see §4.1 delta) splits tests in
> lockstep — same per-slice scenario lists, just routed to
> dedicated test files so each stays under ~500 LOC and
> compile-iteration time during Slice C-E development stays
> short.  Shared fixture moved into `string_ext_test_helpers.h`
> (always called out in §5.3, now formalised).

| TU | Test file | Core scenarios |
|---|---|---|
| `cel_string_ext_codepoint` | `cel_string_ext_codepoint_test.cc` | Slice A: charAt boundary + multi-byte matrix, lowerAscii / upperAscii fold matrix + embedded-NUL guard, every Unicode-whitespace code-point × trim parameterised table, reverse mixed-width code-point matrix.  Plus the shared `UnaryEnvelopeTable` over all 4 `_at_v` kernels. |
| `cel_string_ext_search` | `cel_string_ext_search_test.cc` | Slice B: indexOf / lastIndexOf spec rows + boundary matrix (negative pos, pos beyond byte size).  substring spec rows + boundary (negative start, end<start, end>size).  replace spec rows + empty-needle interleaving + n=0 / n<0 / chained limit behaviour. |
| `cel_string_ext_list` | `cel_string_ext_list_test.cc` | Slice C: split / join.  Empty list, single-element list, very long element strings, arena-list lifetime, sep-vs-no-sep variants. |
| `cel_string_ext_quote` | `cel_string_ext_quote_test.cc` | Slice D quote: every escape sequence (`\\`, `\"`, `\n`, `\t`, `\r`, `\0`, `\xNN`).  Verbatim strings (no escape needed) round-trip. |
| `cel_string_format` (directive parser + renderer) | `cel_string_format_test.cc` | Per-directive happy path × per accepting-CelKind × precision boundaries × negative precision rejected × precision > 1000 rejected.  Malformed format strings: `%` at end, `%.` with no type, unknown directive type, repeated `.` in precision.  Arg-list mismatches: too few args, too many args, kind mismatch per directive.  Per-CelKind `%s` canonical-form test: timestamp formats as RFC3339, duration as Go-style, list as `[a, b, c]`, map as `{k: v}`, null as `null`, bool as `true`/`false`.  Cache behaviour: same format string 100× hits cache, alternating format strings recompile.  Boundary: empty format string (returns empty), very long format string (4 KiB literal + 100 directives). |
| Codegen wiring | `overload_table_test.cc` (existing) | Seed-count bump 158 → 178.  Per-overload `LookupById` returns the right runtime export.  No new `kExplicitlyUnimplementedIds` entries. |
| E2E | `compiler_v2/e2e/m12_test.cc` (new) | ~30 tests covering the spec rows from each `string_ext.textproto` section: at least one per function, plus the three biggest sections (`format` 78 rows, `quote` 21 rows, `last_index_of` / `index_of` 14 each) get a 5-test sample.  Locks the Compile → Plan → Eval pipeline end-to-end. |

### 5.2 Conformance lock

  - [ ] `string_ext.textproto` 0/216 → 94/216 PASS.  The
        remaining 122 stay SKIP (44 `disable_check`-by-design
        + 78 `envelope`-pending-harness-unification).
  - [ ] No regressions on other fixtures.
  - [ ] `scripts/check_conformance_monotonic.sh` baseline
        updated to 1382 + 94 = 1476.

### 5.3 Shared test fixtures

  - `MakeStringArg(s)` / `MakeIntArg(i)` / `MakeListArg(vec<string>)`
    in a new `compiler_v2/runtime/string_ext_test_helpers.h`.
    Mirrors `cel_matches_test.cc`'s `MakeStr` / `MakeInt`
    pattern but generalised to the multi-arg shape format
    needs.
  - `EncodeUtf8(code_point)` — builds the byte sequence for a
    Unicode code point, used in the multi-byte boundary tests
    so they're spec-cited (test comments cite the code point
    explicitly).

## 6. Slicing

Ordered by dependency + risk.  Slice A is the foundation;
B-D parallelize once A lands.

### Slice A — code-point iterator + 5 simplest functions (~1.5 days)

The UTF-8 decode + length helpers live here.  Functions:
`reverse`, `lowerAscii`, `upperAscii`, `trim`, `charAt`.
Each is ≤ 30 LOC of kernel + ~10 tests.  Establishes the
runtime TU shape + the test-file pattern that B-D inherit.

`cel_string_ext.{cc,h}` skeleton lands here; `cel_string_format`
TU stays empty.

> **Shipped 2026-05-20.** `compiler_v2/runtime/cel_string_ext.{h,cc}`
> + `cel_string_ext_test.cc` landed with:
> - `Utf8Decode` / `Utf8DecodeMulti` shared UTF-8 helpers (1- to
>   4-byte sequence decoder; malformed input falls back to a
>   1-byte advance carrying the raw byte, mirrors cel-cpp's
>   `internal::Utf8Decode`).
> - `PrevCodepoint` reverse-direction code-point boundary walker.
> - `IsUnicodeWhitespace` verbatim port of cel-cpp's whitespace
>   classifier (`common/values/string_value.cc::IsUnicodeWhitespace`).
> - 5 kernels: `cel_string_char_at_at_vv`,
>   `cel_string_lower_ascii_at_v`, `cel_string_upper_ascii_at_v`,
>   `cel_string_trim_at_v`, `cel_string_reverse_at_v`.  ASCII-fold
>   helpers reuse the source span when no byte would change;
>   `trim` always returns a narrowed subspan (no alloc); `charAt`
>   / `reverse` arena-alloc the output.
> - ~38 unit tests across the 3VL/kind-mismatch envelope (TEST_P
>   over the 4 unary kernels), charAt's boundary + multi-byte
>   matrix, fold matrix + embedded-NUL guard, the 16 Unicode
>   whitespace code-points × trim matrix, and reverse's
>   mixed-width code-point matrix.
> - BUILD wiring: new `:cel_string_ext` cc_library + matching
>   `:cel_string_ext_test`.  WASM-side export wiring + checker
>   registration + overload-table seeding all defer to Slice F.
> - `bazel test //compiler_v2/runtime/...`: 18/18 PASS.  Lint
>   clean (PCH path validated).
>
> Plan-vs-execution delta: `Utf8DecodeMulti` factored out of
> `Utf8Decode` to stay under the `readability-function-size`
> threshold (40 statements); semantically identical to the
> single-function form sketched in §4.1.

### Slice B — search/extract family (~2 days)

`indexOf`, `lastIndexOf`, `substring`, `replace`.  All
code-point indexed (substring) or byte-level search returning
code-point indices (indexOf).  Each function has a 2-arg and
3-arg overload — total 6 kernel additions.  Boundary matrix
gets bigger here (negative indices, end-before-start,
empty-needle, needle-longer-than-haystack).

> **Shipped 2026-05-20.**  8 new kernels added to
> `cel_string_ext.{h,cc}`:
> `cel_string_index_of_at_vv` / `_at_vvv`,
> `cel_string_last_index_of_at_vv` / `_at_vvv`,
> `cel_string_substring_at_vv` /
> `cel_string_substring_range_at_vvv`,
> `cel_string_replace_at_vvv` /
> `cel_string_replace_n_at_vvvv`.  Shared helpers
> (`IndexOfImpl` / `LastIndexOfImpl` / `CodepointToByteOffset` /
> `ValidatePos` / `BuildReplaced`) live in the file's anonymous
> namespace and mirror cel-cpp `StringValue::IndexOf` /
> `::LastIndexOf` / `::Substring` / `::Replace` line-for-line.
>
> Notable spec-parity calls:
> - `IndexOf3`'s pos>byte_size error + `LastIndexOf3`'s
>   pos<0||pos>byte_size error are pre-flight checks against the
>   haystack's BYTE length (cel-cpp `extensions/strings.cc`
>   `IndexOf3` / `LastIndexOf3`).
> - `IndexOf` clamps negative pos to 0; `LastIndexOf3` rejects.
> - Empty-needle `Replace` interleaves `replacement` before every
>   code-point + once trailing (cel-cpp `Replace` lines
>   ~1405-1430).  Empty-input + empty-needle returns just the
>   trailing `replacement`.
> - `replace(s, old, new, 0)` returns the original string
>   verbatim; negative limit ≡ INT64_MAX (cel-cpp parity).
>
> 32 new tests across the indexOf / lastIndexOf / substring /
> replace matrices (spec-row + boundary + envelope).  Total test
> count now ~70.
>
> `bazel test //compiler_v2/runtime/...`: 18/18 PASS.
> `scripts/lint.sh`: clean.

### Slice C — list-bridging family (~1.5 days)

`split`, `join`.  Both bridge string ↔ list-of-string.  Touch
the arena-allocator path used by `cel_list_concat` /
`cel_string_concat`.  Test surface includes the
list-element-allocation lifetime (a `split` result is a list
of strings each carrying spans into the arena; the spans
remain valid until `arena_reset` at the next Eval).

> **Shipped 2026-05-20.**  4 new kernels in `cel_string_ext_list.cc`:
> `cel_string_split_at_vv`, `cel_string_split_n_at_vvv`,
> `cel_string_join_at_v`, `cel_string_join_sep_at_vv`.  Public ABI
> additions in `cel_string_ext.h`; BUILD wiring picks up the new
> `.cc` automatically (existing `:cel_string_ext` cc_library
> already aggregates).
>
> Implementation highlights:
> - `split` ranges computed in a local `std::vector<pair<u32,u32>>`
>   then directly stamped into the arena list's element array via
>   `cel_mem_base() + elements_offset + k*stride` — avoids the
>   per-element temp-slot dance `cel_list_append_at` would require.
> - Output strings are arena subspans into the source `s`; list
>   backing + headers + element CelValues are arena-allocated.
>   Lifetime ends at the next `arena_reset` per the existing
>   per-Eval arena contract.
> - `join` validates every element kind upfront before allocating
>   the output buffer — half-built strings can't escape on
>   type-mismatch.
> - cel-cpp's final-piece rule for `split` is mirrored verbatim
>   (`splits.empty() || !sep.empty() || pos < len` → push trailing
>   piece).  Locks the "empty haystack + non-empty sep → `[""]`"
>   spec case.
> - Host-backed lists (`CEL_LIST_HOST`) error with
>   `CEL_ERR_TYPE_MISMATCH` for now — deferred to a follow-up if a
>   user needs `join()` over proto-repeated fields.
>
> 22 new tests (`cel_string_ext_list_test.cc`) — split spec rows +
> boundary matrix (empty haystack, empty sep code-point split,
> trailing delimiter), join spec rows + boundary matrix (empty
> list, single element, multi-byte sep, non-string element error).
>
> `bazel test //compiler_v2/runtime/...`: 20/20 PASS (+1 from the
> new list_test target).  `scripts/lint.sh`: clean.

### Slice D — `quote` + start of `format` (~1.5 days)

`quote` is small (one TU function, ~40 LOC) — landing it
forces the escape-sequence test matrix.

Format directive parser lands here as a standalone TU + tests,
but without the per-(directive × CelKind) renderer.  The
parser produces `DirectiveOp[]`; tests assert the parsed
sequence + every malformed-input rejection path.  Renderer
stays stubbed (`ABSL_CHECK(false) << "format renderer is a
stub until M12 Slice E"`).

> **Shipped 2026-05-20.**  Two new kernel TUs land:
> - `cel_string_ext_quote.cc` adds `cel_string_quote_at_v` (1
>   public kernel + one local `AppendQuoteCodepoint` helper).
>   Mirrors cel-cpp `common/values/string_value.cc::AppendQuoteCodePoint`
>   exactly — escapes only `\a \b \f \n \r \t \v \\ \"`; every
>   other byte (NUL, every byte in `[0x01, 0x1F]`, every UTF-8
>   continuation byte) passes through verbatim.  The 9 spec rows
>   from `string_ext.textproto::quote` all land via the unit test
>   matrix; e2e rows defer to Slice F's conformance run.
> - `cel_string_format.{h,cc}` + `cel_string_format_internal.h`
>   add the format directive parser.  The internal header exposes
>   `DirectiveOp` + `ParseFormat` to the test TU directly so the
>   parser matrix can run without the renderer.  The public
>   `cel_string_format_at_vv` entry point parses but then
>   `ABSL_CHECK(false)`-fails until Slice E lands the renderer
>   (CLAUDE.md unimplemented-feature rule).
>
> Parser design choices worth a note for future-Claude:
> - **`%%` does NOT coalesce into a surrounding literal run.**
>   Source `a%%b` becomes 2 ops: `Lit(0, 1)` ("a") + `Lit(2, 2)`
>   ("%b").  The leading `%` byte at source offset 1 is consumed
>   but is NOT emitted in the rendered output, so the literal
>   range must skip it.  Renderer (Slice E) walks each
>   `kLiteral` op's byte range as a contiguous source-bytes-to-
>   output-bytes copy; one op per contiguous emit range.
> - **Precision is silently dropped on non-numeric directives.**
>   `%.5s` parses as `kSubstring` with `precision == kPrecisionDefault`
>   (cel-cpp's `ParseAndFormatClause` does the same — `s` doesn't
>   consult the parsed precision).  Asserted by
>   `ParseFormatTest.PrecisionSilentlyDroppedOnSubstring`.
> - **Diagnostic strings are byte-identical to cel-cpp's**
>   (`unexpected end of format string`, `unable to find end of
>   precision specifier`, `precision specifier exceeds maximum of
>   1000`, `unrecognized formatting clause "<c>"`).  Locks
>   conformance-output diff-clean against upstream.
>
> 22 quote tests + 27 parser tests; full runtime suite
> `bazel test //compiler_v2/runtime/...`: 22/22 PASS (+2 from
> the new test targets).  `scripts/lint.sh`: clean.
>
> Plan-vs-execution delta: `cel_string_format` is its own
> cc_library (not absorbed into `:cel_string_ext`) because Slice
> E will pull in `absl/strings/str_format` (for `FormatDouble`)
> and potentially `absl/container/btree_map` (for `%s` map
> rendering) that don't belong on the kernel library.  Slice F
> still adds one `--export=cel_string_format_at_vv` line to
> `cel_runtime_wasm.bin` and lists `:cel_string_format` as a dep
> alongside `:cel_string_ext`.

### Slice E — `format` renderer + per-CelKind dispatch (~2.5 days)

The largest single chunk.  Per-(directive × CelKind) render
matrix.  Re-uses the `%s` canonical-form helpers that already
exist scattered across the runtime (timestamp's RFC3339
formatter, duration's Go-style formatter, type-of's type-name
table) — consolidates them into one `RenderCanonical` entry
point.

Cache wiring also lands here: parsed `DirectiveOp[]` cached in
a single-slot most-recent-format cache (cf. `cel_matches`'s
`CachedInitialized` flag + the empty-pattern lesson learned —
apply the same `FormatInitialized` flag here from day one).

E2E tests in `compiler_v2/e2e/m12_test.cc` land here.

### Slice F — overload-table wiring + conformance lock (~0.5 day)

20 seeds added to `overload_table.cc`; counted by the
`overload_table_test::SeedCountIsMonotonic` test.  Conformance
run locks 94 PASS unlock.

Closeout per CLAUDE.md ## Closing out a planning doc.

**Total estimate: 9-10 working days.**  At the user's
AI-assisted pace, ~3-4 calendar days of focused work.

## 7. Risks

  - **Format directive parsing is error-prone.**  Mitigate
    with exhaustive malformed-input test matrix (every
    invalid form a parser could see — trailing `%`, `%.`,
    `%.5` with no type, unknown type byte, repeated `.`,
    precision > 1000, etc.).  Cel-cpp's `formatting_test.cc`
    has ~40 negative cases; we lift the same matrix.
  - **UTF-8 boundary semantics.**  `charAt` / `substring` /
    `reverse` operate on code points; `indexOf` returns a
    code-point index from a byte-level search.  Easy to mix
    up.  The shared `Utf8DecodeAt` helper + a thorough
    multi-byte test matrix (1-byte ASCII, 2-byte (`ñ`),
    3-byte (`α`), 4-byte (`🐱`)) are the load-bearing guard.
  - **`split` arena-lifetime hazards.**  A `split` result is
    a list of strings whose spans point into the arena; the
    list and its elements must all live until the next
    `arena_reset`.  Mitigate by allocating the list backing +
    all element bytes contiguously, and pinning the lifetime
    contract in `cel_string_ext.h` doc comments.
  - **`format` cache invalidation on the empty format string.**
    Same class of bug as `cel_matches`'s empty-pattern (caught
    by conformance on Phase C C3).  Apply the
    `FormatInitialized` flag from day one and write the
    regression test in Slice E.
  - **Float formatting reproducibility.**  Cel-cpp's
    `formatting.cc` uses `absl::StrFormat` with explicit
    precision; we mirror.  `%f` with default precision 6 must
    produce byte-identical output to cel-cpp on the same
    input — pin via a TEST_P that compares against cel-cpp's
    `Format("%f", x)` output for a sample of doubles
    including NaN, ±Inf, subnormals.
  - **Spec drift across cel-cpp versions.**  cel-cpp's
    `extensions/strings.cc` evolves; if a new overload
    surfaces between our vendoring and conformance run, the
    `CoverageTripwireClassifiesEveryStandardId` test will
    catch it.  Add a Slice F bullet: re-fetch cel-cpp at
    milestone close and re-run the tripwire.

## 8. Open questions

  1. **Where does `Utf8DecodeAt` live?**  It's used by
     `cel_string_ext.cc` AND `cel_string_format.cc` AND
     potentially by future `network_ext` (IP-string parsing).
     Three options: (a) standalone `cel_utf8.h` in runtime;
     (b) inline in `cel_string_ext.h` as `inline` definitions;
     (c) inside `cel_string_ext.cc` as static, duplicated in
     `cel_string_format.cc`.  Recommend (a) — one TU per
     concern is cheap and the helper is genuinely shared.
  2. **Format precision cap source.**  Cel-cpp's default is
     1000.  Should we expose this as a Compile option, or
     bake the 1000 in?  Recommend baking in for M12; an option
     surface is a one-day follow-up if a user asks.
  3. **`%s` canonical form for `list` and `map`.**  cel-cpp
     renders a list as `[a, b, c]` with one space after each
     comma.  Map ordering is non-deterministic per langdef but
     cel-cpp sorts keys lexicographically for `%s`.  We mirror
     verbatim; pin via a test that compares against cel-cpp's
     output.
  4. **`quote` byte-vs-string ambiguity.**  Spec says
     `strings.quote(string) -> string`; corpus has 21 cases.
     What does `quote` do on bytes containing invalid UTF-8?
     Cel-cpp escapes per byte (`\xNN`); we mirror.
  5. **Cache size for format.**  Same as `matches`: single
     slot is enough for the typical "log a structured event
     once per request" workload.  Multi-format hot paths can
     escalate to LRU in a follow-up if profiling shows it.

## 9. Closeout gate (to copy into the PR description)

Per `compiler_v2/conformance/README.md` and CLAUDE.md closeout
discipline:

  - [ ] `bazel test //compiler_v2/...` green.
  - [ ] `bazel test //compiler_v2/runtime:cel_string_ext_test
        //compiler_v2/runtime:cel_string_format_test
        //compiler_v2/e2e:m12_test` green (~165 new test cases).
  - [ ] `bazel run //compiler_v2/conformance:run_conformance` —
        pass count delta is **+94 PASS** (1382 → 1476).  Skip
        count drops by 94 (`string_ext.textproto::ext_unimpl`
        zeroes out); fail count unchanged.
  - [ ] `scripts/check_conformance_monotonic.sh --update`
        bumps baseline to 1476.
  - [ ] `scripts/lint.sh` clean across all touched files.
  - [ ] `scripts/check_doc_drift.sh` clean (or net-zero new
        findings).
  - [ ] `per-component-test-coverage.md` rows ticked for:
        UTF-8 code-point iterator, string-ext kernels (per
        function), format directive parser, format renderer
        (per CelKind), quote escape sequences.
  - [ ] `testing-checklist.md` rows ticked for the same
        components × pipeline stages.
  - [ ] `overload_table.cc` historical breadcrumb comment in
        `overload_table_test.cc` extended (`158 → 177 — added
        19 string_ext overload seeds`).
  - [ ] WAT traces under `doc/implementation-plan/rewrite/wat/`
        added for at least: `cel_string_format_at_vv` (the
        most complex new ABI), `cel_string_split_at_vv` (the
        list-allocation shape), and `cel_string_quote_at_v`
        (the in-place escape pattern).
  - [ ] `language-feature-unlock-analysis.md` headline number
        refreshed.
  - [ ] Status header on this doc flipped to `shipped <date>`
        with a one-paragraph "what landed" summary.
  - [ ] Future work section appended.

## 9b. Hand-off note (2026-05-20)

Pause point: Slices A-C are done and committed (commits 64f3815,
164417f, 198ecf2 refactor, c5b07b7); Slices D-F are pending.  Pick
up at Slice D in a fresh session.

**What's solid and ready to build on:**

- 17 kernels in 3 per-topic TUs (`cel_string_ext_codepoint.cc`,
  `cel_string_ext_search.cc`, `cel_string_ext_list.cc`) wired via
  the single `:cel_string_ext` cc_library.
- Shared helpers in `cel_string_ext_internal.h` (Poison, 3VL absorb,
  span / slot writers, UTF-8 decode + reverse walker).  Slice D/E
  pull these in via the same `using celwasm::string_ext_internal::…`
  pattern the existing TUs use.
- `string_ext_test_helpers.h` fixture (`StringExtFixture` +
  `MakeStr` / `MakeInt` / `MakeError` / `MakeUnknown` / `ExpectStr` /
  `ExpectInt` / `ExpectError` / `ExpectKind`) — Slice D/E tests
  inherit from the same fixture; no new builders needed for
  scalar args.
- `bazel test //compiler_v2/runtime/...`: 20/20 PASS.  `scripts/lint.sh`
  clean across every file touched.

**What to do next (Slice D — quote + format parser):**

1. **Quote** (~80 LOC kernel + ~30 tests).  New TU
   `cel_string_ext_quote.cc` adding `cel_string_quote_at_v`.  Per
   cel-cpp `StringValue::Quote` (`common/values/string_value.cc`):
   wrap input in `"..."`, escape `\`, `"`, `\n`, `\t`, `\r`, `\b`,
   `\f`, `\v`, `\a`, NUL, and any byte < 0x20 as `\xNN`.  Verbatim
   bytes (printable ASCII + non-ASCII multi-byte) pass through.
   Spec rows live in `string_ext.textproto::quote` (21 rows; pick
   ~10 for the unit-test matrix, the rest land via the e2e file in
   Slice F).
2. **Format directive parser** (~300 LOC).  New TU pair
   `cel_string_format.{h,cc}`.  Public ABI per §4.3:
   `cel_string_format_at_vv(out, fmt, args_list)`.  Slice D ships
   the parser only — produces a `std::vector<DirectiveOp>` where
   each op is either a literal-byte range or a directive entry
   `(precision, kind: %d/%f/%e/%s/%b/%o/%x/%X)`.  Renderer body
   stays `ABSL_CHECK(false) << "format renderer is a stub until M12
   Slice E"` per the unimplemented-feature rule.

   Parse rejects (every malformed shape gets a unit test):
   - `%` at end-of-string
   - `%.` with no type byte
   - `%.<n>` with no type byte
   - unknown type byte (anything outside `dfes bxX o`)
   - `%.5.3<type>` (repeated `.` in precision)
   - precision > 1000

**What to do at Slice E — format renderer:**

1. Per-(directive × CelKind) renderer matrix.  Per §4.3 table:
   - `%d` accepts INT, UINT.
   - `%f` / `%e` accept DOUBLE, INT, UINT (default precision 6).
   - `%s` accepts STRING, BYTES, BOOL, TIMESTAMP, DURATION, LIST,
     MAP, NULL_VALUE, TYPE, INT, UINT, DOUBLE.
   - `%b` accepts INT, UINT.
   - `%o` accepts INT, UINT.
   - `%x` / `%X` accept INT, UINT, STRING, BYTES.

   `%s` canonical-form helpers (timestamp → RFC3339, duration →
   Go-style, list → `[a, b, c]`, map → `{k: v}` with
   lexicographically-sorted keys) — consolidate the existing
   scattered helpers in the runtime into one `RenderCanonical`
   entry point.

2. Per-Instance single-slot most-recent-format cache.  Mirror
   `cel_matches.cc`'s `CachedInitialized` flag + the empty-pattern
   lesson — write the regression test for empty format string in
   Slice E.

3. E2E tests in `compiler_v2/e2e/m12_test.cc` (new) — ~30 tests
   covering the spec rows from each `string_ext.textproto` section.

**What to do at Slice F — overload table + conformance lock:**

1. Register cel-cpp's `StringsCheckerLibrary` in
   `compiler_v2/frontend/parse_and_check.cc::ConfigureCheckerBuilder`
   (one `builder.AddLibrary(cel::extensions::StringsCheckerLibrary())`
   call alongside the existing `ComprehensionsV2CheckerLibrary`).
   BUILD.bazel for `:parse_and_check` gains
   `@cel-cpp//extensions:strings`.
2. Seed 19 overload IDs in
   `compiler_v2/codegen/overload_table.cc` per the §4.2 table.
   Bump `kBuiltinSeedCount` 158 → 177 in `overload_table_test.cc`
   with a breadcrumb line:
   `// M12.F: 158 → 177 — added 19 string_ext overload seeds.`
   Note: `string_ext` overload IDs are NOT in cel-cpp's
   `StandardOverloadIds` (they're extension-only), so the coverage
   tripwire test does NOT enumerate them — they live in
   `kBuiltinSeeds` without a tripwire arm.
3. Wasm export wiring in `BUILD.bazel` (`cel_runtime_wasm.bin`):
   add 19 `-Wl,--export=cel_string_<…>` lines (one per kernel
   from the §4.2 table).
4. Update `:cel_runtime_wasm.bin`'s `deps` to include
   `:cel_string_ext`.
5. Run `bazel run //compiler_v2/conformance:run_conformance`,
   verify +94 PASS (1382 → 1476), bump
   `scripts/check_conformance_monotonic.sh` baseline.
6. Closeout per CLAUDE.md "Closing out a planning doc": flip
   header to `shipped <date>`, populate §10 with surfaced
   follow-ups (the `value_errors` envelope unification still
   sitting at 78 SKIPs; the LRU format cache).

**Open questions still standing from §8:**

- `Utf8DecodeAt` placement: shared helper now lives in
  `cel_string_ext_internal.h`.  Will the format parser also need
  it (yes — `%s` rendering with a string arg may want code-point
  iteration for truncation)?  If so, no move needed — already in
  the right place.  Slice D should confirm.

- Format precision cap: stays at 1000 (cel-cpp default).  Bake in.

**Risk to watch on Slice E:**

- Float formatting reproducibility (§7 Risk #5).  `%f` with
  default precision 6 must produce byte-identical output to
  cel-cpp on the same input.  Pin via a TEST_P comparing against a
  small reference table (NaN, ±Inf, subnormals, 0.0, 1.0, π).
  cel-cpp uses `absl::StrFormat` with explicit precision — we
  mirror.

## 10. Future work surfaced (to fill at closeout)

(Populated when the milestone ships.  Candidates already
identified during planning:)

  - **Envelope unification for `value_errors` + `eval_error`
    matchers.**  The 78 `string_ext.textproto::envelope` SKIPs
    are blocked on harness work that doesn't belong in M12.
    Tracked in `conformance-unlock-plan.md` Slice 3 area.
  - **Format option surface.**  Compile-time
    `StringsExtensionFormatOptions` (max_precision, allowed
    directives) currently baked-in; expose if a user asks.
  - **LRU format cache.**  Single-slot cache is fine for
    typical workloads but a multi-format hot path (e.g.,
    structured logging that interleaves several format
    strings) thrashes.  Profile-first before adding.
  - **Generalised UTF-8 helpers in `cel_utf8.{c,h}`.**  If
    `network_ext`'s IP-string parsing lands later and needs
    code-point iteration too, factor here.
  - **`splitN` shorthand.**  cel-cpp's `string_split_string_int`
    is the `N`-limited variant; spec doesn't ship a
    `splitN(s, sep, n)` alias.  If a user demands it, trivial
    to add.
