# 91-contract-coherence — cross-boundary lens pass

Lens: coherence of the end-to-end stories across the 13 component notes —
(a) Compiler→Program→Engine→Instance lifecycle + thread safety, (b) error
propagation compiler→runtime→eval→Value, (c) 3VL/unknown propagation incl.
partial eval and the host-fn unknown sentinel, (d) custom-fn dispatch across
@host / CEL-defined / @component.  Every inter-note disagreement below was
settled by reading the code on this tree (2026-06-10); citations are to the
**current working tree**, which matters — see finding 1.

## 0. What coheres (no action)

- The @native/kCelDefined ghost is reported identically by three notes:
  celfn §2.1 (P0, backend never shipped on master), compiler-toplevel §2.2
  (`library_modules`/`CompileLibraryBodies` dead surface), codegen-memory
  §2.4 (`rodata_base_override` citing the nonexistent `library_module.cc`).
  celfn validation 1 and compiler-toplevel validation 3 are the same probe —
  dedupe in the consolidated backlog.  celfn §5's "open architectural fork"
  (single-module inlining vs bundled library module) is the one decision the
  new custom-fn doc must actually make, not just record.
- `Engine::AddComponent` staleness (header says Unimplemented, code is real)
  is flagged consistently by eval-public §2.1, celfn §2.5, testing-system
  §2.2 — one fix, three notes satisfied.
