# M13 post-Slice-C.2 review — 2026-05-21

## Verdict (one paragraph)

**Mixed.**  Slice C.2 lands the compiler-side embedder surface for
M13: `Compiler::Builder::AddLibrary(FunctionLibrary)` and the
convenience `AddFunction(string_view)` parser-driven overload, plus
the `function_libraries_` accumulator, the deferred-status pattern
on the Builder, the cross-library overload-id uniqueness check at
`Build()`, and 7 new tests covering the matrix.  The shape is
clean and matches the existing Builder ergonomics
(`DeclareVariable` returns lvalue ref, `Build()` is rvalue-consumed).
The dominant concern is by design and must not be missed: the
shipped Compiler **stores** `function_libraries_` but
`Compiler::Compile` (`compiler.cc:174-188`) never reads it.  The
underlying `celwasm::CompileOptions` plumbing is untouched, so the
declared custom fns are invisible to the checker and to the
`OverloadTable`.  Any source that references a registered custom fn
will fail at cel-cpp's check stage with "undeclared function" —
exactly as if the library had never been registered.  This is the
intentional C.3 boundary, but neither the code, nor the doc, nor
the test suite signals it loudly enough.  Top three to look at
first:

  1. **`function_libraries_` is stored but unwired in `Compile`**
     (`compiler.cc:174-188`) — P1, by design but undocumented.
  2. **Slice plan doc §12 still says Slice C is one slice**
     (`m13-custom-fns.md:1875-1879`) — P2 doc drift; C.1 already
     surfaced this and C.2 didn't reconcile.
  3. **`AddFunction` parse errors are not localised**
     (`compiler.cc:120-135`) — the deferred-status message wraps
     `ParseCelfnSource`'s status but loses which `AddFunction`
     call raised it.  P2 ergonomics.

## Architectural drift

### A1. `function_libraries_` is stored but `Compile` never consumes it — P1, by design

`compiler.cc:174-188` (`Compiler::Compile`) translates only
`declared_variables_` into `celwasm::CheckOptions::variable_specs`
before forwarding to `celwasm::Compile(source, inner)`.  The
`function_libraries_` accumulator is invisible to the pipeline:

```cpp
// compiler.cc:174-188
absl::StatusOr<Program> Compiler::Compile(absl::string_view source,
                                          const CompilerOptions& opts) const {
  celwasm::CompileOptions inner;
  inner.mem_size_bytes = opts.mem_size_bytes;
  inner.check.container = opts.container;
  inner.optimize_level = opts.optimize_level;
  inner.check.variable_specs.reserve(declared_variables_.size());
  for (const auto& decl : declared_variables_) {
    inner.check.variable_specs.push_back(
        absl::StrCat(decl.name, ":", CelTypeToSpec(decl.type)));
  }
  auto artifact_or = celwasm::Compile(source, inner);
  // …
}
```

A source like `"name.is_number()"` registered through
`AddFunction("bool @host.is_number(this string s);")` will fail
inside `celwasm::Compile` at the checker stage — the call-site
gets "undeclared reference to 'is_number'" or similar.  None of
the new tests exercise `Compiler::Compile` against a source that
references a custom fn, so the gap is silent in green CI.

