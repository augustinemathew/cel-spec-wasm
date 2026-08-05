"""Bazel macro that emits TWO cc_test targets per e2e source — one per
link mode (kDynamic, kStatic) — so every e2e test runs under both
modes without per-test refactoring.

The same `srcs` and `deps` are reused; only a `defines` entry differs.
`CELWASM_E2E_USE_STATIC_LINK_MODE` is read by `e2e/link_mode_e2e_helpers.h`
to pick the `CompilerOptions::LinkMode` used by every CompilePlan call.

Usage in `e2e/BUILD.bazel`:

    load(":link_mode_e2e_test.bzl", "link_mode_e2e_cc_test")

    link_mode_e2e_cc_test(
        name = "operators_test",
        srcs = ["operators_test.cc"],
        deps = [...],
    )

Result: `//e2e:m5_test_dynamic` AND `//e2e:m5_test_static` — both
exercise the same TEST_F bodies, against opposite link modes.
"""

load("@rules_cc//cc:defs.bzl", "cc_test")

def link_mode_e2e_cc_test(name, srcs, deps, defines = None, **kwargs):
    """Emits <name>_dynamic and <name>_static cc_test targets."""
    base_defines = list(defines) if defines else []

    cc_test(
        name = name + "_dynamic",
        srcs = srcs,
        deps = deps,
        defines = base_defines,
        **kwargs
    )

    cc_test(
        name = name + "_static",
        srcs = srcs,
        deps = deps,
        defines = base_defines + ["CELWASM_E2E_USE_STATIC_LINK_MODE"],
        **kwargs
    )
