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
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

// `GlobalEngine`, `CompilePlan`, and `EvalOk` come from the shared
// link-mode-aware e2e helpers — pulling them in here means this
// fixture runs under both kDynamic and kStatic when built via the
// `link_mode_e2e_cc_test` bazel macro.
using ::celwasm::e2e::CompilePlan;
using ::celwasm::e2e::EvalOk;

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

// The two-argument `lastIndexOf` has its own runtime kernel
// (`cel_string_last_index_of_at_vvv`) from the one-argument form; only
// the conformance corpus was reaching it.  Value pinned against
// cel-cpp by cel_cpp_oracle_test's LastIndexOfAgrees.
TEST_F(SearchE2ETest, LastIndexOfWithPos) {
  // Two `b`s at 1 and 3; searching back from 1 finds the first.
  EXPECT_TRUE(EvalBool(R"("abcb".lastIndexOf("b", 1) == 1)"));
  EXPECT_TRUE(EvalBool(R"("abcb".lastIndexOf("b") == 3)"));
}

// Multi-byte code points take the `Utf8DecodeMulti` path, which the
// ASCII fixtures above never reach: `size()` counts code points rather
// than bytes, and `charAt` indexes by code point.  Pinned by
// cel_cpp_oracle_test's MultiByteUtf8Agrees.
TEST_F(CodePointE2ETest, MultiByteCodePoints) {
  EXPECT_TRUE(EvalBool(R"(size("héllo") == 5)"));
  EXPECT_EQ(EvalString(R"("héllo".charAt(1))"), "é");
}

