// Tests for BuildCelAbi: given a StaticLayout with a populated
// `variables` vector, the emitted `CelAbi` message has one
// `VariableEntry` per variable with name, local_index, slot_offset,
// and repr copied across.

#include "abi/cel_abi_emit.h"

#include <string>
#include <utility>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "absl/status/status_matchers.h"
#include "absl/types/span.h"
#include "compiler/codegen/expr_lower.h"
#include "compiler/codegen/layout_pass.h"
#include "compiler/codegen/resolve_pass.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

// Run parse -> check -> resolve -> layout given a declared variable
// list, return the final StaticLayout.  Mirrors the helper in
// layout_pass_test.cc so the emitter tests read naturally.
StaticLayout LayoutWith(absl::string_view expression,
                        std::vector<std::string> variable_specs) {
  CheckOptions opts;
  opts.variable_specs = std::move(variable_specs);
  auto ta = ParseAndCheck(expression, opts);
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  return *std::move(layout);
}

TEST(CelAbiEmitTest, EmptyWhenNoVariablesReferenced) {
  StaticLayout layout = LayoutWith("42", {});
  auto abi = BuildCelAbi(layout, {}, celwasm::abi::LINK_MODE_DYNAMIC);
  ASSERT_THAT(abi, IsOk());
  EXPECT_EQ(abi->version(), 1u);
  EXPECT_EQ(abi->variables_size(), 0);
  EXPECT_EQ(abi->fields_size(), 0);
  // ResolvePass always seeds a sentinel attribute row at index 0
  // so `attribute_id` indexing is uniform even for literal-only
  // programs with no path-bearing nodes.
  ASSERT_EQ(abi->attributes_size(), 1);
  EXPECT_EQ(abi->attributes(0).id(), 0u);
  EXPECT_EQ(abi->attributes(0).variable(), "");
}

TEST(CelAbiEmitTest, SingleIntVariableRoundTrips) {
  StaticLayout layout = LayoutWith("x", {"x:int"});
  auto abi = BuildCelAbi(layout, {}, celwasm::abi::LINK_MODE_DYNAMIC);
  ASSERT_THAT(abi, IsOk());
  ASSERT_EQ(abi->variables_size(), 1);
  const auto& v = abi->variables(0);
  EXPECT_EQ(v.name(), "x");
  EXPECT_EQ(v.local_index(), 0u);
  EXPECT_EQ(v.slot_offset(), layout.variables[0].slot_offset);
  EXPECT_EQ(v.repr(), static_cast<uint32_t>(Repr::kInt));
}

TEST(CelAbiEmitTest, EveryScalarReprMaps) {
  // `b || s == "x" || i > 0 || d > 0.0` — not actually M2-compilable
  // (kCall root), but ResolvePass + LayoutPass run fine and fill
  // layout.variables with four distinct repr kinds.
  StaticLayout layout = LayoutWith("b || s == \"x\" || i > 0 || d > 0.0",
                                   {"b:bool", "s:string", "i:int", "d:double"});
  auto abi = BuildCelAbi(layout, {}, celwasm::abi::LINK_MODE_DYNAMIC);
  ASSERT_THAT(abi, IsOk());
  ASSERT_EQ(abi->variables_size(), 4);

  // First-seen order (ResolvePass is deterministic).
  EXPECT_EQ(abi->variables(0).name(), "b");
  EXPECT_EQ(abi->variables(0).repr(), static_cast<uint32_t>(Repr::kBool));
  EXPECT_EQ(abi->variables(1).name(), "s");
  EXPECT_EQ(abi->variables(1).repr(), static_cast<uint32_t>(Repr::kString));
  EXPECT_EQ(abi->variables(2).name(), "i");
  EXPECT_EQ(abi->variables(2).repr(), static_cast<uint32_t>(Repr::kInt));
  EXPECT_EQ(abi->variables(3).name(), "d");
  EXPECT_EQ(abi->variables(3).repr(), static_cast<uint32_t>(Repr::kDouble));
}

