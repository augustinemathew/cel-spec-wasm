# frontend-ir — design notes (undefined)

Scope: `compiler/frontend/` (parse_and_check, status_tags) + `compiler/ir/`
(typed_ast, annotations).  Verified against code + tests on branch
`m28-configurable-linking`, 2026-06-10.  All line numbers from that tree.

## 1. Verified architecture

### 1.1 Component responsibility and data flow

`ParseAndCheck(expression, CheckOptions) -> absl::StatusOr<TypedAst>` is the
single frontend entry point (parse_and_check.h:69-70).  Pipeline order, fixed
in `ParseAndCheck` (parse_and_check.cc:1464-1506):

1. `LoadDescriptorPool` (cc:160-195) — `std::monostate` schema → process-wide
   `generated_pool()` (cc:163-165); `SchemaProtoSource` → in-process
   `google::protobuf::compiler::Parser` parse (cc:94-120, file name set to the
   path, cc:118); `SchemaDescriptorSet` → binary `FileDescriptorSet` load
   (cc:134-158, duplicate file ⇒ InvalidArgument cc:152-154).  Non-default
   schemas build a `MergedDescriptorDatabase(schema_db, generated_db)` pool so
   user schemas overlay the generated pool (cc:185-193).
2. `cel::CreateTypeCheckerBuilder(pool)` (cc:1469).
3. `ConfigureCheckerBuilder` (cc:1055-1085):
   - `AddCheckerLibraries` (cc:999-1053) registers, unconditionally:
     standard library, ComprehensionsV2 checker lib (needed for
     `cel.@mapInsert`/`cel.@mapInsertOverwrite`, cc:1003-1013), strings ext,
     encoders ext, math ext, `OptionalCheckerLibrary` (cc:1052).
   - `RegisterNetworkExtDecls` (cc:988-993) — hand-built decls (cel-cpp ships
     no network lib): `ip`/`cidr`/`isIP`/`ip.isCanonical` globals; `net.IP`
     receiver methods; `net.CIDR` `containsIP`/`containsCIDR`/`ip`/`masked`/
     `prefixLength`; `string()` overloads via `MergeFunction` (cc:839-849);
     bare `net.IP`/`net.CIDR` type-literal variables of `TypeType(inner)`
     (cc:966-972).  `net.IP`/`net.CIDR` are zero-parameter `OpaqueType`s
     (cc:830-835).  Always registered — not gated on options.
   - container (cc:1067-1069); variable specs (cc:1070-1077); custom-fn decls
     from `CheckOptions::function_libraries` via
     `RegisterCustomFunctionsOnChecker` (cc:798-823) — decls grouped by
     fn_name, one `FunctionDecl` per name with one `AddOverload` per
     `CelfnDecl` (cross-library overload-id collision filtering is the
     CALLER's contract — `Compiler::Builder::Build`, parse_and_check.h:58-61).
4. `RunTypeCheck` (cc:1159-1192): builds the macro registry (standard +
   bindings_ext `cel.bind` + comprehensions_v2 + math macros, cc:1121-1139)
   with a single shared `ParserOptions` source of truth
   (`DefaultParserOptions`, cc:1094-1111: `enable_optional_syntax = true`,
   `max_recursion_depth = 16384`); parses; `checker.Check`; on
   `!result->IsValid()` returns InvalidArgument with
   `kUndeclaredReferencesUrl` payload = deduped newline-joined root namespace
   components of every `"undeclared reference to '<sym>'"` issue
   (cc:1173-1189, extractor cc:1147-1157).
5. `InlineConstantReferences` (cc:1266-1286) — in-place AST rewrite: every
   kIdent whose `reference_map` entry `has_value()` (enum-name constants like
   `TestAllTypes.NestedEnum.BAR`) becomes a kConstant carrying that value.
   Re-implements cel-cpp's reference_resolver walk without pulling in
   FlatExprBuilder (cc:1204-1207).  Idempotent.
6. `InlineTypeIdentifierReferences` (cc:1452-1460) — MUST run after step 5
   (cc:1416-1418, m9 §7 R2): every kIdent whose Reference is value-less AND
   whose `type_map` entry is `TypeType(inner)` becomes a kConstant with
   `string_value = <spec type-name>` (`MaybeRewriteTypeIdent`, cc:1427-1450).
   Name resolution: `SpecTypeName` (cc:1366-1394) — primitives → `"int"` etc.
   (cc:1300-1323), wrappers → `"google.protobuf.Int64Value"` etc.
   (cc:1325-1347), well-knowns (cc:1349-1364), `null_type`, message FQN,
   `"list"`, `"map"`, `"type"`, bare type-param → `"type"` (cc:1375-1381),
   abstract → declared name (`net.IP`, `optional_type`; cc:1387-1389).
   Inner kinds `function`/`error`/`dyn` → nullopt = leave the kIdent alone
   (cc:1390-1393).  Unspecified primitive/well-known enum values
   `ABSL_CHECK(false)` (cc:1314-1319, 1339-1343, 1357-1359).
7. `RejectDyn` (cc:644-671) — the static-subset gate (§1.2 below).  Failure
   is InvalidArgument tagged `kStaticSubsetViolationUrl` with body =
   comma-joined offending expr ids (cc:662-670).
8. `PopulateAnnotations(ast, pool, annotations)` (typed_ast.cc:176-186) —
   seeds `repr` + `field_number` (§1.3).
9. Returns `TypedAst{ast, annotations, variables}` (cc:1504-1505).

The rewrites in 5/6 run BEFORE RejectDyn and PopulateAnnotations so all later
passes see kConstant nodes uniformly (cc:1485-1496); the `type_map` entry of a
rewritten node is unchanged (still `TypeType(inner)`), so `ReprOf` stamps
`Repr::kType` on the new constant (cc:1441-1445).

### 1.2 The static-subset gate (RejectDyn) — exact admit/reject surface

Precondition: checked AST (`FailedPrecondition` otherwise, cc:645-648).
Per-node check (`CheckSubsetNode`, cc:609-642): a node violates if its
`type_map` entry is missing (`"no type_map entry"` counted as dyn,
cc:633-635) or `UnacceptableLabel` returns non-null.

`UnacceptableLabel` (cc:352-379) rejects: `dyn`, `error`, `FunctionTypeSpec`,
`ParamTypeSpec`, `UnsetTypeSpec`, and RECURSIVELY any of those inside
`list<...>` element type, `map<K,V>` key or value type, or any
`abstract<...>` parameter (cc:363-377).  So implicit dyn — bare `[]`,
heterogeneous `[1,"two"]`, `optional<dyn>` — is rejected, not just explicit
`dyn(...)` calls.  Pinned by e2e/m4_test.cc:360-384
(`BareEmptyListLiteralRejected`, `HeterogeneousListRejected`).
Not recursed: the inner of a `TypeType` (`has_type()` is not descended).

Carve-outs that ADMIT otherwise-dyn shapes, in dispatch order
(CheckSubsetNode cc:611-630):

1. **`dyn(x)` passthrough** (`IsDynPassthroughCall`, cc:592-607): global
   1-arg `dyn` call admits iff arg is itself a 1-arg `dyn` call (recursive
   collapse) or arg's checker type `has_primitive() || has_null() ||
   has_type()` (`ArgIsAdmissibleScalar`, cc:551-562 — the `has_type()` arm is
   the M9 `dyn(type-value)` extension).  Recursion continues into the arg
   only; the call node's own `dyn` type is never inspected.  Aggregate /
   message / dyn-typed args fall through and reject
   (parse_and_check_test.cc:584-618).
2. **Select-through-Any** (`IsSelectThroughAny`, cc:575-590): a kSelect typed
   `dyn` admits if its operand types as `google.protobuf.Any` (well-known or
   message-FQN spelling) — transitively, so `msg.single_any.x.y` admits
   (cc:573-574).  Runtime unwrap is `ProtoBacking::ReadField`'s job
   (cc:566-571).
3. **`math.@min` / `math.@max`** (`IsMathMinMaxCall`, cc:415-418): global
   calls with those names (the `math.greatest`/`least` macro expansions)
   admit their checker-assigned `dyn` result, and `CheckSubsetCall` skips
   any kListExpr arg (the macro-built mixed-numeric list literal, cc:427-432).
4. **`<target>.format([list-literal])`** (`IsFormatCallWithListLiteralArgs`,
   cc:400-405): receiver-style `format` with exactly one arg that is a
   kListExpr — the entire args subtree is admitted without recursion
   (cc:423-426); the target is still checked (cc:422).  Only the
   literal-list shape; `list<dyn>`-typed variables stay rejected (cc:393-399).
5. **`cel.bind` shape** (`IsCelBindShape`, cc:474-482): a comprehension whose
   iter_range is an empty list literal AND loop_condition is `kConst(false)`
   skips checking iter_range (types `list(dyn)`) and loop_step (unreachable
   `kIdent(accu_var)`); accu_init / loop_condition / result are still checked
   (`CheckSubsetComprehension`, cc:484-503).  Shape-matched, not
   macro-provenance-matched.
6. **id == 0 nodes** skip the type lookup but children are still recursed
   (cc:631-640).

All violations are accumulated (not first-failure) and reported as
`expr id=N is <label> (<formatted type>)` lines (cc:653-663).

### 1.3 Annotation stamping done by THIS component

`PopulateAnnotations` (typed_ast.cc:176-186) writes exactly two fields:

- **`repr`** — one entry per `type_map` id, `ReprOf(TypeSpec)`
  (typed_ast.cc:54-71).  Mapping: primitive/wrapper → scalar Repr (wrappers
  reuse the wrapped primitive's repr — nullness tracked elsewhere,
  typed_ast_test.cc:49-52); Timestamp/Duration; `kAny → kMessage`; null;
  list; map; message; `has_type() → kType`; abstract named exactly
  `"optional_type"` → `kOptional` (typed_ast.cc:63-66); everything else
  (dyn, error, other abstracts, function, param, unset) → `kUnknown` so the
  validator can flag it (typed_ast.cc:67-70).  Note RejectDyn runs BEFORE
  PopulateAnnotations, so in the success path no node-level kUnknown from a
  dyn type survives except via the carve-outs (the `dyn(...)` call node, the
  Any-select node, math minmax node, format args — those nodes' reprs are
  kUnknown and downstream ResolvePass forwarding/codegen handles them).
- **`field_number`** — `FieldNumberVisitor` (typed_ast.cc:139-172) walks every
  SelectExpr; if operand type `has_message_type()` and both
  `FindMessageTypeByName(fqn)` and `FindFieldByName(field)` resolve, stamps
  `field->number()`.  Otherwise (map operand, unknown message/field, null
  pool) leaves 0; codegen treats 0 as "no info" (typed_ast.cc:137-138,
  typed_ast.h:96-103).  Needed because cel-cpp's `reference_map` records
  entries only for Ident/Call/Struct, not field numbers (typed_ast.cc:134-136).

Every OTHER `NodeAnnotation` field (`overload_id`, `local_index`, `scope_id`,
`attribute_id`, `message_type_id`, `storage`, `map_origin`, `list_origin`,
`comp_*`, `select_key_rodata_offset`) is declared in annotations.h:81-137 but
populated downstream by `compiler/codegen` ResolvePass/LayoutPass (per the
field comments, e.g. annotations.h:60-62, 92-99, 107-111, 129-136).  The IR
headers are the schema; the frontend stamps only repr + field_number.

### 1.4 Key types and contracts

- **`Repr`** (annotations.h:17-39): 16 enumerators incl. `kEnum` and
  `kOptional`.  `kOptional` is stamped when the type is the cel-cpp
  AbstractType named `"optional_type"` (annotations.h:33-38).
  `ReprName`/`StorageKindName` return `"?"` for out-of-range (open fallback,
  annotations.cc:43, 57 — pinned by annotations_test.cc:32-44);
  `OriginName` `ABSL_CHECK(false)`s instead (annotations.cc:69-71).
- **`StorageKind`/`Storage`** (annotations.h:46-58): kNone / kStaticRodata /
  kWorkspaceSlot / kLocal; payload is rodata offset | slot offset | local idx.
- **`Origin`** (annotations.h:71-77): kDynamic (default) / kArena / kHost —
  map/list backing dispatch, populated by ResolvePass visitors.
- **`overload_id` is `absl::string_view` into cel-cpp's reference_map
  storage; lifetime tied to the TypedAst** (annotations.h:86-89).  This is a
  deliberate plan delta from the original interned-uint32 design
  (design.md "Plan-vs-execution delta", ~line 463-481).
- **`WasmAnnotations`** (annotations.h:140-158): `flat_hash_map<int64_t,
  NodeAnnotation>`; `operator[]` default-creates, `Find` returns nullptr.
  Negative/INT64_MIN ids addressable (annotations_test.cc:96-102).
- **`TypedAst`** (typed_ast.h:47-90): move-only owned bundle of
  `unique_ptr<cel::Ast>` + `WasmAnnotations` + `vector<Variable>`.
  Deliberately NOT a heavier IR — downstream passes read `type_map`,
  `reference_map`, and annotations simultaneously (typed_ast.h:38-46).
- **`Variable{name, repr}`** (typed_ast.h:33-36): captured in
  `variable_specs` order; codegen binds entry i to eval-fn parameter i.  Repr
  stored here (via `ReprOf(cel::Type)` at spec-parse time,
  parse_and_check.cc:340) rather than re-derived from type_map because
  unreferenced variables never appear in type_map (typed_ast.h:29-32).
- **Variable-spec grammar** (`name:Type`, parse_and_check.h:33-38; parser
  cc:202-342): primitives + `timestamp`/`duration`/`any` + `dyn` + `type`
  (cc:213, 220) + `list<T>`/`map<K,V>` recursive + message FQN via
  `pool->FindMessageTypeByName`.  Whitespace tolerated; trailing garbage
  rejected (cc:335-339, test cc:176-184, 468-475).
- **`status_tags.h`** (h:29-32): `kStaticSubsetViolationUrl` body =
  comma-joined expr ids; `kUndeclaredReferencesUrl` body = newline-joined
  deduped root symbols.  Consumer: conformance runner classifies
  static-subset → `kStaticSubset`, undeclared-refs → `kExtensionUnimpl` iff
  roots ⊆ ext-lib list (conformance/runner.cc:690-706).
- **Visibility**: both packages default `//:internal`
  (frontend/BUILD.bazel:3, ir/BUILD.bazel:3); neither is public API.
- **`CelfnType → cel::Type`** (cc:677-772): scalar table (kType/kOptional
  scalar arm returns nullopt, cc:697-711); list/map structural; proto via
  pool; the kType/kOptional structural mapping is an explicit
  `ABSL_CHECK(false) ... stub until m24` (cc:767-771).

## 2. Doc-vs-code discrepancies

- **P1 — design.md claims the implicit-dyn RejectDyn gap is still open.**
  design.md:2968-2976 ("our static-subset gate only catches explicit
  `dyn(...)` calls, not implicit dyn from these list inferences") and the
  unticked checklist row design.md:4008-4011 ("`RejectDyn` tightening —
  catch implicit dyn from heterogeneous `[1, "two"]` and bare `[]`").
  Code recurses through container/abstract types (parse_and_check.cc:360-377)
  and e2e/m4_test.cc:360-384 asserts both rejections (comments there credit
  M5.A).  The doc rows were never reconciled.
- **P1 — design.md §4.1 NodeAnnotation schema is six fields behind.**
  design.md ~414-460 reproduces a `NodeAnnotation` ending at
  `map_origin`/`list_origin` and the delta callout (~486-503) says exactly
  "three new fields beyond the original §4.1 schema" (attribute_id,
  map_origin, list_origin).  annotations.h:99-136 additionally has
  `message_type_id`, `comp_aux_local_base`, `comp_iter_local_index`,
  `comp_accu_local_index`, `comp_iter2_local_index`,
  `select_key_rodata_offset` — none reflected in design.md §4.1.
- **P1 — public header overstates the gate.**  parse_and_check.h:65-66
  ("validates that `expression` falls inside the static subset (no DYN /
  ERROR / type-param / function / unset nodes)") omits all five admit
  carve-outs (§1.2 items 1-5).  A reader concludes `dyn(1) == 1u`,
  `"x %s".format([1,"a"])`, `math.greatest(1, 2u)`, `cel.bind(...)`, and
  selects through `Any` are rejected; all are admitted
  (parse_and_check.cc:400-503, 551-630; tests parse_and_check_test.cc:542-628).
- **P2 — dyn-passthrough-plan.md admission summary missing the type-value
  arm.**  dyn-passthrough-plan.md:7-9 says the argument must be "a primitive
  scalar or `null`"; code also admits `has_type()` (parse_and_check.cc:561,
  comment cc:556-562).  The extension is documented in
  m9-type-subsystem.md:507-511 but the dyn plan's "What landed" header was
  not reconciled (CLAUDE.md closeout rule 4).
- **P2 — design.md §4.1 claims a per-kind populated-ness audit that doesn't
  exist as described.**  design.md ~507-513: "A per-kind audit at ResolvePass
  end DCHECKs the populated-ness pattern per kind."  The only audit in
  resolve_pass.cc is `KConstReprAudit` (resolve_pass.cc:29-31, 607-608) —
  kConst repr only, not per-kind field checks.  (Boundary note: resolve_pass
  is the codegen component, but the claim is about the IR schema's
  zero-sentinel contract.)
- **P2 — stale milestone comment on StorageKind.**  annotations.h:44-45
  ("M1 only emits `kStaticRodata`") — LayoutPass now emits all three kinds
  (kWorkspaceSlot/kLocal are exercised throughout, e.g. design.md §4.2
  "every call expression's node storage is kWorkspaceSlot").  Grandfathered
  milestone reference; misleads about current behavior.
- **P2 — parse_and_check.h variable-spec doc omits `dyn` and `type`.**
  h:34-38 lists the accepted type keywords; `ParsePrimitiveType` also accepts
  `dyn` (cc:213, used by `RejectDynStillRejectsDynVariable` test cc:575-582)
  and `type` (cc:220).

## 3. Validation items

- **Is `Repr::kEnum` reachable from this component at all?**
  `ReprOf(cel::TypeSpec)` has no enum arm (cel-cpp's `TypeSpecKind` variant,
  third_party/cel-cpp/common/ast/metadata.h:546-551, has no enum
  alternative), and `ReprOf(cel::Type)`'s `kEnum` arm (typed_ast.cc:99-100)
  has exactly one caller — `ParseVariableSpec` (parse_and_check.cc:340) —
  whose `TypeParser` can only produce primitives/list/map/message.  Settle:
  add a unit test calling `ReprOf` paths + `grep -rn "Repr::kEnum"
  compiler/` and trace whether anything outside eval-side ABI decode
  (eval/internal/abi_decode.cc:135-136) ever PRODUCES kEnum; if not, the
  enumerator is wire-format-only and the docs should say so.
- **Is the `"no type_map entry"` violation arm (parse_and_check.cc:633-635)
  dead in practice?**  The dyn-passthrough probe (dyn-passthrough-plan.md
  Risk #3 callout, lines 315-336) found cel-cpp materializes type_map entries
  even for dyn call sites despite ast.h claiming omission.  Settle: a
  RejectDyn unit test on a hand-built `cel::Ast` with a typed root and an
  untyped child asserting the violation message contains "no type_map
  entry"; plus an oracle/probe over a real checked AST asserting every
  reachable node id has a type_map entry.
- **Does the format-args carve-out miscompile or runtime-error on message
  elements?**  Comment cc:396-399 asserts a `list<dyn>` variable "might carry
  messages, which RenderString errors out on" — implying the admitted
  literal-list shape with a message element (`"%s".format([msg_var])`)
  produces a clean runtime error.  Settle: e2e test compiling
  `"%s".format([c])` with `c:celwasm.testdata.Customer` bound; assert it
  compiles (gate admits) and `Eval` returns a structured error, not a trap or
  wrong value.
- **Can a non-`cel.bind` expression hit the `IsCelBindShape` skip?**  The
  shape test (cc:474-482) matches any comprehension with empty-list
  iter_range + `false` loop_condition, regardless of macro provenance.
  Comprehensions only arise from macros today, but comprehensions_v2 +
  optMap/optFlatMap expand to comprehensions too.  Settle: probe test
  enumerating each registered macro's expansion (`cel.bind`, `optMap`,
  `optFlatMap`, `transformList` over `[]`) and asserting only `cel.bind`
  matches `IsCelBindShape` — i.e. `[].all(x, false)`-style shapes don't get
  loop_step/iter_range skipped unintentionally (n.b. `[]` iter_range already
  rejects via accu_init/result checks? verify which node carries the
  violation).
- **Does `IsSelectThroughAny` admit `has(msg.any_field.x)` (test_only) and
  what does codegen do with it?**  The carve-out checks only operand typing
  (cc:575-590); no test in parse_and_check_test.cc covers select-through-Any
  at all (frontend admission is pinned only by e2e/m7a tests, if anywhere).
  Settle: frontend unit tests `msg.single_any.x` admits / `dyn`-typed
  non-Any operand still rejects, using a schema fixture with an Any field.
- **Wrapper-typed variable specs:** `ParsePrimitiveType` has no
  `google.protobuf.Int64Value` keyword, but the FQN path goes through
  `FindMessageTypeByName` → `cel::Type::Message(descriptor)`.  Does cel-cpp
  normalize wrapper-message descriptors to wrapper types (→ Repr kInt via
  `kIntWrapper`) or leave them kStruct (→ kMessage)?  Settle: unit test
  `variable_specs={"w:google.protobuf.Int64Value"}` asserting the resulting
  `Variable::repr` and root repr of `w`.

## 4. Test coverage observations

Pinned well:
- Repr mapping is exhaustively covered at the TypeSpec level: every primitive,
  wrapper, well-known, null, list, map, message, type-of-type, dyn, error,
  function, param, unset, generic abstract, `optional_type` (incl.
  `optional<dyn>` stamping kOptional while RejectDyn rejects at a different
  layer) — typed_ast_test.cc:24-171.
- Field-number resolution: 18-row matrix over every proto field kind with
  non-contiguous numbers (typed_ast_test.cc:267-429), test_only, nested
  chains, unknown field/message, non-message operand, null pool
  (typed_ast_test.cc:431-548); real-plumbing duplicates through ParseAndCheck
  against the generated pool (parse_and_check_test.cc:287-412).
- dyn-passthrough admit/reject matrix: literal/ident/nested/scalar-select/
  cross-`==` admit; dyn-variable/list/map/message/`dyn(msg).field` reject
  (parse_and_check_test.cc:542-628).
- Variable-spec parser negatives: no colon, empty name, unknown type,
  unbalanced list/map, trailing garbage, missing schema files, bad proto
  source (parse_and_check_test.cc:432-516).
- AST-shape pins for kIdent/kSelect/member-kCall/has() guard against vendored
  parser drift (parse_and_check_test.cc:198-285).
- Status-payload classification has consumers in conformance/runner.cc but
  see gap below.

Gaps:
- **No frontend unit test for the four non-dyn RejectDyn carve-outs**:
  select-through-Any, math.@min/@max, `.format([...])`, `cel.bind` shape.
  All are pinned (if at all) only in e2e/conformance, far from the gate.
- **No test asserts the status payloads themselves** — nothing in
  parse_and_check_test.cc calls `GetPayload(kStaticSubsetViolationUrl)` or
  checks the comma-joined id body / newline-joined roots body.  The contract
  in status_tags.h:16-27 is untested at the producer.
- **annotations_test.cc `ReprNameTest.CoversEveryEnumerator` is missing
  `Repr::kOptional`** (rows end at kType, annotations_test.cc:16-24) despite
  the test name; annotations.cc:40-41 handles it.
- **No test for `InlineConstantReferences` / `InlineTypeIdentifierReferences`
  at the frontend unit level** — the rewrites (parse_and_check.cc:1266-1460)
  are pinned only via m7/m9 e2e + conformance.  No unit test asserts e.g.
  that `type(1) == int` leaves a kConstant root or that a value-bearing
  Reference wins over the type-ident rewrite (ordering invariant
  cc:1436-1437).
- **No test for the implicit-dyn rejections at the frontend level** — bare
  `[]` / `[1,"two"]` rejection lives in e2e/m4_test.cc:360-384 (through the
  full Compile), not in parse_and_check_test.cc.
- `RegisterNetworkExtDecls` and `RegisterCustomFunctionsOnChecker` have no
  unit tests in this package (covered indirectly by m18/m13 e2e suites).

## 5. Design decisions worth preserving

- **No heavier IR.**  `TypedAst` deliberately wraps cel-cpp's `cel::Ast` +
  side maps instead of defining a new tree; downstream passes consume
  `type_map` + `reference_map` + `WasmAnnotations` simultaneously
  (typed_ast.h:38-46).  All compiler-specific facts live in the side map,
  keyed by expr id — the AST itself is only mutated by the two
  constant-inlining rewrites, which happen before any annotation/gating pass
  so the rest of the pipeline never sees a kIdent for a resolved constant.
- **The static-subset gate is type-driven with shape-matched carve-outs.**
  The default is "any dyn/error/function/param/unset anywhere in a node's
  type — including inside containers and abstract params — rejects"; every
  admission is an explicit, narrowly shape-matched predicate justified by a
  runtime kernel that dispatches per-kind (format render, math fold,
  polymorphic equals, Any unwrap) or by unreachability (cel.bind's dead
  loop).  Admissions recurse into the safe subtree only; nothing is admitted
  because "the checker said dyn but it's probably fine".
- **Violations are collected, not first-fail**, and reported with expr ids
  both in the message and machine-readably in the status payload — the
  conformance harness routes on payloads, never on message substrings
  (status_tags.h:8-14).  Keep payload-based classification when adding new
  failure categories.
- **`dyn(x)` is the identity function** — admitted dyn calls never become a
  runtime kind; there is no CEL_DYN.  Codegen/ResolvePass forward the
  argument's annotations onto the call node (dyn-passthrough-plan.md:15-28).
- **Repr stamped on `Variable` at spec-parse time, not re-derived** —
  because unreferenced variables never appear in type_map but still shape the
  eval function signature (typed_ast.h:29-32).
- **`field_number` re-resolved by the frontend while the pool is live**
  (Option B from M3 G2): cel-cpp's reference_map cannot supply it; 0 is the
  "resolve by name / not a message field" sentinel (typed_ast.h:96-103).
- **One `ParserOptions` source of truth** shared between macro registration
  and the Parse call — divergence produces "macro not found" / "unexpected ?"
  failures (parse_and_check.cc:1087-1093).  `enable_optional_syntax` must be
  flipped together with `OptionalCheckerLibrary` (cc:1049-1052).
- **Rewrite ordering is load-bearing**: InlineConstantReferences →
  InlineTypeIdentifierReferences → RejectDyn → PopulateAnnotations.  The
  type-ident rewriter keys off "Reference has no value" and so depends on the
  constant rewrite having consumed the value-bearing ones first
  (cc:1416-1418, 1436-1437).
- **overload_id as borrowed string_view** (vs interned uint32) — accepted
  lifetime coupling to the TypedAst in exchange for dropping a
  uint→string round-trip per call site (design.md delta callout ~463-481).
  Any future TypedAst serialization must re-own these views.
- **`optional_type` detection is exact-name at the TypeSpec level and
  `Is<OptionalType>` at the Type level** — deliberately NOT "any
  OpaqueType", so future cel-cpp abstract types don't silently light up the
  optional codegen path (typed_ast.cc:107-112, typed_ast_test.cc:154-160).
- **Schema overlay, not replacement**: user schemas merge OVER the generated
  pool via MergedDescriptorDatabase, so well-known types always resolve
  (parse_and_check.cc:185-193).
