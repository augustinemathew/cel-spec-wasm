#include "compiler_v2/compile.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "binaryen-c.h"
#include "compiler/testdata/e2e_fixture.pb.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

// Force generated-pool registration of `celwasm.testdata.Customer`
// + `Address` so the variable-spec parser can resolve them by FQN.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<celwasm::testdata::Customer>();
      google::protobuf::LinkMessageReflection<celwasm::testdata::Address>();
      return 0;
    }();

// --- Per-kind kConst round-trip through Compile() ------------------------

TEST(CompileTest, CompilesScalarBool) {
  EXPECT_THAT(Compile("true").status(), IsOk());
}
TEST(CompileTest, CompilesScalarInt) {
  EXPECT_THAT(Compile("42").status(), IsOk());
}
TEST(CompileTest, CompilesScalarUint) {
  EXPECT_THAT(Compile("42u").status(), IsOk());
}
TEST(CompileTest, CompilesScalarDouble) {
  EXPECT_THAT(Compile("3.14").status(), IsOk());
}
TEST(CompileTest, CompilesScalarNull) {
  EXPECT_THAT(Compile("null").status(), IsOk());
}
TEST(CompileTest, CompilesScalarString) {
  EXPECT_THAT(Compile("\"hi\"").status(), IsOk());
}
TEST(CompileTest, CompilesScalarBytes) {
  EXPECT_THAT(Compile("b\"x\"").status(), IsOk());
}

// --- Error propagation ---------------------------------------------------

TEST(CompileTest, ControlFlowKCallCompilesGreen) {
  // M5.G (Slice 2) lit up `_&&_` / `_||_` / `!_` / `_?_:_`.
  // Originally a pending-Unimplemented fixture; rewritten as
  // positive coverage at the M5.G enabling commit.
  EXPECT_THAT(Compile("true && false").status(), IsOk());
  EXPECT_THAT(Compile("true || false").status(), IsOk());
  EXPECT_THAT(Compile("!true").status(), IsOk());
  EXPECT_THAT(Compile("true ? 1 : 2").status(), IsOk());
}

TEST(CompileTest, ParseFailureSurfacesAsInvalidArgument) {
  // Unclosed paren — parser rejects, facade forwards the status.
  EXPECT_THAT(Compile("(1"), StatusIs(absl::StatusCode::kInvalidArgument));
}

// --- Artifact shape ------------------------------------------------------

TEST(CompileTest, ArtifactCarriesAstLayoutAndEvalFunction) {
  auto art_or = Compile("42");
  ASSERT_THAT(art_or, IsOk());
  const CompiledArtifact& art = *art_or;
  EXPECT_TRUE(art.ast.has_ast());
  EXPECT_FALSE(art.layout.rodata.empty());
  EXPECT_EQ(art.layout.rodata_base, 16u);
  EXPECT_NE(art.eval_fn.func, nullptr);
}

TEST(CompileTest, ModuleImportsCelMemoryAndExportsEvalByDefault) {
  // Per Plan §5 Commit F: under the (A) two-phase topology, expr no
  // longer defines its own memory — it imports `cel.memory` from the
  // host (the same memory the cel_runtime.wasm instance imports), so
  // both modules see the same bytes when wired up by `Engine::Plan`.
  auto art_or = Compile("42");
  ASSERT_THAT(art_or, IsOk());
  BinaryenModuleRef raw = art_or->module.raw();
  EXPECT_TRUE(BinaryenHasMemory(raw));

  // Memory should NOT be in the export table (it's an import now).
  bool saw_memory_export = false;
  bool saw_eval_export = false;
  const auto n = BinaryenGetNumExports(raw);
  for (BinaryenIndex i = 0; i < n; ++i) {
    BinaryenExportRef e = BinaryenGetExportByIndex(raw, i);
    const char* name = BinaryenExportGetName(e);
    if (std::strcmp(name, "memory") == 0) saw_memory_export = true;
    if (std::strcmp(name, "eval") == 0) saw_eval_export = true;
  }
  EXPECT_FALSE(saw_memory_export);
  EXPECT_TRUE(saw_eval_export);

  // Memory import should be (cel, memory).  The internal binaryen
  // name of the memory is also "memory" (set by AddMemoryImport in
  // codegen/module.cc).
  const char* mod_name = BinaryenMemoryImportGetModule(raw, "memory");
  const char* base_name = BinaryenMemoryImportGetBase(raw, "memory");
  ASSERT_NE(mod_name, nullptr);
  ASSERT_NE(base_name, nullptr);
  EXPECT_STREQ(mod_name, "cel");
  EXPECT_STREQ(base_name, "memory");
}

