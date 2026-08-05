# Stale / partial-feature inventory

Status: deliverable of m39 §7 (`rewrite/m39-component-removal.md`) —
compiled 2026-08-04 against branch `rip-out-components` @ `0aac27d`.
Purpose: owner triage before the upstream-facing PR.  Nothing
partially-working ships without an explicit line here.

> **Component/plugin items are being removed by m39.**  They are
> recorded in §1 as **resolved-by-removal** and must not be
> re-litigated in triage — the archive branch
> `component-functions-archive` preserves the work.

Sources mined (all at HEAD of this branch):

  - `scripts/bug_pins.py list` — 17 CELBUG pins (P1=16, P2=1), all in
    `e2e/known_bugs_test.cc`; 0 unmigrated prose skips.
  - `scripts/bug_pins.py skips` — 40 CELSKIP blocks (by-design /
    harness-limit / deferred-feature).
  - `grep "is a stub until"` over first-party `.cc/.h` — **zero
    hits**: no `ABSL_CHECK(false) << "... stub until <milestone>"`
    stubs remain in the tree.  (The remaining `ABSL_CHECK(false)`
    sites are closed-enum switch defaults, which is the sanctioned
    invariant form, not stubs.)
  - `PROPOSALS.md` — 10 open entries, each re-verified against HEAD
    (verification noted per entry below).
  - `doc/implementation-plan/cleanup-backlog.md` — 29 open items.
  - `bindings/c/` — README states "not yet functional"; §1.
  - Self-found: user-guide status table (`doc/user-guide/index.md`
    §10 🟡/⛔ rows), `custom-functions.md` reserved-syntax callout,
    two skips whose recorded blocker has since landed (§10), and one
    stale source comment.

