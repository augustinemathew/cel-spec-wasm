# M17 — `encoders` extension (`base64.encode` / `base64.decode`)

Status: **plan — drafted 2026-05-24, not yet started.**

Scope is the cel-cpp `encoders` extension library: two global
functions (`base64.encode(bytes) -> string`,
`base64.decode(string) -> bytes`), self-hosted in
`cel_runtime.wasm` against vendored absl
(`absl::Base64Escape` / `absl::Base64Unescape`).  Conformance
ceiling: **+4 PASS** — `encoders_ext.textproto` 0/4 → 4/4.
Baseline `1576 → 1580`.

This is the smallest extension milestone to date (2 functions, 2
overloads, no parser, no list bridging) — it follows the M12
`string_ext` self-host pattern verbatim at ~1/10th the surface,
so it doubles as the reference template for future single-purpose
ext libraries (`math_ext` subsets, `sets`, `lists`).

## 1. Why M17

`encoders` is a tiny but frequently-requested CEL extension:
policies that embed signatures, certificate thumbprints, or
opaque tokens reach for `base64.encode` / `base64.decode`
constantly, and today they hit the same `ext_unimpl` wall every
other extension did pre-M12.  The conformance corpus already
ships `encoders_ext.textproto` and the conformance runner already
lists `base64` in `ExtensionNamespaceRoots()`
(`compiler_v2/conformance/runner.cc:534`) — the infrastructure
*expects* this extension; only the kernels + wiring are missing.

It's also the lowest-risk self-host target in the backlog: pure
operate-on-`CelValue` kernels, no descriptor-pool reads, no UTF-8
code-point iteration, no format parser, no arena-list
construction.  absl already vendors the exact primitives
(`absl/strings/escaping.h`) in the wasm runtime (Phase C C1).

## 2. Scope

In scope — 2 functions × 1 overload each:

| Function | Overload ID (expected) | Signature | Notes |
|---|---|---|---|
| `base64.encode(b)` | `base64_encode_bytes` | `bytes -> string` | Standard RFC 4648 alphabet, **with** padding (`=`).  `absl::Base64Escape`. |
| `base64.decode(s)` | `base64_decode_string` | `string -> bytes` | Standard alphabet, padding **optional** (corpus row `decode/hello_without_padding` feeds `'aGVsbG8'`).  `absl::Base64Unescape` is lenient on missing padding.  Invalid input → `CEL_ERROR(kInvalidArgument, "invalid base64 data")`. |

> **Validated against vendored cel-cpp (2026-05-24 probe).** The
> two overload IDs, signatures, error message, and absl
> primitives above were confirmed by reading
> `third_party/cel-cpp/extensions/encoders.cc` directly (after
> `third_party/fetch_cel_cpp.sh`):
> - `MakeOverloadDecl("base64_encode_bytes", StringType(),
>   BytesType())` and `MakeOverloadDecl("base64_decode_string",
>   BytesType(), StringType())` — exact ID strings to seed in §4.2.
> - `Base64Encode` → `absl::Base64Escape(...)`.
> - `Base64Decode` → `absl::Base64Unescape(...)`; on `false`
>   returns `InvalidArgumentError("invalid base64 data")` — that
>   exact message string is what M17's kernel must emit.
> - Checker entry point: `EncodersCheckerLibrary()` (declared in
>   `extensions/encoders.h`).
>
> The resolve_pass still stamps `overload_id` from cel-cpp's
> `Reference`, so a Slice A probe-print remains cheap insurance,
> but the strings are now source-confirmed, not from memory.

Out of scope (deliberately):

  - **Web-safe / URL alphabet** (`-_` instead of `+/`).  The
    cel-cpp `encoders` extension is standard-alphabet only; spec
    doesn't define a web-safe variant.  Don't add.
  - **Streaming / chunked encode.**  Kernels are
    whole-value; no streaming surface.
  - **Hex / base32 / base58.**  Not in the `encoders` library.
  - **Strict-padding decode mode.**  cel-cpp accepts unpadded
    input (the corpus requires it); we match.  No option to
    toggle.

## 3. Why self-hosted in runtime (not host trampolines)

Same trade as M12 §3 and Phase C's `matches` kernel:

  - **One round trip per call, not per Eval.**  Self-hosted in
    `cel_runtime.wasm` each call is a single `(call
    $cel_base64_*)` op against linear memory — no wasm→host
    boundary crossing.
  - **No host-side state.**  Encode/decode are pure functions of
    their single input span; nothing to cache, no per-Instance
    state.
  - **absl primitives already in the wasm runtime.**
    `absl::Base64Escape(absl::string_view, std::string*)` and
    `absl::Base64Unescape(absl::string_view, std::string*) ->
    bool` are free to call (Phase C C1 vendored
    `@com_google_absl//absl/strings`).  `Base64Unescape`
    returning `false` is the invalid-input signal.
  - **No descriptor-pool reads / externref roundtrip** — cleanest
    possible self-host target.

