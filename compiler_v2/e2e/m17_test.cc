// M17 e2e test suite — the `encoders` extension functions
// (cel-cpp's `extensions/encoders.cc`) lit up end-to-end through
// Compiler::Compile → Engine::Plan → Instance::Eval.  Source
// expressions match the conformance rows from
// `tests/simple/testdata/encoders_ext.textproto` verbatim — a
// regression here surfaces before the conformance run.
//
// Two functions, sliced into three fixtures per
// `rewrite/m17-encoders-ext.md` §5.2:
//
//   - EncodeE2ETest     base64.encode(bytes) -> string
//   - DecodeE2ETest     base64.decode(string) -> bytes
//   - RoundTripE2ETest  decode(encode(b)) == b

#include <string>

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/value.h"
#include "gtest/gtest.h"

namespace celwasm::api {
namespace {

Engine& GlobalEngine() {
  static Engine* engine = [] {
    auto e = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(e);
    return new Engine(*std::move(e));
  }();
  return *engine;
}

Instance CompilePlan(const Compiler& compiler, absl::string_view source) {
  auto program = compiler.Compile(source);
  ABSL_CHECK_OK(program) << source;
  auto instance = GlobalEngine().Plan(*program);
  ABSL_CHECK_OK(instance) << source;
  return *std::move(instance);
}

Value EvalOk(Instance& instance, const Activation& activation) {
  auto v = instance.Eval(activation);
  ABSL_CHECK_OK(v);
  return *std::move(v);
}

// base64.encode(...) yields a STRING.
std::string EvalString(absl::string_view source) {
  Compiler::Builder b;
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler);
  auto instance = CompilePlan(*compiler, source);
  Activation a;
  auto v = EvalOk(instance, a);
  ABSL_CHECK(v.kind() == Value::Kind::kString)
      << source << " kind=" << static_cast<int>(v.kind());
  return std::string(*v.AsString());
}

// base64.decode(...) yields BYTES.  Returned as std::string so the
// test can hold arbitrary (possibly non-UTF-8) byte sequences.
std::string EvalBytes(absl::string_view source) {
  Compiler::Builder b;
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler);
  auto instance = CompilePlan(*compiler, source);
  Activation a;
  auto v = EvalOk(instance, a);
  ABSL_CHECK(v.kind() == Value::Kind::kBytes)
      << source << " kind=" << static_cast<int>(v.kind());
  return std::string(*v.AsBytes());
}

// ──────────────────────────────────────────────────────────────
// EncodeE2ETest — base64.encode(bytes) -> string.
// ──────────────────────────────────────────────────────────────

class EncodeE2ETest : public ::testing::Test {};

TEST_F(EncodeE2ETest, Hello) {
  // encoders_ext.textproto::encode/hello.
  EXPECT_EQ(EvalString(R"(base64.encode(b'hello'))"), "aGVsbG8=");
}

TEST_F(EncodeE2ETest, Empty) {
  EXPECT_EQ(EvalString(R"(base64.encode(b''))"), "");
}

TEST_F(EncodeE2ETest, HelloWorld) {
  EXPECT_EQ(EvalString(R"(base64.encode(b'Hello World!'))"),
            "SGVsbG8gV29ybGQh");
}

TEST_F(EncodeE2ETest, PaddingShapes) {
  // 3-byte input → no padding; 2 bytes → one '='; 1 byte → two '='.
  EXPECT_EQ(EvalString(R"(base64.encode(b'abc'))"), "YWJj");
  EXPECT_EQ(EvalString(R"(base64.encode(b'ab'))"), "YWI=");
  EXPECT_EQ(EvalString(R"(base64.encode(b'a'))"), "YQ==");
}

// ──────────────────────────────────────────────────────────────
// DecodeE2ETest — base64.decode(string) -> bytes.
// ──────────────────────────────────────────────────────────────

class DecodeE2ETest : public ::testing::Test {};

TEST_F(DecodeE2ETest, Hello) {
  // encoders_ext.textproto::decode/hello.
  EXPECT_EQ(EvalBytes(R"(base64.decode('aGVsbG8='))"), "hello");
}

TEST_F(DecodeE2ETest, HelloWithoutPadding) {
  // encoders_ext.textproto::decode/hello_without_padding — the
  // load-bearing unpadded-input contract (§7 risk).
  EXPECT_EQ(EvalBytes(R"(base64.decode('aGVsbG8'))"), "hello");
}

TEST_F(DecodeE2ETest, Empty) {
  EXPECT_EQ(EvalBytes(R"(base64.decode(''))"), "");
}

// ──────────────────────────────────────────────────────────────
// RoundTripE2ETest — decode(encode(b)) == b.
// ──────────────────────────────────────────────────────────────

class RoundTripE2ETest : public ::testing::Test {};

TEST_F(RoundTripE2ETest, HelloWorld) {
  // encoders_ext.textproto::round_trip/hello.
  EXPECT_EQ(EvalBytes(R"(base64.decode(base64.encode(b'Hello World!')))"),
            "Hello World!");
}

TEST_F(RoundTripE2ETest, EmptyRoundTrips) {
  EXPECT_EQ(EvalBytes(R"(base64.decode(base64.encode(b'')))"), "");
}

}  // namespace
}  // namespace celwasm::api
