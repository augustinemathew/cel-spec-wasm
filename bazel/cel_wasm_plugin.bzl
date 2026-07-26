"""Starlark macro that compiles a celfn .idl + author user_fns.cc
into a Component-Model wasm component, in one hermetic Bazel
target.

Pipeline (each step a Bazel action):

  1. `cel generate --idl=<idl> --out_dir=<gen> --language=cpp` →
     `<gen>/fns.wit`, `<gen>/codec.h`, `<gen>/generated_stub.cc`,
     `<gen>/user_fns.h`.
  2. `wit-bindgen c <gen>/fns.wit --world customfn --out-dir <wit>` →
     `<wit>/customfn.h`, `<wit>/customfn.c`,
     `<wit>/customfn_component_type.o`.  (m26 §7.5.1: world name is
     hardcoded `customfn` to match the cel generate emitters.)
  3. wasi-sdk `cc_binary` on
     `[user_fns.cc, generated_stub.cc, customfn.c]` (and
     `customfn_component_type.o` linked as additional link input),
     with `<gen>` and `<wit>` on the include path, deps for proto
     libraries if any.  Transitions to the wasm32-wasip2 platform
     (//third_party/wasi_sdk:wasm32_wasip2) so the wasi-preview2
     cc_toolchain — Component-Model native — produces a `.wasm`
     that is already a CM component (preamble `0x1000d`, NOT a
     core module).
  4. `cel embed-decls --plugin=<core> --idl=<idl> --out=<name>.wasm` →
     the final self-describing artifact: the step-3 component with the
     verbatim `.idl` declaration text embedded as the top-level
     `cel.fns` custom section, ready for `Plugin::Load`
     (doc/implementation-plan/rewrite/m35-plugin-ergonomics.md §4).

No separate `wasm-tools component new` wrap step is needed when
targeting wasm32-wasip2: the toolchain emits the component
directly.  cel_runtime stays on the wasm32-wasi-threads stack;
the two toolchains live side-by-side and toolchain resolution
picks one based on the target platform constraint
(`wasi_threads_on` vs `wasi_threads_off`).

Toolchain plumbing in //:MODULE.bazel:
  - `//tools/cel:cel`              — host-binary that emits the four files.
  - `//third_party/wit_bindgen:wit-bindgen` — host-selected wit-bindgen CLI.
  - `//third_party/wasm_tools:wasm-tools`   — host-selected wasm-tools CLI.
  - `//third_party/wasi_sdk:wasm32_wasi`    — target platform for step 3.
  - `@com_google_absl//absl/time`           — absl::Duration / Time, via
    the wasi-sdk cc_toolchain.

Out-of-scope (deferred to follow-up workstreams):
  - Go authoring (`language = "go"`) — H.4.
  - `--adapt` to wasi-preview1 reactor — components that import WASI
    today need the author to wrap manually.  Pure-compute fns (the
    common case) need no WASI imports.
"""

load("//third_party/wasi_sdk:wasm_cc_binary.bzl", "wasm_p2_cc_binary")

# Deps the author can opt into when their IDL uses Duration /
# Timestamp — codec.h conditionally pulls `absl/time/time.h`.  Caller
# adds `@com_google_absl//absl/time` to `deps` when relevant.  We
# don't auto-add it here: linking absl into a wasm32-wasi component
# drags `wasi_snapshot_preview1::fd_close` imports that wasm-tools
# can't satisfy without a preview1→preview2 adapter.
_AUTOMATIC_WASM_DEPS = []

# ── Step 1: cel generate ──────────────────────────────────────────────

