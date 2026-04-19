#include "compiler/codegen/module.h"

#include <cstdint>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "binaryen-c.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;

// A module that is empty but for a nop function is the degenerate case
// Binaryen accepts.  `Validate()` should agree and `Serialize()` should
// yield the canonical 8-byte preamble.
TEST(WasmModuleTest, EmptyModuleValidatesAndSerializesPreamble) {
  WasmModule m;
  BinaryenExpressionRef body = BinaryenNop(m.raw());
  m.AddFunction("nop", {}, BinaryenTypeNone(), {}, body);

  EXPECT_THAT(m.Validate(), IsOk());

  auto bytes_or = m.Serialize();
  ASSERT_THAT(bytes_or, IsOk());
  const auto& bytes = *bytes_or;
  ASSERT_GE(bytes.size(), 8u);
  // \0asm magic + version 1.
  EXPECT_EQ(bytes[0], 0x00);
  EXPECT_EQ(bytes[1], 0x61);
  EXPECT_EQ(bytes[2], 0x73);
  EXPECT_EQ(bytes[3], 0x6D);
  EXPECT_EQ(bytes[4], 0x01);
  EXPECT_EQ(bytes[5], 0x00);
  EXPECT_EQ(bytes[6], 0x00);
  EXPECT_EQ(bytes[7], 0x00);
}

TEST(WasmModuleTest, MoveConstructTransfersOwnership) {
  WasmModule src;
  BinaryenExpressionRef body = BinaryenNop(src.raw());
  src.AddFunction("f", {}, BinaryenTypeNone(), {}, body);

  WasmModule dst(std::move(src));
  EXPECT_THAT(dst.Validate(), IsOk());
  // Moved-from module holds nullptr internally; it should not be used
  // further, but its destructor must still be safe.  The smoke test
  // is that this test doesn't crash on scope exit.
}

TEST(WasmModuleTest, MoveAssignDisposesPrevious) {
  WasmModule a, b;
  BinaryenExpressionRef body_a = BinaryenNop(a.raw());
  BinaryenExpressionRef body_b = BinaryenNop(b.raw());
  a.AddFunction("fa", {}, BinaryenTypeNone(), {}, body_a);
  b.AddFunction("fb", {}, BinaryenTypeNone(), {}, body_b);

  a = std::move(b);
  EXPECT_THAT(a.Validate(), IsOk());
}

TEST(WasmModuleTest, SetMemoryExportsUnderGivenName) {
  WasmModule m;
  ASSERT_THAT(m.SetMemory(/*initial_pages=*/1, std::nullopt, "memory"),
              IsOk());
  EXPECT_TRUE(BinaryenHasMemory(m.raw()));
  EXPECT_THAT(m.Validate(), IsOk());
  // Exported under the configured name.  Binaryen enumerates exports
  // in insertion order.
  ASSERT_EQ(BinaryenGetNumExports(m.raw()), 1u);
  BinaryenExportRef exp = BinaryenGetExportByIndex(m.raw(), 0);
  EXPECT_EQ(BinaryenExportGetKind(exp), BinaryenExternalMemory());
  EXPECT_STREQ(BinaryenExportGetName(exp), "memory");
}

