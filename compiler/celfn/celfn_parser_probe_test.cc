// M13 Slice B probe — `.celfn` ANTLR4 parser smoke test.
//
// Validates that the ANTLR4-generated parser accepts the canonical
// IDL examples from `doc/implementation-plan/rewrite/m13-custom-fns.md`
// §3.4 and emits a parse tree with the expected top-level shape.
// This is the WAT-first equivalent for the parser: the grammar is
// the spec, the probe is its first executable witness.  No
// semantic validation here — that lives in the visitor/walker on
// top of the parse tree (next slice).

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "antlr4-runtime.h"
#include "compiler/celfn/CelfnLexer.h"
#include "compiler/celfn/CelfnParser.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

// Collecting error listener — accumulates all syntax errors instead
// of printing to stderr.  ANTLR's default listener silently lets
// `parser.file()` return a (potentially garbled) tree on errors,
// which would mask grammar bugs.  We trap them here.
class CollectingErrorListener : public antlr4::BaseErrorListener {
 public:
  void syntaxError(antlr4::Recognizer* /*recognizer*/,
                   antlr4::Token* /*offending_symbol*/, size_t line,
                   size_t column, const std::string& msg,
                   std::exception_ptr /*e*/) override {
    std::stringstream ss;
    ss << "line " << line << ":" << column << " " << msg;
    errors_.push_back(ss.str());
  }

  const std::vector<std::string>& errors() const {
    return errors_;
  }

 private:
  std::vector<std::string> errors_;
};

// Parses `source` and returns (parse-tree-root, error-list).  On
// success, errors is empty.
struct ParseResult {
  celwasm_celfn::CelfnParser::FileContext* file = nullptr;
  std::vector<std::string> errors;
  // Keep the ANTLR objects alive so the parse tree pointers stay
  // valid.  Heap-allocated because antlr4::ANTLRInputStream takes
  // a string_view-like ref to the source.
  std::unique_ptr<antlr4::ANTLRInputStream> input;
  std::unique_ptr<celwasm_celfn::CelfnLexer> lexer;
  std::unique_ptr<antlr4::CommonTokenStream> tokens;
  std::unique_ptr<celwasm_celfn::CelfnParser> parser;
  std::unique_ptr<CollectingErrorListener> error_listener;
};

ParseResult ParseCelfn(const std::string& source) {
  ParseResult r;
  r.input = std::make_unique<antlr4::ANTLRInputStream>(source);
  r.lexer = std::make_unique<celwasm_celfn::CelfnLexer>(r.input.get());
  r.tokens = std::make_unique<antlr4::CommonTokenStream>(r.lexer.get());
  r.parser = std::make_unique<celwasm_celfn::CelfnParser>(r.tokens.get());
  r.error_listener = std::make_unique<CollectingErrorListener>();
  r.lexer->removeErrorListeners();
  r.lexer->addErrorListener(r.error_listener.get());
  r.parser->removeErrorListeners();
  r.parser->addErrorListener(r.error_listener.get());
  r.file = r.parser->file();
  r.errors = r.error_listener->errors();
  return r;
}

TEST(CelfnParserProbe, ParsesEmptyFile) {
  auto r = ParseCelfn("");
  EXPECT_TRUE(r.errors.empty())
      << "errors: " << (r.errors.empty() ? "" : r.errors[0]);
  ASSERT_NE(r.file, nullptr);
  // No module directive, no file items.
  EXPECT_EQ(r.file->moduleDirective(), nullptr);
  EXPECT_EQ(r.file->fileItem().size(), 0u);
}

TEST(CelfnParserProbe, ParsesModuleDirective) {
  auto r = ParseCelfn("Module foo;");
  EXPECT_TRUE(r.errors.empty())
      << "errors: " << (r.errors.empty() ? "" : r.errors[0]);
  ASSERT_NE(r.file, nullptr);
  ASSERT_NE(r.file->moduleDirective(), nullptr);
  EXPECT_EQ(r.file->moduleDirective()->Identifier()->getText(), "foo");
}

TEST(CelfnParserProbe, ParsesHostDecl) {
  auto r = ParseCelfn("string @host.upper(this string s);");
  EXPECT_TRUE(r.errors.empty())
      << "errors: " << (r.errors.empty() ? "" : r.errors[0]);
  ASSERT_EQ(r.file->fileItem().size(), 1u);
  auto* host = r.file->fileItem(0)->hostFnDecl();
  ASSERT_NE(host, nullptr);
  // Grammar shape: type '@' 'host' '.' Identifier '(' params ')' ';'
  // (`host` is a keyword string, not an Identifier — so only one
  // Identifier appears in the rule, accessed without an index.)
  ASSERT_NE(host->Identifier(), nullptr);
  EXPECT_EQ(host->Identifier()->getText(), "upper");
}

