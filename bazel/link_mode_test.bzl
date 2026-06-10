"""Bazel macro that emits TWO cc_test targets per source — one per
link mode (kDynamic, kStatic) — so any test in the project can opt
into running under both modes without per-test refactoring.

This is the project-wide generalisation of `e2e/link_mode_e2e_test.bzl`
(originally written for the e2e suite).  Use either; the cc_test
emission shape is the same.  The companion header for non-e2e tests
that need the link-mode value at compile time is
`bazel/link_mode_test_helpers.h` (provides `kTestLinkMode`).

Usage:

    load("//bazel:link_mode_test.bzl", "link_mode_cc_test")

    link_mode_cc_test(
        name = "engine_test",
        srcs = ["engine_test.cc"],
        deps = [
            "//bazel:link_mode_test_helpers",
            ...
        ],
    )

Result: `:engine_test_dynamic` AND `:engine_test_static`.  The
`_static` target is built with `-DCELWASM_TEST_USE_STATIC_LINK_MODE`
defined; the header reads that macro to surface `kTestLinkMode`.
"""

load("@rules_cc//cc:defs.bzl", "cc_test")

def link_mode_cc_test(name, srcs, deps, defines = None, **kwargs):
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
        defines = base_defines + ["CELWASM_TEST_USE_STATIC_LINK_MODE"],
        **kwargs
    )
