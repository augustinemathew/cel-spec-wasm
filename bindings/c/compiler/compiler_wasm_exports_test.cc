// Native-config tests for the `cew_*` export layer of `compiler.wasm`
// (compiler_wasm_exports.cc).  Each test builds a
// `celwasm.compile.CompileRequest`, serializes it, and drives
// `cew_compile` / `cew_program` / `cew_error` / `cew_reset` exactly as
// the JS caller does — minus the wasm boundary itself (covered by the
// TS round-trip tests + the conformance corpus).

#include "bindings/c/compiler/compiler_wasm_exports.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "bindings/c/compiler/compile_request.pb.h"
#include "google/protobuf/descriptor.pb.h"
#include "gtest/gtest.h"

namespace {

using ::celwasm::compile::CompileRequest;

// The wasm magic number: "\0asm".  A successfully compiled Program's
// bytes begin with this.
constexpr uint8_t kWasmMagic[4] = {0x00, 0x61, 0x73, 0x6d};

bool ProgramStartsWithWasmMagic() {
  return cew_program_len() >= static_cast<int>(sizeof(kWasmMagic)) &&
         std::memcmp(cew_program(), kWasmMagic, sizeof(kWasmMagic)) == 0;
}

// Serializes `request` and runs it through cew_compile.
int Compile(const CompileRequest& request) {
  const std::string bytes = request.SerializeAsString();
  return cew_compile(reinterpret_cast<const uint8_t*>(bytes.data()),
                     static_cast<int>(bytes.size()));
}

CompileRequest StaticRequest(const std::string& source) {
  CompileRequest request;
  request.set_source(source);
  request.set_link_mode(celwasm::compile::LINK_MODE_STATIC);
  return request;
}

// Serialized FileDescriptorSet for `celwasm.test.Widget { string label = 1; }`
// — a message defined ONLY in these bytes (never in the generated pool), so
// a compile that resolves it proves the descriptor_set field is load-bearing.
std::string BuildWidgetFds() {
  using google::protobuf::FieldDescriptorProto;
  google::protobuf::FileDescriptorSet fds;
  google::protobuf::FileDescriptorProto* file = fds.add_file();
  file->set_name("widget.proto");
  file->set_package("celwasm.test");
  file->set_syntax("proto3");
  google::protobuf::DescriptorProto* widget = file->add_message_type();
  widget->set_name("Widget");
  FieldDescriptorProto* label = widget->add_field();
  label->set_name("label");
  label->set_number(1);
  label->set_type(FieldDescriptorProto::TYPE_STRING);
  label->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  return fds.SerializeAsString();
}

class CewCompileTest : public ::testing::Test {
 protected:
  // Every test leaves the one-shot result state clean for the next.
  void TearDown() override {
    cew_reset();
  }
};

TEST_F(CewCompileTest, CompilesConstantExpression) {
  const int len = Compile(StaticRequest("1 + 2"));
  ASSERT_GT(len, 0) << (cew_error() != nullptr ? cew_error() : "");
  EXPECT_EQ(cew_program_len(), len);
  EXPECT_EQ(cew_error(), nullptr);
  EXPECT_TRUE(ProgramStartsWithWasmMagic());
}

TEST_F(CewCompileTest, CompilesVariableExpression) {
  CompileRequest request = StaticRequest("x + y");
  auto* x = request.add_variables();
  x->set_name("x");
  x->set_type("int");
  auto* y = request.add_variables();
  y->set_name("y");
  y->set_type("int");
  const int len = Compile(request);
  ASSERT_GT(len, 0) << (cew_error() != nullptr ? cew_error() : "");
  EXPECT_TRUE(ProgramStartsWithWasmMagic());
}

// The load-bearing property of the proto boundary: `source` is
// length-delimited, so a CEL `b'\x00'` byte literal — a raw NUL byte in
// the source text — compiles instead of truncating at a NUL-terminated
// `const char*` crossing.
TEST_F(CewCompileTest, EmbeddedNulSourceCompiles) {
  std::string source = "b'";
  source.push_back('\0');
  source += "' < b'";
  source.push_back('\x01');
  source += "'";
  const int len = Compile(StaticRequest(source));
  ASSERT_GT(len, 0) << (cew_error() != nullptr ? cew_error() : "");
  EXPECT_TRUE(ProgramStartsWithWasmMagic());
}

TEST_F(CewCompileTest, DynamicLinkModeProducesThinModule) {
  const int static_len = Compile(StaticRequest("1 + 2"));
  ASSERT_GT(static_len, 0) << (cew_error() != nullptr ? cew_error() : "");

  CompileRequest dynamic_request;
  dynamic_request.set_source("1 + 2");
  dynamic_request.set_link_mode(celwasm::compile::LINK_MODE_DYNAMIC);
  const int dynamic_len = Compile(dynamic_request);
  ASSERT_GT(dynamic_len, 0) << (cew_error() != nullptr ? cew_error() : "");

  // The static Program bakes in the ~MB runtime; the dynamic one imports
  // it.  The size gap is the linking working.
  EXPECT_LT(dynamic_len * 10, static_len);
}

// The proto3 zero value of `link_mode` is LINK_MODE_DYNAMIC (numbering
// matches abi/cel_abi.proto), so an absent field compiles dynamic — the
// documented wire default, pinned here.
TEST_F(CewCompileTest, AbsentLinkModeCompilesDynamic) {
  CompileRequest dynamic_request;
  dynamic_request.set_source("1 + 2");
  dynamic_request.set_link_mode(celwasm::compile::LINK_MODE_DYNAMIC);
  const int dynamic_len = Compile(dynamic_request);
  ASSERT_GT(dynamic_len, 0);

  CompileRequest absent_request;
  absent_request.set_source("1 + 2");
  const int absent_len = Compile(absent_request);
  ASSERT_GT(absent_len, 0);
  EXPECT_EQ(absent_len, dynamic_len);
}

TEST_F(CewCompileTest, UnknownLinkModeValueFails) {
  CompileRequest request;
  request.set_source("1 + 2");
  // Open-set wire data from a hypothetical newer schema.
  request.set_link_mode(static_cast<celwasm::compile::LinkMode>(2));
  EXPECT_EQ(Compile(request), -1);
  ASSERT_NE(cew_error(), nullptr);
  EXPECT_NE(std::strstr(cew_error(), "unsupported link_mode"), nullptr);
}

TEST_F(CewCompileTest, ContainerIsApplied) {
  CompileRequest request = StaticRequest("x");
  request.set_container("com.example");
  auto* var = request.add_variables();
  var->set_name("com.example.x");
  var->set_type("int");
  // `x` resolves to `com.example.x` only through the container.
  const int len = Compile(request);
  ASSERT_GT(len, 0) << (cew_error() != nullptr ? cew_error() : "");
}

TEST_F(CewCompileTest, FnDeclCompiles) {
  CompileRequest request = StaticRequest("s.upper()");
  request.add_fns("string @host.upper(this string s);");
  auto* var = request.add_variables();
  var->set_name("s");
  var->set_type("string");
  const int len = Compile(request);
  ASSERT_GT(len, 0) << (cew_error() != nullptr ? cew_error() : "");
}

TEST_F(CewCompileTest, BadFnDeclFailsAtCompile) {
  CompileRequest request = StaticRequest("1 + 2");
  request.add_fns("not a valid celfn decl");
  EXPECT_EQ(Compile(request), -1);
  ASSERT_NE(cew_error(), nullptr);
  EXPECT_GT(std::strlen(cew_error()), 0u);
}

TEST_F(CewCompileTest, BadVariableDeclFails) {
  CompileRequest request = StaticRequest("1 + 2");
  auto* var = request.add_variables();
  var->set_name("");
  var->set_type("int");
  EXPECT_EQ(Compile(request), -1);
  ASSERT_NE(cew_error(), nullptr);
  EXPECT_GT(std::strlen(cew_error()), 0u);
}

TEST_F(CewCompileTest, OutOfRangeOptimizeLevelFails) {
  CompileRequest request = StaticRequest("1 + 2");
  request.set_optimize_level(99);
  EXPECT_EQ(Compile(request), -1);
  ASSERT_NE(cew_error(), nullptr);
}

TEST_F(CewCompileTest, OptimizeLevelTwoCompiles) {
  CompileRequest request = StaticRequest("1 + 2");
  request.set_optimize_level(2);
  const int len = Compile(request);
  ASSERT_GT(len, 0) << (cew_error() != nullptr ? cew_error() : "");
}

TEST_F(CewCompileTest, DescriptorSetResolvesMessageType) {
  CompileRequest request = StaticRequest("w.label");
  request.set_descriptor_set(BuildWidgetFds());
  auto* var = request.add_variables();
  var->set_name("w");
  var->set_type("celwasm.test.Widget");
  const int len = Compile(request);
  ASSERT_GT(len, 0) << (cew_error() != nullptr ? cew_error() : "");
}

TEST_F(CewCompileTest, InvalidDescriptorSetFails) {
  CompileRequest request = StaticRequest("1 + 2");
  request.set_descriptor_set("\xff\xff\xff\xff");
  EXPECT_EQ(Compile(request), -1);
  ASSERT_NE(cew_error(), nullptr);
  EXPECT_NE(std::strstr(cew_error(), "invalid descriptor set"), nullptr);
}

TEST_F(CewCompileTest, TypeCheckFailureSurfacesDiagnostic) {
  EXPECT_EQ(Compile(StaticRequest("1 + 'a'")), -1);
  EXPECT_EQ(cew_program(), nullptr);
  ASSERT_NE(cew_error(), nullptr);
  EXPECT_GT(std::strlen(cew_error()), 0u);
}

TEST_F(CewCompileTest, MalformedRequestBytesFail) {
  // A varint field header promising more bytes than supplied.
  const uint8_t garbage[] = {0x0a, 0xff, 0x01};
  EXPECT_EQ(cew_compile(garbage, sizeof(garbage)), -1);
  ASSERT_NE(cew_error(), nullptr);
  EXPECT_NE(std::strstr(cew_error(), "malformed CompileRequest"), nullptr);
}

TEST_F(CewCompileTest, NullOrEmptyRequestFails) {
  EXPECT_EQ(cew_compile(nullptr, 0), -1);
  ASSERT_NE(cew_error(), nullptr);
  const uint8_t byte = 0;
  EXPECT_EQ(cew_compile(&byte, 0), -1);
  ASSERT_NE(cew_error(), nullptr);
}

TEST_F(CewCompileTest, ResetClearsResultState) {
  ASSERT_GT(Compile(StaticRequest("1 + 2")), 0);
  cew_reset();
  EXPECT_EQ(cew_program(), nullptr);
  EXPECT_EQ(cew_program_len(), 0);
  EXPECT_EQ(cew_error(), nullptr);
}

TEST_F(CewCompileTest, NextCompileReplacesPreviousResult) {
  ASSERT_GT(Compile(StaticRequest("1 + 2")), 0);
  // A failing compile clears the previous Program and stashes an error.
  EXPECT_EQ(Compile(StaticRequest("1 +")), -1);
  EXPECT_EQ(cew_program(), nullptr);
  EXPECT_EQ(cew_program_len(), 0);
  EXPECT_NE(cew_error(), nullptr);
}

// The JS calling convention end-to-end: write the request into a
// cew_alloc'd buffer, compile, free.
TEST_F(CewCompileTest, AllocWriteCompileFreeRoundTrip) {
  const std::string bytes = StaticRequest("1 + 2").SerializeAsString();
  void* buf = cew_alloc(static_cast<int>(bytes.size()));
  ASSERT_NE(buf, nullptr);
  std::memcpy(buf, bytes.data(), bytes.size());
  const int len = cew_compile(static_cast<const uint8_t*>(buf),
                              static_cast<int>(bytes.size()));
  cew_free(buf);
  ASSERT_GT(len, 0) << (cew_error() != nullptr ? cew_error() : "");
  EXPECT_TRUE(ProgramStartsWithWasmMagic());
}

}  // namespace