TEST(CelAbiEmitTest, VariableSlotOffsetsAreContiguous) {
  // Three int variables — slots stride at `SlotAllocator::kSlotStride`
  // (32B) so every slot stays 16-byte aligned for the runtime
  // helpers' memory.atomic.* ops.  The emitter just copies from
  // layout.variables without re-computing, so this locks both the
  // layout pass contract AND the emitter's transparent pass-through.
  StaticLayout layout = LayoutWith("x + y + z", {"x:int", "y:int", "z:int"});
  auto abi = BuildCelAbi(layout, {}, celwasm::abi::LINK_MODE_DYNAMIC);
  ASSERT_THAT(abi, IsOk());
  ASSERT_EQ(abi->variables_size(), 3);
  EXPECT_EQ(abi->variables(0).slot_offset(), layout.workspace_base);
  EXPECT_EQ(abi->variables(1).slot_offset(), layout.workspace_base + 32u);
  EXPECT_EQ(abi->variables(2).slot_offset(), layout.workspace_base + 64u);
  // 16-byte alignment is the load-bearing invariant.
  EXPECT_EQ(abi->variables(1).slot_offset() % 16u, 0u);
  EXPECT_EQ(abi->variables(2).slot_offset() % 16u, 0u);
}

// Serialize-round-trip — proves the emitted proto survives wire
// serialization + parse.  Decoder library tests (landing alongside
// abi_decode in a following commit) cover the parse-from-bytes
// path; this one stays within proto serialization.
TEST(CelAbiEmitTest, EmittedProtoSerializesAndRoundTripsThroughProtoParse) {
  StaticLayout layout = LayoutWith("x", {"x:int"});
  auto abi = BuildCelAbi(layout, {}, celwasm::abi::LINK_MODE_DYNAMIC);
  ASSERT_THAT(abi, IsOk());
  std::string bytes;
  ASSERT_TRUE(abi->SerializeToString(&bytes));

  celwasm::abi::CelAbi parsed;
  ASSERT_TRUE(parsed.ParseFromString(bytes));
  EXPECT_EQ(parsed.version(), 1u);
  ASSERT_EQ(parsed.variables_size(), 1);
  EXPECT_EQ(parsed.variables(0).name(), "x");
  EXPECT_EQ(parsed.variables(0).slot_offset(), layout.variables[0].slot_offset);
  EXPECT_EQ(parsed.variables(0).repr(), static_cast<uint32_t>(Repr::kInt));
}

// --- fields[] (M2.C.4) ----------------------------------------------------

TEST(CelAbiEmitTest, FieldRefsEmittedDenselyWithSentinelAtZero) {
  StaticLayout layout;  // no variables — fields arrive independently.
  const std::vector<FieldRefRow> field_refs = {
      {},  // sentinel at index 0
      {/*field_number=*/1, /*name=*/"name",
       /*owner_fqn=*/"celwasm.testdata.Customer"},
      {/*field_number=*/9, /*name=*/"billing_address",
       /*owner_fqn=*/"celwasm.testdata.Customer"},
  };
  auto abi = BuildCelAbi(layout, absl::MakeConstSpan(field_refs),
                         celwasm::abi::LINK_MODE_DYNAMIC);
  ASSERT_THAT(abi, IsOk());

  ASSERT_EQ(abi->fields_size(), 3);
  // Sentinel: id=0, empty metadata.
  EXPECT_EQ(abi->fields(0).id(), 0u);
  EXPECT_EQ(abi->fields(0).field_number(), 0u);
  EXPECT_EQ(abi->fields(0).name(), "");
  // Real rows carry their id + metadata.
  EXPECT_EQ(abi->fields(1).id(), 1u);
  EXPECT_EQ(abi->fields(1).field_number(), 1u);
  EXPECT_EQ(abi->fields(1).name(), "name");
  EXPECT_EQ(abi->fields(1).owner_fqn(), "celwasm.testdata.Customer");
  EXPECT_EQ(abi->fields(2).id(), 2u);
  EXPECT_EQ(abi->fields(2).name(), "billing_address");
}

// --- link_mode ------------------------------------------------------------
//
// The link-mode marker is embedder-tooling metadata, not an engine
// routing input — see
// doc/implementation-plan/rewrite/m28-configurable-linking.md §6.

