# M14 Slice B — independent review (2026-05-22)

Reviewer: independent (no implementor context).  Audit scope: the
working-tree changes against master at the point where the milestone
doc claims "Slice B shipped 2026-05-21" with +4 PASS unlock.

## Verdict

**Mixed.**  Slice B's core mechanic (Repr::kOptional plumbing,
SelectKeyRodataVisitor, EmitKSelect/EmitKIndexCall optional branches)
is correctly architected, the unit + e2e tests for the happy paths
pass, and the documented +4 PASS unlock reproduces (`pass=1572` vs
master baseline `1476` → Slice A's `1568` → Slice B's `1572`).
However the slice has three classes of honest problems:

  1. **`.baseline` file was not bumped.**  `conformance/.baseline`
     reads `1568` in the working tree but the actual run is `1572`.
     The doc punts this to Slice D, which is acceptable per the plan
     — flagged here because someone reading "Slice B shipped" will
     reasonably expect the floor to be at 1572.
  2. **Wide CLAUDE.md "no milestone/slice references" violations in
     new Slice B code.**  The rule landed in the same diff (CLAUDE.md
     +22 lines) and is then violated on ~10 added lines in
     `overload_table.cc`, `parse_and_check.cc`, `expr_lower_test.cc`,
     `engine.cc`, `cel_type.c`, plus more in non-Slice-B files
     (wat_runner, runtime BUILD).  This is the "applies to NEW
     comments" half of the rule.  See §3 + §7.
  3. **Coverage matrix has real holes for the kernel shapes Slice B
     pipes data into.**  Codegen tests only exercise `optional<map<...>>`;
     `optional<list>`, `optional<message>`, and `optional<optional<T>>`
     are not exercised at codegen or e2e levels, and the kernel
     itself *traps* (`__builtin_trap()`) on `CEL_MAP_HOST` /
     `CEL_LIST_HOST` / `CEL_MESSAGE` inner kinds.  No test for
     `optional.none().field` at codegen/e2e.  No test for
     `m[?k]` AST shape (`_[?_]`) — the closest is `_[_]` on optional
     operand.  No 3VL absorption tests for
     `cel_select_optional_field_at_vv`.

