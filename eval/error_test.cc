#include "compiler_v2/api/error.h"

#include "gtest/gtest.h"

namespace celwasm::api {
namespace {

TEST(ErrorCodeTest, NamesCoverEveryCode) {
  EXPECT_EQ(ErrorCodeName(ErrorCode::kOverflow), "overflow");
  EXPECT_EQ(ErrorCodeName(ErrorCode::kDivideByZero), "divide_by_zero");
  EXPECT_EQ(ErrorCodeName(ErrorCode::kModulusByZero), "modulus_by_zero");
  EXPECT_EQ(ErrorCodeName(ErrorCode::kTypeMismatch), "type_mismatch");
  EXPECT_EQ(ErrorCodeName(ErrorCode::kFieldNotFound), "field_not_found");
  EXPECT_EQ(ErrorCodeName(ErrorCode::kKeyNotFound), "key_not_found");
  EXPECT_EQ(ErrorCodeName(ErrorCode::kIndexOutOfBounds), "index_out_of_bounds");
  EXPECT_EQ(ErrorCodeName(ErrorCode::kUnknownType), "unknown_type");
  EXPECT_EQ(ErrorCodeName(ErrorCode::kCustomFnFailed), "custom_fn_failed");
  EXPECT_EQ(ErrorCodeName(ErrorCode::kHostAdapterError), "host_adapter_error");
  EXPECT_EQ(ErrorCodeName(ErrorCode::kTimeout), "timeout");
}

TEST(ErrorPayloadTest, DefaultsAreWellFormed) {
  ErrorPayload p;
  EXPECT_EQ(p.code, ErrorCode::kHostAdapterError);
  EXPECT_EQ(p.message, "");
  EXPECT_EQ(p.expr_id, 0u);
}

TEST(ErrorPayloadTest, AggregateInitHonoursFields) {
  ErrorPayload p{
      .code = ErrorCode::kDivideByZero, .message = "no zero", .expr_id = 17};
  EXPECT_EQ(p.code, ErrorCode::kDivideByZero);
  EXPECT_EQ(p.message, "no zero");
  EXPECT_EQ(p.expr_id, 17u);
}

}  // namespace
}  // namespace celwasm::api
