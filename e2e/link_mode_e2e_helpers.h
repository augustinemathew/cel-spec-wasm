// Shared e2e helpers + link-mode dispatch for every test in `e2e/`.
//
// **Why this exists.** Every e2e test wants the same Compile → Plan →
// Eval scaffolding, and we want every test exercised under BOTH
// `CompilerOptions::LinkMode::kDynamic` AND `kStatic`.  Without this
// header, each test fixture re-declares a private `CompilePlan` that
// uses the compiler's default link mode — so the suite runs the
// default mode only, leaving the other mode untested.
//
// **How both modes are covered.** This header reads a compile-time
// macro `CELWASM_E2E_USE_STATIC_LINK_MODE` and selects the e2e link
// mode accordingly.  The bazel macro `link_mode_e2e_cc_test`
// (in `e2e/BUILD.bazel`) emits two `cc_test` targets per source file
// — `<name>_dynamic` and `<name>_static` — with the macro defined on
// the latter.  Both targets run the same TEST_F bodies; only the
// `CompilePlan` helper's link-mode choice differs.
//
// **Usage in a test file.**  Replace the per-file `CompilePlan` /
// `EvalOk` / `GlobalEngine` duplicates with:
//
//   #include "e2e/link_mode_e2e_helpers.h"
//   ...
//   auto instance = e2e::CompilePlan(*compiler, "1 + 2");
//   EXPECT_EQ(*e2e::EvalOk(instance, activation).AsInt(), 3);
//
// **What's NOT here.**  Activation-aware variants, ProtoMessage-
// taking variants, expression-source-only one-arg overloads where
// the test owns its own Compiler — those stay in the per-file
// fixture headers if they need test-specific behaviour.

#ifndef CELWASM_E2E_LINK_MODE_E2E_HELPERS_H_
#define CELWASM_E2E_LINK_MODE_E2E_HELPERS_H_

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"

namespace celwasm::e2e {

// Link mode used by every e2e test in this binary.  Selected at
// compile time via the bazel `link_mode_e2e_cc_test` macro.  Dual
// emission means each test source builds twice — once per mode —
// so every TEST_F runs under both link modes without per-test
// changes.
inline constexpr CompilerOptions::LinkMode kE2ELinkMode =
#ifdef CELWASM_E2E_USE_STATIC_LINK_MODE
    CompilerOptions::LinkMode::kStatic;
#else
    CompilerOptions::LinkMode::kDynamic;
#endif

// A process-wide `Engine` shared across tests.  Engine instantiation
// is expensive (wasmtime compile of `cel_runtime.wasm`); per-Plan
// state lives on the `Instance` so a single Engine serves every
// test in this binary.
inline Engine& GlobalEngine() {
  static Engine* engine = [] {
    auto e = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(e);
    return new Engine(*std::move(e));
  }();
  return *engine;
}

// Returns `CompilerOptions` with `link_mode` already set to the e2e
// link mode.  Use in tests that call `compiler.Compile(expr, opts)`
// directly (rather than through `CompilePlan`) so the explicit-options
// path also picks up the dual-mode dispatch.  Example:
//   auto program = compiler->Compile("inc(x)", e2e::DefaultOpts());
inline CompilerOptions DefaultOpts() {
  CompilerOptions opts;
  opts.link_mode = kE2ELinkMode;
  return opts;
}

// Compile + Plan in one call.  `ABSL_CHECK`s each stage's status —
// use the explicit `compiler.Compile(...)` / `engine.Plan(...)` form
// in tests that assert on a Compile or Plan failure.
inline Instance CompilePlan(const Compiler& compiler,
                            absl::string_view source) {
  auto program = compiler.Compile(source, DefaultOpts());
  ABSL_CHECK_OK(program) << source;
  auto instance = GlobalEngine().Plan(*program);
  ABSL_CHECK_OK(instance) << source;
  return *std::move(instance);
}

// Convenience for `Eval(activation)`.  ABSL_CHECKs the status — use
// the raw `instance.Eval(activation)` form when inspecting an
// expected failure.
inline Value EvalOk(Instance& instance, const Activation& activation) {
  auto v = instance.Eval(activation);
  ABSL_CHECK_OK(v);
  return *std::move(v);
}

// Convenience for `Eval()` (no activation).
inline Value EvalOk(Instance& instance) {
  auto v = instance.Eval();
  ABSL_CHECK_OK(v);
  return *std::move(v);
}

}  // namespace celwasm::e2e

#endif  // CELWASM_E2E_LINK_MODE_E2E_HELPERS_H_
