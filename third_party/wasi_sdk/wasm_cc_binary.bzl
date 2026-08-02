"""Outgoing-platform-transition wrapper around a `cc_binary`.

A `cc_binary` that targets wasm32-wasi must be built under
`--platforms=//third_party/wasi_sdk:wasm32_wasi`.  Downstream native
rules — typically a `genrule` that embeds the .wasm bytes as a
C/C++ source array — run under the default host platform and can't
flip `--platforms` for one input.

`wasm_cc_binary` is the bridge: it depends on a `cc_binary` with an
outgoing transition to `wasm32_wasi`, exposes the resulting wasm
artefact as a single file with the wrapper's name, and is usable
as a normal `srcs` input from native-config consumers.

Usage:

    cc_binary(
        name = "cel_runtime_wasm.bin",
        srcs = [...],
        linkopts = [...],
        tags = ["manual"],
    )

    wasm_cc_binary(
        name = "cel_runtime_wasm",
        binary = ":cel_runtime_wasm.bin",
    )

    genrule(
        name = "cel_runtime_wasm_bytes_cc",
        srcs = [":cel_runtime_wasm"],
        outs = ["cel_runtime_wasm_bytes.cc"],
        cmd = "...od -An -tu1 -v $(location :cel_runtime_wasm)...",
    )
"""

# Command-line compile/link flag lists that must NOT survive into the
# wasm configuration.  They are set for the host C++ build (`--copt`,
# `--linkopt`, and the `--config=asan` / `--config=tsan` blocks in
# .bazelrc that expand to them), and a wasm32-wasi cross-compile has no
# use for any of them: the wasm build's flags come from the wasi-sdk
# cc_toolchain plus the target's own BUILD attributes, neither of which
# is a command-line option and neither of which this clears.
#
# Clearing them is load-bearing, not tidiness.  `--config=asan` adds
# `--copt=-fsanitize=address`, which wasi-sdk clang refuses:
#
#   clang: error: unsupported option '-fsanitize=address'
#          for target 'wasm32-unknown-wasi'
#
# so without the reset, `bazel test --config=asan //...` fails at
# `cel_runtime.wasm` before a single test runs.  The same reset also
# keeps the emitted wasm independent of the host config — the runtime's
# `-O3 -flto` comes from runtime/BUILD.bazel, so a host `--copt=-O1`
# can no longer quietly de-optimize it, and the wasm actions stay
# cache-hits across a host-config switch.
_HOST_FLAG_OPTIONS = [
    "//command_line_option:copt",
    "//command_line_option:conlyopt",
    "//command_line_option:cxxopt",
    "//command_line_option:linkopt",
]

def _wasm_transition_impl(_settings, _attr):
    settings = {
        "//command_line_option:platforms": str(
            Label("//third_party/wasi_sdk:wasm32_wasi"),
        ),
        # The transition is one-way; cc_binary's host-config dependencies
        # (compilers etc.) come from toolchain resolution, not from the
        # target config, so we don't need to thread the host platform.
    }
    for opt in _HOST_FLAG_OPTIONS:
        settings[opt] = []
    return settings

_wasm_transition = transition(
    implementation = _wasm_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:platforms"] + _HOST_FLAG_OPTIONS,
)

def _wasm_p2_transition_impl(_settings, _attr):
    """Transitions to the wasm32-wasip2 (non-threads) platform.

    Used by the `cel_wasm_component` Starlark macro (m26 §6) so the
    Component-Model preview2 ABI's non-shared-memory requirement is
    satisfied at the wasi-sdk cc_binary step.
    """
    settings = {
        "//command_line_option:platforms": str(
            Label("//third_party/wasi_sdk:wasm32_wasip2"),
        ),
    }
    for opt in _HOST_FLAG_OPTIONS:
        settings[opt] = []
    return settings

_wasm_p2_transition = transition(
    implementation = _wasm_p2_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:platforms"] + _HOST_FLAG_OPTIONS,
)

def _wasm_cc_binary_impl(ctx):
    src_files = ctx.attr.binary[0][DefaultInfo].files.to_list()
    if len(src_files) != 1:
        fail(
            "wasm_cc_binary: expected exactly one file from binary, got " +
            str(len(src_files)),
        )
    out = ctx.actions.declare_file(ctx.label.name + ".wasm")
    ctx.actions.symlink(output = out, target_file = src_files[0])
    return [DefaultInfo(files = depset([out]))]

wasm_cc_binary = rule(
    implementation = _wasm_cc_binary_impl,
    attrs = {
        "binary": attr.label(
            cfg = _wasm_transition,
            mandatory = True,
            doc = "A `cc_binary` target; built under the wasm32-wasi-threads platform.",
        ),
    },
)

wasm_p2_cc_binary = rule(
    implementation = _wasm_cc_binary_impl,
    attrs = {
        "binary": attr.label(
            cfg = _wasm_p2_transition,
            mandatory = True,
            doc = "A `cc_binary` target; built under the wasm32-wasip2 (Component-Model native) platform.",
        ),
    },
)