Disposition: this is **the explicit C.3 boundary** — Slice C.2
shipped the Builder surface and storage, C.3 will wire
`OverloadTableBuilder::RegisterCustom` per decl + propagate the
declarations into the cel-cpp `TypeCheckerBuilder` so call-site
resolution succeeds.  But:

  - The header docblock on `function_libraries_`
    (`compiler.h:164-171`) says only "in a follow-on commit"
    without naming the slice or surface — a reader can't tell
    whether this is an in-flight wiring or a deferred design.
    Tighten to:

    > Storage only — Slice C.3 wires these into the underlying
    > `celwasm::CompileOptions` (one
    > `OverloadTableBuilder::RegisterCustom` call per decl, plus
    > a `cel::TypeCheckerBuilder::AddFunction` for call-site
    > resolution).  Until C.3 lands, sources that reference a
    > custom-fn name fail at the checker.

  - At least one **negative** test should be added that pins
    this boundary explicitly — call it
    `CompileReferencingRegisteredCustomFnFailsUntilC3` — so a
    future fix to C.3 surfaces as a test flip, not as silent
    behavioural drift.  **P1, ~10 min.**

  - `Compiler::Compile` itself should `ABSL_CHECK(false) <<
    "function_libraries_ wiring is a stub until M13 Slice C.3"`
    **iff** `!function_libraries_.empty()` AND the source
    actually invokes a custom fn — but detecting the latter
    requires AST inspection, so the cheaper option is to
    document the gap loudly in the header docblock and rely on
    the explicit negative test above.  Per CLAUDE.md "unimplemented
    features" rule: stubs MUST crash at the reachable edge.  But
    here the "reachable edge" is "user wrote a CEL expression
    using the custom fn name" — which crashes via the checker
    today with "undeclared function".  That's effectively the
    tripwire, just expressed through cel-cpp's diagnostic rather
    than our own.  Acceptable, but the negative test pins the
    invariant explicitly.

### A2. `m13-custom-fns.md` §12 still describes Slice C as monolithic — P2 (carried over from C.1)

C.1's review already flagged this (D1 / A2 in
`2026-05-21-post-slice-c1.md`).  C.2 didn't reconcile.
`m13-custom-fns.md:1875-1879` still says:

> Slice C — host backend end-to-end: CLI flag,
> `Compiler::Builder::AddLibrary`, `OverloadTableBuilder::RegisterCustom`,
> `cel_fn.*` trampolines in the host adapter,
> `RuntimeBindings::AddFunction` integration, host e2e tests …

