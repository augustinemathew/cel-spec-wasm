// DISPOSABLE PROBE (m35 slice A4) — delete at milestone closeout.
//
// Question (doc/implementation-plan/rewrite/m35-plugin-ergonomics.md
// §3.3): does the pinned wasmtime C API (v43.0.1) expose
// component-level export lookup against a parsed
// `wasmtime_component_t`, WITHOUT instantiation — so `Engine::Use`
// can check the plugin's WIT interface + per-decl exports statically
// at registration time?
//
// Answer: YES.  `wasmtime_component_get_export_index(
//     const wasmtime_component_t*,
//     const wasmtime_component_export_index_t* instance,  // nullable
//     const char* name, size_t name_len)`
// is declared at wasmtime v43.0.1
// include/wasmtime/component/component.h:136-140 (the pinned
// `@wasmtime_*` archives; see MODULE.bazel).  It takes only the
// parsed component — no store, no instance — and the nullable
// `instance` parameter supports the two-level lookup the engine
// needs (interface index first, then the decl's kebab-case export
// nested inside it).  This probe demonstrates exactly that against
// the real macro-built demo plugin.
//
// (The instance-level variant the Plan path uses today,
// `wasmtime_component_instance_get_export_index`, lives at
// include/wasmtime/component/instance.h:42-46 and additionally
// requires a store context — that is the API this probe shows we do
// NOT need for the static check.)

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/memory/memory.h"
#include "absl/strings/string_view.h"
#include "gtest/gtest.h"
#include "tools/cpp/runfiles/runfiles.h"
#include "wasm.h"
#include "wasmtime/component/component.h"

namespace celwasm {
namespace {

using ::bazel::tools::cpp::runfiles::Runfiles;

// Loads `demo_plugin.wasm` (the `cel_wasm_plugin`-macro-built
// Component-Model component from fns.idl: `Module customfn;` +
// @plugin.{greet,add,len}) from the runfiles tree.
std::vector<uint8_t> LoadDemoPluginBytes() {
  std::string error;
  auto runfiles = absl::WrapUnique(Runfiles::CreateForTest(&error));
  ABSL_CHECK(runfiles != nullptr) << "runfiles init failed: " << error;
  const std::string path = runfiles->Rlocation(
      "_main/e2e/plugin_fixtures/cel_wasm_plugin_demo/"
      "demo_plugin.wasm");
  ABSL_CHECK(!path.empty()) << "demo_plugin.wasm not in runfiles";
  std::ifstream f(path, std::ios::binary);
  ABSL_CHECK(f.is_open()) << "failed to open " << path;
  return {(std::istreambuf_iterator<char>(f)),
          std::istreambuf_iterator<char>()};
}

class ComponentExportProbe : public ::testing::Test {
 protected:
  void SetUp() override {
    engine_ = wasm_engine_new();
    ASSERT_NE(engine_, nullptr);
    const std::vector<uint8_t> bytes = LoadDemoPluginBytes();
    wasmtime_error_t* err = wasmtime_component_new(
        engine_, bytes.data(), bytes.size(), &component_);
    ASSERT_EQ(err, nullptr) << "wasmtime_component_new failed";
    ASSERT_NE(component_, nullptr);
  }

  void TearDown() override {
    if (component_ != nullptr) wasmtime_component_delete(component_);
    if (engine_ != nullptr) wasm_engine_delete(engine_);
  }

  // Component-level lookup: parsed component only — no store, no
  // instance anywhere in scope.
  wasmtime_component_export_index_t* Lookup(
      const wasmtime_component_export_index_t* parent,
      absl::string_view name) {
    return wasmtime_component_get_export_index(component_, parent,
                                               name.data(), name.size());
  }

  wasm_engine_t* engine_ = nullptr;
  wasmtime_component_t* component_ = nullptr;
};

// The two-level lookup Engine::Use needs: the WIT interface instance
// (`cel:<module>/fns@0.1.0`, module `customfn` from the demo idl's
// Module directive), then each decl's kebab-case export nested under
// it — all against the PARSED component, pre-instantiation.
TEST_F(ComponentExportProbe, InterfaceAndNestedExportsResolveStatically) {
  wasmtime_component_export_index_t* iface =
      Lookup(nullptr, "cel:customfn/fns@0.1.0");
  ASSERT_NE(iface, nullptr)
      << "interface export not resolvable on the parsed component";

  for (const absl::string_view fn :
       {"greet-string-int", "add-int-int", "len-string"}) {
    wasmtime_component_export_index_t* exp = Lookup(iface, fn);
    EXPECT_NE(exp, nullptr) << "nested export `" << fn << "` not found";
    if (exp != nullptr) wasmtime_component_export_index_delete(exp);
  }
  wasmtime_component_export_index_delete(iface);
}

// Missing names return NULL (the API's not-found signal) rather than
// erroring — the shape Engine::Use's FailedPrecondition mapping needs.
TEST_F(ComponentExportProbe, MissingExportsReturnNull) {
  EXPECT_EQ(Lookup(nullptr, "cel:absent/fns@0.1.0"), nullptr);

  wasmtime_component_export_index_t* iface =
      Lookup(nullptr, "cel:customfn/fns@0.1.0");
  ASSERT_NE(iface, nullptr);
  EXPECT_EQ(Lookup(iface, "no-such-export"), nullptr);
  wasmtime_component_export_index_delete(iface);
}

}  // namespace
}  // namespace celwasm
