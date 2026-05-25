// M12 e2e test suite — the `string_ext` extension functions
// (cel-cpp's `extensions/strings.cc`) lit up end-to-end through
// Compiler::Compile → Engine::Plan → Instance::Eval.  Source
// expressions match conformance rows from
// `tests/simple/testdata/string_ext.textproto` where possible —
// each section samples 1-3 rows so a regression here surfaces
// before the conformance run.
//
// Sliced by function family per `rewrite/m12-string-ext.md` §6:
//
//   - CodePointE2ETest      Slice A — charAt / lowerAscii /
//                                     upperAscii / trim / reverse
//   - SearchE2ETest         Slice B — indexOf / lastIndexOf /
//                                     substring / replace
//   - ListBridgeE2ETest     Slice C — split / join
//   - QuoteE2ETest          Slice D — quote
//   - FormatE2ETest         Slice E — format directive renderer

#include <string>

#include "absl/log/absl_check.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "eval/activation.h"
#include "compiler/compiler.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "compiler/program.h"
#include "eval/value.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

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

bool EvalBool(absl::string_view source) {
  Compiler::Builder b;
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler);
  auto instance = CompilePlan(*compiler, source);
  Activation a;
  auto v = EvalOk(instance, a);
  ABSL_CHECK(v.kind() == Value::Kind::kBool)
      << source << " kind=" << static_cast<int>(v.kind());
  return *v.AsBool();
}

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

// ──────────────────────────────────────────────────────────────
// CodePointE2ETest — Slice A coverage.
// ──────────────────────────────────────────────────────────────

class CodePointE2ETest : public ::testing::Test {};

TEST_F(CodePointE2ETest, CharAtBasic) {
  EXPECT_EQ(EvalString(R"("hello".charAt(1))"), "e");
}

TEST_F(CodePointE2ETest, CharAtEndOfString) {
  // Spec contract: i == codepoint_count(s) returns "" (sentinel).
  EXPECT_EQ(EvalString(R"("hello".charAt(5))"), "");
}

TEST_F(CodePointE2ETest, LowerAscii) {
  EXPECT_EQ(EvalString(R"("HELLO World".lowerAscii())"), "hello world");
}

TEST_F(CodePointE2ETest, UpperAscii) {
  EXPECT_EQ(EvalString(R"("hello World".upperAscii())"), "HELLO WORLD");
}

TEST_F(CodePointE2ETest, Trim) {
  EXPECT_EQ(EvalString(R"("  spaces around  ".trim())"), "spaces around");
}

TEST_F(CodePointE2ETest, Reverse) {
  EXPECT_EQ(EvalString(R"("hello".reverse())"), "olleh");
}

TEST_F(CodePointE2ETest, ReverseMultiByteUtf8) {
  // 🐱😀😛 → reversed code-point order, each emoji's 4 bytes stays
  // together.
  EXPECT_EQ(EvalString(R"("🐱😀😛".reverse())"), "😛😀🐱");
}

// ──────────────────────────────────────────────────────────────
// SearchE2ETest — Slice B coverage.
// ──────────────────────────────────────────────────────────────

class SearchE2ETest : public ::testing::Test {};

TEST_F(SearchE2ETest, IndexOf) {
  EXPECT_TRUE(EvalBool(R"("hello world".indexOf("world") == 6)"));
}

TEST_F(SearchE2ETest, IndexOfMiss) {
  EXPECT_TRUE(EvalBool(R"("hello".indexOf("Z") == -1)"));
}

TEST_F(SearchE2ETest, IndexOfWithPos) {
  // Two occurrences of "ll"; starting at pos 5 should miss.
  EXPECT_TRUE(EvalBool(R"("hello world".indexOf("o", 5) == 7)"));
}

TEST_F(SearchE2ETest, LastIndexOf) {
  EXPECT_TRUE(EvalBool(R"("hello world".lastIndexOf("o") == 7)"));
}

TEST_F(SearchE2ETest, Substring) {
  EXPECT_EQ(EvalString(R"("hello world".substring(6))"), "world");
}

TEST_F(SearchE2ETest, SubstringRange) {
  EXPECT_EQ(EvalString(R"("hello world".substring(0, 5))"), "hello");
}

TEST_F(SearchE2ETest, Replace) {
  EXPECT_EQ(EvalString(R"("hello world".replace("world", "there"))"),
            "hello there");
}

TEST_F(SearchE2ETest, ReplaceN) {
  EXPECT_EQ(EvalString(R"("aaa".replace("a", "b", 2))"), "bba");
}

