#include "bindings/c/cel_capi.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "gtest/gtest.h"

namespace {

// The wasm magic number: "\0asm".  A successfully compiled Program's
// bytes begin with this (abi_decode.cc locates the cel.abi section
// the same way).
constexpr uint8_t kWasmMagic[4] = {0x00, 0x61, 0x73, 0x6d};

// RAII guard so a test that asserts mid-way still frees the buffers.
class FreeGuard {
 public:
  explicit FreeGuard(void* ptr) : ptr_(ptr) {}
  FreeGuard(const FreeGuard&) = delete;
  FreeGuard& operator=(const FreeGuard&) = delete;
  ~FreeGuard() {
    cel_free(ptr_);
  }

 private:
  void* ptr_;
};

bool StartsWithWasmMagic(const uint8_t* bytes, size_t len) {
  return len >= sizeof(kWasmMagic) &&
         std::memcmp(bytes, kWasmMagic, sizeof(kWasmMagic)) == 0;
}

TEST(CelCapi, CompilesConstantExpression) {
  uint8_t* wasm = nullptr;
  size_t len = 0;
  char* err = nullptr;
  const CelStatus st = cel_compile("1 + 2", nullptr, &wasm, &len, &err);
  FreeGuard wasm_guard(wasm);
  FreeGuard err_guard(err);

  EXPECT_EQ(st, CEL_STATUS_OK);
  EXPECT_EQ(err, nullptr);
  ASSERT_NE(wasm, nullptr);
  EXPECT_GT(len, 0u);
  EXPECT_TRUE(StartsWithWasmMagic(wasm, len));
}

TEST(CelCapi, CompilesVariableExpression) {
  CelCompileOpts* opts = cel_compile_opts_new();
  ASSERT_NE(opts, nullptr);

  char* decl_err = nullptr;
  ASSERT_EQ(cel_compile_opts_declare_var(opts, "x:int", &decl_err),
            CEL_STATUS_OK);
  ASSERT_EQ(decl_err, nullptr);
  ASSERT_EQ(cel_compile_opts_declare_var(opts, "y:int", &decl_err),
            CEL_STATUS_OK);
  ASSERT_EQ(decl_err, nullptr);

  uint8_t* wasm = nullptr;
  size_t len = 0;
  char* err = nullptr;
  const CelStatus st = cel_compile("x + y", opts, &wasm, &len, &err);
  FreeGuard wasm_guard(wasm);
  FreeGuard err_guard(err);
  cel_compile_opts_free(opts);

  EXPECT_EQ(st, CEL_STATUS_OK);
  EXPECT_EQ(err, nullptr);
  ASSERT_NE(wasm, nullptr);
  EXPECT_GT(len, 0u);
  EXPECT_TRUE(StartsWithWasmMagic(wasm, len));
}

TEST(CelCapi, CompilesAggregateVariableTypes) {
  CelCompileOpts* opts = cel_compile_opts_new();
  ASSERT_NE(opts, nullptr);

  char* decl_err = nullptr;
  EXPECT_EQ(cel_compile_opts_declare_var(opts, "items:list<int>", &decl_err),
            CEL_STATUS_OK);
  EXPECT_EQ(decl_err, nullptr);
  EXPECT_EQ(cel_compile_opts_declare_var(opts, "m:map<string, int>", &decl_err),
            CEL_STATUS_OK);
  EXPECT_EQ(decl_err, nullptr);

  uint8_t* wasm = nullptr;
  size_t len = 0;
  char* err = nullptr;
  const CelStatus st =
      cel_compile("items.size() + m.size()", opts, &wasm, &len, &err);
  FreeGuard wasm_guard(wasm);
  FreeGuard err_guard(err);
  cel_compile_opts_free(opts);

  EXPECT_EQ(st, CEL_STATUS_OK) << (err != nullptr ? err : "");
  ASSERT_NE(wasm, nullptr);
  EXPECT_TRUE(StartsWithWasmMagic(wasm, len));
}

TEST(CelCapi, ContainerAndOptimizeAndLinkModeAreAccepted) {
  CelCompileOpts* opts = cel_compile_opts_new();
  ASSERT_NE(opts, nullptr);
  cel_compile_opts_set_container(opts, "com.example");
  cel_compile_opts_set_optimize_level(opts, 2);
  cel_compile_opts_set_link_mode(opts, CEL_LINK_MODE_DYNAMIC);

  uint8_t* wasm = nullptr;
  size_t len = 0;
  char* err = nullptr;
  const CelStatus st = cel_compile("1 + 2", opts, &wasm, &len, &err);
  FreeGuard wasm_guard(wasm);
  FreeGuard err_guard(err);
  cel_compile_opts_free(opts);

  EXPECT_EQ(st, CEL_STATUS_OK) << (err != nullptr ? err : "");
  ASSERT_NE(wasm, nullptr);
  EXPECT_TRUE(StartsWithWasmMagic(wasm, len));
}

TEST(CelCapi, SyntacticallyBadExpressionFailsWithDiagnostic) {
  uint8_t* wasm = nullptr;
  size_t len = 0;
  char* err = nullptr;
  const CelStatus st = cel_compile("1 +", nullptr, &wasm, &len, &err);
  FreeGuard wasm_guard(wasm);
  FreeGuard err_guard(err);

  EXPECT_NE(st, CEL_STATUS_OK);
  EXPECT_EQ(wasm, nullptr);
  EXPECT_EQ(len, 0u);
  ASSERT_NE(err, nullptr);
  EXPECT_GT(std::strlen(err), 0u);
}

TEST(CelCapi, ReferenceToUndeclaredVariableFails) {
  uint8_t* wasm = nullptr;
  size_t len = 0;
  char* err = nullptr;
  // No vars declared; `x` is unbound -> type-check failure.
  const CelStatus st = cel_compile("x + 1", nullptr, &wasm, &len, &err);
  FreeGuard wasm_guard(wasm);
  FreeGuard err_guard(err);

  EXPECT_EQ(st, CEL_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(wasm, nullptr);
  ASSERT_NE(err, nullptr);
  EXPECT_GT(std::strlen(err), 0u);
}

TEST(CelCapi, BadOptimizeLevelFails) {
  CelCompileOpts* opts = cel_compile_opts_new();
  ASSERT_NE(opts, nullptr);
  cel_compile_opts_set_optimize_level(opts, 99);

  uint8_t* wasm = nullptr;
  size_t len = 0;
  char* err = nullptr;
  const CelStatus st = cel_compile("1 + 2", opts, &wasm, &len, &err);
  FreeGuard wasm_guard(wasm);
  FreeGuard err_guard(err);
  cel_compile_opts_free(opts);

  EXPECT_EQ(st, CEL_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(wasm, nullptr);
  ASSERT_NE(err, nullptr);
}

TEST(CelCapi, MalformedVarDeclFails) {
  CelCompileOpts* opts = cel_compile_opts_new();
  ASSERT_NE(opts, nullptr);

  char* err = nullptr;
  // No ':' separator.
  EXPECT_EQ(cel_compile_opts_declare_var(opts, "xyz", &err),
            CEL_STATUS_INVALID_ARGUMENT);
  FreeGuard err_guard(err);
  ASSERT_NE(err, nullptr);
  EXPECT_GT(std::strlen(err), 0u);

  // Empty name.
  char* err2 = nullptr;
  EXPECT_EQ(cel_compile_opts_declare_var(opts, ":int", &err2),
            CEL_STATUS_INVALID_ARGUMENT);
  FreeGuard err2_guard(err2);
  ASSERT_NE(err2, nullptr);

  // Unparsable type spec (unterminated list).
  char* err3 = nullptr;
  EXPECT_EQ(cel_compile_opts_declare_var(opts, "z:list<int", &err3),
            CEL_STATUS_INVALID_ARGUMENT);
  FreeGuard err3_guard(err3);
  ASSERT_NE(err3, nullptr);

  cel_compile_opts_free(opts);
}

TEST(CelCapi, BadHostFnDeclFailsAtCompile) {
  CelCompileOpts* opts = cel_compile_opts_new();
  ASSERT_NE(opts, nullptr);
  // Staged without error; the parse failure surfaces at compile time.
  EXPECT_EQ(cel_compile_opts_declare_host_fn(opts, "not a valid celfn decl"),
            CEL_STATUS_OK);

  uint8_t* wasm = nullptr;
  size_t len = 0;
  char* err = nullptr;
  const CelStatus st = cel_compile("1 + 2", opts, &wasm, &len, &err);
  FreeGuard wasm_guard(wasm);
  FreeGuard err_guard(err);
  cel_compile_opts_free(opts);

  EXPECT_NE(st, CEL_STATUS_OK);
  EXPECT_EQ(wasm, nullptr);
  ASSERT_NE(err, nullptr);
}

TEST(CelCapi, ValidHostFnDeclCompilesAndIsCallable) {
  CelCompileOpts* opts = cel_compile_opts_new();
  ASSERT_NE(opts, nullptr);
  EXPECT_EQ(cel_compile_opts_declare_host_fn(
                opts, "string @host.upper(this string s);"),
            CEL_STATUS_OK);

  char* decl_err = nullptr;
  ASSERT_EQ(cel_compile_opts_declare_var(opts, "s:string", &decl_err),
            CEL_STATUS_OK);

  uint8_t* wasm = nullptr;
  size_t len = 0;
  char* err = nullptr;
  const CelStatus st = cel_compile("s.upper()", opts, &wasm, &len, &err);
  FreeGuard wasm_guard(wasm);
  FreeGuard err_guard(err);
  cel_compile_opts_free(opts);

  EXPECT_EQ(st, CEL_STATUS_OK) << (err != nullptr ? err : "");
  ASSERT_NE(wasm, nullptr);
  EXPECT_TRUE(StartsWithWasmMagic(wasm, len));
}

TEST(CelCapi, NullRequiredArgsYieldInternal) {
  uint8_t* wasm = nullptr;
  size_t len = 0;
  char* err = nullptr;
  EXPECT_EQ(cel_compile(nullptr, nullptr, &wasm, &len, &err),
            CEL_STATUS_INTERNAL);
  EXPECT_EQ(cel_compile("1 + 2", nullptr, nullptr, &len, &err),
            CEL_STATUS_INTERNAL);
  EXPECT_EQ(cel_compile("1 + 2", nullptr, &wasm, nullptr, &err),
            CEL_STATUS_INTERNAL);
}

TEST(CelCapi, FreeAndOptsFreeAcceptNull) {
  cel_free(nullptr);
  cel_compile_opts_free(nullptr);
  SUCCEED();
}

}  // namespace
