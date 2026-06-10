// `kTestLinkMode` — the `CompilerOptions::LinkMode` a test binary should
// use when calling `Compile()`.  Selected at compile time by the
// `bazel/link_mode_test.bzl::link_mode_cc_test` macro: the `_static`
// variant defines `CELWASM_TEST_USE_STATIC_LINK_MODE`; the `_dynamic`
// variant does not.
//
// Usage in unit tests:
//
//   #include "bazel/link_mode_test_helpers.h"
//   ...
//   CompilerOptions opts;
//   opts.link_mode = celwasm::kTestLinkMode;
//   auto program = compiler->Compile("expr", opts);
//
// Why this header exists separately from
// `e2e/link_mode_e2e_helpers.h`: the e2e helpers also bundle
// `CompilePlan`/`EvalOk`/`GlobalEngine` which are e2e-specific
// scaffolding.  Unit tests under `compiler/` and `eval/` don't need
// that — they just need the `LinkMode` selector.

#ifndef CELWASM_BAZEL_LINK_MODE_TEST_HELPERS_H_
#define CELWASM_BAZEL_LINK_MODE_TEST_HELPERS_H_

#include "compiler/compiler.h"

namespace celwasm {

inline constexpr CompilerOptions::LinkMode kTestLinkMode =
#ifdef CELWASM_TEST_USE_STATIC_LINK_MODE
    CompilerOptions::LinkMode::kStatic;
#else
    CompilerOptions::LinkMode::kDynamic;
#endif

}  // namespace celwasm

#endif  // CELWASM_BAZEL_LINK_MODE_TEST_HELPERS_H_