// Position-bounded search over MULTI-BYTE text walks the code-point
// loop rather than the byte fast path, and a negative position is an
// error for both directions.  `indexOf` used to clamp a negative pos
// and return 0 -- an earlier reading of cel-cpp's IndexOf3 took its
// missing `pos < 0` check at face value, missing that the comparison
// against an unsigned Size() rejects negatives anyway.  Pinned by
// cel_cpp_oracle_test's SearchPositionEdgesAgree.
TEST_F(SearchE2ETest, PositionBoundedSearchEdges) {
  EXPECT_TRUE(EvalBool(R"("héllo".indexOf("l", 1) == 2)"));
  EXPECT_TRUE(EvalBool(R"("héllo".lastIndexOf("l", 3) == 3)"));
  for (const absl::string_view source :
       {R"("abc".indexOf("a", -1))", R"("abc".lastIndexOf("a", -1))"}) {
    Compiler::Builder b;
    auto compiler = std::move(b).Build();
    ASSERT_TRUE(compiler.ok()) << compiler.status();
    auto instance = CompilePlan(*compiler, source);
    Activation a;
    auto v = instance.Eval(a);
    ASSERT_TRUE(v.ok()) << source << ": " << v.status();
    EXPECT_TRUE(v->IsError())
        << source << " kind=" << static_cast<int>(v->kind());
  }
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

// The escape switch has one arm per C-style control character plus the
// double quote; the existing rows only drove backslash, so the rest
// were reached solely by the conformance corpus.  All eight are pinned
// against cel-cpp by cel_cpp_oracle_test's StringsQuoteEscapesAgree.
TEST_F(QuoteE2ETest, EscapesEveryControlCharacter) {
  EXPECT_EQ(EvalString(R"(strings.quote("a\tb"))"), R"("a\tb")");
  EXPECT_EQ(EvalString(R"(strings.quote("a\nb"))"), R"("a\nb")");
  EXPECT_EQ(EvalString(R"(strings.quote("a\rb"))"), R"("a\rb")");
  EXPECT_EQ(EvalString(R"(strings.quote("a\bb"))"), R"("a\bb")");
  EXPECT_EQ(EvalString(R"(strings.quote("a\fb"))"), R"("a\fb")");
  EXPECT_EQ(EvalString(R"(strings.quote("a\vb"))"), R"("a\vb")");
  EXPECT_EQ(EvalString(R"(strings.quote("a\ab"))"), R"("a\ab")");
  EXPECT_EQ(EvalString(R"(strings.quote("q\"z"))"), R"("q\"z")");
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

// Out-of-range code-point and substring indices are CEL errors, not
// traps — they route through the string-ext kernels' shared `Poison`
// helper, which no e2e row was reaching.  Agreement with cel-cpp on
// all three is pinned by cel_cpp_oracle_test's
// StringExtRangeErrorsAgree.
TEST_F(CodePointE2ETest, OutOfRangeIndicesAreErrors) {
  for (const absl::string_view src : {R"("abc".charAt(9))",
                                      R"("abc".substring(9))",
                                      R"("abc".substring(2, 1))"}) {
    Compiler::Builder b;
    auto compiler = std::move(b).Build();
    ASSERT_TRUE(compiler.ok()) << compiler.status();
    auto instance = CompilePlan(*compiler, src);
    Activation a;
    auto v = instance.Eval(a);
    ASSERT_TRUE(v.ok()) << src << ": " << v.status();
    EXPECT_TRUE(v->IsError())
        << src << " kind=" << static_cast<int>(v->kind());
  }
}

// `%e` (scientific) and `%o` (octal) each have their own renderer;
// only the conformance corpus was reaching them.  Values pinned
// against cel-cpp by cel_cpp_oracle_test's
// ScientificAndOctalFormatAgree.
TEST_F(FormatE2ETest, ScientificAndOctalVerbs) {
  EXPECT_EQ(EvalString(R"("%e".format([1.5]))"), "1.500000e+00");
  EXPECT_EQ(EvalString(R"("%o".format([8]))"), "10");
}

// `%s` over a timestamp and over a type name each have their own
// append helper alongside the duration one.  Pinned by
// cel_cpp_oracle_test's TimestampAndTypeFormatAgree.
TEST_F(FormatE2ETest, TimestampAndTypeVerbs) {
  EXPECT_EQ(EvalString(R"("%s".format([timestamp(0)]))"),
            "1970-01-01T00:00:00Z");
  EXPECT_EQ(EvalString(R"("%s".format([type(1)]))"), "int");
}

// Every format verb dispatches per operand kind.  The suite only ever
// drove the int / double / string arms, so the uint, bool and bytes
// branches -- and the NaN / +-Infinity special cases shared by the
// fixed and scientific renderers -- were reached solely by the
// conformance corpus.  All of these are pinned against cel-cpp by
// cel_cpp_oracle_test's FormatVerbOperandKindsAgree and
// FormatNonFiniteDoublesAgree.
//
// `%f` / `%e` over an INTEGER operand is deliberately absent: we
// coerce and render where cel-cpp errors.  That divergence is pinned
// as CELW-0019 in e2e/known_bugs_test.cc.
TEST_F(FormatE2ETest, VerbsAcrossOperandKinds) {
  EXPECT_EQ(EvalString(R"("%s".format([true]))"), "true");
  EXPECT_EQ(EvalString(R"("%s".format([1u]))"), "1");
  EXPECT_EQ(EvalString(R"("%s".format([b"ab"]))"), "ab");
  EXPECT_EQ(EvalString(R"("%d".format([1u]))"), "1");
  // `%d` legitimately accepts a double (oracle-confirmed).
  EXPECT_EQ(EvalString(R"("%d".format([1.7]))"), "1.700000");
  EXPECT_EQ(EvalString(R"("%b".format([5u]))"), "101");
  EXPECT_EQ(EvalString(R"("%b".format([true]))"), "1");
  EXPECT_EQ(EvalString(R"("%o".format([8u]))"), "10");
  EXPECT_EQ(EvalString(R"("%x".format([255u]))"), "ff");
  EXPECT_EQ(EvalString(R"("%x".format([b"ab"]))"), "6162");
  EXPECT_EQ(EvalString(R"("%X".format([255u]))"), "FF");
}

TEST_F(FormatE2ETest, NonFiniteDoubles) {
  EXPECT_EQ(EvalString(R"("%f".format([double("inf")]))"), "Infinity");
  EXPECT_EQ(EvalString(R"("%f".format([double("nan")]))"), "NaN");
  EXPECT_EQ(EvalString(R"("%e".format([double("-inf")]))"), "-Infinity");
  EXPECT_EQ(EvalString(R"("%s".format([double("inf")]))"), "Infinity");
}

// `%s` over a Duration uses the canonical proto-JSON form
// (cel_string_format_render.cc AppendDurationCanonical): zero renders
// "0s"; the sign is a single leading "-" for a negative whole or
// fractional part; and the fraction width steps 3 / 6 / 9 digits
// depending on the lowest non-zero place, rather than being stripped.
TEST_F(FormatE2ETest, DurationCanonicalForms) {
  EXPECT_EQ(EvalString(R"("%s".format([duration("0s")]))"), "0s");
  EXPECT_EQ(EvalString(R"("%s".format([duration("90s")]))"), "90s");
  EXPECT_EQ(EvalString(R"("%s".format([duration("-90s")]))"), "-90s");
  // millisecond place -> 3 digits
  EXPECT_EQ(EvalString(R"("%s".format([duration("1.500s")]))"), "1.500s");
  // microsecond place -> 6 digits
  EXPECT_EQ(EvalString(R"("%s".format([duration("0.000500s")]))"), "0.000500s");
  // nanosecond place -> 9 digits
  EXPECT_EQ(EvalString(R"("%s".format([duration("0.000000001s")]))"),
            "0.000000001s");
  // a negative fraction takes the same single leading sign
  EXPECT_EQ(EvalString(R"("%s".format([duration("-0.500s")]))"), "-0.500s");
}

// Malformed format strings and argument-kind mismatches surface as
// CEL errors from the format kernel (cel_string_format.cc), not as
// traps.  Diagnostic strings track cel-cpp's ParsePrecision.
TEST_F(FormatE2ETest, MalformedFormatAndArgMismatchAreErrors) {
  const absl::string_view kErrorSources[] = {
      // `%` with nothing after it — ParsePrecision's end-of-string arm.
      R"("%".format([]))",
      // `.` with no digits, and digits running to the end — both are
      // "unable to find end of precision specifier".
      R"("%.".format([1]))",
      R"("%.2".format([1]))",
      // Argument kind does not match the directive.
      R"("%d".format(["not an int"]))",
      R"("%s and %d".format(["a"]))",
  };
  for (const absl::string_view src : kErrorSources) {
    Compiler::Builder b;
    auto compiler = std::move(b).Build();
    ASSERT_TRUE(compiler.ok()) << compiler.status();
    auto instance = CompilePlan(*compiler, src);
    Activation a;
    auto v = instance.Eval(a);
    ASSERT_TRUE(v.ok()) << src << ": " << v.status();
    EXPECT_TRUE(v->IsError())
        << src << " kind=" << static_cast<int>(v->kind());
  }
}

// An empty result commits through CommitResult's empty-buffer arm,
// which writes a zero-length CEL_STRING rather than allocating.
TEST_F(FormatE2ETest, EmptyResultCommitsAsEmptyString) {
  EXPECT_EQ(EvalString(R"("".format([]))"), "");
  EXPECT_EQ(EvalString(R"("%s".format([""]))"), "");
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

// Empty-needle replace interleaves the replacement BEFORE each code
// point (plus one trailing copy while limit budget remains) — the
// dedicated interleave arm in cel_string_ext_search.cc, which no
// non-empty needle can reach.  Values oracle-confirmed
// (testdata/cel_cpp_oracle_test.cc StringReplaceEmptyNeedle*).
TEST_F(MultiFunctionE2ETest, ReplaceEmptyNeedleInterleaves) {
  EXPECT_EQ(EvalString(R"("abc".replace("", "-"))"), "-a-b-c-");
}

TEST_F(MultiFunctionE2ETest, ReplaceEmptyNeedleWithLimit) {
  EXPECT_EQ(EvalString(R"("abc".replace("", "-", 2))"), "-a-bc");
}

// Empty-separator split explodes per code point (limit-bounded) —
// the explode arm in cel_string_ext_list.cc.  Oracle-confirmed
// (StringSplitEmptySep*).
TEST_F(MultiFunctionE2ETest, SplitEmptySeparatorExplodesPerCodePoint) {
  EXPECT_EQ(EvalBool(R"("abc".split("") == ["a", "b", "c"])"), true);
  EXPECT_EQ(EvalBool(R"("abc".split("", 2) == ["a", "bc"])"), true);
}

// A transform with nothing to change takes the no-mutation fast path
// (the input span is vended back untouched).
TEST_F(MultiFunctionE2ETest, CaseTransformNoOpFastPath) {
  EXPECT_EQ(EvalString(R"("123 !?".upperAscii())"), "123 !?");
  EXPECT_EQ(EvalString(R"("123 !?".lowerAscii())"), "123 !?");
}

// A specifier/argument kind mismatch surfaces a CEL error Value at
// eval — the formatter's error path, which the happy-path suites
// above never take.
TEST_F(MultiFunctionE2ETest, FormatKindMismatchSurfacesError) {
  Compiler::Builder b;
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler);
  auto instance = CompilePlan(*compiler, R"("%d".format(["not a number"]))");
  Activation a;
  auto v = instance.Eval(a);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->IsError());
}

}  // namespace
}  // namespace celwasm