def _cel_generate(name, idl, extra_includes, tags):
    """Emits the four files into the package-private gen tree."""
    gen_dir = name + "_gen"
    outs = [
        gen_dir + "/fns.wit",
        gen_dir + "/codec.h",
        gen_dir + "/generated_stub.cc",
        gen_dir + "/user_fns.h",
    ]
    inc_flags = (
        ("--include=" + ",".join(extra_includes)) if extra_includes else ""
    )
    native.genrule(
        name = name + "_gen_files",
        srcs = [idl],
        outs = outs,
        cmd = (
            "$(execpath //tools/cel:cel) generate " +
            "--idl=$(execpath {idl}) ".format(idl = idl) +
            "--language=cpp " +
            "--out_dir=$(RULEDIR)/{gen} ".format(gen = gen_dir) +
            inc_flags
        ),
        tools = ["//tools/cel:cel"],
        tags = tags,
    )
    return outs, gen_dir

# ── Step 2: wit-bindgen c ─────────────────────────────────────────────

def _wit_bindgen_c(name, fns_wit, tags):
    """Emits customfn.{h,c,_component_type.o} from fns.wit."""
    wit_dir = name + "_wit"
    outs = [
        wit_dir + "/customfn.h",
        wit_dir + "/customfn.c",
        wit_dir + "/customfn_component_type.o",
    ]
    native.genrule(
        name = name + "_wit_files",
        srcs = [fns_wit],
        outs = outs,
        cmd = (
            "$(execpath //third_party/wit_bindgen:wit-bindgen) c " +
            "$(execpath {wit}) ".format(wit = fns_wit) +
            "--world customfn " +
            "--out-dir $(RULEDIR)/{wit_dir}".format(wit_dir = wit_dir)
        ),
        tools = ["//third_party/wit_bindgen:wit-bindgen"],
        tags = tags,
    )
    return outs, wit_dir

# ── Step 3: wasi-sdk core wasm module ─────────────────────────────────

def _core_wasm(name, srcs, headers, gen_dir, wit_dir, component_type_obj,
               deps, copts, tags):
    """Builds the wasm32-wasi core module via the existing wasi-sdk toolchain."""
    core_name = name + "_core"
    native.cc_binary(
        name = core_name + ".bin",
        srcs = srcs + [component_type_obj],
        deps = deps + _AUTOMATIC_WASM_DEPS + headers,
        copts = copts + [
            "-I$(GENDIR)/" + native.package_name() + "/" + gen_dir,
            "-I$(GENDIR)/" + native.package_name() + "/" + wit_dir,
            # Component-Model export visibility — wit-bindgen attaches
            # `__attribute__((__weak__, __export_name__(...)))` to its
            # canonical-ABI helpers, which need the linker to keep them.
            "-fvisibility=default",
        ],
        linkopts = [
            "-Wl,--strip-all",
        ],
        # `manual` so the bare cc_binary doesn't try to link at default
        # platform; the wasm_p2_cc_binary wrapper below does the
        # platform transition.  Plus any user-supplied tags.
        tags = ["manual"] + tags,
        target_compatible_with = ["@platforms//cpu:wasm32"],
    )
    wasm_p2_cc_binary(
        name = core_name,
        binary = ":" + core_name + ".bin",
        tags = tags,
    )
    return ":" + core_name

# ── Step 4: cel embed-decls ───────────────────────────────────────────

def _embed_decls(name, core_label, idl, tags):
    """Embeds the `.idl` declaration text into the component.

    Appends the verbatim idl bytes as the top-level `cel.fns` custom
    section (via `cel embed-decls`), producing the final
    self-describing `<name>.wasm` — the artifact carries its own
    declarations, and `Plugin::Load` parses them back out.  The
    output keeps the `<name>.wasm` filename so consumers can
    `data = [":<name>"]` and the file lands unchanged in runfiles.
    """
    native.genrule(
        name = name,
        srcs = [core_label, idl],
        outs = [name + ".wasm"],
        cmd = (
            "$(execpath //tools/cel:cel) embed-decls " +
            "--plugin=$(execpath {core}) ".format(core = core_label) +
            "--idl=$(execpath {idl}) ".format(idl = idl) +
            "--out=$@"
        ),
        tools = ["//tools/cel:cel"],
        tags = tags,
        visibility = ["//visibility:public"],
    )