## 4. Final file structure

### 4.1 New runtime files

| File | ~LOC | Contents |
|---|---|---|
| `compiler_v2/runtime/cel_base64_ext.h` | ~40 | Public ABI: `cel_base64_encode_at_v` + `cel_base64_decode_at_v` declarations + lifetime doc comment (output bytes/string are arena-allocated, valid until the next `arena_reset`). |
| `compiler_v2/runtime/cel_base64_ext.cc` | ~120 | Both kernels.  Shared helpers (`BorrowBytes` / `BorrowString` / `WriteString` / `WriteBytes` / 3VL `AbsorbUnary`) reuse `cel_string_ext_internal.h` where the shapes already exist; anything base64-specific lives in this TU's anonymous namespace. |
| `compiler_v2/runtime/cel_base64_ext_test.cc` | ~250 | Unit tests — see §5. |

**BUILD wiring.**  New `:cel_base64_ext` cc_library mirroring
`:cel_string_ext`:

```python
cc_library(
    name = "cel_base64_ext",
    srcs = ["cel_base64_ext.cc"],
    hdrs = ["cel_base64_ext.h"],
    deps = [
        ":cel_runtime",
        ":cel_string_ext",  # for cel_string_ext_internal.h envelope helpers
        "@com_google_absl//absl/strings",
    ],
)

cc_test(
    name = "cel_base64_ext_test",
    srcs = ["cel_base64_ext_test.cc"],
    deps = [":cel_base64_ext", ":string_ext_test_helpers", "@gtest//:gtest_main"],
)
```

`cel_runtime_wasm.bin` gains `:cel_base64_ext` in `deps` and the
two `-Wl,--export=` lines (via `wasm_exports.txt`, §4.4).

### 4.2 Codegen change summary

