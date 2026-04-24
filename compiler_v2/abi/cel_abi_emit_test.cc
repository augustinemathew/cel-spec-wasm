// Tests for BuildCelAbi: given a StaticLayout with a populated
// `variables` vector, the emitted `CelAbi` message has one
// `VariableEntry` per variable with name, local_index, slot_offset,
// and repr copied across.

#include "compiler_v2/abi/cel_abi_emit.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/status/status_matchers.h"
#include "compiler_v2/abi/cel_abi.pb.h"
#include "compiler_v2/codegen/layout_pass.h"
#include "compiler_v2/codegen/resolve_pass.h"
#include "compiler_v2/frontend/parse_and_check.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/ir/typed_ast.h"
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
  auto abi = BuildCelAbi(layout);
  ASSERT_THAT(abi, IsOk());
  EXPECT_EQ(abi->version(), 1u);
  EXPECT_EQ(abi->variables_size(), 0);
  EXPECT_EQ(abi->fields_size(), 0);
  EXPECT_EQ(abi->attributes_size(), 0);
}

TEST(CelAbiEmitTest, SingleIntVariableRoundTrips) {
  StaticLayout layout = LayoutWith("x", {"x:int"});
  auto abi = BuildCelAbi(layout);
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
  auto abi = BuildCelAbi(layout);
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
  auto abi = BuildCelAbi(layout);
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
  auto abi = BuildCelAbi(layout);
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

}  // namespace
}  // namespace celwasm