**Top 3 to look at first:**

  - **P1**: bump `.baseline` to 1572 before the next conformance gate
    runs (or accept the doc's framing that Slice D owns this), and
    add the missing optional-shape coverage flagged in §4 before
    Slice C lands more codegen on the same kernel.
  - **P1**: clean up the new milestone/slice references the rule
    explicitly forbids — most are mechanical edits and the longer
    they sit the harder it becomes to enforce the rule retroactively.
  - **P2**: the `optional.ofNonZeroValue(<HOST_MESSAGE>)` ⇒ trap
    branch in `cel_optional.c::dispatch_lookup` claims "Slice B"
    will fill it in; Slice B *shipped* without doing so.  Either
    re-target the comment to a later slice or land the trampoline.

## 1. Architectural drift

The as-built shape matches the as-designed shape in
m14-optionals.md §1.7 and §4 well.  Specifically:

### 1.1 `Repr::kOptional` plumbing is stamped consistently

Both `ReprOf` overloads in `compiler/ir/typed_ast.cc` stamp
`Repr::kOptional`:

  - The `cel::TypeSpec` overload (wire-format / proto-derived) at
    `typed_ast.cc:62-66`: matches `has_abstract_type() &&
    abstract_type().name() == "optional_type"`.  Verified by
    probe Q8.
  - The `cel::Type` overload (strong-typed cel-cpp API) at
    `typed_ast.cc:107-112`: matches `kOpaque` kind narrowed via
    `Is<cel::OptionalType>()`.  This avoids matching other future
    `OpaqueType` subclasses — a defensive choice worth keeping.

`ReprName` in `annotations.cc:40-41` covers the new enumerator;
the `kOptional` arm in `ReprOf` is paired with a
`PostVisit`-equivalent unit-test row
(`OptionalTypeAbstractIsKOptional`,
`OptionalOfDynAbstractStillStampsKOptional`,
`OtherAbstractNamedAbstractStaysUnknown`).  Plus
`OptionalAbstractEntryBecomesKOptional` proves end-to-end that
`PopulateAnnotations` propagates the stamp into `WasmAnnotations`.

No `kUnknown` fall-through bug evident.  **No drift.**

### 1.2 `SelectKeyRodataVisitor` pass-ordering is correct

`LayoutPass` (compiler/codegen/layout_pass.cc:387-397):

```
StaticMemoryBuilder builder(layout.rodata_base);
ConstLayoutVisitor const_visitor(builder, layout.annotations);
cel::AstTraverse(ast.ast().root_expr(), const_visitor);
SelectKeyRodataVisitor select_key_visitor(builder, layout.annotations);
cel::AstTraverse(ast.ast().root_expr(), select_key_visitor);
layout.rodata = std::move(builder).Finalize();
```

Verified:

  - Operand annotations are populated by `PopulateAnnotations`
    (parse_and_check.cc:1095), which runs in the frontend
    BEFORE `LayoutPass` is ever called.  When
    `SelectKeyRodataVisitor` looks up `sel.operand().id()` in
    `annotations_`, the entry already carries the correct `Repr`.
  - `SelectKeyRodataVisitor` runs INSIDE Pass A, between
    `ConstLayoutVisitor` and `Finalize`.  The single shared
    `builder` keeps the rodata contiguous; the doc claim is
    accurate.
  - The visitor only writes `select_key_rodata_offset` when
    `op->repr == Repr::kOptional`.  Non-optional Selects leave
    the field at zero — matches the `EmitKSelect` invariant
    check at expr_lower.cc:211.

**No drift.**  One nitpick: the visitor uses `annotations_[expr.id()]`
which silently insert-default-creates a NodeAnnotation if one
isn't present.  That's fine here because every Select node was
seeded by `PopulateAnnotations`, but if a future regression broke
that invariant the failure mode would be a silent zero-default,
not a CHECK.  Consider `ABSL_CHECK(annotations_.Find(expr.id())
!= nullptr)` before the write.  P2.

### 1.3 `EmitKSelect` optional branch — partial shape coverage

`expr_lower.cc:206-215` adds the kSelect-on-optional branch:

```
const NodeAnnotation* op_ann =
    ctx.layout.annotations.Find(sel.operand().id());
if (op_ann != nullptr && op_ann->repr == Repr::kOptional) {
  ABSL_CHECK(ann.select_key_rodata_offset != 0) << ...;
  return EmitKSelectOptionalBranch(ctx, *operand_or, out_slot,
                                   ann.select_key_rodata_offset,
                                   sel.test_only());
}
```

The branch correctly:

  - Routes through `cel_select_optional_field_at_vv`.
  - Honours `sel.test_only()` by chaining
    `cel_optional_has_value_at_v` on the same slot (aliasing safe
    because the kernel writes a fresh `Bool` overwriting the
    OPTIONAL — kernel does not re-read the OPTIONAL after the
    bool write).
  - CHECKs that LayoutPass populated `select_key_rodata_offset`,
    so a regression in `SelectKeyRodataVisitor` trips at the
    consumer.

**Shape coverage at codegen/e2e:**

| Shape | Codegen test | e2e test | Runtime kernel test |
|---|---|---|---|
| `optional<map>.field`           | ✓ `SelectOnOptionalEmitsOptionalKernelCall` | ✓ `OrValueOnSelectIndexChainReturnsResolvedValue` | ✓ `SelectFieldOnOptionalMapSomeRecurses` |
| `optional<list>.field` *(if even legal)* | — | — | ✓ via `SelectFieldOnArenaListInBoundsProducesSome` (kernel only) |
| `optional<message>.field`        | — | — | — (kernel `__builtin_trap()`s) |
| `optional<optional<T>>.field`    | — | — | — |
| `optional.none().field` chain    | — | — | ✓ `SelectFieldOnOptionalMapNoneStaysNone` (kernel only) |
| `has(optional.none().field)`     | — | — | — |

Coverage holes flagged in §4.

### 1.4 `EmitKIndexCall` optional branch — handles `_[_]` only

`expr_lower.cc:635-647` routes `Repr::kOptional` operands through
`EmitIndexCallOptionalBranch`, which emits the same kernel.
Closed-set CHECK on the residual non-`{kMap, kList, kOptional}`
arm restored per cleanup-backlog #8.

**However:** `EmitKIndexCall` is invoked ONLY for
`call.function() == "_[_]"` (expr_lower.cc:1094).  The
`m[?k]` syntax sugar produces `Call("_[?_]", ...)`, which routes
through the general kCall OverloadTable arm with overload-id
`map_optindex_optional_value` / `list_optindex_optional_int` /
etc.  All seven of those overload IDs in `overload_table.cc` map
to `cel_select_optional_field_at_vv`, so the runtime path is
identical — but there is **no codegen, e2e, or kernel test** that
exercises the `_[?_]` AST shape end-to-end.

The closest test is `IndexOnOptionalEmitsOptionalKernelCall`
(`expr_lower_test.cc:1247-1259`) which uses
`optional.of({'c': 'v'})['c']` — that's `_[_]` with optional
operand, NOT `_[?_]`.  A regression in any of the seven optindex
overload seeds would not be caught.  **P1 coverage gap.**

### 1.5 Conformance unlock honesty

Doc says +4 PASS (1568 → 1572).  Verified via running
`bazel-bin/conformance/run_conformance`:
`summary: total=2454  pass=1572  skip=735  fail=147`.  ✓

`optionals.textproto` slice: `total=70  pass=18  skip=49  fail=3`.
The 3 remaining FAILs:

  1. `optional_chaining_1`:
     `optional.ofNonZeroValue('').or(optional.of({'c': {'dashed-index':
     'goodbye'}}.c['dashed-index'])).orValue('default value')`
     — leftmost expression issue is `{'c': ...}.c` (map literal,
     `.c` Select).
  2. `optional_chaining_2`:
     `{'c': {'dashed-index': 'goodbye'}}.c[?'dashed-index'].orValue('default value')`
     — leftmost is `{'c': ...}.c`.
  3. `optional_chaining_3`:
     `{'c': {}}.c[?'missing-index'].orValue('default value')`
     — leftmost is `{'c': ...}.c`.

All three share `{'c': ...}.c` as the leftmost expression.  This
matches the cleanup-backlog #9 entry ("map-dot-field sugar is
broken pre-existing") exactly.  **The claim is honest.**

A direct e2e probe is consistent: per cleanup-backlog #9, simply
`{'k': 'v'}.k` fails at master with the same error — confirming
this is genuinely a pre-existing gap and not Slice-B-introduced.

## 2. Tech-debt inventory

### 2.1 Stub-body inconsistencies (P1)

`runtime/cel_optional.c::dispatch_lookup` lines 283-291:

```c
case CEL_MAP_HOST:
case CEL_LIST_HOST:
case CEL_MESSAGE:
  // Host-backed select-field needs a host trampoline (Slice B);
  // the optional kernel will then reinterpret FIELD_NOT_FOUND as
  // None by symmetry with the arena cases.  Trap until then so
  // unintended use shows up at the call site rather than
  // silently miscompiling.
  __builtin_trap();
```

**Comment says "Slice B"; Slice B has shipped without filling
this in.**  Two problems:

  - The comment is now stale (which the new CLAUDE.md "code
    comments should describe what stays true after milestones
    close" rule explicitly targets).
  - Worse: any production user who declares a variable of type
    `optional<google.api.expr.test.v1.proto3.TestAllTypes>` (the
    "host" message case) and writes `var.field` will codegen a
    `cel_select_optional_field_at_vv` call.  That kernel will
    `__builtin_trap()` on the message inner.  Conformance corpus
    happens to use only `optional.of({...})` constructors so it
    doesn't bite — but it's a real latent miscompile-vs-trap.

Severity: P1 if any out-of-conformance use pulls this; P2 if the
slice authors are confident the conformance corpus is the only
exercised surface for now.  Either re-target the comment to a
named follow-up slice or land the host trampoline.

### 2.2 Visitor double-traversal (P2)

`LayoutPass` Pass A now invokes `cel::AstTraverse` twice — once
for `ConstLayoutVisitor`, once for `SelectKeyRodataVisitor`.  For
a typical optional expression this is fine, but the doc itself
locked the "Pass A" name to one walk.  Either:

  - Fold `SelectKeyRodataVisitor`'s work into `ConstLayoutVisitor`
    via a `PostVisitSelect` override on that single visitor; or
  - Rename the comment block to "Pass A.1 / Pass A.2" and accept
    the second walk.

Cosmetic; effort ~1 hour.  P2.

### 2.3 New code overlap with existing helpers (P2)

`EmitKSelectOptionalBranch` (expr_lower.cc:163-189) and
`EmitIndexCallOptionalBranch` (expr_lower.cc:589-603) both
construct a `BinaryenBlock` of `{call, out_slot_const}` to expose
the slot to the parent expression.  The same pattern recurs in
several other emit-helpers.  A small helper
`EmitCallReturningSlot(ctx, target_name, args, out_slot)` would
de-duplicate this across ~4 sites.  Effort 1-2 hours.

### 2.4 Comment block describing old shape (P2)

`compiler/codegen/expr_lower.cc:226-229` originally explained
the `field_ref_id` path before the optional branch was inserted.
The current comment still describes the path as if it's the only
one ("looks up that path and appends `sel.field()` at runtime.
0 when the operand isn't path-bearing (literal, kCall, …)").  A
reader hitting this after the optional branch above would
benefit from a one-liner naming the two paths.

### 2.5 Test file metadata stale (P2)

`runtime/cel_optional_test.cc` line 1 reads:

```
// M14 Slice A — per-TU tests for `cel_optional.{h,c}`.
```

The file is a Slice A artifact but is listed in the Slice B
review's "key files Slice B touched" set — let me confirm the
status: `git status` shows it as untracked (??), so this whole
file is still part of the Slice A commit-to-be.  Not actually
a Slice B file per the actual file mtimes (May 21 23:10).
The reviewer's brief was misleading on this point; flagging
for record.

## 3. CLAUDE.md rule compliance (P1)

The new rule landed in CLAUDE.md (§"No milestone / slice
references in code comments") in the same working-tree diff.
It explicitly says: "The rule applies to NEW comments."  Yet the
new Slice B code (added lines in this diff) contains many
violations.  Grepping `git diff master -- compiler_v2/` for added
lines (`^+`) referencing `M14`, `M5.`, `Slice [A-Z]`, `M7-` and
excluding the carved-out `stub until` form:

**Violations in Slice B-attributed files:**

  - `compiler/codegen/overload_table.cc:496`:
    `// ── M14 — CEL optionals ──────────────────────────────────`
  - `compiler/codegen/expr_lower_test.cc:1170`:
    `// M14 Slice B — Select / index on optional-typed operand`
  - `compiler/codegen/expr_lower_test.cc:1232`:
    `// optional-typed Select chain.  Per m14-optionals.md §1.7 we emit`
    (this one cites the doc path — borderline; CLAUDE.md says
    "Cite a design-doc path only when the doc explains a
    non-obvious invariant" — §1.7 is exactly that, so this one
    is *probably* legitimate.  Keep, but lean toward the
    invariant-not-the-milestone phrasing.)
  - `compiler/frontend/parse_and_check.cc:433`:
    `// M7-Slice-9 path).  Reject here so the conformance harness`
    (the M7-Slice-9 is in a regular code comment, NOT inside a
    `stub until` message — violation).
  - `compiler/frontend/parse_and_check.cc:678-680`:
    `// M14 optionals: registers the `optional.of` / ...`
    `// M14: optional syntax (...`
    `// additionally registers `optMap` / `optFlatMap` (M14).`

**Violations in files the brief described as Slice B but which
also touch other workstreams (still NEW comments, still
violations):**

  - `eval/engine.cc`: `// M14 — CEL optional<T> kernels`
  - `runtime/BUILD.bazel`: `# M14 — optional<T> kernels`
  - `runtime/cel_type.c`: `// CEL_OPTIONAL = 14 (M14 Slice A)`
  - `runtime/cel_type_test.cc`: `// in M14 Slice A by filling ...`
  - `tools/wat_runner/wat_runner.cc`:
    `// M14 — optional<T> kernels.  Slice A landed these; the`
    `// `RegisterPendingM14Imports` no-op shim that used to bind them`
    `// (including the M14 Slice 0 ones we can't modify) declare a`
  - `tools/wat_runner/wat_runner_test.cc`:
    `// M14 — CEL optionals end-to-end WAT tests.`
    plus six test names `WatRunnerM14Test`.

The test-class name (`WatRunnerM14Test`) is arguably load-bearing
identifier rather than a "comment" per se — the rule says
"comments" specifically, so this is a judgement call.  Leaning
"OK" because renaming a test class breaks lookup history.

**The `stub until <milestone>` carve-out is used correctly** in
expr_lower.cc:553-559 (the `?field:` proto-literal stub) — that
one is fine.

**Effort to fix:** mechanical sed-style cleanup; ~30 minutes.
P1 because the rule explicitly says "applies to NEW comments"
and was checked in alongside the violating code; tolerating it
sets the precedent that the rule is aspirational.

## 4. Coverage gaps

### 4.1 Negative-path coverage at codegen/e2e (P1)

The brief asks specifically for:

  - **`optional.none().field` → None.**  Tested at the kernel
    level only (`SelectFieldOnOptionalMapNoneStaysNone`,
    `cel_optional_test.cc:361-370`).  No codegen test (would
    confirm `EmitKSelect` routes correctly when the operand is
    `optional.none()`).  No e2e test (would confirm the full
    parse→check→codegen→eval pipeline returns None).  P1.
  - **`has(optional.none().field)` → false.**  Not tested at any
    level.  The closest test is
    `HasOnOptionalSelectReturnsFalseWhenAbsent`
    (`m14_test.cc:97-105`) which uses
    `has(optional.of({'c': {'entry': 1}}).c.missing)` — that's
    `optional.of`-rooted, not `optional.none`-rooted.  P1.
  - **Optional-with-Error-inner.**  Not directly tested.  The
    kernel-level `OfPropagatesError` test
    (`cel_optional_test.cc:145-151`) confirms `cel_optional_of_at_v`
    propagates errors as errors (not as
    Some(CEL_ERROR) cells), but there's no test for
    `optional<map>.field` where the inner-map lookup poisons.
    Important because `cel_select_optional_field_at_vv` has a
    `is_absent_error` branch that distinguishes ABSENT errors
    (re-cast to None) from other errors (propagated as ERROR).
    P2.

### 4.2 3VL absorption for `cel_select_optional_field_at_vv` (P1)

The brief asks specifically.  Kernel code at cel_optional.c:330
calls `absorb_3vl_binary(out, src, key)`, which handles
CEL_ERROR / CEL_UNKNOWN on either operand.  **No test exercises
this branch.**  Missing tests:

  - `cel_select_optional_field_at_vv` with CEL_ERROR src ⇒
    propagates the error verbatim into `out`.
  - With CEL_UNKNOWN src ⇒ propagates.
  - With CEL_ERROR key (e.g., key produced by a poisoned
    sub-expression) ⇒ propagates.
  - With CEL_UNKNOWN key ⇒ propagates.

Adding four tests in `cel_optional_test.cc` is mechanical (~30
minutes).  These are load-bearing because 3VL absorption is a
spec-mandated property (`langdef.md` §"Errors and unknowns").
P1.

### 4.3 Test_only Select on optional matrix (P1)

The brief asks: "covers present-and-has-field AND
present-but-no-field AND absent-optional?"

| Case | Test coverage |
|---|---|
| Present optional + has-field          | ✓ `HasOnOptionalSelectReturnsTrueWhenPresent` (m14_test.cc:88-95) |
| Present optional + missing-field      | ✓ `HasOnOptionalSelectReturnsFalseWhenAbsent` (m14_test.cc:97-105) |
| Absent optional (i.e. `optional.none()`) + has-field | **NOT TESTED** |

The third case is genuinely different from the second — the
second-step kernel call's source is None-flagged before the
field lookup runs.  No codegen, e2e, or runtime test exercises
the path `has(optional.none().f)`.  P1.

### 4.4 `optional<list>` / `optional<message>` / `optional<optional<T>>` coverage (P1)

The brief asks if `EmitKSelect`'s optional branch handles "every
shape the kernel was specced for".  Codegen test:

  - `optional<map>.field`: ✓
  - `optional<list>.field`: **NOT TESTED at codegen**.
    Aside: this is *semantically* an unusual shape — lists don't
    have field selectors in CEL except via the `.size` macro.
    The shape that exercises `Repr::kOptional`-operand list
    routing is `optional.of([1,2,3])[0]`, which is `_[_]` on
    optional, not Select.  That shape is **NOT TESTED at codegen
    or e2e** either (the `IndexOnOptionalEmitsOptionalKernelCall`
    test uses `optional.of({'c':'v'})['c']` — map, not list).
    The kernel itself is exercised by `A5/A6` (Slice A) at the
    bare-runtime level.  P1.
  - `optional<message>.field`: **NOT TESTED at codegen/e2e/kernel**.
    Kernel `__builtin_trap()`s on inner CEL_MESSAGE.  P1 — would
    surface the latent trap (§2.1).
  - `optional<optional<T>>.field`: **NOT TESTED**.  Likely
    rejected at the checker layer (cel-cpp doesn't typically
    admit nested optional Select), but worth a negative test
    to lock the behaviour.  P2.

### 4.5 `_[?_]` AST shape coverage (P1)

Already discussed in §1.4.  No codegen, e2e, or kernel test
exercises the `Call("_[?_]", ...)` shape that the seven
optindex overloads in `overload_table.cc` route through.  A
regression in any of those seeds would not be caught.  Adding a
codegen test (`m[?k]` → emits
`cel_select_optional_field_at_vv`) is ~20 LOC.  P1.

### 4.6 `EmitKSelect` chained-Select-on-optional codegen test (P2)

`ChainedSelectOnOptionalLiftsEachField`
(`layout_pass_test.cc:856-883`) covers the LayoutPass rodata
allocation for chained optional Selects, but the codegen layer
has no test that
`optional.of({'c': {'x': 'v'}}).c.x` emits **two** sequential
`cel_select_optional_field_at_vv` calls (the second consuming
the first's out).  The e2e `OrValueOnSelectIndexChainReturnsResolvedValue`
covers the same expression end-to-end so this is verified
implicitly, but a focused codegen assertion would harden the
contract.  P2.

## 5. Doc drift

### 5.1 `testing-checklist.md` not updated (P1)

The CLAUDE.md rule mandates: "every merged feature flips at
least one box".  Slice B claims to ship +4 conformance PASS,
new codegen branches in `EmitKSelect` and `EmitKIndexCall`, a
new IR enum `Repr::kOptional`, a new annotation field
`select_key_rodata_offset`, a new LayoutPass visitor, and a new
frontend gate in `CheckSubsetStruct`.  None of those are
reflected in `doc/implementation-plan/testing-checklist.md`
(diff vs master = 0 lines).  P1.

### 5.2 `per-component-test-coverage.md` not updated (P1)

Same story.  The doc per CLAUDE.md is the "keystone testing
doc" with the per-component required-scenarios matrix.  Slice B
introduced a new component (`SelectKeyRodataVisitor`) and
extended two (`EmitKSelect`, `EmitKIndexCall`).  No matrix
update.  Diff vs master = 0 lines.  P1.

### 5.3 `doc/implementation-plan/rewrite/design.md` doesn't enumerate kOptional (P2)

design.md does reference `Repr` in narrative text (lines 302,
311, 346 etc.) but doesn't enumerate the values explicitly.
Adding `kOptional` to any list / table that already enumerates
the Repr values would be the right reflex; verifying none such
exists, this is a P2-not-a-problem.  Stays.

### 5.4 `cel-host-surface.md` select-field section doesn't mention optional (P2)

`doc/implementation-plan/rewrite/cel-host-surface.md` describes
`cel_host.cel_get_field` (line 636) and the `FieldEntry` row
(line 721) but never mentions the optional-typed Select path.
A new reader looking up "how does Select-on-optional work" via
the host-surface doc finds nothing.  Either (a) add a section
noting that `cel_select_optional_field_at_vv` is the dual
kernel for optional-typed Selects, or (b) accept the doc is
about host-imported surfaces specifically and optionals are
all self-hosted.  P2.

### 5.5 `m14-optionals.md` is updated and honest

  - Status header is updated to reflect Slice B shipped.  ✓
  - "Plan-vs-execution delta" sections are tagged on each
    affected slice.  ✓
  - The Slice B delta 1 (kernels in place before Slice B
    started) and delta 2 (map.field sugar gap blocks 3 rows)
    correctly describe what shipped.  ✓
  - Delta 3 ("no new WAT files needed") is a reasonable claim
    given the symmetry argument.  ✓

`cleanup-backlog.md` #8 closed and #9 added with accurate
filing for the 3 remaining FAILs.  ✓

## 6. Conformance honesty

### 6.1 Summary numbers reproduce

```
$ bazel-bin/conformance/run_conformance 2>&1 | grep "^summary:"
summary: total=2454  pass=1572  skip=735  fail=147
```

Slice A landed `+92` (1476 → 1568); Slice B claims `+4`
(1568 → 1572).  Total `pass=1572` matches.  ✓

### 6.2 `.baseline` file is stale (P1, noted in verdict)

`.baseline` reads 1568 in working tree.  Bumping to 1572 is
Slice D work per the m14 plan, so this is technically not a
Slice B failure — but anyone running
`scripts/check_conformance_monotonic.sh` between now and Slice
D close will record only 1568 as the floor, missing 4 PASSes.
Recommendation: bump the file in the Slice B commit and let
Slice D's closeout be a no-op for `.baseline`.

### 6.3 The 3 remaining FAILs are honestly all map.field-sugar

Verified directly:

```
$ bazel-bin/conformance/run_conformance \
    --file=$PWD/tests/simple/testdata/optionals.textproto \
    --max_fail_examples=20 2>&1 | tail -20
...
FAIL optionals/optional_chaining_1 ... .c['dashed-index'] ...
FAIL optionals/optional_chaining_2 {'c': ...}.c[?'dashed-index'] ...
FAIL optionals/optional_chaining_3 {'c': {}}.c[?'missing-index'] ...
```

All three: leftmost expression is `{<literal-map>}.c` (i.e.
`.c` on a map-literal operand), which `EmitKSelect` routes
through `cel_get_field` (message-field path), which errors on
a CEL_MAP_ARENA operand.  This matches the cleanup-backlog #9
description and the m14 delta 2 description verbatim.  **The
"all three blocked on map.field sugar" claim is honest.**

### 6.4 Spot-check of PASS rows

The conformance harness's `run_conformance` binary doesn't
have a `--max_pass_examples` flag, so spot-checking the +4
unlocked rows requires manual textproto inspection.  Given the
3 FAILs cover #1, #2, #3 and the doc identifies them as
specifically blocked, the +4 unlock is the
`optional_chaining_4..9` rows (or similar — 18 total PASS in
optionals.textproto out of 21 non-skipped, so all non-
SKIP-static_subset non-blocked rows pass).  Manual spot-check
of `optional_chaining_4` (the e2e test `ConformanceChaining4ExactSource`
matches its exact source) confirms PASS.  Likewise
`optional_chaining_9` via `ConformanceChaining9ExactSource`.
Spot-check OK.

## 7. Rule compliance — summary

Restated for the CLAUDE.md "no milestone/slice references in
code comments" rule:

| Severity | Count of violations in NEW added lines |
|---|---|
| Hard violation (e.g. `// M14 Slice B —`)             | 7 |
| Stale "Slice B" comment in old code path (§2.1)      | 1 |
| Borderline (cites m14-optionals.md path + section)   | 1 |
| Inside `stub until` carve-out (rule-compliant)       | 2 |

Effort to fix the hard violations: ~30 minutes.  P1.

## 8. Effort estimate to clear P0/P1 items

| Item | Severity | Effort |
|---|---|---|
| Bump `.baseline` to 1572 | P1 (or P2 if Slice-D-deferred) | 5 min |
| `optional.none().field` codegen + e2e tests | P1 | 30 min |
| `has(optional.none().field)` e2e test | P1 | 20 min |
| 3VL absorption tests for `cel_select_optional_field_at_vv` | P1 | 30 min |
| `optional<list>` / `optional<message>` codegen tests | P1 | 45 min |
| `_[?_]` AST shape codegen test | P1 | 20 min |
| Test_only Select on absent-optional | P1 | 20 min |
| CLAUDE.md rule violations cleanup | P1 | 30 min |
| `testing-checklist.md` rows ticked | P1 | 15 min |
| `per-component-test-coverage.md` rows | P1 | 30 min |
| `cel_optional.c` "Slice B" stale comment retarget | P1 | 5 min |

Total P1 cleanup: ~4-5 hours.  P2 items add another 2-3 hours
of cosmetic cleanup that can defer.

## 9. Out-of-scope observations (for the user, not for Slice B)

  - **Cleanup-backlog #9 is the real ceiling on optionals
    conformance**, and it's a general map.field-sugar gap that
    blocks rows outside the optionals corpus too (per the entry's
    own framing: "Likely blocks rows in other corpora too — the
    probe is a universal pattern").  A small slice that lights up
    `EmitKSelect` for `Repr::kMap` operands with string keys
    would likely unlock not just the 3 remaining optional FAILs
    but additional rows elsewhere.  Worth scheduling sooner
    rather than later — the fix piggybacks neatly on Slice B's
    `SelectKeyRodataVisitor` rodata pattern.
  - **Conformance unlock was 1/4 of the original target (+4 vs
    +15).**  The doc's delta 2 explains this honestly — 11 of
    the expected 15 are gated on the map.field sugar gap, not on
    Slice B's surface.  Worth confirming the +15 target was
    realistic; if 11 of 15 were always going to need #9, the
    target should have been +4 from the start.