TEST(CelfnParserProbe, ParsesPluginDecl) {
  auto r = ParseCelfn("bool @plugin.allow(this string user, string r);");
  EXPECT_TRUE(r.errors.empty())
      << "errors: " << (r.errors.empty() ? "" : r.errors[0]);
  ASSERT_EQ(r.file->fileItem().size(), 1u);
  auto* comp = r.file->fileItem(0)->pluginFnDecl();
  ASSERT_NE(comp, nullptr);
  // `bool @plugin.allow(this string user, string r);`
  // Grammar shape: type '@' 'plugin' '.' Identifier '(' params ')' ';'
  // One Identifier: "allow" (fn name).  No alias.
  EXPECT_EQ(comp->Identifier()->getText(), "allow");
}

TEST(CelfnParserProbe, ParsesCelDefinedFn) {
  auto r =
      ParseCelfn("bool @native.is_number(this string s) = s.matches(\"a\");");
  EXPECT_TRUE(r.errors.empty())
      << "errors: " << (r.errors.empty() ? "" : r.errors[0]);
  ASSERT_EQ(r.file->fileItem().size(), 1u);
  auto* def = r.file->fileItem(0)->nativeFnDecl();
  ASSERT_NE(def, nullptr);
  EXPECT_EQ(def->Identifier()->getText(), "is_number");
  ASSERT_NE(def->celExprBody(), nullptr);
  // Body text matched as one token; trims to the raw expression.
  EXPECT_THAT(def->celExprBody()->getText(), ::testing::HasSubstr("s.matches"));
}

TEST(CelfnParserProbe, ParsesAllThreeShapesInOneFile) {
  const std::string source = R"(
// fns.celfn
Module foo;

// CEL-defined.
bool @native.is_number(this string s) = s.matches("^[0-9]+$");

// Host-backed.
string @host.upper(this string s);

// Plugin-backed.
bool @plugin.allow(this string user, string r);
)";
  auto r = ParseCelfn(source);
  EXPECT_TRUE(r.errors.empty())
      << "errors: " << (r.errors.empty() ? "" : r.errors[0]);
  ASSERT_NE(r.file->moduleDirective(), nullptr);
  EXPECT_EQ(r.file->moduleDirective()->Identifier()->getText(), "foo");
  ASSERT_EQ(r.file->fileItem().size(), 3u);
  EXPECT_NE(r.file->fileItem(0)->nativeFnDecl(), nullptr);
  EXPECT_NE(r.file->fileItem(1)->hostFnDecl(), nullptr);
  EXPECT_NE(r.file->fileItem(2)->pluginFnDecl(), nullptr);
}

TEST(CelfnParserProbe, ParsesProtoTypeArgument) {
  auto r = ParseCelfn("bool @host.is_admin(proto(acme.User) user);");
  EXPECT_TRUE(r.errors.empty())
      << "errors: " << (r.errors.empty() ? "" : r.errors[0]);
  ASSERT_EQ(r.file->fileItem().size(), 1u);
  auto* host = r.file->fileItem(0)->hostFnDecl();
  ASSERT_NE(host, nullptr);
  ASSERT_NE(host->params(), nullptr);
  ASSERT_EQ(host->params()->param().size(), 1u);
  auto* p = host->params()->param(0);
  ASSERT_NE(p->type()->protoType(), nullptr);
  EXPECT_EQ(p->type()->protoType()->qualifiedIdentifier()->getText(),
            "acme.User");
}

TEST(CelfnParserProbe, ParsesAggregateTypes) {
  auto r = ParseCelfn(
      "list<int> @host.pick_evens(list<int> xs);"
      "bool @host.has_key(map<string, bool> m, string k);");
  EXPECT_TRUE(r.errors.empty())
      << "errors: " << (r.errors.empty() ? "" : r.errors[0]);
  ASSERT_EQ(r.file->fileItem().size(), 2u);
}

TEST(CelfnParserProbe, RejectsBareDecl) {
  // `bool plain_name(int x);` — no backend prefix: not `@host.`, not
  // `@native.`, not `@plugin.`.  Every declaration must name its
  // backend, so a bare `<type> <name>(...)` matches no production and
  // fails to parse.
  auto r = ParseCelfn("bool plain_name(int x);");
  EXPECT_FALSE(r.errors.empty()) << "bare-name decl should not parse";
}

TEST(CelfnParserProbe, RejectsNativeDeclWithoutBody) {
  // `@native.` requires a `= <cel-expr>` body; the body-less form
  // matches no production and fails to parse.
  auto r = ParseCelfn("bool @native.plain_name(int x);");
  EXPECT_FALSE(r.errors.empty()) << "@native without a body should not parse";
}

TEST(CelfnParserProbe, IgnoresLineAndBlockComments) {
  auto r = ParseCelfn(R"(
// header comment
Module foo;
/* block
   comment */
bool @host.f(int x);
)");
  EXPECT_TRUE(r.errors.empty())
      << "errors: " << (r.errors.empty() ? "" : r.errors[0]);
  ASSERT_EQ(r.file->fileItem().size(), 1u);
}

}  // namespace
}  // namespace celwasm