TEST(CelAbiEmitTest, LinkModeDynamicRoundTripsThroughProtoParse) {
  StaticLayout layout = LayoutWith("42", {});
  auto abi = BuildCelAbi(layout, {}, celwasm::abi::LINK_MODE_DYNAMIC);
  ASSERT_THAT(abi, IsOk());
  EXPECT_EQ(abi->link_mode(), celwasm::abi::LINK_MODE_DYNAMIC);

  std::string bytes;
  ASSERT_TRUE(abi->SerializeToString(&bytes));
  celwasm::abi::CelAbi parsed;
  ASSERT_TRUE(parsed.ParseFromString(bytes));
  EXPECT_EQ(parsed.link_mode(), celwasm::abi::LINK_MODE_DYNAMIC);
}

TEST(CelAbiEmitTest, LinkModeStaticRoundTripsThroughProtoParse) {
  StaticLayout layout = LayoutWith("42", {});
  auto abi = BuildCelAbi(layout, {}, celwasm::abi::LINK_MODE_STATIC);
  ASSERT_THAT(abi, IsOk());
  EXPECT_EQ(abi->link_mode(), celwasm::abi::LINK_MODE_STATIC);

  std::string bytes;
  ASSERT_TRUE(abi->SerializeToString(&bytes));
  celwasm::abi::CelAbi parsed;
  ASSERT_TRUE(parsed.ParseFromString(bytes));
  EXPECT_EQ(parsed.link_mode(), celwasm::abi::LINK_MODE_STATIC);
}

// Backward compatibility — the load-bearing property of the field.
// A Program emitted by the pre-link-mode schema carries no field-7
// bytes at all; decoding it must yield LINK_MODE_DYNAMIC, which
// matches the actual shape of every such Program.  Legacy bytes are
// hand-rolled from the proto wire format (field 1 `version`, varint
// tag 0x08, value 1) rather than serialized through the current
// schema, so the test stays honest if the current schema ever starts
// emitting field 7 unconditionally.
TEST(CelAbiEmitTest, LegacyBytesWithoutLinkModeFieldDecodeAsDynamic) {
  const std::string legacy_bytes = {0x08, 0x01};  // version = 1, nothing else
  celwasm::abi::CelAbi parsed;
  ASSERT_TRUE(parsed.ParseFromString(legacy_bytes));
  EXPECT_EQ(parsed.version(), 1u);
  EXPECT_EQ(parsed.link_mode(), celwasm::abi::LINK_MODE_DYNAMIC);
}

// Dynamic mode serializes to bytes WITHOUT a field-7 tag (proto3
// omits default-valued scalar fields), i.e. a dynamic-mode Program's
// section is byte-identical to one from the pre-link-mode schema.
// Pins that adding the field did not change the wire bytes of
// existing-shape Programs.  Asserted structurally: fields serialize
// in field-number order, so the STATIC bytes are exactly the DYNAMIC
// bytes plus the trailing field-7 varint pair (tag (7 << 3) | 0 =
// 0x38, value 1).
TEST(CelAbiEmitTest, LinkModeDynamicSerializesWithoutFieldSevenTag) {
  StaticLayout layout = LayoutWith("42", {});
  auto dynamic_abi = BuildCelAbi(layout, {}, celwasm::abi::LINK_MODE_DYNAMIC);
  auto static_abi = BuildCelAbi(layout, {}, celwasm::abi::LINK_MODE_STATIC);
  ASSERT_THAT(dynamic_abi, IsOk());
  ASSERT_THAT(static_abi, IsOk());
  std::string dynamic_bytes;
  std::string static_bytes;
  ASSERT_TRUE(dynamic_abi->SerializeToString(&dynamic_bytes));
  ASSERT_TRUE(static_abi->SerializeToString(&static_bytes));
  EXPECT_EQ(static_bytes, dynamic_bytes + std::string("\x38\x01"));
}

