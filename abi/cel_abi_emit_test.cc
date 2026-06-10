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
  // Three int variables — slots at workspace_base, +24, +48.  The
  // emitter just copies from layout.variables without re-computing,
  // so this locks both the layout pass contract AND the emitter's
  // transparent pass-through.
  StaticLayout layout = LayoutWith("x + y + z", {"x:int", "y:int", "z:int"});
  auto abi = BuildCelAbi(layout, {}, celwasm::abi::LINK_MODE_DYNAMIC);
  ASSERT_THAT(abi, IsOk());
  ASSERT_EQ(abi->variables_size(), 3);
  EXPECT_EQ(abi->variables(0).slot_offset(), layout.workspace_base);
  EXPECT_EQ(abi->variables(1).slot_offset(), layout.workspace_base + 24u);
  EXPECT_EQ(abi->variables(2).slot_offset(), layout.workspace_base + 48u);
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

}  // namespace
}  // namespace celwasm