# ── Public macro ──────────────────────────────────────────────────────

def cel_wasm_plugin(
        name,
        idl,
        user_fns,
        deps = [],
        extra_includes = [],
        copts = None,
        tags = None):
    """Compile a celfn .idl + author user_fns.cc into a CM wasm component.

    Args:
      name: target name.  Output is `<name>.wasm` plus internal
            intermediate targets (gen / wit / core).
      idl: label of the `.idl` celfn source (input to `cel generate`
            and, verbatim, to the embedded `cel.fns` section).
      user_fns: list of labels — the author's `user_fns.cc` (and any
                extra `.cc/.h` they want compiled with it).  These
                implement the declarations in the generated `user_fns.h`.
      deps: cc_library deps for the wasi-sdk `cc_binary` step — usually
            proto cc_proto_library targets when any decl is
            `proto(...)`-typed.  `@com_google_absl//absl/time` is added
            automatically (the emitter unconditionally uses it).
      extra_includes: passed verbatim to
            `cel generate --extra_includes=<inc>` (one flag per entry).
            Typical use: `["acme/user.pb.h"]` for proto-typed fns so
            the generated `user_fns.h` finds the proto header.
      copts: extra copts for the wasi-sdk cc_binary step (default: []).
      tags: bazel tags to apply to the final wasm plugin target.

    The WIT package name is not configurable: it is always
    `cel:<module>/fns@0.1.0`, derived from the IDL's `Module foo;`
    directive (fallback module `customfn`) — see
    doc/implementation-plan/rewrite/m35-plugin-ergonomics.md §4.

    Produces:
      `<name>.wasm` — a self-describing Component-Model component
      carrying its declarations in an embedded `cel.fns` custom
      section.  The embedder loads it via `Plugin::Load(bytes)`
      (`Engine::AddPlugin(plugin_bytes, lib)` remains the
      explicit-decls escape hatch).
    """
    copts = copts if copts != None else []
    tags = tags if tags != None else []

    # Step 1: cel generate.
    gen_outs, gen_dir = _cel_generate(name, idl, extra_includes, tags)
    fns_wit = [o for o in gen_outs if o.endswith("fns.wit")][0]
    codec_h = [o for o in gen_outs if o.endswith("codec.h")][0]
    user_fns_h = [o for o in gen_outs if o.endswith("user_fns.h")][0]
    generated_stub_cc = [o for o in gen_outs if o.endswith("generated_stub.cc")][0]

    # Step 2: wit-bindgen c.
    wit_outs, wit_dir = _wit_bindgen_c(name, fns_wit, tags)
    customfn_h = [o for o in wit_outs if o.endswith("customfn.h")][0]
    customfn_c = [o for o in wit_outs if o.endswith("customfn.c")][0]
    component_type_o = [
        o for o in wit_outs if o.endswith("customfn_component_type.o")
    ][0]

    # Headers go into a cc_library so the cc_binary can pick them up as
    # deps; the actual -I flags are added via copts (above) since the
    # generated headers live in bazel-bin and aren't part of any
    # package's `hdrs`.
    native.cc_library(
        name = name + "_gen_hdrs",
        hdrs = [codec_h, user_fns_h, customfn_h],
        tags = ["manual"] + tags,
        target_compatible_with = ["@platforms//cpu:wasm32"],
    )

    # Step 3: wasi-sdk core wasm.
    core_label = _core_wasm(
        name = name,
        srcs = user_fns + [generated_stub_cc, customfn_c],
        headers = [":" + name + "_gen_hdrs"],
        gen_dir = gen_dir,
        wit_dir = wit_dir,
        component_type_obj = component_type_o,
        deps = deps,
        copts = copts,
        tags = tags,
    )

    # Step 4: embed the .idl declaration text as the `cel.fns`
    # custom section, producing the final `<name>.wasm`.
    _embed_decls(name = name, core_label = core_label, idl = idl, tags = tags)