TEST(CompileTest, ModuleInstallsCelResetAndCelAllocImports) {
  auto art_or = Compile("42");
  ASSERT_THAT(art_or, IsOk());
  BinaryenModuleRef raw = art_or->module.raw();
  // Binaryen models function imports as functions whose body is null.
  EXPECT_NE(BinaryenGetFunction(raw, "cel_reset"), nullptr);
  EXPECT_NE(BinaryenGetFunction(raw, "cel_alloc"), nullptr);
}

TEST(CompileTest, SerializedBytesStartWithWasmPreamble) {
  auto art_or = Compile("42");
  ASSERT_THAT(art_or, IsOk());
  const auto& bytes = art_or->wasm_bytes;
  ASSERT_GE(bytes.size(), 8u);
  EXPECT_EQ(bytes[0], 0x00);
  EXPECT_EQ(bytes[1], 0x61);
  EXPECT_EQ(bytes[2], 0x73);
  EXPECT_EQ(bytes[3], 0x6D);
}

// --- Option flow ---------------------------------------------------------

TEST(CompileTest, SerializeFalseLeavesWasmBytesEmpty) {
  CompileOptions opts;
  opts.serialize = false;
  auto art_or = Compile("42", opts);
  ASSERT_THAT(art_or, IsOk());
  EXPECT_TRUE(art_or->wasm_bytes.empty());
}