// ──────────────────────────────────────────────────────────────
// ListBridgeE2ETest — Slice C coverage.
// ──────────────────────────────────────────────────────────────

class ListBridgeE2ETest : public ::testing::Test {};

TEST_F(ListBridgeE2ETest, SplitJoinRoundTrip) {
  EXPECT_EQ(EvalString(R"("a,b,c".split(",").join("|"))"), "a|b|c");
}

TEST_F(ListBridgeE2ETest, JoinEmptyList) {
  EXPECT_EQ(EvalString(R"([].join(","))"), "");
}

TEST_F(ListBridgeE2ETest, JoinSimple) {
  EXPECT_EQ(EvalString(R"(["x", "y", "z"].join("-"))"), "x-y-z");
}

TEST_F(ListBridgeE2ETest, SplitN) {
  EXPECT_TRUE(EvalBool(R"("a,b,c,d".split(",", 2).size() == 2)"));
}

// ──────────────────────────────────────────────────────────────
// QuoteE2ETest — Slice D coverage.
// ──────────────────────────────────────────────────────────────

class QuoteE2ETest : public ::testing::Test {};

TEST_F(QuoteE2ETest, Verbatim) {
  // strings.quote("verbatim") == "\"verbatim\""
  EXPECT_TRUE(EvalBool(R"(strings.quote("verbatim") == "\"verbatim\"")"));
}

TEST_F(QuoteE2ETest, EscapesNewline) {
  EXPECT_TRUE(EvalBool(R"(strings.quote("a\nb") == "\"a\\nb\"")"));
}

TEST_F(QuoteE2ETest, EscapesBackslash) {
  EXPECT_TRUE(EvalBool(R"(strings.quote("\\") == "\"\\\\\"")"));
}

TEST_F(QuoteE2ETest, Empty) {
  EXPECT_EQ(EvalString(R"(strings.quote(""))"), "\"\"");
}

// ──────────────────────────────────────────────────────────────
// FormatE2ETest — Slice E coverage.  Mirrors representative rows
// from `string_ext.textproto::format` (78 rows; pick 8 for the
// e2e gate, the rest get hit by conformance).
// ──────────────────────────────────────────────────────────────

class FormatE2ETest : public ::testing::Test {};

TEST_F(FormatE2ETest, NoSubstitution) {
  EXPECT_EQ(EvalString(R"("no substitution".format([]))"), "no substitution");
}

TEST_F(FormatE2ETest, MidStringSubstitution) {
  EXPECT_EQ(EvalString(R"("str is %s and some more".format(["filler"]))"),
            "str is filler and some more");
}

TEST_F(FormatE2ETest, PercentEscaping) {
  EXPECT_EQ(EvalString(R"("%% and also %%".format([]))"), "% and also %");
}

TEST_F(FormatE2ETest, DecimalAndString) {
  EXPECT_EQ(EvalString(R"("%d %s".format([42, "hello"]))"), "42 hello");
}

TEST_F(FormatE2ETest, FixedDefaultPrecision) {
  EXPECT_EQ(EvalString(R"("%f".format([2.71828]))"), "2.718280");
}

TEST_F(FormatE2ETest, BinaryFromInt) {
  EXPECT_EQ(EvalString(R"("%b".format([5]))"), "101");
}

TEST_F(FormatE2ETest, HexLowerFromInt) {
  EXPECT_EQ(EvalString(R"("%x".format([30]))"), "1e");
}

TEST_F(FormatE2ETest, HexLowerFromString) {
  EXPECT_EQ(EvalString(R"("%x".format(["Hello world!"]))"),
            "48656c6c6f20776f726c6421");
}

TEST_F(FormatE2ETest, StringFromList) {
  EXPECT_EQ(EvalString(R"("%s".format([["abc", 3.14, null]]))"),
            "[abc, 3.14, null]");
}

// ──────────────────────────────────────────────────────────────
// MultiFunctionE2ETest — chained calls across families.
// ──────────────────────────────────────────────────────────────

class MultiFunctionE2ETest : public ::testing::Test {};

TEST_F(MultiFunctionE2ETest, SplitJoinFormat) {
  EXPECT_EQ(EvalString(R"("a,b,c".split(",").join("-").upperAscii())"),
            "A-B-C");
}

TEST_F(MultiFunctionE2ETest, FormatWithReceiverChain) {
  EXPECT_EQ(EvalString(R"("count=%d".format(["hello".size()]))"), "count=5");
}

}  // namespace
}  // namespace celwasm
