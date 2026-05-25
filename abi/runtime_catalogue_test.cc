// Invariants over the runtime catalogue.  Compile-time validation
// happens via the catalogue's internal static_asserts; this test
// adds the data-shape checks: no duplicate names across namespaces,
// arities are in the supported 0-5 range, returns_i32 matches the
// kind sentinel, and FindBuiltinHelper agrees with the per-namespace
// spans.  Pre-empts the kind of drift that motivated the catalogue
// in the first place.

#include "compiler_v2/abi/runtime_catalogue.h"

#include <cstdint>
#include <string>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/abi/cel_abi.pb.h"
#include "gtest/gtest.h"

namespace celwasm::abi {
namespace {

// Per-namespace uniqueness is the load-bearing invariant — names
// across namespaces CAN collide by design (the kDynamic dispatcher
// `cel.cel_list_at` and the trampoline `cel_host.cel_list_at` it
// tail-calls share a name).
TEST(RuntimeCatalogue, NoDuplicateNamesWithinNamespace) {
  auto check = [](absl::Span<const AbiHelper> span, absl::string_view ns) {
    absl::flat_hash_set<absl::string_view> seen;
    for (const auto& h : span) {
      EXPECT_TRUE(seen.insert(h.name).second)
          << "duplicate helper `" << ns << "." << h.name << "` in catalogue";
    }
  };
  check(CelRuntimeHelpers(), "cel");
  check(CelHostFunctions(), "cel_host");
  check(CelEnvFunctions(), "cel_env");
}

// Cross-namespace name collisions are admitted but exercised — the
// list/map dispatch pattern uses paired entries.  Confirm the
// expected collisions exist so a future cleanup that breaks the
// pattern surfaces here.
TEST(RuntimeCatalogue, ExpectedCrossNamespaceCollisions) {
  constexpr absl::string_view kPaired[] = {
      "cel_list_at",  "cel_list_size", "cel_list_in",     "cel_list_eq",
      "cel_list_concat", "cel_map_lookup", "cel_map_size", "cel_map_in",
      "cel_map_eq",
  };
  for (absl::string_view name : kPaired) {
    EXPECT_NE(FindBuiltinHelper(AbiModule::kCelRuntime, name), nullptr)
        << "expected runtime dispatcher `cel." << name << "`";
    EXPECT_NE(FindBuiltinHelper(AbiModule::kCelHost, name), nullptr)
        << "expected host trampoline `cel_host." << name << "`";
  }
}

TEST(RuntimeCatalogue, AritiesInRange) {
  // 0 args = niladic helpers (`arena_reset`, `arena_cursor`).
  // 5 args = the M12 `cel_string_replace_n_at_vvvv` upper bound.
  for (const auto& h : CelRuntimeHelpers()) {
    EXPECT_LE(h.num_args, 5u) << "runtime helper `" << h.name
                              << "` has unsupported arity " << +h.num_args;
  }
  for (const auto& h : CelHostFunctions()) {
    // Host trampolines always have an out_slot + 1+ operands.
    EXPECT_GE(h.num_args, 2u) << "host trampoline `" << h.name
                              << "` has arity < 2 (no out_slot)";
    EXPECT_LE(h.num_args, 5u) << "host trampoline `" << h.name
                              << "` has unsupported arity " << +h.num_args;
  }
  for (const auto& h : CelEnvFunctions()) {
    EXPECT_GE(h.num_args, 1u);
    EXPECT_LE(h.num_args, 5u);
  }
}

TEST(RuntimeCatalogue, ModuleAssignmentMatchesNamespace) {
  for (const auto& h : CelRuntimeHelpers()) {
    EXPECT_EQ(h.module, AbiModule::kCelRuntime) << h.name;
  }
  for (const auto& h : CelHostFunctions()) {
    EXPECT_EQ(h.module, AbiModule::kCelHost) << h.name;
  }
  for (const auto& h : CelEnvFunctions()) {
    EXPECT_EQ(h.module, AbiModule::kCelEnv) << h.name;
  }
}

TEST(RuntimeCatalogue, HostAndEnvTrampolinesReturnVoid) {
  // Host trampolines and env functions write results through
  // out_slots in linear memory; the wasm function itself always
  // returns void.  Only runtime helpers can return i32 (iter
  // handles, count helpers, arena_alloc).
  for (const auto& h : CelHostFunctions()) {
    EXPECT_FALSE(h.returns_i32) << "cel_host." << h.name
                                << " returns i32 — host trampolines "
                                   "should write through out_slot";
  }
  for (const auto& h : CelEnvFunctions()) {
    EXPECT_FALSE(h.returns_i32) << "cel_env." << h.name;
  }
}

TEST(RuntimeCatalogue, FindBuiltinHelperResolvesEveryEntry) {
  for (const auto& h : CelRuntimeHelpers()) {
    const auto* found = FindBuiltinHelper(AbiModule::kCelRuntime, h.name);
    ASSERT_NE(found, nullptr) << h.name;
    EXPECT_EQ(found->module, AbiModule::kCelRuntime);
    EXPECT_EQ(found->num_args, h.num_args);
  }
  for (const auto& h : CelHostFunctions()) {
    const auto* found = FindBuiltinHelper(AbiModule::kCelHost, h.name);
    ASSERT_NE(found, nullptr) << h.name;
    EXPECT_EQ(found->module, AbiModule::kCelHost);
  }
  for (const auto& h : CelEnvFunctions()) {
    const auto* found = FindBuiltinHelper(AbiModule::kCelEnv, h.name);
    ASSERT_NE(found, nullptr) << h.name;
    EXPECT_EQ(found->module, AbiModule::kCelEnv);
  }
}

TEST(RuntimeCatalogue, FindBuiltinHelperRejectsUnknown) {
  EXPECT_EQ(FindBuiltinHelper(AbiModule::kCelRuntime, ""), nullptr);
  EXPECT_EQ(FindBuiltinHelper(AbiModule::kCelRuntime, "not_a_helper"), nullptr);
  EXPECT_EQ(FindBuiltinHelper(AbiModule::kCelRuntime, "cel_phlogiston"),
            nullptr);
  // kCelFn never resolves through the catalogue — custom-fn arities
  // come from per-compile registration.
  EXPECT_EQ(FindBuiltinHelper(AbiModule::kCelFn, "anything"), nullptr);
}

TEST(RuntimeCatalogue, AbiModuleNameAgreesWithWireNamespace) {
  EXPECT_EQ(AbiModuleName(AbiModule::kCelRuntime), "cel");
  EXPECT_EQ(AbiModuleName(AbiModule::kCelHost), "cel_host");
  EXPECT_EQ(AbiModuleName(AbiModule::kCelEnv), "cel_env");
  EXPECT_EQ(AbiModuleName(AbiModule::kCelFn), "cel_fn");
}

TEST(RuntimeCatalogue, AbiVersionIsNonZero) {
  // A zero version would be the default-initialized proto field
  // value, indistinguishable from "ABI section omitted".  Future
  // schema evolutions might leverage that — for now, refuse it.
  EXPECT_GT(kRuntimeAbiVersion, 0u);
}

// Spot-check a few familiar shapes — guards against accidental
// arity flips during catalogue edits.  These are the "obvious"
// kernels every CEL evaluator must have; if their arity changes
// here without a coordinated runtime + codegen change, something
// is wrong.
TEST(RuntimeCatalogue, KernelArityCanaries) {
  struct Canary {
    absl::string_view name;
    uint8_t arity;
    bool returns_i32;
  };
  constexpr Canary kCanaries[] = {
      // out + a + b
      {"cel_int_add_at_vv", 3, false},
      {"cel_equals_at_vv", 3, false},
      // out + v
      {"cel_int_neg_at_v", 2, false},
      {"cel_type_of_at_v", 2, false},
      // size + capacity write-thru
      {"cel_list_create", 2, false},
      // Iter ABI — i32 returns
      {"cel_map_iter_init", 1, true},
      {"cel_map_count", 1, true},
      {"cel_list_arena_view", 1, true},
      {"arena_alloc", 1, true},
      // Niladic
      {"arena_reset", 0, false},
      // 5-arg upper bound
      {"cel_string_replace_n_at_vvvv", 5, false},
  };
  for (const auto& c : kCanaries) {
    const auto* h = FindBuiltinHelper(AbiModule::kCelRuntime, c.name);
    ASSERT_NE(h, nullptr) << c.name;
    EXPECT_EQ(h->num_args, c.arity) << c.name;
    EXPECT_EQ(h->returns_i32, c.returns_i32) << c.name;
  }
}

// ─── CheckRuntimeAbiVersion (Slice E) ────────────────────────────

TEST(CheckRuntimeAbiVersion, MatchingVersionsOk) {
  CelAbi abi;
  abi.set_runtime_abi_version(kRuntimeAbiVersion);
  EXPECT_TRUE(CheckRuntimeAbiVersion(abi).ok());
}

TEST(CheckRuntimeAbiVersion, ZeroVersionEmptyAbiOk) {
  // Minimal synthetic fixture: section present but carries no
  // surface.  Tolerated so WAT-only tests don't break.
  CelAbi abi;
  EXPECT_TRUE(CheckRuntimeAbiVersion(abi).ok());
}

TEST(CheckRuntimeAbiVersion, ZeroVersionWithVariablesRejected) {
  CelAbi abi;
  abi.add_variables()->set_name("x");
  const absl::Status s = CheckRuntimeAbiVersion(abi);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_NE(s.message().find("predates ABI versioning"),
            absl::string_view::npos)
      << s.message();
}

TEST(CheckRuntimeAbiVersion, ZeroVersionWithFieldsRejected) {
  CelAbi abi;
  abi.add_fields()->set_name("f");
  EXPECT_EQ(CheckRuntimeAbiVersion(abi).code(),
            absl::StatusCode::kFailedPrecondition);
}

TEST(CheckRuntimeAbiVersion, ZeroVersionWithAttributesRejected) {
  CelAbi abi;
  abi.add_attributes()->set_variable("u");
  EXPECT_EQ(CheckRuntimeAbiVersion(abi).code(),
            absl::StatusCode::kFailedPrecondition);
}

TEST(CheckRuntimeAbiVersion, ZeroVersionWithTypesRejected) {
  CelAbi abi;
  abi.add_types()->set_fully_qualified_name("acme.User");
  EXPECT_EQ(CheckRuntimeAbiVersion(abi).code(),
            absl::StatusCode::kFailedPrecondition);
}

TEST(CheckRuntimeAbiVersion, MismatchedVersionRejectedWithBothNumbers) {
  CelAbi abi;
  abi.set_runtime_abi_version(kRuntimeAbiVersion + 1);
  const absl::Status s = CheckRuntimeAbiVersion(abi);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition);
  // Both versions should appear in the diagnostic so the user
  // knows which side is stale.
  const std::string msg(s.message());
  EXPECT_NE(msg.find(absl::StrCat("v", kRuntimeAbiVersion + 1)),
            std::string::npos)
      << msg;
  EXPECT_NE(msg.find(absl::StrCat("v", kRuntimeAbiVersion)), std::string::npos)
      << msg;
}

}  // namespace
}  // namespace celwasm::abi