// Forward compatibility — wire bytes are an open set, so an unknown
// future enum value (a mode a newer compiler stamps) must pass
// through gracefully: parse succeeds, the numeric value is preserved
// (proto3 open-enum semantics), and re-serialization keeps it.  No
// CHECK / hard failure on the decode path.
TEST(CelAbiEmitTest, UnknownFutureLinkModeValueParsesAndIsPreserved) {
  // Hand-rolled wire bytes: field 7 varint tag 0x38, value 2 — an
  // enum value this schema does not define.
  const std::string future_bytes = {0x38, 0x02};
  celwasm::abi::CelAbi parsed;
  ASSERT_TRUE(parsed.ParseFromString(future_bytes));
  EXPECT_EQ(static_cast<int>(parsed.link_mode()), 2);

  std::string reserialized;
  ASSERT_TRUE(parsed.SerializeToString(&reserialized));
  celwasm::abi::CelAbi reparsed;
  ASSERT_TRUE(reparsed.ParseFromString(reserialized));
  EXPECT_EQ(static_cast<int>(reparsed.link_mode()), 2);
}

// --- BuildRequiredFunctions ----------------------------------------
//
// Unit level: fabricated import lists + real FunctionLibrary decls.
// The import-surface-equals-table invariant against a REAL compiled
// module (including the O2 unused-import drop) is pinned at the
// pipeline level in compiler/internal/compile_test.cc.

CelfnType FnScalar(CelfnType::Kind kind) {
  CelfnType t;
  t.kind = kind;
  return t;
}

FunctionLibrary HostDiscountLib() {
  return *FunctionLibrary::Builder()
              .AddHost("discount_pct", FnScalar(CelfnType::Kind::kInt),
                       {CelfnParam{/*is_receiver=*/false,
                                   FnScalar(CelfnType::Kind::kString), "s"}})
              .Build();
}

TEST(BuildRequiredFunctionsTest, EmptyImportsYieldEmptyTable) {
  const std::vector<FunctionLibrary> libs = {HostDiscountLib()};
  EXPECT_TRUE(BuildRequiredFunctions({}, libs).empty());
}

TEST(BuildRequiredFunctionsTest, HostRowCarriesAllFields) {
  const std::vector<WasmModule::FunctionImportName> imports = {
      {"cel_fn", "discount_pct_string"}};
  const std::vector<FunctionLibrary> libs = {HostDiscountLib()};
  const auto rows = BuildRequiredFunctions(imports, libs);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].overload_id(), "discount_pct_string");
  EXPECT_EQ(rows[0].fn_name(), "discount_pct");
  EXPECT_EQ(rows[0].backend(), celwasm::abi::RequiredFunction::HOST);
  ASSERT_EQ(rows[0].param_types_size(), 1);
  EXPECT_EQ(rows[0].param_types(0).kind(),
            celwasm::abi::FnType::FN_KIND_STRING);
  EXPECT_EQ(rows[0].return_type().kind(), celwasm::abi::FnType::FN_KIND_INT);
  EXPECT_FALSE(rows[0].is_receiver());
}

TEST(BuildRequiredFunctionsTest, PluginRowWithProtoParamAndReceiver) {
  CelfnType user;
  user.kind = CelfnType::Kind::kProto;
  user.proto_fqn = "acme.User";
  auto lib =
      *FunctionLibrary::Builder()
           .AddPlugin("is_adult", FnScalar(CelfnType::Kind::kBool),
                      {CelfnParam{/*is_receiver=*/true, user, "u"}})
           .Build();
  const std::vector<WasmModule::FunctionImportName> imports = {
      {"cel_fn", lib.decls()[0].overload_id}};
  const std::vector<FunctionLibrary> libs = {std::move(lib)};
  const auto rows = BuildRequiredFunctions(imports, libs);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].fn_name(), "is_adult");
  EXPECT_EQ(rows[0].backend(), celwasm::abi::RequiredFunction::PLUGIN);
  ASSERT_EQ(rows[0].param_types_size(), 1);
  EXPECT_EQ(rows[0].param_types(0).kind(), celwasm::abi::FnType::FN_KIND_PROTO);
  EXPECT_EQ(rows[0].param_types(0).proto_fqn(), "acme.User");
  EXPECT_TRUE(rows[0].is_receiver());
}