- The three-enum numbering story (ir::Repr ≠ shared::CelType ≠ wire CelKind ≠
  Value::Kind) is explicitly reconciled by abi-shared §5 ("three enums, three
  jobs") and matches eval-public §1.6 and runtime-kernel §1.2.  Restate
  abi-shared's alignment-rules paragraph verbatim in the new ABI doc.
- Error-message loss at the host→wasm boundary (cleanup-backlog #31) is told
  compatibly by eval-internal D1 (drop site cel_host.cc kError arm),
  eval-public validation 1 (trap path loses the status code), and
  tools-examples §1.5 (example 08 documents the loss) — but see finding 7 for
  the part of that story the notes get wrong.

## 1. P1 — the tree mutated under the readers; uncommitted gates supersede two headline P0s

`compiler/internal/compile.cc`, `compiler/internal/compile_test.cc`,
`eval/engine.cc`, `eval/engine_test.cc`, and `e2e/known_bugs_test.cc` carry
**uncommitted** working-tree changes (compile.cc mtime 2026-06-10 00:13:58 —
inside the 00:11–00:22 window the notes were written; `git show
HEAD:compiler/internal/compile.cc` has zero hits for the new symbol).  The
diff adds two cross-boundary gates:

- `ValidateExprStaticRegion` (compile.cc:57-70, called from
  `RunFrontAndLayout` at compile.cc:450-452, i.e. **both link modes**, skipped
  only when `rodata_base_override != 0`): rejects rodata+workspace past
  `CELWASM_RESERVED_LOW_MEMORY_BYTES` with ResourceExhausted at Compile.
- `ValidateAbiSlotExtents` (eval/engine.cc, Plan-time): rejects a Program
  whose `cel.abi` variable slot extends past the 8192-byte window.

These resolve, in the working tree, findings the notes carry as open:

- codegen-memory §2.1 (P0 "workspace is bounded nowhere ... no compile-time
  or run-time tripwire") — true at HEAD, fixed in the working tree.  Its
  validation items 1 and 2 are answered: known_bugs_test.cc:815-835 now
  documents the root cause (no-op `SlotAllocator::Release` → 1000-term chain
  allocates 2009 slots = 48 216 B written over runtime statics in shared
  memory; N=2000 reached dlmalloc state → the "unaligned atomic" trap) and
  `LongArith_166Terms_RejectedAtCompile` / `_2000Terms_...` assert the gate.
- runtime-kernel §3 V1 / §2.5 (backlog #16, the 10K-literal-list wasmtime
  panic "must originate outside the kernels") — hypothesis confirmed AND
  closed: the test is now `LiteralIntListInScanRejectedAtCompileAt10K`
  (working-tree diff of known_bugs_test.cc).
- benchmarking §1.3's corpus finding #2 (dynamic-mode silent rodata
  miscompare, OPERATORS.md:325-334) — the dynamic-mode hole the gate closes;
  OPERATORS.md and the m28 doc §10 follow-up text are now stale against the
  working tree.
- compiler-toplevel §1.4 ("the rodata budget is STATIC link mode only") and
  codegen-memory §1.6's enforcement table ("dynamic mode has no budget
  check") describe HEAD, not the tree.

Consequences for the consolidation pass: (i) re-anchor every citation to one
commit — compiler-toplevel and celfn already cite different line ranges for
the same `compile.cc` function (`InstallOverloadImportsExport` at "265-299"
vs "301-357") because they read different versions; eval-public's engine.cc
lines are pre-diff (+30 lines); (ii) the new memory-model doc must describe
the gated topology, not the unbounded one; (iii) `rodata_base_override` is no
longer dead — the new gate's comment names it as the kStatic relocation seam
(compile.cc:446-450), contradicting codegen-memory §2.4's "zero production
users" (verify which is true post-commit).

## 2. P1 — design-heritage vs codegen-memory on SlotAllocator: codegen-memory is right

design-heritage §1.1 (§6 row) claims "`Release` is no longer a no-op —
free-list reuse landed" and §13 row "reuse is release-based"; codegen-memory
§1.5 says Release is a no-op and `peak_slots` counts total acquires.  Code:
`slot_allocator.cc:24-28` — `void SlotAllocator::Release(uint32_t offset) {
... (void)offset; }` with the comment "Naive path (M1–M9): no-op.  At M10
this returns `offset` to a free-list ... for now both paths behave the same."
design-heritage misread slot_allocator.h's *intended-design* trace (h:60-77)
as shipped.  This is load-bearing, not cosmetic: the no-op Release is the
root cause of the long-arith workspace overflow (known_bugs_test.cc:818-823)
and the reason the new compile gate caps `+`-chains at ~165 terms.  A design
doc transcribing design-heritage here would document an allocator that does
not exist and miss the capacity cliff it causes.

## 3. P1 — design-heritage cites a deleted drift gate as alive

design-heritage §4: "`runtime_catalogue_consistency_test` pins
exports↔catalogue↔imports coherence (wasm_exports.txt:14-16)".  abi-shared
§2.3 and runtime-kernel §2.11 report the test was **deleted** as tautological
(commit 511c3ec8).  Grep confirms: the only repo hits are the two stale
comments (`runtime/wasm_exports.txt:14`, `runtime/BUILD.bazel:12`); no BUILD
target exists.  design-heritage trusted the stale comment it should have been
auditing.  The new docs must state the actual guarantee — "both sides derive
from the same `// cel:codegen-export` markers" — plus abi-shared validation
5's once-per-toolchain-bump `wasm-dis` check as the residual manual audit.

## 4. P1 — design-heritage vs tools-examples on the WAT regression net: tools-examples is right

design-heritage §1.2/§4: "59 `.wat` files ... assembled + executed by
`wat_runner_test` per build".  tools-examples §2.2: 63 files, ~28 loaded, the
target is `manual`-tagged.  Verified: `ls doc/implementation-plan/rewrite/wat/
*.wat | wc -l` = 63; `tools/wat_runner/BUILD.bazel:13,35` tags both the
binary and the test `manual`, so nothing runs "per build" — only via
`run_full_suite.sh`'s query.  design-heritage's §4 claim that the WAT corpus
"is the only mechanism that caught the inline-md vs on-disk drift" overstates
a net that covers under half the corpus and runs only at milestone gates.
The new testing doc should carry tools-examples' numbers and validation 2
(re-assemble the 35 orphan WATs) — not design-heritage's.

## 5. P1 — three divergent 3VL absorption precedences; no note connects them

The boundary has THREE absorption implementations with TWO different
precedence rules, and operand routing decides which one an expression hits:

- **Runtime kernel** `absorb_3vl_binary` (runtime/cel_internal.h:83-102):
  ERROR(a) → ERROR(b) → UNKNOWN(a) → UNKNOWN(b) — **error dominates unknown**
  across operands.
- **cel_host trampolines** `AbsorbBinary` (eval/internal/
  cel_host_error.cc:134-145): first-operand-wins — **UNKNOWN(a) beats
  ERROR(b)**, pinned by `AbsorbBinaryTest.FirstOperandUnknownBeatsSecond
  OperandError` (cel_host_error_test.cc:251).
- **Custom-fn trampoline** `AbsorbUnknownOrErrorArg` (eval/engine.cc:603-624):
  scans all args, **error dominates unknown** (explicit `!have_error` guard
  on the unknown arm).

Per codegen-lowering §1 (kCall ladder), kHost-origin aggregate operands call
`cel_host_*` trampolines DIRECTLY while kDynamic-origin operands go through
the runtime dispatcher (which absorbs kernel-side first).  So
`unknownX OP errorY` propagates the **unknown** when the operands are
host-routed and the **error** when arena/dynamic-routed or inside an @host
fn.  eval-internal D5 flags only host-code-vs-surface-doc; eval-public §1.4
describes engine.cc's rule as if it were the only one; runtime-kernel §1.5
states no precedence at all.  The new eval doc must state one rule, cite the
oracle verdict (eval-internal validation 4 — run it for all three layers),
and fix the two losers.

## 6. P1 — payload.unk: the runtime's comment contradicts the host's writer; the merge dereferences attribute ids

Code-vs-code, confirmed both sides:

- runtime/cel_3vl.c:104-108 (comment in `cel_unknown_merge`): "An empty
  UnknownSet (payload.unk == 0) is a legal UNKNOWN — the host `get_field`
  trampoline mints UNKNOWNs **that way** for FULL attribute-pattern matches".
- eval/internal/cel_host.cc:1546-1551: the host get_field trampoline writes
  `unk.payload.unk = attribute_id` — **non-zero** for every real pattern
  match (ids are 1-based; abi-shared §1.1 sentinel discipline).  Likewise
  instance.cc PartialEval variable stamping and `ReturnUnknown`'s
  `kFunctionUnknownSentinel = 0xFFFFFFFF` (eval-public §1.4).

`cel_unknown_merge` (cel_3vl.c:94-130) treats non-zero `payload.unk` as a
2-word descriptor offset and dereferences it.  codegen-lowering §1 supplies
reachability: `_&&_`/`_||_` lower as eager calls to `cel_and`/`cel_or`, which
merge two UNKNOWN operands via `cel_unknown_merge` (cel_3vl.c:147,185,222).
So `a.x && b.y` under PartialEval with both attributes FULL-matched feeds two
attribute ids (or the 0xFFFFFFFF sentinel) into the merge as descriptor
offsets — garbage reads, possibly OOB.  eval-internal D3 + validation 2
already name this; runtime-kernel's note is silent on it despite owning
cel_3vl.c, and no note observes that the runtime comment's factual claim
about the host is simply false.  Treat as one P1 with the e2e probe
(`unknown_a && unknown_b`) as the settle.

## 7. P1 — tools-examples' "the code survives" claim is false for example 08's own error; the smoke test pins the bug

tools-examples §1.5: "a host-fn ErrorPayload's **code** survives the wasm
round-trip; the free-text message does not — decoded errors carry
`'runtime error code N'`."  Those two clauses contradict each other, and the
code settles it: example 08 sets `ErrorCode::kInvalidArgument`
(examples/08_function_errors_and_unknowns.cc:56-58); `DecodeCelError`'s
switch (eval/instance.cc:233-253) has **no kInvalidArgument arm** — exactly
eval-internal D4 — so the wire code 18 falls to `default:` and decodes as
`kHostAdapterError` + message `"runtime error code 18"`.  For this example
the code does NOT survive; the numeric is recoverable only by parsing the
synthesized message.  Known codes that ARE in the switch decode to
`ErrorCodeName(code)` (instance.cc:248), not "runtime error code N" — so the
example's output shape is itself the D4 symptom, and
examples_smoke_test.sh:41 (`error value: runtime error code 18`) enshrines
the bug as documented behavior.  Fixing D4 will break the smoke test — the
fix commit must update example 08's header comment (08:21,26-28), the smoke
assertion, and tools-examples §1.5 together.

## 8. P2 — custom-fn arity is enforced nowhere consistently across four components

Each note holds a fragment; assembled, the contract has a hole at every
seam: the `.celfn` Builder has **no params cap** and `num_args =
uint8_t(params.size()) + 1` wraps at 255 (function_library.cc:205, celfn
§1.2); OverloadTable registers any arity; codegen's `InstallOverloadImport`
silently returns false for `num_args` ∉ [1,5] (verified: compile.cc:276-296,
`default: return false`) so a 5-value-arg decl emits a module whose call
target was never imported (failure shape unprobed — celfn validation 4);
`Engine::AddFunction` rejects only arity 0 (eval-public §1.3); the catalogue
arity-bounds test covers builtins only (abi-shared §4).  An embedder can
register a 6-arg @host fn engine-side that the compiler mis-emits.  The new
custom-fn doc must place the cap at ONE layer (Builder, per celfn validation
6) and the others CHECK against it.

## 9. P2 — `mem_size_bytes` in kDynamic plausibly breaks Plan past 256 KiB; compile side and engine side were never reconciled

compiler-toplevel §1.3: kDynamic stamps the `cel.memory` import's initial
page count = `PagesForBytes(mem_size_bytes)` (compile.cc:36-39, max 1024).
eval-public §2.3: the engine does NOT create that memory — it defines
`cel.memory` on the linker from the **runtime's own exported** memory, which
is `(memory 4 1024 shared)` (memory-layout-design.md:34).  Wasm import
matching requires the provided memory's size ≥ the declared min, so any
`mem_size_bytes` > 256 KiB (5+ pages) should fail instantiation at Plan
unless the memory already grew — i.e. the knob's only dynamic-mode effect
beyond 4 pages is to break Plan, while in the default kStatic it is a
verified no-op (compiler-toplevel §1.3) and the arena it claims to size lives
in dlmalloc (expr_lower.h:177-181).  The CLI still advertises it
(`--mem_size_bytes`, tools-examples §1.1, cel.cc:79-82).  No note connects
the two sides.  Probe: kDynamic + `mem_size_bytes = 1 MiB` + Plan; then
either fix the import stamping or delete the knob end-to-end (public option,
CLI flag, vestigial LoweringOptions field).

## 10. P2 — the lifecycle thread-safety story stops at Program; Compiler's contract is undocumented and the internal caveat's premise is false

eval-public §1.7 gives the eval half a complete, tested contract (Engine
setup single-threaded, Plan concurrent, Instance thread-owned, Instance
outlives Engine).  The compile half has nothing: no "thread" anywhere in
compiler/compiler.h or compiler/program.h (grep), while Binaryen's optimize
knobs are process-global (module.h:155-165) — and that caveat's own
mitigation claim, "serialised by `celwasm::Compiler` ownership", is false:
Compiler is copyable pure data and one-Compiler-many-Programs is the
documented pattern (compiler-toplevel §1.1), so two threads compiling with
`optimize_level > 0` through two distinct Compilers race on
`BinaryenSetOptimizeLevel`.  The new architecture doc's lifecycle section
must state the Compiler-side rule (serialize Compile calls process-wide when
optimize_level > 0, or move to `BinaryenModuleRunPasses` per module.h's own
suggestion) so the end-to-end story has no undocumented segment.

## 11. Disposition notes for the consolidation pass

- Findings 2, 3, 4 are corrections **to design-heritage**; its supersession
  map is otherwise the right skeleton, but every row it sourced from a header
  comment rather than a `.cc` body needs the codegen-memory treatment
  (read the implementation, then the comment).
- Finding 1's re-anchoring should happen before the discrepancy register
  merges, or the register will double-count fixed-in-tree items as open P0s.
- The 3VL findings (5, 6) plus eval-internal D1/D2/D4 together define the
  "error & unknown wire contract" section the new ABI doc needs: one
  precedence rule, one payload.unk contract (descriptor vs attribute-id —
  pick one), one error wire shape (bare code vs message-carrying), each with
  its oracle case.
