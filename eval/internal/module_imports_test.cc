// Unit coverage for `ModuleImportsCelNamespace` — the predicate
// `Engine::Plan` uses to route a Program between static linking (no
// `"cel"` imports: runtime helpers are defined functions) and dynamic
// linking (helpers + memory imported from `"cel"`).  Modules are built
// synthetically from WAT via `wasmtime_wat2wasm` + `wasmtime_module_new`
// so each import-list shape is pinned in isolation from the compiler.

#include "eval/internal/module_imports.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"
#include "gtest/gtest.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

std::vector<uint8_t> Wat2Wasm(absl::string_view wat) {
  wasm_byte_vec_t out;
  wasmtime_error_t* err = wasmtime_wat2wasm(wat.data(), wat.size(), &out);
  ABSL_CHECK(err == nullptr);
  std::vector<uint8_t> bytes(out.data, out.data + out.size);
  wasm_byte_vec_delete(&out);
  return bytes;
}

struct ModuleDeleter {
  void operator()(wasmtime_module_t* m) const {
    wasmtime_module_delete(m);
  }
};
using ModulePtr = std::unique_ptr<wasmtime_module_t, ModuleDeleter>;

class ModuleImportsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    engine_ = wasm_engine_new();
    ABSL_CHECK(engine_ != nullptr);
  }

  void TearDown() override {
    wasm_engine_delete(engine_);
  }

  ModulePtr Compile(absl::string_view wat) {
    std::vector<uint8_t> bytes = Wat2Wasm(wat);
    wasmtime_module_t* module = nullptr;
    wasmtime_error_t* err =
        wasmtime_module_new(engine_, bytes.data(), bytes.size(), &module);
    ABSL_CHECK(err == nullptr);
    return ModulePtr(module);
  }

  wasm_engine_t* engine_ = nullptr;
};

struct ImportCase {
  absl::string_view name;
  absl::string_view wat;
  bool expected;
};

class ModuleImportsMatrixTest
    : public ModuleImportsTest,
      public ::testing::WithParamInterface<ImportCase> {};

TEST_P(ModuleImportsMatrixTest, RoutesByCelNamespacePresence) {
  ModulePtr module = Compile(GetParam().wat);
  EXPECT_EQ(ModuleImportsCelNamespace(module.get()), GetParam().expected);
}

INSTANTIATE_TEST_SUITE_P(
    ImportShapes, ModuleImportsMatrixTest,
    ::testing::ValuesIn<ImportCase>({
        // Static-mode shape: no imports at all.
        {"NoImports", R"WAT(
            (module
              (func (export "eval") (result i32) (i32.const 1)))
          )WAT",
         false},
        // Static-mode shape: WASI imports only (absl/cctz linked into
        // the expr module pull these in; they must not flip routing).
        {"OnlyWasiImport", R"WAT(
            (module
              (import "wasi_snapshot_preview1" "proc_exit"
                (func (param i32))))
          )WAT",
         false},
        // Static-mode shape: host trampolines only.  `cel_host` is a
        // distinct namespace from `cel` and must not match.
        {"OnlyCelHostImport", R"WAT(
            (module
              (import "cel_host" "cel_get_field"
                (func (param i32 i32 i32))))
          )WAT",
         false},
        // Dynamic-mode shape: a single `cel.*` import (the memory)
        // suffices.
        {"SingleCelMemoryImport", R"WAT(
            (module
              (import "cel" "memory" (memory 1)))
          )WAT",
         true},
        // Dynamic-mode shape: `cel.*` flips the predicate even when
        // `cel_host.*` imports are interleaved.
        {"CelAndCelHostImports", R"WAT(
            (module
              (import "cel_host" "cel_get_field"
                (func (param i32 i32 i32)))
              (import "cel" "arena_reset" (func)))
          )WAT",
         true},
        // Dynamic-mode shape: multiple `cel.*` function imports.
        {"MultipleCelFunctionImports", R"WAT(
            (module
              (import "cel" "arena_reset" (func))
              (import "cel" "arena_alloc" (func (param i32) (result i32)))
              (import "cel" "cel_and" (func (param i32 i32 i32))))
          )WAT",
         true},
        // Boundary: module-name match is exact (size == 3 + memcmp),
        // not a prefix test — `"celx"` must not match.
        {"CelPrefixedLongerName", R"WAT(
            (module
              (import "celx" "arena_reset" (func)))
          )WAT",
         false},
        // Boundary: a proper prefix of "cel" must not match either.
        {"CelPrefixShorterName", R"WAT(
            (module
              (import "ce" "arena_reset" (func)))
          )WAT",
         false},
    }),
    [](const ::testing::TestParamInfo<ImportCase>& info) {
      return std::string(info.param.name);
    });

}  // namespace
}  // namespace celwasm