TEST(CompileTest, MemSizeBytesFlowsToCelResetSecondArg) {
  CompileOptions opts;
  opts.mem_size_bytes = 128u * 1024u;
  auto art_or = Compile("42", opts);
  ASSERT_THAT(art_or, IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(art_or->eval_fn.func);
  BinaryenExpressionRef call = BinaryenBlockGetChildAt(body, 0);
  ASSERT_STREQ(BinaryenCallGetTarget(call), "cel_reset");
  BinaryenExpressionRef arg1 = BinaryenCallGetOperandAt(call, 1);
  EXPECT_EQ(BinaryenConstGetValueI32(arg1),
            static_cast<int32_t>(opts.mem_size_bytes));
}

TEST(CompileTest, EvalExportNameIsHonoured) {
  CompileOptions opts;
  opts.eval_export_name = "my_eval";
  auto art_or = Compile("42", opts);
  ASSERT_THAT(art_or, IsOk());
  BinaryenModuleRef raw = art_or->module.raw();
  bool saw = false;
  const auto n = BinaryenGetNumExports(raw);
  for (BinaryenIndex i = 0; i < n; ++i) {
    if (std::strcmp(BinaryenExportGetName(BinaryenGetExportByIndex(raw, i)),
                    "my_eval") == 0) {
      saw = true;
      break;
    }
  }
  EXPECT_TRUE(saw);
}

TEST(CompileTest, MemSizeBytesLargerThanOnePageGrowsPageCount) {
  // Ask for 3 * 64 KiB; memory should be declared with at least 3 pages.
  CompileOptions opts;
  opts.mem_size_bytes = 3u * 64u * 1024u;
  auto art_or = Compile("42", opts);
  ASSERT_THAT(art_or, IsOk());
  BinaryenModuleRef raw = art_or->module.raw();
  ASSERT_TRUE(BinaryenHasMemory(raw));
  // Serialize and validate that it's a legal module — the page-count
  // arithmetic is validated transitively by Binaryen's validator.
  EXPECT_THAT(art_or->module.Validate(), IsOk());
}

// ────────────────────────────────────────────────────────────────────
// M3 — map literals + indexing.  These are compile-side tests: they
// drive the full Compile() pipeline (parse → check → resolve →
// layout → emit → validate) and assert that map-bearing source
// reaches a valid wasm artifact with the expected import surface,
// without instantiating wasmtime.  E2E (Compile→Plan→Eval) lives in
// `compiler_v2/e2e/m3_test.cc`.
// ────────────────────────────────────────────────────────────────────

namespace {

bool ModuleImports(BinaryenModuleRef raw, absl::string_view module_name,
                   absl::string_view base_name) {
  // Binaryen models function imports as functions whose body is null
  // and whose `module` / `base` attrs are populated.  Walk every
  // function and match on (module, base).
  const auto n = BinaryenGetNumFunctions(raw);
  for (BinaryenIndex i = 0; i < n; ++i) {
    BinaryenFunctionRef f = BinaryenGetFunctionByIndex(raw, i);
    const char* m = BinaryenFunctionImportGetModule(f);
    const char* b = BinaryenFunctionImportGetBase(f);
    if (m == nullptr || b == nullptr) continue;
    if (module_name == m && base_name == b) return true;
  }
  return false;
}

}  // namespace

TEST(CompileMapTest, EmptyMapLiteralRejected) {
  // Bare `{}` types as `map<dyn, dyn>`; the M5.A static-subset gate
  // walks list/map element types recursively and rejects implicit
  // dyn (m5-kcall-comprehensions.md §5).  The internal N=0 codegen
  // path stays in place — it's reached from M5.I comprehensions
  // whose `accu_init` is `{}` / `[]` after macro expansion.
  EXPECT_FALSE(Compile("{}").status().ok());
}

TEST(CompileMapTest, ScalarMapLiteralCompiles) {
  EXPECT_THAT(Compile("{\"a\": 1, \"b\": 2}").status(), IsOk());
}

TEST(CompileMapTest, MapLiteralIndexingCompiles) {
  EXPECT_THAT(Compile("{\"a\": 1}[\"a\"]").status(), IsOk());
}

TEST(CompileMapTest, BoundMapIdentIndexingCompiles) {
  CompileOptions opts;
  opts.check.variable_specs = {"m:map<string,int>"};
  EXPECT_THAT(Compile("m[\"k\"]", opts).status(), IsOk());
}

TEST(CompileMapTest, BoundMapEveryAllowedKeyKindCompiles) {
  // langdef restricts map keys to bool / int / uint / string.
  // Each kind should compile cleanly when indexed by a literal of
  // the matching scalar type — the M3 ResolvePass map_origin
  // visitor stamps `kHost` on the bound ident regardless of key
  // kind, and the host-arm dispatch in expr_lower is shape-
  // agnostic.
  struct Case {
    const char* spec;
    const char* expr;
  };
  for (const Case& c : {
           Case{"m:map<bool,int>", "m[true]"},
           Case{"m:map<int,int>", "m[1]"},
           Case{"m:map<uint,int>", "m[1u]"},
           Case{"m:map<string,int>", "m[\"k\"]"},
       }) {
    CompileOptions opts;
    opts.check.variable_specs = {c.spec};
    EXPECT_THAT(Compile(c.expr, opts).status(), IsOk())
        << "spec=" << c.spec << " expr=" << c.expr;
  }
}

TEST(CompileMapTest, ModuleImportsRuntimeMapEntryPoints) {
  // A map-bearing program must drag in every runtime map symbol the
  // codegen may emit calls to: cel_map_create, cel_map_insert,
  // cel_map_lookup_arena, cel_map_lookup.  No lazy import gating
  // (CLAUDE.md "always link the runtime fully").
  auto art_or = Compile("{\"a\": 1}[\"a\"]");
  ASSERT_THAT(art_or, IsOk());
  BinaryenModuleRef raw = art_or->module.raw();
  for (const char* fn : {"cel_map_create", "cel_map_insert",
                         "cel_map_lookup_arena", "cel_map_lookup"}) {
    EXPECT_TRUE(ModuleImports(raw, "cel", fn)) << "missing import cel." << fn;
  }
}

TEST(CompileMapTest, ModuleImportsHostMapLookup) {
  // The kHost arm + kDynamic dispatcher both reach
  // `cel_host.cel_map_lookup`.  The compile pipeline installs it
  // unconditionally; verify it's wired even on a literal-only
  // expression so the `BindRuntimeExport` loop in `Engine::Plan`
  // can rely on it.
  auto art_or = Compile("{\"a\": 1}[\"a\"]");
  ASSERT_THAT(art_or, IsOk());
  EXPECT_TRUE(
      ModuleImports(art_or->module.raw(), "cel_host", "cel_map_lookup"));
}

TEST(CompileMapTest, RuntimeImportsAlsoPresentForLiteralOnlyMap) {
  // A literal-only (non-indexed) map still drags in the cel_host
  // import — codegen never decides to omit it based on AST shape.
  // M5.A note: bare `{}` was the original probe but is now correctly
  // rejected by the static-subset gate; switch to a typed literal
  // that covers the same "no indexing" shape.
  auto art_or = Compile("{\"a\": 1}");
  ASSERT_THAT(art_or, IsOk());
  EXPECT_TRUE(
      ModuleImports(art_or->module.raw(), "cel_host", "cel_map_lookup"));
}

TEST(CompileMapTest, EmittedModuleSerializesAndValidates) {
  CompileOptions opts;
  opts.check.variable_specs = {"m:map<string,int>"};
  auto art_or = Compile("m[\"k\"]", opts);
  ASSERT_THAT(art_or, IsOk());
  EXPECT_THAT(art_or->module.Validate(), IsOk());
  ASSERT_GE(art_or->wasm_bytes.size(), 8u);
  EXPECT_EQ(art_or->wasm_bytes[0], 0x00);
  EXPECT_EQ(art_or->wasm_bytes[1], 0x61);
  EXPECT_EQ(art_or->wasm_bytes[2], 0x73);
  EXPECT_EQ(art_or->wasm_bytes[3], 0x6D);
}

TEST(CompileMapTest, MapTypedIdentLandsAsRefSlotVariable) {
  // A `map<...>` variable should be declared on the ABI's variables[]
  // table with `repr == kMap` (interned to the wire enum).  Stamps
  // the bridge between the M3 ResolvePass map_origin annotation and
  // the host marshal layer.
  CompileOptions opts;
  opts.check.variable_specs = {"m:map<string,int>"};
  auto art_or = Compile("m[\"k\"]", opts);
  ASSERT_THAT(art_or, IsOk());
  // ABI is emitted as a custom section; here we inspect the layout
  // pass output directly (the ABI proto mirrors layout.variables).
  bool saw_map_var = false;
  for (const auto& v : art_or->layout.variables) {
    if (v.name == "m") {
      saw_map_var = true;
      EXPECT_EQ(v.repr, Repr::kMap);
    }
  }
  EXPECT_TRUE(saw_map_var);
}

TEST(CompileMapTest, MapLiteralProgramLayoutReservesWorkspaceForMap) {
  // kCreateMap nodes need a 24-byte slot in the workspace so the
  // emitted `cel_map_create` call has somewhere to write the
  // result CelValue.  Compile() must report a non-zero workspace
  // size for any map-bearing program.
  auto art_or = Compile("{\"a\": 1}");
  ASSERT_THAT(art_or, IsOk());
  EXPECT_GT(art_or->layout.workspace_bytes, 0u);
}

// ────────────────────────────────────────────────────────────────────
// M3 — proto map field access.  `customer.metadata["env"]` is the
// M3.G dispatch path: `kSelect` on a `map<K,V>`-typed field flows
// through `ProtoBacking::ReadField` (host returns
// `Value::HostMap(ProtoMap{…})`) and the `kCallExpr(_[_])` arm
// emits a `cel_host.cel_map_lookup` call.  These compile-side
// tests cover the schema → check → resolve → layout → emit
// pipeline; e2e (Compile→Plan→Eval) requires M2.C.0b's
// `CelGetFieldImpl` to land first and lives in `m3_test.cc`.
// ────────────────────────────────────────────────────────────────────

namespace {

// Compile against the `celwasm.testdata.Customer` schema (M3
// fixture has `map<string,string> metadata` + `map<int32,int32>
// tier_quotas`).  Uses the generated descriptor pool so no
// SchemaProtoSource path is needed at test time.
absl::StatusOr<CompiledArtifact> CompileWithCustomer(absl::string_view expr) {
  CompileOptions opts;
  opts.check.variable_specs = {"c:celwasm.testdata.Customer"};
  return Compile(expr, opts);
}

}  // namespace

TEST(CompileProtoMapTest, StringKeyedProtoMapFieldIndexCompiles) {
  EXPECT_THAT(CompileWithCustomer("c.metadata[\"env\"]").status(), IsOk());
}

TEST(CompileProtoMapTest, IntKeyedProtoMapFieldIndexCompiles) {
  EXPECT_THAT(CompileWithCustomer("c.tier_quotas[1]").status(), IsOk());
}

TEST(CompileProtoMapTest, ProtoMapFieldAsBareSelectCompiles) {
  // `c.metadata` (no indexing) — kSelect produces a `map<...>`
  // result that should reach a workspace slot via ResolvePass +
  // LayoutPass.  Pure compile-side: doesn't exercise the host
  // dispatch arm.
  EXPECT_THAT(CompileWithCustomer("c.metadata").status(), IsOk());
}

TEST(CompileProtoMapTest, MapFieldIndexImportsHostMapLookup) {
  // `c.metadata["env"]` reaches `cel_host.cel_map_lookup` because
  // ResolvePass stamps the kSelect with map_origin == kHost.
  // The compiled artifact must declare that import on the module
  // so wasmtime instantiation succeeds at Plan time.
  auto art_or = CompileWithCustomer("c.metadata[\"env\"]");
  ASSERT_THAT(art_or, IsOk());
  EXPECT_TRUE(
      ModuleImports(art_or->module.raw(), "cel_host", "cel_map_lookup"));
  // It also imports the runtime exports that codegen may emit.
  EXPECT_TRUE(ModuleImports(art_or->module.raw(), "cel", "cel_map_lookup"));
  EXPECT_TRUE(
      ModuleImports(art_or->module.raw(), "cel", "cel_map_lookup_arena"));
}

TEST(CompileProtoMapTest, ProtoMapFieldRefRecordedOnAbi) {
  // The host trampoline needs to know the field's number + name
  // to call `ProtoBacking::ReadField` with the right descriptor.
  // Codegen interns the (field_number, field_name) into the
  // emitter's `field_refs` table; that table is what
  // `compile.cc::InstallHostAbi` serialises into the `cel.abi`
  // custom section.
  auto art_or = CompileWithCustomer("c.metadata[\"env\"]");
  ASSERT_THAT(art_or, IsOk());
  bool saw_metadata_field = false;
  for (const auto& fr : art_or->eval_fn.field_refs) {
    if (fr.name == "metadata" && fr.owner_fqn == "celwasm.testdata.Customer") {
      saw_metadata_field = true;
      EXPECT_EQ(fr.field_number, 10u);  // proto field number from .proto
    }
  }
  EXPECT_TRUE(saw_metadata_field);
}

TEST(CompileProtoMapTest, ProtoMapEmittedModuleSerializesAndValidates) {
  auto art_or = CompileWithCustomer("c.tier_quotas[1]");
  ASSERT_THAT(art_or, IsOk());
  EXPECT_THAT(art_or->module.Validate(), IsOk());
  ASSERT_GE(art_or->wasm_bytes.size(), 8u);
  EXPECT_EQ(art_or->wasm_bytes[0], 0x00);
  EXPECT_EQ(art_or->wasm_bytes[1], 0x61);
  EXPECT_EQ(art_or->wasm_bytes[2], 0x73);
  EXPECT_EQ(art_or->wasm_bytes[3], 0x6D);
}

TEST(CompileProtoMapTest, ProtoMapFieldLayoutGetsContiguousSelectAndCallSlots) {
  // `c.metadata["env"]` lays out as: c slot (variable), kSelect
  // result slot, kCallExpr(_[_]) result slot.  Pin the totals
  // so an accidental slot fusion or duplication trips this test.
  auto art_or = CompileWithCustomer("c.metadata[\"env\"]");
  ASSERT_THAT(art_or, IsOk());
  ASSERT_EQ(art_or->layout.variables.size(), 1u);
  EXPECT_EQ(art_or->layout.variables[0].name, "c");
  EXPECT_EQ(art_or->layout.variables[0].repr, Repr::kMessage);
  // 1 variable + kSelect + kCallExpr = 3 cells = 72 B.
  EXPECT_EQ(art_or->layout.workspace_bytes, 72u);
  EXPECT_EQ(art_or->layout.peak_slots, 2u);
}

}  // namespace
}  // namespace celwasm
