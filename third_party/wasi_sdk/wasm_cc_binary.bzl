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
