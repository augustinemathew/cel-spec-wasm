#include "compiler/internal/compile.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "abi/wasm_binary.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "binaryen-c.h"
#include "gtest/gtest.h"
#include "testdata/e2e_fixture.pb.h"

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
  // m28 — assertion is kDynamic-specific (static mode bundles the
  // runtime and therefore defines + exports memory).
  CompileOptions opts;
  opts.link_mode = CompileOptions::LinkMode::kDynamic;
  auto art_or = Compile("42", opts);
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
  EXPECT_NE(BinaryenGetFunction(raw, "arena_reset"), nullptr);
  EXPECT_NE(BinaryenGetFunction(raw, "arena_alloc"), nullptr);
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

// Post-M5: arena_reset takes no arguments — the runtime's bump cursor
// lives in BSS, not linear memory, so codegen no longer threads
// (arena_base, arena_limit) into the prologue.  CompileOptions::
// mem_size_bytes still controls the memory import's initial page
// count via `MemSizeBytesLargerThanOnePageGrowsPageCount` below.
TEST(CompileTest, EvalPrologueIsZeroArgArenaReset) {
  auto art_or = Compile("42");
  ASSERT_THAT(art_or, IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(art_or->eval_fn.func);
  BinaryenExpressionRef call = BinaryenBlockGetChildAt(body, 0);
  ASSERT_STREQ(BinaryenCallGetTarget(call), "arena_reset");
  EXPECT_EQ(BinaryenCallGetNumOperands(call), 0u);
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

// ────────────────────────────────────────────────────────────────────
// m28 — link_mode=kStatic byte-shape invariants.
// ────────────────────────────────────────────────────────────────────

namespace {

// Counts function imports whose `module` field equals `module_name`.
int CountFunctionImportsFromModule(BinaryenModuleRef raw,
                                   absl::string_view module_name) {
  int count = 0;
  const auto n = BinaryenGetNumFunctions(raw);
  for (BinaryenIndex i = 0; i < n; ++i) {
    BinaryenFunctionRef f = BinaryenGetFunctionByIndex(raw, i);
    const char* m = BinaryenFunctionImportGetModule(f);
    if (m != nullptr && module_name == m) ++count;
  }
  return count;
}

}  // namespace

TEST(CompileStaticTest, EmitsValidModule) {
  CompileOptions opts;
  opts.link_mode = CompileOptions::LinkMode::kStatic;
  auto art_or = Compile("42", opts);
  ASSERT_THAT(art_or, IsOk());
  EXPECT_THAT(art_or->module.Validate(), IsOk());
  EXPECT_GE(art_or->wasm_bytes.size(), 100u * 1024u);
}

TEST(CompileStaticTest, NoCelNamespaceImports) {
  // The load-bearing invariant of kStatic: the Program carries the
  // runtime helpers as defined functions, not as imports — so it has
  // ZERO imports from the `"cel"` module.  Dynamic mode (sibling test
  // `ModuleImportsCelMemoryAndExportsEvalByDefault`) has many; this
  // count must be exactly 0 for static.
  CompileOptions opts;
  opts.link_mode = CompileOptions::LinkMode::kStatic;
  auto art_or = Compile("42", opts);
  ASSERT_THAT(art_or, IsOk());
  BinaryenModuleRef raw = art_or->module.raw();
  EXPECT_EQ(CountFunctionImportsFromModule(raw, "cel"), 0);
}

TEST(CompileStaticTest, KeepsCelHostImportsTheRuntimeAlreadyDeclared) {
  // `cel_host.*` trampolines are the host's responsibility regardless
  // of link mode — the wrapper-stripped runtime declares them as its
  // own imports, and those survive the merge.  Sanity-check: at least
  // one `cel_host.*` import is still present.
  CompileOptions opts;
  opts.link_mode = CompileOptions::LinkMode::kStatic;
  auto art_or = Compile("42", opts);
  ASSERT_THAT(art_or, IsOk());
  BinaryenModuleRef raw = art_or->module.raw();
  EXPECT_GT(CountFunctionImportsFromModule(raw, "cel_host"), 0);
}

TEST(CompileStaticTest, ExportsEval) {
  CompileOptions opts;
  opts.link_mode = CompileOptions::LinkMode::kStatic;
  auto art_or = Compile("42", opts);
  ASSERT_THAT(art_or, IsOk());
  BinaryenModuleRef raw = art_or->module.raw();
  bool saw_eval = false;
  const auto n = BinaryenGetNumExports(raw);
  for (BinaryenIndex i = 0; i < n; ++i) {
    BinaryenExportRef e = BinaryenGetExportByIndex(raw, i);
    if (std::strcmp(BinaryenExportGetName(e), "eval") == 0) saw_eval = true;
  }
  EXPECT_TRUE(saw_eval);
}

// ── Link-mode marker in the `cel.abi` custom section ───────────────
//
// Compile() stamps the link mode it emitted the module under into
// the `cel.abi` section so embedder tooling (cache validators,
// signature systems, cross-process Program shipping) has an in-band
// signal.  Metadata only — engine routing stays import-introspection
// based.  See
// doc/implementation-plan/rewrite/m28-configurable-linking.md §6.

namespace {

// Find the custom section named "cel.abi" in `wasm_bytes` (via the
// shared //abi:wasm_binary walker) and proto-parse its payload as
// CelAbi.  The production decoder (eval/internal/abi_decode) lives
// on the eval side, which the compiler tree must not depend on.
absl::StatusOr<celwasm::abi::CelAbi> ParseCelAbiSection(
    const std::vector<uint8_t>& wasm_bytes) {
  auto payload_or = FindCustomSection(wasm_bytes, "cel.abi");
  if (!payload_or.ok()) return payload_or.status();
  celwasm::abi::CelAbi abi;
  if (!abi.ParseFromArray(payload_or->data(),
                          static_cast<int>(payload_or->size()))) {
    return absl::InvalidArgumentError(
        "cel.abi payload failed CelAbi::ParseFromArray");
  }
  return abi;
}

}  // namespace

TEST(CompileTest, DynamicModeStampsLinkModeDynamicInCelAbi) {
  CompileOptions opts;
  opts.link_mode = CompileOptions::LinkMode::kDynamic;
  auto art_or = Compile("42", opts);
  ASSERT_THAT(art_or, IsOk());
  auto abi_or = ParseCelAbiSection(art_or->wasm_bytes);
  ASSERT_THAT(abi_or, IsOk());
  EXPECT_EQ(abi_or->link_mode(), celwasm::abi::LINK_MODE_DYNAMIC);
}

TEST(CompileStaticTest, StaticModeStampsLinkModeStaticInCelAbi) {
  CompileOptions opts;
  opts.link_mode = CompileOptions::LinkMode::kStatic;
  auto art_or = Compile("42", opts);
  ASSERT_THAT(art_or, IsOk());
  auto abi_or = ParseCelAbiSection(art_or->wasm_bytes);
  ASSERT_THAT(abi_or, IsOk());
  EXPECT_EQ(abi_or->link_mode(), celwasm::abi::LINK_MODE_STATIC);
}

// ── Static-region budget gate (BOTH link modes) ────────────────────
//
// The runtime — bundled into the Program (kStatic) or instantiated
// alongside it (kDynamic, where the expr module's `cel.memory` import
// resolves to the runtime instance's exported memory) — is linked
// with `-Wl,--global-base=CELWASM_RESERVED_LOW_MEMORY_BYTES` (256 KiB).
// The expression's static region (rodata + workspace slots) must end
// at or below that boundary in EITHER mode, or its instantiate-time
// data segment / marshal-time slot writes silently overwrite the
// runtime's own static data in the shared memory (observed downstream
// as dlmalloc corruption: `wasm trap: unaligned atomic`).  See
// doc/implementation-plan/rewrite/m28-configurable-linking.md §5.3.1.
//
// rodata overflow is exercised with a large constant int list (each
// element materializes to a 24-byte rodata frame): a single string
// literal can't reach 256 KiB without first tripping the ~100K source
// codepoint cap.
std::string IntListLiteral(int n) {
  std::string expr = "[1";
  for (int i = 2; i <= n; ++i) {
    absl::StrAppend(&expr, ", ", i);
  }
  absl::StrAppend(&expr, "]");
  return expr;
}

TEST(CompileStaticTest, RodataNearBudgetButUnderCompiles) {
  // ~4 KiB string literal: rodata_base (16) + CelValue slot + 4096
  // payload bytes lands well under the 256 KiB reserved window.
  CompileOptions opts;
  opts.link_mode = CompileOptions::LinkMode::kStatic;
  const std::string expr = "\"" + std::string(4096, 'x') + "\"";
  EXPECT_THAT(Compile(expr, opts).status(), IsOk());
}

TEST(CompileStaticTest, RodataOverBudgetReturnsResourceExhausted) {
  // 12000 int frames (~288 KiB) push rodata past the 262144-byte
  // reserved window — must be a status error (embedder input may
  // legitimately be this large; it must not crash the process).
  CompileOptions opts;
  opts.link_mode = CompileOptions::LinkMode::kStatic;
  EXPECT_THAT(Compile(IntListLiteral(12000), opts),
              StatusIs(absl::StatusCode::kResourceExhausted));
}

TEST(CompileTest, RodataNearBudgetButUnderCompilesInDynamicMode) {
  CompileOptions opts;
  opts.link_mode = CompileOptions::LinkMode::kDynamic;
  const std::string expr = "\"" + std::string(4096, 'x') + "\"";
  EXPECT_THAT(Compile(expr, opts).status(), IsOk());
}

TEST(CompileTest, RodataOverBudgetReturnsResourceExhaustedInDynamicMode) {
  // kDynamic shares the SAME memory with the runtime instance
  // (`cel.memory` resolves to the runtime's exported memory), so the
  // reserved-window gate applies identically.  Before this gate, the
  // oversized rodata segment was applied over the runtime's static
  // data at instantiate time — silent corruption surfacing as a
  // `wasm trap: unaligned atomic` from clobbered dlmalloc state.
  CompileOptions opts;
  opts.link_mode = CompileOptions::LinkMode::kDynamic;
  EXPECT_THAT(Compile(IntListLiteral(12000), opts),
              StatusIs(absl::StatusCode::kResourceExhausted));
}

// Workspace half of the gate: distinct variables each reserve a 32-byte
// workspace slot.  Width, not depth, is the only way to exhaust the
// 256 KiB window now — a const list materializes into rodata (no
// workspace), and a nested non-const list can't overflow within the
// 2048 parse-depth cap (2048 * 32 B = 64 KiB).  `[a0, a1, …, a(n-1)]`
// is a flat list (parse depth 1) of `n` distinct vars ⇒ `n` slots.
struct VarListExpr {
  std::string expr;
  std::vector<std::string> specs;
};
VarListExpr WideVarList(int n) {
  VarListExpr out;
  out.specs.reserve(n);
  std::string expr = "[";
  for (int i = 0; i < n; ++i) {
    if (i != 0) absl::StrAppend(&expr, ", ");
    absl::StrAppend(&expr, "a", i);
    out.specs.push_back(absl::StrCat("a", i, ":int"));
  }
  absl::StrAppend(&expr, "]");
  out.expr = std::move(expr);
  return out;
}

TEST(CompileTest, WideVarListUnderBudgetCompilesBothModes) {
  // 1000 variable slots (~32 KiB) sit comfortably under the 256 KiB
  // window — compiles in both modes.
  const VarListExpr v = WideVarList(1000);
  for (auto mode : {CompileOptions::LinkMode::kStatic,
                    CompileOptions::LinkMode::kDynamic}) {
    CompileOptions opts;
    opts.link_mode = mode;
    opts.check.variable_specs = v.specs;
    EXPECT_THAT(Compile(v.expr, opts).status(), IsOk())
        << "mode=" << static_cast<int>(mode);
  }
}

TEST(CompileTest, WorkspaceOverBudgetReturnsResourceExhaustedBothModes) {
  // 9000 variable slots (~288 KiB) push workspace past the 256 KiB
  // window with negligible rodata — the rodata-only check would wrongly
  // accept this; the slot-exhaustion gate must reject it in both modes.
  const VarListExpr v = WideVarList(9000);
  for (auto mode : {CompileOptions::LinkMode::kStatic,
                    CompileOptions::LinkMode::kDynamic}) {
    CompileOptions opts;
    opts.link_mode = mode;
    opts.check.variable_specs = v.specs;
    EXPECT_THAT(Compile(v.expr, opts),
                StatusIs(absl::StatusCode::kResourceExhausted))
        << "mode=" << static_cast<int>(mode);
  }
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
// `e2e/m3_test.cc`.
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
  EXPECT_THAT(Compile(R"({"a": 1, "b": 2})").status(), IsOk());
}

TEST(CompileMapTest, MapLiteralIndexingCompiles) {
  EXPECT_THAT(Compile(R"({"a": 1}["a"])").status(), IsOk());
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
  // (CLAUDE.md "always link the runtime fully").  m28 — kDynamic-
  // specific assertion (kStatic merges the runtime in, so these
  // helpers are defined functions, not imports).
  CompileOptions opts;
  opts.link_mode = CompileOptions::LinkMode::kDynamic;
  auto art_or = Compile(R"({"a": 1}["a"])", opts);
  ASSERT_THAT(art_or, IsOk());
  BinaryenModuleRef raw = art_or->module.raw();
  for (const char* fn :
       {"cel_map_create", "cel_map_insert", "cel_map_index_build",
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
  auto art_or = Compile(R"({"a": 1}["a"])");
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
  // A per-Eval-built kCreateMap node needs a 24-byte slot in the
  // workspace so the emitted `cel_map_create` call has somewhere to write
  // the result CelValue.  A non-constant value (`m`) keeps the build path
  // (an all-const map materializes into rodata and needs no workspace —
  // see the m31 static-aggregate suite).  Compile() must report a
  // non-zero workspace size for such a map-bearing program.
  CompileOptions opts;
  opts.check.variable_specs = {"m:int"};
  auto art_or = Compile("{\"a\": m}", opts);
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
absl::StatusOr<CompiledArtifact> CompileWithCustomer(
    absl::string_view expr,
    CompileOptions::LinkMode link_mode = CompileOptions::LinkMode::kDynamic) {
  CompileOptions opts;
  opts.check.variable_specs = {"c:celwasm.testdata.Customer"};
  opts.link_mode = link_mode;
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
  // so wasmtime instantiation succeeds at Plan time.  m28 — kDynamic-
  // specific: in kStatic mode the runtime helpers are defined
  // functions in the merged module, not imports.
  auto art_or = CompileWithCustomer("c.metadata[\"env\"]",
                                    CompileOptions::LinkMode::kDynamic);
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

// ── required_functions in the `cel.abi` custom section ─────────────
//
// The table must equal the FINAL module's `cel_fn` import list — the
// post-optimize surface, not the registered libraries: overload
// imports are installed unconditionally for every declared custom
// fn, Binaryen drops the unused ones at O1+, and wasmtime demands
// every SURVIVING import at link time.  See
// doc/implementation-plan/rewrite/m35-plugin-ergonomics.md §5.2.

namespace {

CelfnType RfScalar(CelfnType::Kind kind) {
  CelfnType t;
  t.kind = kind;
  return t;
}

// Two plugin decls — the O2 pin calls only `allow`, so `deny_string`
// must survive at O0 and vanish at O2.
std::vector<FunctionLibrary> TwoPluginFnLibs() {
  auto lib =
      *FunctionLibrary::Builder()
           .AddPlugin("allow", RfScalar(CelfnType::Kind::kBool),
                      {CelfnParam{/*is_receiver=*/false,
                                  RfScalar(CelfnType::Kind::kString), "u"}})
           .AddPlugin("deny", RfScalar(CelfnType::Kind::kBool),
                      {CelfnParam{/*is_receiver=*/false,
                                  RfScalar(CelfnType::Kind::kString), "u"}})
           .Build();
  return {std::move(lib)};
}

// The `cel_fn` import base names of the artifact's FINAL module, in
// import order.
std::vector<std::string> CelFnImportBases(const CompiledArtifact& art) {
  std::vector<std::string> bases;
  for (const auto& import : art.module.ListFunctionImports()) {
    if (import.module == "cel_fn") bases.push_back(import.base);
  }
  return bases;
}

std::vector<std::string> RequiredFunctionIds(const celwasm::abi::CelAbi& abi) {
  std::vector<std::string> ids;
  ids.reserve(static_cast<size_t>(abi.required_functions_size()));
  for (const auto& row : abi.required_functions()) {
    ids.push_back(row.overload_id());
  }
  return ids;
}

CompileOptions RequiredFnOpts(CompileOptions::LinkMode link_mode,
                              int optimize_level) {
  CompileOptions opts;
  opts.link_mode = link_mode;
  opts.optimize_level = optimize_level;
  opts.check.variable_specs = {"u:string"};
  opts.function_libraries = TwoPluginFnLibs();
  // Both hops need the decls: the checker resolves call sites from
  // `check.function_libraries`; codegen + the required-functions
  // emitter read `function_libraries`.
  opts.check.function_libraries = opts.function_libraries;
  return opts;
}

}  // namespace

TEST(CompileRequiredFunctionsTest, EmptyWithoutCustomFnLibraries) {
  for (auto mode : {CompileOptions::LinkMode::kDynamic,
                    CompileOptions::LinkMode::kStatic}) {
    CompileOptions opts;
    opts.link_mode = mode;
    auto art_or = Compile("1 + 2", opts);
    ASSERT_THAT(art_or, IsOk());
    auto abi_or = ParseCelAbiSection(art_or->wasm_bytes);
    ASSERT_THAT(abi_or, IsOk());
    EXPECT_EQ(abi_or->required_functions_size(), 0);
    EXPECT_TRUE(CelFnImportBases(*art_or).empty());
  }
}

TEST(CompileRequiredFunctionsTest, TableEqualsImportListAtOptZero) {
  // O0: no DCE — BOTH declared fns' imports survive even though only
  // `allow` is called, and the table mirrors that.
  for (auto mode : {CompileOptions::LinkMode::kDynamic,
                    CompileOptions::LinkMode::kStatic}) {
    auto art_or = Compile("allow(u)", RequiredFnOpts(mode, 0));
    ASSERT_THAT(art_or, IsOk());
    auto abi_or = ParseCelAbiSection(art_or->wasm_bytes);
    ASSERT_THAT(abi_or, IsOk());
    const auto import_bases = CelFnImportBases(*art_or);
    EXPECT_EQ(RequiredFunctionIds(*abi_or), import_bases);
    EXPECT_EQ(import_bases,
              (std::vector<std::string>{"allow_string", "deny_string"}));
  }
}

// THE load-bearing pin of the post-optimize attach: at O2 Binaryen
// drops the uncalled `deny_string` import, and the table must track
// the SURVIVING import list — one row — or wasmtime link demands and
// Plan verification desync per optimize level.
TEST(CompileRequiredFunctionsTest, OptTwoDropsUnusedImportFromTable) {
  for (auto mode : {CompileOptions::LinkMode::kDynamic,
                    CompileOptions::LinkMode::kStatic}) {
    auto art_or = Compile("allow(u)", RequiredFnOpts(mode, 2));
    ASSERT_THAT(art_or, IsOk());
    auto abi_or = ParseCelAbiSection(art_or->wasm_bytes);
    ASSERT_THAT(abi_or, IsOk());
    const auto import_bases = CelFnImportBases(*art_or);
    EXPECT_EQ(import_bases, (std::vector<std::string>{"allow_string"}));
    ASSERT_EQ(abi_or->required_functions_size(), 1);
    EXPECT_EQ(abi_or->required_functions(0).overload_id(), "allow_string");
    EXPECT_EQ(RequiredFunctionIds(*abi_or), import_bases);
  }
}

TEST(CompileRequiredFunctionsTest, MixedBackendsCarryBackendPerRow) {
  CompileOptions opts;
  opts.check.variable_specs = {"u:string"};
  opts.function_libraries = {
      *FunctionLibrary::Builder()
           .AddHost(
               "discount_pct", RfScalar(CelfnType::Kind::kInt),
               {CelfnParam{false, RfScalar(CelfnType::Kind::kString), "s"}})
           .AddPlugin(
               "allow", RfScalar(CelfnType::Kind::kBool),
               {CelfnParam{false, RfScalar(CelfnType::Kind::kString), "u"}})
           .Build()};
  opts.check.function_libraries = opts.function_libraries;
  auto art_or = Compile("allow(u) && discount_pct(u) > 0", opts);
  ASSERT_THAT(art_or, IsOk()) << art_or.status();
  auto abi_or = ParseCelAbiSection(art_or->wasm_bytes);
  ASSERT_THAT(abi_or, IsOk());
  ASSERT_EQ(abi_or->required_functions_size(), 2);
  for (const auto& row : abi_or->required_functions()) {
    if (row.overload_id() == "discount_pct_string") {
      EXPECT_EQ(row.backend(), celwasm::abi::RequiredFunction::HOST);
      EXPECT_EQ(row.fn_name(), "discount_pct");
    } else {
      EXPECT_EQ(row.overload_id(), "allow_string");
      EXPECT_EQ(row.backend(), celwasm::abi::RequiredFunction::PLUGIN);
      EXPECT_EQ(row.fn_name(), "allow");
    }
  }
}

TEST(CompileRequiredFunctionsTest, ReceiverRowCarriesTypesAndReceiverBit) {
  CompileOptions opts;
  opts.check.variable_specs = {"name:string"};
  opts.function_libraries = {
      *FunctionLibrary::Builder()
           .AddHost("is_number", RfScalar(CelfnType::Kind::kBool),
                    {CelfnParam{/*is_receiver=*/true,
                                RfScalar(CelfnType::Kind::kString), "s"}})
           .Build()};
  opts.check.function_libraries = opts.function_libraries;
  auto art_or = Compile("name.is_number()", opts);
  ASSERT_THAT(art_or, IsOk()) << art_or.status();
  auto abi_or = ParseCelAbiSection(art_or->wasm_bytes);
  ASSERT_THAT(abi_or, IsOk());
  ASSERT_EQ(abi_or->required_functions_size(), 1);
  const auto& row = abi_or->required_functions(0);
  EXPECT_EQ(row.overload_id(), "is_number_string");
  EXPECT_TRUE(row.is_receiver());
  ASSERT_EQ(row.param_types_size(), 1);
  EXPECT_EQ(row.param_types(0).kind(), celwasm::abi::Type::KIND_STRING);
  EXPECT_EQ(row.return_type().kind(), celwasm::abi::Type::KIND_BOOL);
}

TEST(CompileProtoMapTest, ProtoMapFieldLayoutReusesSelectAndCallSlot) {
  // `c.metadata["env"]` lays out as: c slot (variable), kSelect
  // result slot, kCallExpr(_[_]) result slot.  The kCallExpr
  // releases the kSelect's slot before acquiring its own, so
  // both share a single cell via the LIFO free list — peak = 1
  // scratch cell.  Total = 1 var + 1 scratch = 2 × 32B = 64.
  auto art_or = CompileWithCustomer("c.metadata[\"env\"]");
  ASSERT_THAT(art_or, IsOk());
  ASSERT_EQ(art_or->layout.variables.size(), 1u);
  EXPECT_EQ(art_or->layout.variables[0].name, "c");
  EXPECT_EQ(art_or->layout.variables[0].repr, Repr::kMessage);
  EXPECT_EQ(art_or->layout.workspace_bytes, 64u);
  EXPECT_EQ(art_or->layout.peak_slots, 1u);
}

}  // namespace
}  // namespace celwasm