`compiler_v2/codegen/overload_table.cc` — seed 2 overload IDs.
Both extension-only (NOT in cel-cpp's `StandardOverloadIds`), so
no coverage-tripwire arm.  Seed count `177 → 179`.

| Overload ID | Runtime export |
|---|---|
| `base64_encode_bytes` | `cel_base64_encode_at_v` |
| `base64_decode_string` | `cel_base64_decode_at_v` |

Breadcrumb in `overload_table_test.cc`:
`// M17: 177 → 179 — added 2 encoders (base64) overload seeds.`

No `expr_lower.cc` change — both route through the existing
`EmitGeneralCall` arm (same path `string_ext` / `matches` use).
No `compile.cc::OverloadHelperArity` change — both are `_at_v`
(arity 2), already in the helper-suffix table.

**No static-subset (`RejectDyn`) change.**  Unlike M12's
`format`, neither overload takes a `list(dyn)` or
heterogeneous arg — `encode` takes concrete `bytes`, `decode`
takes concrete `string`.  `CheckSubsetCall` admits them
unchanged.

### 4.3 ABI catalogue

`compiler_v2/abi/runtime_catalogue.cc` — 2 new `K_AT_V` entries
(arity 2 = out + 1 value):

```cc
K_AT_V("cel_base64_encode_at_v"),
K_AT_V("cel_base64_decode_at_v"),
```

`runtime_catalogue_test.cc` asserts the catalogue matches
`wasm_exports.txt`; both update together.

### 4.4 Runtime export registry

`compiler_v2/runtime/wasm_exports.txt` — append under a new
`# m17 encoders (base64) extension kernels.` comment:

```
cel_base64_encode_at_v
cel_base64_decode_at_v
```

### 4.5 Frontend (checker library registration)

`compiler_v2/frontend/parse_and_check.cc::ConfigureCheckerBuilder`
— register the encoders library alongside the existing strings /
comprehensions libraries:

```cc
// M17 encoders: base64.encode(bytes)->string, base64.decode(string)->bytes.
if (auto s = builder.AddLibrary(cel::extensions::EncodersCheckerLibrary());
    !s.ok()) {
  return s;
}
```

`BUILD.bazel` for `:parse_and_check` gains
`@cel-cpp//extensions:encoders`.

### 4.6 Host-side import bridge

`compiler_v2/api/internal/engine.cc::kRuntimeExports` — add the 2
export names to the linker-bind table so the expr module's
`(import "cel" "cel_base64_*")` lookups resolve (the regression
mode M12 §6 Slice F flagged: missing bind → `instantiate(expr):
unknown import` at conformance pre-flight).

## 5. Test coverage strategy

### 5.1 Unit tests (`cel_base64_ext_test.cc`)

Reuses the `StringExtFixture` + `MakeStr` / `MakeBytes` /
`MakeError` / `MakeUnknown` / `ExpectStr` / `ExpectBytes` /
`ExpectError` / `ExpectKind` helpers from
`string_ext_test_helpers.h` (add `MakeBytes` / `ExpectBytes` if
not already present — they mirror `MakeStr` / `ExpectStr`).

| Group | Scenarios |
|---|---|
| Encode happy path | `b''` → `""`; `b'hello'` → `"aGVsbG8="`; `b'Hello World!'` → `"SGVsbG8gV29ybGQh"`; 1-/2-/3-byte tails to exercise every padding shape (`0`, `1`, `2` `=`); a byte sequence with high bytes (`b'\xff\xfe\xfd'`) to confirm raw-byte (non-UTF-8) input encodes fine. |
| Decode happy path | `"aGVsbG8="` → `b'hello'`; **`"aGVsbG8"` (no padding) → `b'hello'`** (the corpus row); `""` → `b''`; mixed-padding shapes round-trip. |
| Decode errors | invalid alphabet byte (`"!!!!"`), bad length (`"a"`), embedded whitespace if cel-cpp rejects it — assert `CEL_ERR_INVALID_ARGUMENT` (confirm the exact error kind cel-cpp emits during Slice A). |
| Round trip | `decode(encode(b)) == b` over a table: empty, ASCII, all-256-byte-values blob, 1 KiB random-ish blob. |
| 3VL envelope | `TEST_P` over both kernels: ERROR in → ERROR out (propagated verbatim); UNKNOWN in → UNKNOWN out; wrong-kind in (e.g. INT to encode) → `CEL_ERR_TYPE_MISMATCH`. |
| Arena lifetime | output span points into the arena; valid until next `arena_reset`; large output (1 KiB decode) allocates correctly. |

Target ~35 unit tests.

### 5.2 E2E tests (`compiler_v2/e2e/m17_test.cc`)

Lands in Slice B (needs checker registration + overload seeding +
wasm exports — same seam as M12 §6 Slice F).  Covers the
Compile → Plan → Eval pipeline for both functions + the corpus
rows verbatim.  ~10 tests across `EncodeE2ETest` /
`DecodeE2ETest` / `RoundTripE2ETest`.  The file is written
up-front in this milestone (see deliverable alongside this doc)
so Slice B's "lights up" commit is a pure wiring change.

### 5.3 Conformance lock

  - [ ] `encoders_ext.textproto` 0/4 → 4/4 PASS.
  - [ ] No regressions on other fixtures.
  - [ ] `compiler_v2/conformance/.baseline` bumped `1576 → 1580`.
  - [ ] `scripts/check_conformance_monotonic.sh` passes.

## 6. Slicing

Ordered by dependency.  Small milestone — 3 slices, ~1.5 days
total at AI-assisted pace.

### Slice 0 — WAT traces (~0.25 day, non-negotiable)

Per CLAUDE.md "WAT-first": before any production C++, hand-write
the two kernel-call shapes and run them through
`tools/wat_runner` with stub impls.

  - [ ] `doc/implementation-plan/rewrite/wat/m17_base64_encode.wat`
        — locks the `cel_base64_encode_at_v(out, bytes)` call
        shape (read bytes span from in-slot, call, write string to
        out-slot).
  - [ ] `doc/implementation-plan/rewrite/wat/m17_base64_decode.wat`
        — `cel_base64_decode_at_v(out, string)` + the
        error-out shape for invalid input.
  - [ ] Walkthrough paragraphs appended to `wat-traces.md`.

### Slice A — runtime kernels + unit tests (~0.75 day)

  - [ ] **Overload-id probe first** (§2 gate) — confirm
        `base64_encode_bytes` / `base64_decode_string` are the
        strings `EncodersCheckerLibrary()` stamps.
  - [ ] `cel_base64_ext.{h,cc}` — both kernels.  `encode` via
        `absl::Base64Escape`; `decode` via `absl::Base64Unescape`
        with `false` → `CEL_ERROR(kInvalidArgument, ...)`.  3VL
        absorb on the input slot; kind-mismatch guard
        (encode wants BYTES, decode wants STRING).  Output
        arena-allocated.
  - [ ] `cel_base64_ext_test.cc` — §5.1 matrix (~35 tests).
  - [ ] BUILD: `:cel_base64_ext` + `:cel_base64_ext_test`
        (§4.1).  Add `MakeBytes` / `ExpectBytes` to
        `string_ext_test_helpers.h` if absent.
  - [ ] `bazel test //compiler_v2/runtime:cel_base64_ext_test`
        green; `scripts/lint.sh` clean.

> Wasm export wiring + checker registration + overload-table
> seeding defer to Slice B (mirrors M12 Slice A→F seam).

### Slice B — pipeline wiring + conformance lock (~0.5 day)

  - [ ] **Frontend** — register `EncodersCheckerLibrary()`
        (§4.5); BUILD dep `@cel-cpp//extensions:encoders`.
  - [ ] **Codegen** — seed 2 overload IDs in `overload_table.cc`
        (§4.2); bump `overload_table_test.cc` seed count `177 →
        179` with breadcrumb.
  - [ ] **ABI catalogue** — 2 `K_AT_V` entries (§4.3).
  - [ ] **Exports** — 2 lines in `wasm_exports.txt` (§4.4);
        confirm present in final wasm via `wasm-objdump -x`.
  - [ ] **Host bridge** — 2 names in `engine.cc::kRuntimeExports`
        (§4.6).
  - [ ] **Wasm deps** — `:cel_base64_ext` added to
        `cel_runtime_wasm.bin` deps.
  - [ ] **E2E** — `compiler_v2/e2e/m17_test.cc` green (§5.2).
  - [ ] **Conformance** — `bazel run
        //compiler_v2/conformance:run_conformance`, verify +4
        (`encoders_ext` 0/4 → 4/4); bump `.baseline` to 1580.
  - [ ] Closeout per CLAUDE.md "Closing out a planning doc".

## 7. Risks

  - **Overload-id mismatch.**  Highest-likelihood failure: the
    seeded strings don't match what `EncodersCheckerLibrary`
    stamps → `overload_id ... not found in OverloadTable` at
    codegen.  Mitigated by the §2 probe-first gate in Slice A.
  - **Unpadded decode.**  The corpus explicitly feeds
    `'aGVsbG8'` (no `=`).  `absl::Base64Unescape` accepts missing
    padding, but confirm with the dedicated unit test
    (`DecodeHelloWithoutPadding`) before the conformance run —
    if absl ever tightened this, decode would need a manual
    re-pad step.
  - **Invalid-input error kind / message.**  The conformance
    corpus has no negative decode row today, so the error
    contract isn't conformance-pinned — but match cel-cpp's
    `kInvalidArgument` + message so a future corpus row lands
    clean.  Message confirmed: `"invalid base64 data"` (probe, §2).
  - **Non-UTF-8 decode output.**  `decode` produces `bytes` that
    may not be valid UTF-8 — make sure the output is written as
    `CEL_BYTES`, not `CEL_STRING`, and that the e2e `AsBytes()`
    path (not `AsString()`) reads it.  Pinned by the
    `RoundTripAll256Bytes` test.

## 8. Open questions

  1. **Does `cel_string_ext_internal.h` already expose the
     byte-span borrow + write helpers `decode` needs, or do they
     need a small base64-local copy?**  Resolve in Slice A — reuse
     if the shapes match, else a 2-helper anonymous-namespace
     copy is fine (don't over-factor for 2 kernels).
  2. **Is `MakeBytes` / `ExpectBytes` already in
     `string_ext_test_helpers.h`?**  M12 was string-centric;
     likely needs adding.  Trivial mirror of `MakeStr`.
  3. ~~**Exact cel-cpp invalid-base64 error message.**~~
     **Resolved (2026-05-24 probe):** `"invalid base64 data"`
     (`extensions/encoders.cc:51`).

## 9. Closeout gate (to copy into the PR description)

  - [ ] `bazel test //compiler_v2/...` green.
  - [ ] `bazel test //compiler_v2/runtime:cel_base64_ext_test
        //compiler_v2/e2e:m17_test` green (~45 new test cases).
  - [ ] `bazel run //compiler_v2/conformance:run_conformance` —
        **+4 PASS** (`encoders_ext` 0/4 → 4/4); baseline
        `1576 → 1580`.
  - [ ] `scripts/check_conformance_monotonic.sh` passes.
  - [ ] `scripts/lint.sh` clean across touched files.
  - [ ] `overload_table_test.cc` breadcrumb extended (`177 → 179`).
  - [ ] WAT traces (`m17_base64_encode.wat`,
        `m17_base64_decode.wat`) + `wat-traces.md` walkthroughs.
  - [ ] `per-component-test-coverage.md` + `testing-checklist.md`
        rows ticked: base64 encode kernel, decode kernel, 3VL
        envelope, conformance lock.
  - [ ] Status header flipped to `shipped <date>` with a
        one-paragraph "what landed".
  - [ ] Future-work section appended.

## 10. Future work surfaced

_(populated at closeout)_