TEST(WasmModuleTest, SetMemoryTwiceFailsPrecondition) {
  WasmModule m;
  ASSERT_THAT(m.SetMemory(1, std::nullopt, ""), IsOk());
  EXPECT_THAT(m.SetMemory(1, std::nullopt, ""),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST(WasmModuleTest, SetMemoryMaxLessThanInitialIsInvalid) {
  WasmModule m;
  EXPECT_THAT(m.SetMemory(4, /*max_pages=*/2, ""),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(WasmModuleTest, AddCelRefsTableIsExternrefTyped) {
  WasmModule m;
  ASSERT_THAT(m.AddCelRefsTable("$cel_refs", /*initial_slots=*/8, 1024),
              IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenTableRef tab = BinaryenGetTable(m.raw(), "$cel_refs");
  ASSERT_NE(tab, nullptr);
  EXPECT_EQ(BinaryenTableGetInitial(tab), 8u);
  EXPECT_EQ(BinaryenTableGetMax(tab), 1024u);
  EXPECT_EQ(BinaryenTableGetType(tab), BinaryenTypeExternref());
}

TEST(WasmModuleTest, AddCelRefsTableMaxLessThanInitialIsInvalid) {
  WasmModule m;
  EXPECT_THAT(m.AddCelRefsTable("refs", /*initial_slots=*/8, /*max=*/4),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(WasmModuleTest, FunctionImportIsCallableFromAFunction) {
  WasmModule m;
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType params_two[2] = {i32, i32};

  // import (module="cel_host", base="add") : (i32, i32) -> i32
  m.AddFunctionImport("host_add", "cel_host", "add", params_two, i32);

  // export "sum_of_consts" : () -> i32 = host_add(40, 2)
  BinaryenExpressionRef operands[2] = {
      BinaryenConst(m.raw(), BinaryenLiteralInt32(40)),
      BinaryenConst(m.raw(), BinaryenLiteralInt32(2)),
  };
  BinaryenExpressionRef body = BinaryenCall(m.raw(), "host_add",
                                            operands, 2, i32);
  m.AddFunction("sum_of_consts", {}, i32, {}, body);
  m.ExportFunction("sum_of_consts", "sum_of_consts");

  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(WasmModuleTest, AddFunctionDeclaresLocals) {
  WasmModule m;
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType locals[3] = {i32, i32, i32};

  // Body sets local 0 to i32.const 7 and returns it.
  BinaryenExpressionRef set = BinaryenLocalSet(
      m.raw(), 0, BinaryenConst(m.raw(), BinaryenLiteralInt32(7)));
  BinaryenExpressionRef get = BinaryenLocalGet(m.raw(), 0, i32);
  BinaryenExpressionRef block_children[2] = {set, get};
  BinaryenExpressionRef body = BinaryenBlock(
      m.raw(), /*name=*/nullptr, block_children, 2, i32);

  m.AddFunction("f", {}, i32, locals, body);

  BinaryenFunctionRef fn = BinaryenGetFunction(m.raw(), "f");
  ASSERT_NE(fn, nullptr);
  EXPECT_EQ(BinaryenFunctionGetNumVars(fn), 3u);
  EXPECT_EQ(BinaryenFunctionGetVar(fn, 0), i32);

  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(WasmModuleTest, ExportFunctionRegistersExternalName) {
  WasmModule m;
  const BinaryenType i32 = BinaryenTypeInt32();
  BinaryenExpressionRef body =
      BinaryenConst(m.raw(), BinaryenLiteralInt32(0));
  m.AddFunction("internal_name", {}, i32, {}, body);
  m.ExportFunction("internal_name", "publicly_visible");

  ASSERT_EQ(BinaryenGetNumExports(m.raw()), 1u);
  BinaryenExportRef exp = BinaryenGetExportByIndex(m.raw(), 0);
  EXPECT_EQ(BinaryenExportGetKind(exp), BinaryenExternalFunction());
  EXPECT_STREQ(BinaryenExportGetName(exp), "publicly_visible");
}

TEST(WasmModuleTest, ExportTableRegistersExternalName) {
  WasmModule m;
  ASSERT_THAT(m.AddCelRefsTable("$cel_refs", 4, std::nullopt), IsOk());
  m.ExportTable("$cel_refs", "cel_refs");

  ASSERT_EQ(BinaryenGetNumExports(m.raw()), 1u);
  BinaryenExportRef exp = BinaryenGetExportByIndex(m.raw(), 0);
  EXPECT_EQ(BinaryenExportGetKind(exp), BinaryenExternalTable());
  EXPECT_STREQ(BinaryenExportGetName(exp), "cel_refs");
}

// Round-trip: the eval-module shape the design doc promises — memory,
// cel_refs table, a host import, a runtime-ish import, and an exported
// eval that calls into them.  This is the template expr_lower will
// flesh out once it lands.
TEST(WasmModuleTest, FullEvalModuleShapeValidates) {
  WasmModule m;
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType exref = BinaryenTypeExternref();

  ASSERT_THAT(m.SetMemory(1, std::nullopt, "memory"), IsOk());
  ASSERT_THAT(m.AddCelRefsTable("$cel_refs", 16, 1024), IsOk());

  // Scalar-field accessor: (externref, i32, i32) -> i32.
  const BinaryenType get_scalar_params[3] = {exref, i32, i32};
  m.AddFunctionImport("cel_host.get_scalar_field",
                      "cel_host", "get_scalar_field",
                      get_scalar_params, i32);

  // Runtime allocator (mirrors compiler/runtime/cel_runtime.c's
  // cel_alloc): i32 -> i32.
  const BinaryenType alloc_params[1] = {i32};
  m.AddFunctionImport("cel_alloc", "cel_runtime", "cel_alloc",
                      alloc_params, i32);

  // eval(msg: externref, type_id: i32, field_id: i32) -> i32 =
  //   cel_host.get_scalar_field(msg, type_id, field_id)
  const BinaryenType eval_params[3] = {exref, i32, i32};
  BinaryenExpressionRef args[3] = {
      BinaryenLocalGet(m.raw(), 0, exref),
      BinaryenLocalGet(m.raw(), 1, i32),
      BinaryenLocalGet(m.raw(), 2, i32),
  };
  BinaryenExpressionRef eval_body = BinaryenCall(
      m.raw(), "cel_host.get_scalar_field", args, 3, i32);
  m.AddFunction("eval", eval_params, i32, {}, eval_body);
  m.ExportFunction("eval", "eval");

  EXPECT_THAT(m.Validate(), IsOk());
  auto bytes = m.Serialize();
  ASSERT_THAT(bytes, IsOk());
  EXPECT_GT(bytes->size(), 8u);
}

// ---- TupleType free function ------------------------------------------------

TEST(TupleTypeTest, EmptyIsNone) {
  EXPECT_EQ(TupleType({}), BinaryenTypeNone());
}

TEST(TupleTypeTest, SingleElementPassesThrough) {
  EXPECT_EQ(TupleType({BinaryenTypeInt64()}), BinaryenTypeInt64());
}

TEST(TupleTypeTest, MultipleElementsFormATuple) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType i64 = BinaryenTypeInt64();
  const BinaryenType parts[3] = {i32, i64, i32};
  BinaryenType tup = TupleType(parts);
  EXPECT_EQ(BinaryenTypeArity(tup), 3u);
  BinaryenType expanded[3];
  BinaryenTypeExpand(tup, expanded);
  EXPECT_EQ(expanded[0], i32);
  EXPECT_EQ(expanded[1], i64);
  EXPECT_EQ(expanded[2], i32);
}

}  // namespace
}  // namespace celwasm
