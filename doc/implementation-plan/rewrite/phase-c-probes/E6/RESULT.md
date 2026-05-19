# Probe E6: rules_foreign_cc + abseil's CMake (Path B fallback)

**Status:** PARTIAL — sketch only.

Per the brief's revised §4, Path A is the chosen path; E2 + E6
exist as risk-mitigation context (what would the fallback cost
if Path A were infeasible).  E3+E4 succeeded, so this probe is
documented to a level sufficient to estimate fallback cost
without a full build.

## Method (not executed)

`rules_foreign_cc` is already a `bazel_dep` in MODULE.bazel
(version 0.15.1).  Its `cmake()` rule wraps an external CMake
build with bazel inputs/outputs.

Sketch of the `BUILD.bazel` that would drive abseil's CMake under
the wasi-sdk toolchain:

```python
load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

# Pull in abseil-cpp as a separate http_archive (NOT the bazel_dep,
# which uses the host bazel toolchain).
http_archive(
    name = "abseil_cpp_src",
    sha256 = "<…>",
    strip_prefix = "abseil-cpp-20260107.0",
    urls = ["https://github.com/abseil/abseil-cpp/.../20260107.0.tar.gz"],
    build_file_content = """
filegroup(
    name = "all_files",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
""",
)

# The cmake() rule.
cmake(
    name = "absl_wasm",
    lib_source = "@abseil_cpp_src//:all_files",
    out_static_libs = [
        "libabsl_strings.a",
        "libabsl_time.a",
        # … other libs per E7 ordering.
    ],
    cache_entries = {
        "CMAKE_TOOLCHAIN_FILE": "$EXT_BUILD_ROOT/wasm_compilation_experiments/exp1_re2/wasi-toolchain.cmake",
        "CMAKE_BUILD_TYPE": "MinSizeRel",
        "ABSL_PROPAGATE_CXX_STD": "ON",
    },
    tags = ["manual"],
)
```

## What this gains over Path A

  - Reuses the proven `exp1_re2/wasi-toolchain.cmake` shape, so the
    same flag set + `absl-wasm.patch` workflow we already know
    works.  No `cc_toolchain_config.bzl` to maintain.

## What this loses vs Path A

  - **Less hermetic.**  `rules_foreign_cc` invokes CMake inside a
    bazel-sandbox-but-not-really shell context; CMake then forks
    out to make/ninja which forks to clang.  Build cache
    invalidation rules diverge from bazel-native targets.
  - **Build-graph opacity.**  bazel cannot see individual `.cc`
    file changes inside the absl tree — the whole `cmake()` rule
    rebuilds on any input change.  Path A's bazel-native cc_library
    rules give per-file incrementality.
  - **Cross-platform fragility.**  CMake's wasi-toolchain.cmake
    relies on a hardcoded `WASI_SDK_PREFIX`; in bazel context we'd
    have to template it via `@bazel_tools` so the wasi-sdk path
    resolves from `@wasi_sdk_<plat>//:all`.  Extra glue.
  - **Sub-optimal subset selection.**  Path A's bazel `cc_library`
    rules let us depend on `absl/time` alone and bazel pulls only
    the transitive .cc files; rules_foreign_cc builds the *whole*
    absl install.  Cost: ~7.4 MB of static libs vs Path A's
    on-demand subset.

## Estimated cost if Path A were blocked

If we had to pivot to B today (we don't):
  - 0.5d to wire `rules_foreign_cc.cmake` calling the existing
    `wasi-toolchain.cmake` (modified to take `WASI_SDK_PREFIX` from
    a bazel-substituted variable).
  - 0.5d to expose the resulting `.a` files via `cc_import` and
    consume from the cel_runtime link step.
  - 1.0d to patch the cross-platform sysroot pathing so the
    bazel-fetched `@wasi_sdk_<plat>` archive (not a host-installed
    SDK) provides the toolchain.

Total: ~2d.  Path A's 0.5d (delta on top of what's now wired)
beats this comfortably.

## Decision

**Stay on Path A.**  Path B is the fallback if a future bazel /
rules_cc change breaks our `cc_toolchain_config.bzl`.  The
toolchain wiring we shipped in E3/E4 is small enough (~250 LoC
config.bzl + 3 wrapper scripts) that maintaining it long-term
is cheaper than chasing the rules_foreign_cc hermeticity
caveats.

## Next-step implication

No action.  Path B can be revisited if Path A regresses; for
now this probe is informational.