Recommendation legend: **SHIP AS-IS** (document, don't block the PR) ·
**FINISH** (with a rough size: S ≤ 1 day, M ≈ 2–4 days, L ≥ 1 week) ·
**REMOVE** (delete per the pre-1.0 rule) ·
**RESOLVED-BY-REMOVAL** (closed by m39 nodes; no action).

---

## 1. Component / plugin backend — resolved-by-removal (m39)

Everything in this section is deleted or mooted by the m39 DAG.
Listed only so triage can strike the matching pins/backlog entries in
the same sweep (m39 §5 already schedules `cleanup-backlog.md` closure
lines and pin deletion).

| Item | Evidence | Disposition |
|---|---|---|
| `@plugin.` backend, wit-bindgen pipeline, wasip2 toolchain, `Plugin` API | m39 §3 inventory | resolved-by-removal |
| CELW-0021 — `list<bool>` plugin decl generates a codec header that fails to compile (`std::vector<bool>` has no `.data()`) | `e2e/known_bugs_test.cc:1911` | resolved-by-removal (pin deleted with the feature, per owner decision) |
| cleanup-backlog **#51** — wasip2 cross-compile break (absl `mutex.cc` under the wasip2 sysroot) | backlog #51 | resolved-by-removal (toolchain deleted) |
| cleanup-backlog **#43** — e2e coverage gap for `cel_plugin.cc` NULL guards | backlog #43 | resolved-by-removal (file deleted) |
| cleanup-backlog **#32** / PROPOSALS **#1** — `Engine::AddPlugin` takes an internal-visibility `FunctionLibrary` (verified still true at `eval/engine.h:185`) | backlog #32; PROPOSALS #1 | resolved-by-removal (`AddPlugin` deleted).  Note: `FunctionLibrary` remains a parameter of the surviving compile-side `DeclareFunctions` path; its visibility is a compile-side question and no longer a public-eval-API defect |
| cleanup-backlog **#44** still-open half — `cel_plugin` Lower rejects type-of-types with `Unimplemented` | backlog #44 | resolved-by-removal (file deleted; the `BindLazy`/`OverrideFunction` half was already closed in m36, and the `CelfnTypeToCelType` half closed in m35 B2 — after m39, **#44 can be closed entirely**) |
| cleanup-backlog **#52(a)** — third first-party UTF-8 validator `IsValidUtf8` in `abi/plugin.cc` | backlog #52 | resolved-by-removal (`abi/plugin.cc` deleted) |
| cleanup-backlog **#52(b)** — `kComponentPreamble`/`kCorePreamble` constants declared in four tests | backlog #52 | mostly resolved: two of the four tests (`abi/plugin_test.cc`, `tools/cel/run_embed_decls_test.cc`) are deleted; re-count the survivors and close if below the dedupe threshold |
| `RegisteredPlugin::hash` observation seam (deferred-feature skip) | `eval/engine_test.cc:1118` | resolved-by-removal (registry deleted) |
| Plugin wire has no eval-error result variant (2 deferred-feature skips) | `e2e/foreign_fn_type_matrix_test.cc:1310,1569` | resolved-by-removal |
| Plugin demo fixture doesn't build for wasm32-wasip2 (harness-limit skips) | `e2e/plugin_dispatch_test.cc:699`, `e2e/plugin_fixtures/.../demo_plugin_e2e_test.cc:288` | resolved-by-removal |
| Go plugin authoring — "⛔ designed; `cel generate --language=go` arm pending" | `doc/user-guide/index.md:481`, `writing-plugins.md:427` | resolved-by-removal (doc deleted per m39 §5) |
| `cel generate` / `cel embed-decls` subcommands + `--idl` flag | `tools/cel/cel.cc:93,106` | resolved-by-removal |
| **`bindings/c/`** — drafted C-bindings header for the component API; README: "**Status: not yet functional.** … Nothing here compiles into a usable library yet" | `bindings/c/README.md:3-6`; sole header `cel_component.h` | resolved-by-removal (m39 node D4).  The FAQ's "no bindings beyond C++" line (`doc/user-guide/faq.md:200`) stays true and needs no edit |

---

## 2. `@native` CEL-defined custom functions — declared syntax, no backend

**What it is:** the third custom-function backend: a `.celfn` decl
with a CEL expression body (`int @native.twice(int x) = x * 2;`),
compiled into the expression module itself.

**Current state: stub (parse-only).**  The grammar has the
`nativeFnDecl` alternative (`compiler/celfn/Celfn.g4:66-67`), the IDL
parser builds `CelfnDecl::Backend::kCelDefined` decls carrying the raw
body (`compiler/celfn/function_library.cc:237`, `function_library.h:86`),
and `Builder::Build()` validates the module-name requirement
(`function_library.cc:306-318`).  Nothing downstream consumes it:
there is no codegen arm (`grep kCelDefined compiler/codegen eval` →
zero hits), and the wire layer refuses it with `ABSL_CHECK(false)`
(`abi/celfn_wire.cc:178` — "kCelDefined decl … no wire backend").
The user guide is honest: "**Not implemented — treat `@native` as
reserved syntax**" (`doc/user-guide/custom-functions.md:168`);
"⛔ … a `@native`-using program does not evaluate"
(`doc/user-guide/index.md:443,477`).

**Doc drift found while verifying:** `doc/user-guide/index.md:486`
claims "doc-comment capture ✅ shipped" and `:487` claims a
"description on `CelfnDecl`" exists (🟡).  Neither is true at HEAD:
`CelfnDecl` (`function_library.h:59-88`) has no description field, and
the grammar's `LineComment`/`BlockComment` (`Celfn.g4:161-164`) are
skipped whitespace, not captured.  Whatever shipped this has since
been reverted or never landed; the two §10 rows must be corrected in
the m39 doc pass regardless of the triage outcome here.

**Recommendation: REMOVE** (S — delete the `nativeFnDecl` grammar
arm, `kCelDefined`, the `body`/`module_name` plumbing, and the two
index.md rows; per "delete rather than deprecate", git keeps it).
Alternative if the owner wants to keep the reservation: SHIP AS-IS is
defensible — every reachable path fails loudly and the docs say
"reserved" — but for an upstream-facing PR, grammar that admits an
unimplementable decl is surface without function.

---

## 3. CLI has no custom-function input (`--celfn`)

**What it is:** a way for `cel compile` / `cel eval` to accept a
`.celfn` declaration file so expressions calling custom functions can
be compiled from the CLI.

**Current state: missing.**  The only IDL-consuming flags belong to
`generate`/`embed-decls` (`tools/cel/cel.cc:93,106`) — both deleted by
m39 — so post-m39 the CLI has **zero** custom-function surface.  The
user guide flags it: "**Missing: `.celfn` IDL input (`--celfn`) 🟡**"
(`doc/user-guide/index.md:427`).  Note the eval side is inherently
C++-only anyway (`@host.` needs a registered C++ callback the generic
CLI cannot supply — `index.md:445`), so the flag only unlocks
*compile*-side type-checking from the CLI.

**Recommendation: FINISH (S)** — wire `--celfn <file>` into
`cel compile`: read file → `ParseCelfnSource` → `DeclareFunctions`,
exactly as `index.md:427-432` sketches.  If deferred, SHIP AS-IS is
acceptable (the limitation is documented in place), but the m39
rewrite of `custom-functions.md` should restate it.

---

## 4. Execution cost limits — none (flagged P0 for semi-trusted input)

**What it is:** any ceiling on evaluation cost: wasmtime fuel, epoch
interruption, `max_wasm_stack`, or an `Eval(deadline)` overload.

**Current state: missing.**  Verified at HEAD: `grep -n
"epoch\|fuel\|max_wasm_stack" eval/engine.cc` → zero hits;
`Instance::Eval` takes no budget.  The security model documents it:
"④ bound evaluation cost ── NOT yet available"
(`doc/user-guide/security-model.md:230`); the FAQ hedges likewise
(`faq.md:201` — "CPU-time limits … still to come").  PROPOSALS #5
marks it "**P0 for accepting semi-trusted expressions**".  CEL
totality bounds non-termination but not polynomial blowup over
host-backed collections.

**Recommendation: FINISH (M)** — epoch interruption + `Eval(deadline)`
(PROPOSALS #5's preferred option).  This is the single most important
open item for an upstream PR whose pitch includes sandboxing: the
sandbox currently bounds *memory and capability*, not *time*.  If it
cannot land pre-PR, the security-model doc already states it plainly —
SHIP AS-IS is honest but weakens the headline claim.

---

## 5. Optionals — partial extension (26/70 conformance)

**What it is:** the CEL optionals extension.

**Current state: partial.**

  - **CELW-0015 (P1, missing-feature):** the static subset rejects the
    whole `.?field` optional-select syntax (`{'a': 1}.?a.value()`)
    at `compiler/frontend/parse_and_check.cc:631-641` (RejectDyn).
    Pin: `e2e/known_bugs_test.cc:698`.  This is the bulk of the 44
    skipped conformance rows ("the rest need `dyn`",
    `doc/user-guide/faq.md:193`).
  - **CORRECTION (2026-08-04, F1 verification):** this inventory
    originally repeated cleanup-backlog #41's claim that
    `optional.ofNonZeroValue(<message>)` traps.  **The claim was
    stale** — #41 was a duplicate of #10, whose fix
    (`cel_host.cel_message_is_zero`) landed 2026-06-10; the m39 F1
    node re-verified the arm end-to-end at HEAD (oracle-confirmed,
    every layer green, conformance row PASS in both link modes —
    `conformance/README.md:189-194`).  #41 is closed as
    fixed-by-#10.  Pin-hygiene lesson: a backlog entry survived its
    own fix by ~2 months because the fixing commit closed #10
    without sweeping for duplicate entries — and this inventory then
    amplified it into an owner decision.  Closure sweeps should grep
    the backlog for the symptom, not just the entry number.
  - Everything else in the extension passes (26/70 green, 0 FAIL
    corpus-wide).

**Recommendation: FINISH (L for CELW-0015).**  CELW-0015 is genuine
feature work (admitting `optional_type` through the static subset);
if deferred, SHIP AS-IS with the FAQ line as the documented boundary.

---

## 6. Host-fn error messages don't cross the wasm boundary

**What it is:** a host callback returning
`Value::Error({code, "msg"})` should surface `"msg"` to the caller.

**Current state: partial.**  The code crosses; the message text is
dropped — the decoded Value reads `"runtime error code N"`
(cleanup-backlog #31; PROPOSALS #2).  `examples/08` currently asserts
the broken behavior as documentation-by-test.

**Recommendation: FINISH (M — ABI addition: carry the message through
the error slot)** or make the code-only contract permanent and say so
in `writing-host-functions.md`.  Owner call; the worst outcome is the
current one, where the example teaches users to expect message loss
without the docs committing to it.

---

## 7. Public-API ergonomics gaps (documented honestly, no wrong behavior)

| Gap | Evidence (verified at HEAD) | Recommendation |
|---|---|---|
| `HostMapView` has no key-enumeration (`Size`/`Get`/`ContainsKey` only — `eval/host_call_context.h:99-103`, no `Keys()`) | PROPOSALS #3 | FINISH (S) or SHIP AS-IS — docs state the limitation |
| No `Value::AsProto<T>()` — typed proto extraction requires `MessageBacking()` (`eval/value.h:219`) + an internal header | PROPOSALS #4 | FINISH (S) or SHIP AS-IS |

Both are additive, non-breaking, and honest in the docs today.
Neither blocks the PR.

---

## 8. Conformance-divergence pin queue (CELW), by feature family

All pins live in `e2e/known_bugs_test.cc` (line per id below); each is
an observed-failing regression test with a machine-parseable CELBUG
block.  None is a crash; all are P1 except CELW-0014 (P2).

### 8.1 Conversions

| Pin | Divergence | Layer |
|---|---|---|
| CELW-0001 (`:1277`) | we accept `int(<duration>)`; cel-cpp rejects | `compiler/codegen/overload_table.cc` (drop seed) |
| CELW-0002 (`:1479`) | we accept `duration(<int>)`; cel-cpp's runtime rejects | same |
| CELW-0003 (`:1427`) | `double("<21-digit string>")` rounds one ULP low | runtime string→double parse |
| CELW-0016 (`:730`) | `double('  3.14  ')` doesn't strip surrounding whitespace | `runtime/cel_convert.c:350-362` |
| CELW-0014 (`:666`, P2) | `string(4294967295.0)` picks scientific where fixed is shorter | `runtime/cel_convert_double_format.cc` |

**Recommendation: FINISH (S each; 0001/0002 are one-line seed drops +
checker-decl removal).**  These are exactly the divergences an
upstream reviewer will diff against cel-cpp first.

### 8.2 `strings.format` (172/216 rows)

| Pin | Divergence |
|---|---|
| CELW-0008 (`:442`) | `%f`/`%e` accept an int operand; cel-cpp errors |
| CELW-0019 (`:1870`) | same family — uint coerced to double |
| CELW-0009 (`:472`) | `%f` of string tokens `NaN`/`Infinity` errors; cel-cpp renders them |

All three land in `runtime/cel_string_format_render.cc`'s
`ToDouble`/`CoerceToDouble`.  **Recommendation: FINISH (S, one batch —
same function).**

### 8.3 string_ext

| Pin | Divergence |
|---|---|
| CELW-0007 (`:410`) | `indexOf`/`lastIndexOf` bound `pos` by byte length, not code points (`runtime/cel_string_ext_search.cc:122-133`) |
| CELW-0018 (`:1530`) | two-arg `substring(start, end)` with `end == size()` returns the tail; cel-cpp errors |

**Recommendation: FINISH (S each).**

### 8.4 dyn-typed list indexing

CELW-0005 (`:234`) / CELW-0006 (`:264`): `[7, 8, 9][dyn(0.0)]` and
`[7, 8, 9][dyn(0u)]` diverge from the `lists.textproto` rows (index
coercion dispatcher, expr_lower index arm + `runtime/cel_list.c`).
**Recommendation: FINISH (M, one slice — shared dispatcher).**

### 8.5 Comprehension map building

CELW-0011 (`:525`): duplicate map key via `transformMapEntry` is
last-write-wins; cel-cpp errors (`runtime/cel_runtime.c:157-162`
`cel_map_insert_at`).  **Recommendation: FINISH (S).**

### 8.6 Timestamp boundary

CELW-0013 (`:585`): the maximum in-range timestamp
(`9999-12-31T23:59:59.999999999Z`) is rejected — the
`seconds == MAX && nanos > 0` guard at `runtime/cel_time_parse.cc:178`
is one nanosecond too strict.  **Recommendation: FINISH (S).**

---

## 9. Capacity envelope — arena sizing and unexposed limits

**What it is:** the per-Eval arena and parser input caps, and whether
an embedder can size them.

**Current state: partial / undocumented.**

  - **CELW-0017 (P1, `known_bugs_test.cc:890`)** + backlog #17:
    `x in xs` over a bound list of 10 k 50-byte strings exhausts the
    per-Eval arena (`runtime/cel_layout.h:16`) mid-scan —
    `FAILED_PRECONDITION: arena OOM`.  Loud, but a realistic workload
    (permission lists) hits it.
  - **backlog #49:** the *native* test/bench arena (64 KiB) SIGSEGVs
    silently on oversized operands instead of failing loudly (wasm
    side is guarded; native side isn't).
  - **backlog #15 + CELSKIP `e2e/known_bugs_test.cc:823`:** the parser
    100 000-codepoint cap is upstream's default and we expose no
    override in `CompilerOptions` — loud and correct-shaped, but an
    unexposed knob.
  - **PROPOSALS #10:** no user-facing page states a single numeric
    limit (they live in `e2e/limits_test.cc` + `runtime/cel_layout.h`
    + CLAUDE.md, which is unpublished).

**Recommendation: FINISH (M)** for the arena-sizing knob (fixes
CELW-0017's class rather than the instance) and **FINISH (S)** for a
published limits page (PROPOSALS #10).  Backlog #49's silent native
segfault is worth the S fix before an upstream PR (silent > loud rule).
Backlog #15: SHIP AS-IS (loud, upstream default).

---

## 10. Skips whose recorded blocker appears to have LANDED (stale-skip audit)

Per the tracking rules, "a skip that lingers after its blocker is gone
is a review finding."  Two found, plus one stale comment:

  1. **`e2e/type_value_test.cc:256`** skips because "Activation
     marshalling for `Repr::kMap` did not exist" — the skip itself
     says "RE-CHECK BEFORE TRUSTING THIS SKIP: as of 2026-07-25
     eval/instance.cc…".  At HEAD, `EncodeMap` exists
     (`eval/instance.cc:877-889`) and `EncodeBoundValue` dispatches
     `Repr::kMap` to it (`:965`).  **Recommendation: FINISH (S) —
     un-skip and confirm green** (not verified by execution here;
     this inventory is read-only and build-heavy agents own the
     output base).
  2. **`e2e/list_test.cc:448`** skips bound `list<string>` on "the
     host-arena gap … waits on the host-arena milestone."  The
     string/bytes activation-buffer marshalling has since landed
     (`EncodeStringOrBytes`, `eval/instance.cc:545`; exercised by
     `e2e/activation_boundary_test.cc` `StringBindBoundary` rows).
     Whether the list *element* read-back path
     (`EncodeFieldResult`) also cleared is not provable by reading —
     **Recommendation: FINISH (S) — re-verify; un-skip if green,
     else re-write the skip naming the surviving blocker.**  Same
     re-check applies to the design.md checkbox
     (`rewrite/design.md:4013` still unticked for string/bytes
     activation marshalling — tick or correct it).
  3. **Stale comment:** `eval/instance.cc:936-937` — "Map / enum /
     unknown activation marshalling not yet implemented" sits directly
     above the switch that dispatches `Repr::kMap` to the implemented
     `EncodeMap`.  **Recommendation: FINISH (trivial) — fix the
     comment when the file is next touched.**

---

## 11. Proto-surface deferred features (reasoned skips, no wrong values)

| Feature | Evidence | Recommendation |
|---|---|---|
| **Top-level Any-unwrap-at-result** — a top-level `google.protobuf.Any{…}` result stays an Any instead of unwrapping; empty Any should be an eval error | CELSKIPs `e2e/wkt_field_set_test.cc:559,577,593` (verified-FAIL conformance rows quoted in the skips) | FINISH (M) — read-side feature; the field-READ unwrap already works |
| **Wrapper auto-peel** — `Int32Value{value: 5}.…` rejected by the upstream checker as `wrapper(int)` select operand; waiting on auto-peel | CELSKIP `e2e/proto_literal_test.cc:825` (upstream-correct rejection, re-confirmed 2026-07-25) | SHIP AS-IS — upstream-consistent |
| **`proto.getExt` / `proto.hasExt` function form** — operator-form extension reads shipped (16/18 rows green, list-eq remainder fixed); the function form isn't registered, so `proto2_ext.textproto` (18 rows) SKIPs as `ext_unimpl` | backlog #40 (remaining scope) | FINISH (S–M — checker/celfn registration only) or SHIP AS-IS |
| **Strong-typed enums** — cel-cpp's own harness skips the same 18 rows; spec-acknowledged future feature | backlog #39; cel-spec issues/119 | SHIP AS-IS — parity with the reference implementation |
| **Unknowns through the e2e harness** — product behavior unit-tested; the e2e fixture lacks an unknown-binding surface | CELSKIP `e2e/type_value_test.cc:296` (harness-limit) | SHIP AS-IS (harness work, not product) |

---

## 12. By-design boundaries (inventoried for completeness — NOT stale)

These CELSKIPs are the static subset working as designed, each with a
citation; no action, listed so triage sees the whole skip census:

  - Static-subset (`RejectDyn`) shapes: `e2e/slot_aliasing_test.cc:115`;
    empty-`{}` map literal typing as `map(dyn, dyn)`:
    `e2e/comprehension_test.cc:584,604,805,920,951`,
    `e2e/wkt_field_set_test.cc:304,318`.
  - No celfn IDL spelling for `Any`/`Struct`/`Value`/`ListValue`/
    `type`/`optional<T>` as host-fn params:
    `e2e/host_fn_type_matrix_test.cc:729-769,1244,1258` (the
    parallel `foreign_fn_type_matrix` rows are resolved-by-removal).
  - Harness limits (wasmtime C-API panics on tail-calls, host
    visibility): `tools/wat_runner/wat_runner_test.cc:554,718`,
    `runtime/cel_runtime_wasm_test.cc:716`, `runtime/cel_list_test.cc:121`,
    `e2e/comprehension_test.cc:841` (malformed test, not product),
    `e2e/foreign_fn_type_matrix_test.cc:1659`.

---

## 13. Infrastructure gaps for an upstream-facing PR (PROPOSALS #6–#10)

All re-verified as still-open at HEAD:

| # | Gap | Recommendation |
|---|---|---|
| P#6 | No root `CONTRIBUTING.md` / `SECURITY.md` (both exist as `doc/` pages GitHub doesn't surface) | FINISH (S — two stub-link files) |
| P#7 | No release contract: `MODULE.bazel version = "0.1.0"` but no tags, no CHANGELOG, no `cel --version`; AOT artifacts are compiler-version sensitive | FINISH (S–M) — the strongest "prime-time" signal after §4 |
| P#8 | No generated API reference (headers sit outside `docs_dir`) | FINISH (M) or SHIP AS-IS |
| P#9 | No language-support matrix ("does `getFullYear` work?" — the most common adopter question) | FINISH (S — table generation from the conformance harness) |
| P#10 | No published capacity envelope | FINISH (S — see §9) |

---

## 14. Housekeeping backlog (open P2s that are debt, not features)

One line each; none blocks the PR; all tracked in
`cleanup-backlog.md` with full context:

  - **#2** — `kRuntimeExports[]` is a third source of truth for
    runtime export names (drift risk).  SHIP AS-IS / cleanup-when-touched.
  - **#3** — `wasi/experiments/exp1_re2/` symlink + cached absl
    install is post-Phase-C garbage.  REMOVE (trivial).
  - **#4/#5/#6** — `runtime/cel_memory.c` inline-asm barrier
    verification + stale "1-page memory"/`cel_alloc` comment block +
    hard-coded 64 KiB `cel_memory_size_()` vs the real 128 KiB.
    FINISH (S, one pass over one file); #5/#6 are the kind of stale
    comment/constant an upstream reviewer trips on.
  - **#48** — loose assertions on limit/error tests (status-code-only,
    no message substring).  FINISH (S) — the "Compilation limits" rule
    requires message asserts.
  - **#50** — policy decision: the PBT divergence miner gates CI but
    "WILL fail when it finds an oracle divergence" — a green gate that
    can go red by discovery.  Owner decision needed (not code).
  - **#19** — celfn IDL parser admits only the 12 base CEL types.
    SHIP AS-IS (documented boundary; interacts with §2's outcome).
  - **#21–#30** — runtime micro-perf items (O(N²) map-eq, per-alloc
    memset, `cv_at` opacity barrier, scratch-alloc in `cel_map_count`,
    …).  SHIP AS-IS — performance backlog, measured and tracked, not
    stale features.

---

## 15. Summary table (ordered by recommendation)

| Recommendation | Item | Section | Size |
|---|---|---|---|
| **FINISH — do before upstream PR** | Execution cost limits (epoch/deadline) — the P0 | §4 | M |
| ~~FINISH~~ NO WORK — stale claim | `optional.ofNonZeroValue(message)` "trap" (#41) — already fixed 2026-06-10 by #10; F1 verified working, oracle-confirmed | §5 | — |
| FINISH | Conversions pin batch CELW-0001/0002/0003/0016 (+0014) | §8.1 | S each |
| FINISH | format-render pin batch CELW-0008/0009/0019 | §8.2 | S (one batch) |
| FINISH | string_ext pins CELW-0007/0018 | §8.3 | S each |
| FINISH | `transformMapEntry` dup-key CELW-0011 | §8.5 | S |
| FINISH | Timestamp max-boundary CELW-0013 | §8.6 | S |
| FINISH | dyn list-index CELW-0005/0006 | §8.4 | M |
| FINISH | Arena sizing knob (CELW-0017 class) + native-arena loud-fail (#49) | §9 | M + S |
| FINISH | Stale-skip re-verification: `type_value:256`, `list_test:448` + `instance.cc:936` comment | §10 | S |
| FINISH | Any-unwrap-at-result | §11 | M |
| FINISH | Release contract (tags/CHANGELOG/`--version`) (P#7) | §13 | S–M |
| FINISH | CONTRIBUTING/SECURITY stubs (P#6), language matrix (P#9), limits page (P#10) | §13 | S each |
| FINISH | Host-fn error-message carriage (P#2/#31) — or document permanently | §6 | M |
| FINISH (optional) | CLI `--celfn` compile-side input | §3 | S |
| FINISH (optional) | `HostMapView::Keys()` (P#3), `Value::AsProto<T>()` (P#4) | §7 | S each |
| FINISH (optional) | `proto.getExt`/`hasExt` function form (#40 rem.) | §11 | S–M |
| FINISH (deferred OK) | `.?field` optional-select through the static subset (CELW-0015) | §5 | L |
| **SHIP AS-IS** | Wrapper auto-peel (upstream-consistent) | §11 | — |
| SHIP AS-IS | Strong-typed enums (#39, upstream parity) | §11 | — |
| SHIP AS-IS | Parser codepoint cap unexposed (#15) | §9 | — |
| SHIP AS-IS | By-design static-subset / IDL-spelling boundaries | §12 | — |
| SHIP AS-IS | Runtime perf backlog #21–#30, #2, #19 | §14 | — |
| SHIP AS-IS | API-reference generation (P#8) if time-boxed out | §13 | — |
| **REMOVE** | `@native` grammar arm + `kCelDefined` plumbing (owner call vs keep-as-reserved) | §2 | S |
| REMOVE | `wasi/experiments/exp1_re2/` garbage (#3) | §14 | trivial |
| **RESOLVED-BY-REMOVAL** | Entire §1 list: plugin backend, CELW-0021, backlog #51/#43/#32/#44-rem./#52(a), plugin skips, Go authoring, `cel generate`/`embed-decls`, `bindings/c/` | §1 | — (m39) |

Counts: FINISH 17 (5 marked optional, 1 deferred-OK) · SHIP AS-IS 6
families · REMOVE 2 · RESOLVED-BY-REMOVAL 13 items · 1 closed as a
stale claim (#41, fixed-by-#10).

Owner decision points, in priority order: (1) §4 cost limits —
land pre-PR or soften the sandbox claim; (2) ~~§5 the ofNonZeroValue
trap~~ closed — stale claim, feature works (F1 verification above);
(3) §2 `@native` remove-vs-reserve — executed as REMOVE in m39/D4 per
the decisions log; (4) §6 error-message carriage vs permanent
code-only contract; (5) §13 P#7 release contract.