TEST(BuildRequiredFunctionsTest, NestedGenericParamMapsRecursively) {
  CelfnType map_t;
  map_t.kind = CelfnType::Kind::kMap;
  map_t.map_kv.push_back(FnScalar(CelfnType::Kind::kString));
  map_t.map_kv.push_back(FnScalar(CelfnType::Kind::kInt));
  CelfnType list_t;
  list_t.kind = CelfnType::Kind::kList;
  list_t.list_element.push_back(std::move(map_t));
  auto lib = *FunctionLibrary::Builder()
                  .AddHost("tally", FnScalar(CelfnType::Kind::kInt),
                           {CelfnParam{false, std::move(list_t), "rows"}})
                  .Build();
  const std::vector<WasmModule::FunctionImportName> imports = {
      {"cel_fn", lib.decls()[0].overload_id}};
  const std::vector<FunctionLibrary> libs = {std::move(lib)};
  const auto rows = BuildRequiredFunctions(imports, libs);
  ASSERT_EQ(rows.size(), 1u);
  const auto& param = rows[0].param_types(0);
  ASSERT_EQ(param.kind(), celwasm::abi::FnType::FN_KIND_LIST);
  ASSERT_EQ(param.params_size(), 1);
  ASSERT_EQ(param.params(0).kind(), celwasm::abi::FnType::FN_KIND_MAP);
  ASSERT_EQ(param.params(0).params_size(), 2);
  EXPECT_EQ(param.params(0).params(0).kind(),
            celwasm::abi::FnType::FN_KIND_STRING);
  EXPECT_EQ(param.params(0).params(1).kind(),
            celwasm::abi::FnType::FN_KIND_INT);
}

TEST(BuildRequiredFunctionsTest, NonCelFnImportsContributeNothing) {
  const std::vector<WasmModule::FunctionImportName> imports = {
      {"cel", "arena_reset"},
      {"cel", "cel_int_add_at_vv"},
      {"cel_host", "cel_get_field"},
      {"scorer", "user_alias"},  // a kCelDefined per-module alias
  };
  const std::vector<FunctionLibrary> libs = {HostDiscountLib()};
  EXPECT_TRUE(BuildRequiredFunctions(imports, libs).empty());
}

TEST(BuildRequiredFunctionsTest, RowsFollowImportOrderNotDeclOrder) {
  auto lib = *FunctionLibrary::Builder()
                  .AddHost("first", FnScalar(CelfnType::Kind::kBool),
                           {CelfnParam{false,
                                       FnScalar(CelfnType::Kind::kString),
                                       "s"}})
                  .AddHost("second", FnScalar(CelfnType::Kind::kBool),
                           {CelfnParam{false, FnScalar(CelfnType::Kind::kInt),
                                       "i"}})
                  .Build();
  // Import order deliberately reversed vs decl order.
  const std::vector<WasmModule::FunctionImportName> imports = {
      {"cel_fn", "second_int"}, {"cel_fn", "first_string"}};
  const std::vector<FunctionLibrary> libs = {std::move(lib)};
  const auto rows = BuildRequiredFunctions(imports, libs);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].overload_id(), "second_int");
  EXPECT_EQ(rows[1].overload_id(), "first_string");
}

TEST(BuildRequiredFunctionsTest, DeclFoundAcrossMultipleLibraries) {
  auto other = *FunctionLibrary::Builder()
                    .AddHost("unrelated", FnScalar(CelfnType::Kind::kBool),
                             {CelfnParam{false,
                                         FnScalar(CelfnType::Kind::kBool),
                                         "b"}})
                    .Build();
  const std::vector<WasmModule::FunctionImportName> imports = {
      {"cel_fn", "discount_pct_string"}};
  const std::vector<FunctionLibrary> libs = {std::move(other),
                                             HostDiscountLib()};
  const auto rows = BuildRequiredFunctions(imports, libs);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].fn_name(), "discount_pct");
}

// A `cel_fn` import that no registered library declares is an
// invariant violation — codegen installed the import FROM those
// libraries — and must crash at the emit site, not ship a
// half-empty row.
TEST(BuildRequiredFunctionsDeathTest, UnmatchedCelFnImportChecks) {
  const std::vector<WasmModule::FunctionImportName> imports = {
      {"cel_fn", "phantom_string"}};
  const std::vector<FunctionLibrary> libs = {};
  EXPECT_DEATH(BuildRequiredFunctions(imports, libs), "phantom_string");
}

}  // namespace
}  // namespace celwasm