As shipped: C.1 = `Engine::Add{Module,Function}` + trampoline + state.
C.2 = `Compiler::Builder::Add{Library,Function}` + storage.
C.3 (pending) = OverloadTable + checker wiring + CLI flag + e2e.
This split is documented in the C.1 review and across multiple
inline comments (`compiler.h:164-171`, the inline section header
`compiler_test.cc:163` "M13 Slice C.2 — Compiler::Builder::AddLibrary
/ AddFunction") but nowhere in the slice plan itself.  **P2,
~15 min** — rewrite §12 Slice C bullet as three sub-bullets.

### A3. `Compiler::Builder::AddFunction` and `Engine::AddFunction` share a name but mean very different things

C.1 shipped `Engine::AddFunction(overload_id, num_args, HostCallback)`
— registering a raw runtime impl by overload-id.  C.2 shipped
`Compiler::Builder::AddFunction(absl::string_view celfn_source)` —
parsing a `.celfn` source string into a Library.

These do orthogonal things on orthogonal objects, but the name
collision is real and reads as confusing in mixed call sites:

```cpp
auto c = std::move(b.AddFunction("string @host.upper(this string s);"))
             .Build();
//             ^ parses IDL source

ASSERT_OK(engine.AddFunction("upper_string", 1, upper_impl));
//                ^ binds a runtime impl by overload-id
```

This is mostly unavoidable — both methods describe "adding a
function" in some sense.  But the design doc's §6 worked example
(`m13-custom-fns.md:235`) already used the Engine shape under the
same name, so newcomers reading the doc will see two
`AddFunction`s and have to disambiguate by receiver type.
Consider naming the Compiler-side convenience
`AddFunctionDecl(string_view celfn_source)` or
`AddFunctionsFromSource(string_view)` to disambiguate.  Not
ships-blocking — the current names match the design doc and the
receivers are typed — but worth a beat of thought before C.3
freezes the shape.  **P2, naming-bikeshed, ~5 min if changed.**

### A4. `cel::` re-export at the bottom of `compiler.h` does not include `celwasm::FunctionLibrary`

`compiler.h:253-257`:

```cpp
namespace cel {
using ::celwasm::api::Compiler;
using ::celwasm::api::CompilerOptions;
using ::celwasm::api::VariableDeclaration;
}  // namespace cel
```

`FunctionLibrary` is declared in `namespace celwasm` (not
`celwasm::api`) and is intentionally not re-exported.  This means
embedder code reading the docblock at the top of `compiler.h`
("Builder::AddLibrary takes a `celwasm::FunctionLibrary`") cannot
write `cel::FunctionLibrary` and must reach into `celwasm::`
directly.  The cross-namespace shape is awkward — most other
public types travel through `cel::` aliasing.

Options: (a) move `FunctionLibrary` into `celwasm::api` (the canonical
public namespace per cel-host-surface.md §1); (b) add a
`using ::celwasm::FunctionLibrary;` alias to the `cel::` re-export
block here.  (a) is the right answer per the §1 convention; (b)
is the lower-touch fix if `celwasm::FunctionLibrary` already has
in-tree callers we'd have to migrate.  **P2, ~10 min for (b),
~30 min for (a).**

## Tech-debt inventory

### T1. `deferred_status_` only stores the first failure; subsequent failures are silently dropped — P2, by design but worth a comment

`compiler.cc:120-135`:

```cpp
Compiler::Builder& Compiler::Builder::AddFunction(
    absl::string_view celfn_source) {
  auto lib_or = celwasm::ParseCelfnSource(celfn_source);
  if (!lib_or.ok()) {
    // Defer to Build() — earlier failure wins (don't overwrite).
    if (deferred_status_.ok()) {
      deferred_status_ = absl::Status(
          lib_or.status().code(),
          absl::StrCat("Compiler::Builder::AddFunction: ",
                       lib_or.status().message()));
    }
    return *this;
  }
  function_libraries_.push_back(*std::move(lib_or));
  return *this;
}
```

The "first-failure-wins" policy is defensible — a typical chain
sees one bad source and four good ones; reporting the FIRST is
the useful signal.  But:

  - The second/third failures are not even logged.  Embedders
    fixing the first failure and re-running will then see the
    second.  If the chain is long, this is N round-trips
    instead of 1.
  - The error message says "Compiler::Builder::AddFunction" but
    does NOT identify WHICH call (no index, no first-N-chars
    snippet of the source).  Compare to `DeclareVariable`'s
    error which names the offending variable.

Two cheap improvements: (a) embed the first 40 chars of
`celfn_source` in the error message (`"AddFunction(source=\"%.40s…\")"`);
(b) consider accumulating ALL parse failures into a joined
status at `Build()` time (e.g. `absl::StrJoin` of all messages),
trading verbosity for completeness.

Today's tests don't pin this — `FirstParseErrorWins`
(`compiler_test.cc:216-224`) verifies the policy but not the
diagnostic.  **P2, ~15 min.**

### T2. `AddFunction(string)` cannot be unit-tested without `ParseCelfnSource` — P2

The convenience method couples Builder to the celfn parser.
Today's tests pass real `.celfn` strings through.  If a future
refactor changes the parser's diagnostic format, every
`AddFunction` test that asserts on `.status().message()`
substrings (e.g. `compiler_test.cc:212-213` "HasSubstr(\"AddFunction\")")
will need re-asserting.  Not a fault; just a coupling worth
acknowledging.  Mitigation: keep the assert narrow (we only
check for the "AddFunction" prefix) — the substring check above
is correctly narrow.  **No action.**

### T3. `Compiler::Builder` accumulator vectors are not pre-reserved — P2

`compiler.cc:114-118` and `120-135` push back unconditionally.
`Build()` does `seen.reserve(declared_variables_.size())` but
not the analogous `seen_overload_ids.reserve(total_decls)` —
which would require summing `lib.decls().size()` first.  Hot
path?  No — Builder construction is one-shot.  Cosmetic.

### T4. `Build()` constructs the cross-library `seen_overload_ids` set even when only one library is present — P2

`compiler.cc:157-166`: the cross-library dup check runs
unconditionally.  When `function_libraries_.size() <= 1`, no
cross-library collision is possible (within-library is already
caught by `FunctionLibrary::Builder::Build`).  Could short-circuit:

```cpp
if (function_libraries_.size() > 1) {
  absl::flat_hash_set<std::string> seen_overload_ids;
  // … cross-library check
}
```

Hot path?  Not by any reasonable measure.  Cosmetic, and the
flat_hash_set construction is the dominant cost there anyway.
**No action**, but the comment could note "no-op for ≤1
library".

### T5. `Compiler::function_libraries()` returns `Span<const FunctionLibrary>` — copy semantics implicit — P2

`compiler.h:155-157`:

```cpp
absl::Span<const celwasm::FunctionLibrary> function_libraries() const {
  return function_libraries_;
}
```

This works because `function_libraries_` is `std::vector` and
the span deduces from contiguous storage.  Subtle: if a future
refactor changes the field to a `std::list` or
`absl::InlinedVector` with non-trivial layout, the span breaks.
Standard C++; flagging for completeness.  **No action.**

### T6. `Compiler` is copyable but `FunctionLibrary` copies are not cheap — P2

`compiler.h:119-122` declares Compiler copy-constructible.  Each
copy clones every `FunctionLibrary`, which itself owns
`std::vector<CelfnDecl>` (each decl carries `std::string`
fn_name + module_name + overload_id + body + params).  For a
library with N decls this is O(N) string copies per Compiler
copy.  C.1's review's T5 noted the wasm_functype ownership
pattern; analogous note here: Compiler copies are not free, and
the type's copyability invites unnecessary copies.

The existing `Compiler(const Compiler&) = default` predates C.2
and matches the "pure data" promise.  Keeping copy semantics is
correct (embedders may want to derive variants of a Compiler).
Just worth knowing.  **No action.**

## Coverage gaps

### C1. No negative test pinning the C.3 boundary — P1

Per A1: a source like `"name.is_number()"` registered through
`AddFunction("bool @host.is_number(this string s);")` should
fail at compile.  Today no test asserts this — the new tests
all stop at `Build()`.  A `TEST` named
`CompileReferencingRegisteredCustomFnFailsUntilSliceC3` (with a
descriptive comment citing the C.3 wiring gap) would pin the
invariant.  When C.3 lands, this test flips OK; future readers
get a binary "is C.3 wired?" signal.  **P1, ~15 min.**

### C2. No test that mixing `AddLibrary` + `AddFunction` accumulates correctly — P2

`MultipleLibrariesWithDistinctOverloadsOk`
(`compiler_test.cc:246-265`) tests two `AddLibrary` calls.
`SingleHostDeclString` (`compiler_test.cc:197-205`) tests one
`AddFunction`.  Nothing tests the mixed case:

```cpp
auto b = Compiler::NewBuilder();
b.AddLibrary(lib_built_programmatically);
b.AddFunction("string @host.upper(this string s);");
auto c = std::move(b).Build();
// ASSERT both libraries present
```

This is the realistic embedder shape (one library from a `.celfn`
file, plus a couple of programmatic decls).  **P2, ~10 min.**

### C3. No test exercising `Build()` with both a deferred parse error AND a duplicate variable name — P2

`Build()`'s order of operations is:

  1. Surface `deferred_status_` if non-OK (line 138).
  2. Validate + dedup variable declarations (lines 140-150).
  3. Cross-library overload-id check (lines 157-166).

Today's tests cover (1) alone and (2) alone.  Combining them
(`AddFunction("garbage").DeclareVariable("x").DeclareVariable("x")`)
verifies that the parse error surfaces FIRST — which is correct
because a malformed library is a more fundamental setup error
than a duplicate name.  Pin it.  **P2, ~10 min.**

### C4. No test that `AddLibrary(empty).AddLibrary(empty)` succeeds — P2

`EmptyLibraryBuildsOk` (`compiler_test.cc:165-174`) covers one
empty library.  Two empty libraries should also succeed (no
overload-ids → no collisions possible) but isn't tested.  Edge
case, low value, but adding 4 lines is cheap.  **P2, ~5 min.**

### C5. No test that the order of `function_libraries()` matches insertion order — P2

The `compiler.h:155-157` accessor promises a span — order is
implied by `vector` but not asserted.  Embedders who care
("registration order should be lookup order in C.3") have no
test pinning this.  **P2, ~5 min.**

### C6. `AddFunction` with multi-decl source not tested — P2

`SingleHostDeclString` (`compiler_test.cc:197-205`) tests a
single-decl source.  `m13-custom-fns.md` and the header docblock
(`compiler.h:215-219`) explicitly say "multi-decl strings work
too" — but no test exercises it:

```cpp
b.AddFunction(R"(
  string @host.upper(this string s);
  string @host.lower(this string s);
)");
// ASSERT both decls landed under a single library
```

This is the canonical "load from a multi-line `.celfn` source"
shape.  **P2, ~10 min.**

## Doc drift

### D1. `m13-custom-fns.md` §12 Slice plan still monolithic (carried from C.1) — P2

See A2.  **~15 min to rewrite as C.1 / C.2 / C.3 / C.4 sub-bullets.**

### D2. `m13-custom-fns.md` §6 worked example still uses 2-arg `engine.AddFunction(...)` (carried from C.1) — P2

Line 235: `ASSERT_OK(engine.AddFunction("upper_string", upper_impl));`.
C.1 shipped 3-arg form; C.2 doesn't touch this line.  **~5 min.**

### D3. `function_libraries_` docblock vague about the C.3 wiring — P1

Per A1.  `compiler.h:164-171` says "in a follow-on commit"
without naming Slice C.3 or naming the surfaces that will be
wired (OverloadTableBuilder::RegisterCustom +
TypeCheckerBuilder::AddFunction).  Tighten to name the slice +
surfaces.  **P1, ~5 min.**

### D4. `cel-host-surface.md` not updated for the new public Builder methods (parallels C.1's D4) — P2

C.1's D4 flagged that `Engine::AddModule` / `Engine::AddFunction`
weren't reflected in `cel-host-surface.md`.  C.2 adds
`Compiler::Builder::AddLibrary` / `AddFunction` to the public
surface — same gap.  Resolve together with C.1's D4.  **P2,
~10 min.**

### D5. No comment in `compiler.cc:174-188` flagging the C.3 wiring gap — P2

The `Compile` method silently ignores `function_libraries_`.  A
one-line `// TODO(M13 Slice C.3): wire function_libraries_ into
celwasm::CompileOptions (OverloadTable + TypeCheckerBuilder).`
comment at the top of the method body is warranted.  Per CLAUDE.md
"no `// TODO(MN): ...` callouts unprompted" — but this is the
already-prompted active slice, and the unimplemented-features
rule says stubs MUST be visible to future readers.  Pick one:
either the docblock-only documentation per D3, or the inline
TODO here.  **P2.**

## Summary tracking

P0 (ships-breaking, before C.3):
  - none.

P1 (must-fix-before-Slice-C.3):
  - A1 / D3 / C1 — the C.3 wiring gap.  Tighten the docblock on
    `function_libraries_` to name the slice + surfaces; add a
    negative test `CompileReferencingRegisteredCustomFnFailsUntilSliceC3`
    that pins the boundary so a future fix flips the test.

P2 (cleanup-when-touched):
  - A2 / D1 (Slice plan §12 still monolithic, carried from C.1)
  - A3 (AddFunction name collision Compiler-vs-Engine)
  - A4 (cel:: namespace re-export missing FunctionLibrary)
  - T1 (deferred_status diagnostic could carry source snippet),
    T3 (no overload-id reservation), T4 (cross-lib check
    unconditional), T5 (span vs vector), T6 (Compiler copy cost)
  - C2 (mixed AddLibrary+AddFunction), C3 (parse-error + dup-
    name interaction), C4 (two empty libs), C5 (order is
    insertion order), C6 (multi-decl source)
  - D2 / D4 / D5 (doc drift carried + new)

C.2 itself is a small, focused slice — surface + accumulator +
cross-library validation + tests.  All P2 items are legitimate
cleanup-when-touched.  The only "real" finding is the C.3 boundary
(A1): without the negative test, a regression in C.3's wiring
would land silently with all 7 new tests still green.

**Recommendation**: in the FIRST commit of Slice C.3, (a) land the
P1 fixes for A1/D3/C1, (b) reconcile §12 (D1), (c) move
`FunctionLibrary` into `celwasm::api` (A4) OR add the `cel::`
alias.  Everything else can travel with C.3's natural file
touches.

— review carried out by Claude Opus 4.7 per the periodic
   code-review rule in CLAUDE.md.
