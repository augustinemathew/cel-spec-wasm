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

def _wasm_transition_impl(_settings, _attr):
    return {
        "//command_line_option:platforms": str(
            Label("//third_party/wasi_sdk:wasm32_wasi"),
        ),
        # The transition is one-way; cc_binary's host-config dependencies
        # (compilers etc.) come from toolchain resolution, not from the
        # target config, so we don't need to thread the host platform.
    }

_wasm_transition = transition(
    implementation = _wasm_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:platforms"],
)

def _wasm_p2_transition_impl(_settings, _attr):
    """Transitions to the wasm32-wasip2 (non-threads) platform.

    Used by the `cel_wasm_component` Starlark macro (m26 §6) so the
    Component-Model preview2 ABI's non-shared-memory requirement is
    satisfied at the wasi-sdk cc_binary step.
    """
    return {
        "//command_line_option:platforms": str(
            Label("//third_party/wasi_sdk:wasm32_wasip2"),
        ),
    }

_wasm_p2_transition = transition(
    implementation = _wasm_p2_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:platforms"],
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

def _wasm_cpp_transition_impl(settings, _attr):
    """wasm32-wasi-threads, with C++ exceptions + RTTI enabled.

    The toolchain's `default_compile_flags` pin `-fno-exceptions
    -fno-rtti` to keep the tiny C runtime small, but cel-cpp's parser /
    type-checker (ANTLR4, `dynamic_cast`/`throw`) and Binaryen require
    both.  `-frtti -fexceptions` are appended AFTER the toolchain flags
    and win (clang takes the last), and the whole `cc_binary` subgraph —
    cel-cpp + Binaryen + the C ABI — sees them because a `--cxxopt`
    transition flows to every C++ compile in the configuration.  Scoped
    to this transition so the runtime's own wasm build keeps its minimal
    flags.
    """
    return {
        "//command_line_option:platforms": str(
            Label("//third_party/wasi_sdk:wasm32_wasi"),
        ),
        "//command_line_option:cxxopt": settings["//command_line_option:cxxopt"] +
                                        ["-frtti", "-fexceptions"],
    }

_wasm_cpp_transition = transition(
    implementation = _wasm_cpp_transition_impl,
    inputs = ["//command_line_option:cxxopt"],
    outputs = [
        "//command_line_option:platforms",
        "//command_line_option:cxxopt",
    ],
)

# Like `wasm_cc_binary`, but for a `cc_binary` whose C++ dependency graph
# (the CEL compiler: cel-cpp + Binaryen) needs exceptions + RTTI.
wasm_cpp_cc_binary = rule(
    implementation = _wasm_cc_binary_impl,
    attrs = {
        "binary": attr.label(
            cfg = _wasm_cpp_transition,
            mandatory = True,
            doc = "A `cc_binary` with exceptions/RTTI-requiring C++ deps; built under wasm32-wasi-threads with -frtti -fexceptions.",
        ),
    },
)
