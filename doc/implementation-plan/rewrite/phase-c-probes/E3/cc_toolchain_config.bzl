"""cc_toolchain_config for wasm32-wasi using the @wasi_sdk_darwin_arm64 SDK.

This is the Path A gateway probe.  The goal is:
  bazel build //…:hello_wasm --platforms=//doc/…/E3:wasm32_wasi
…producing a working wasm32-wasi `.wasm` whose `add` export wasmtime
can invoke.

The reference implementation is `@rules_cc//cc/private/toolchain:
unix_cc_toolchain_config.bzl` (~2k LoC, ~50 features).  This config
is intentionally minimal — only the features wasi-sdk needs to mirror
the standalone `wasi-sdk-25/bin/clang++ --target=wasm32-wasi …`
invocation that exp1_re2 proved works.

Key wiring decisions:

  - tool_paths are package-relative paths.  We place wrapper shim
    binaries (sh scripts) in the same package as the BUILD file
    that calls `cc_toolchain_config`, and the shims exec into the
    real wasi-sdk binaries via absolute path.  This keeps the
    config self-contained while letting bazel locate the tools.

  - cxx_builtin_include_directories names the wasi-sysroot include
    paths; otherwise clang complains about every sysroot include
    being "non-standard".

  - The features wire up:
      * `default_compile_flags` (always-on): --target=wasm32-wasi
        + the C/C++ standard-version flags + warnings.
      * `default_link_flags` (always-on): -nostartfiles
        -Wl,--no-entry --target=wasm32-wasi.
      * `opt_feature` (-c opt): -O3 -flto.
      * `dbg_feature` (-c dbg): -O0 -g.
      * `user_compile_flags` / `user_link_flags`: propagate copts /
        linkopts from BUILD targets.
      * `sysroot`: --sysroot=<wasi-sysroot path>.
      * `unfiltered_compile_flags`: -no-canonical-prefixes (relevant
        for hermetic header paths).
"""

load(
    "@rules_cc//cc:cc_toolchain_config_lib.bzl",
    "feature",
    "flag_group",
    "flag_set",
    "tool_path",
)
load(
    "@rules_cc//cc/common:cc_common.bzl",
    "cc_common",
)
load(
    "@rules_cc//cc/toolchains:cc_toolchain_config_info.bzl",
    "CcToolchainConfigInfo",
)

# Action names used in feature definitions.  These match the strings
# the C++ rules use internally.  Imported as constants from rules_cc
# below for legibility.
_ALL_COMPILE_ACTIONS = [
    "c-compile",
    "c++-compile",
    "c++-header-parsing",
    "c++-module-compile",
    "c++-module-codegen",
    "preprocess-assemble",
    "assemble",
    "linkstamp-compile",
    "lto-backend",
    "clif-match",
]

_ALL_CPP_COMPILE_ACTIONS = [
    "c++-compile",
    "c++-header-parsing",
    "c++-module-compile",
    "c++-module-codegen",
    "linkstamp-compile",
]

_ALL_C_COMPILE_ACTIONS = [
    "c-compile",
]

_ALL_LINK_ACTIONS = [
    "c++-link-executable",
    "c++-link-dynamic-library",
    "c++-link-nodeps-dynamic-library",
]

_ARCHIVE_ACTIONS = [
    "c++-link-static-library",
]

def _impl(ctx):
    sysroot_path = ctx.attr.sysroot_path

    # Explicit -isystem flags for sysroot include dirs.  In a normal
    # unix cc_toolchain, --sysroot=... handles this implicitly via
    # clang's driver, but wasi-sdk's libc++ lives under
    # `<sysroot>/include/wasm32-wasi/c++/v1`, not `<sysroot>/include/c++/v1`,
    # so without explicit -isystem flags clang fails to find `<array>`,
    # `<string>` and friends.
    include_dirs_flags = [
        flag
        for include_dir in ctx.attr.builtin_include_directories
        for flag in ["-isystem", include_dir]
    ]

    # Compile flags applied to every C / C++ compile action.
    default_compile_flags = feature(
        name = "default_compile_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = _ALL_COMPILE_ACTIONS,
                flag_groups = [
                    flag_group(
                        flags = [
                            "--target=wasm32-wasi-threads",
                            "-pthread",
                            "-no-canonical-prefixes",
                            "-Wall",
                            # wasi-emulation defines (matches exp1_re2's
                            # wasi-toolchain.cmake).  cctz uses
                            # `<chrono>`'s steady_clock — needs
                            # _WASI_EMULATED_PROCESS_CLOCKS.  absl base
                            # `sleep_for` needs _WASI_EMULATED_SIGNAL.
                            "-D_WASI_EMULATED_SIGNAL",
                            "-D_WASI_EMULATED_PROCESS_CLOCKS",
                            "-D_WASI_EMULATED_MMAN",
                            "-D_WASI_EMULATED_GETPID",
                        ] + include_dirs_flags,
                    ),
                ],
            ),
            flag_set(
                actions = _ALL_C_COMPILE_ACTIONS,
                flag_groups = [
                    flag_group(flags = ["-std=c11"]),
                ],
            ),
            flag_set(
                actions = _ALL_CPP_COMPILE_ACTIONS,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-std=c++17",
                            "-fno-rtti",
                            "-fno-exceptions",
                        ],
                    ),
                ],
            ),
        ],
    )

    default_link_flags = feature(
        name = "default_link_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = _ALL_LINK_ACTIONS,
                flag_groups = [
                    flag_group(
                        flags = [
                            "--target=wasm32-wasi-threads",
                            "-pthread",
                            "-nostartfiles",
                            "-Wl,--no-entry",
                            "-Wl,--allow-undefined",
                            "-Wl,--shared-memory",
                            "-Wl,--max-memory=67108864",
                            "-lwasi-emulated-signal",
                            "-lwasi-emulated-process-clocks",
                            "-lwasi-emulated-mman",
                            "-lwasi-emulated-getpid",
                        ],
                    ),
                ],
            ),
        ],
    )

    opt_feature = feature(
        name = "opt",
        flag_sets = [
            flag_set(
                actions = _ALL_COMPILE_ACTIONS,
                flag_groups = [flag_group(flags = ["-O3"])],
            ),
        ],
    )

    dbg_feature = feature(
        name = "dbg",
        flag_sets = [
            flag_set(
                actions = _ALL_COMPILE_ACTIONS,
                flag_groups = [flag_group(flags = ["-O0", "-g"])],
            ),
        ],
    )

    # Propagate user-supplied copts from cc_library / cc_binary.
    user_compile_flags = feature(
        name = "user_compile_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = _ALL_COMPILE_ACTIONS,
                flag_groups = [
                    flag_group(
                        flags = ["%{user_compile_flags}"],
                        iterate_over = "user_compile_flags",
                        expand_if_available = "user_compile_flags",
                    ),
                ],
            ),
        ],
    )

    # Propagate user-supplied linkopts.
    user_link_flags = feature(
        name = "user_link_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = _ALL_LINK_ACTIONS,
                flag_groups = [
                    flag_group(
                        flags = ["%{user_link_flags}"],
                        iterate_over = "user_link_flags",
                        expand_if_available = "user_link_flags",
                    ),
                ],
            ),
        ],
    )

    sysroot_feature = feature(
        name = "sysroot",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = _ALL_COMPILE_ACTIONS + _ALL_LINK_ACTIONS,
                flag_groups = [
                    flag_group(
                        flags = ["--sysroot=" + sysroot_path],
                    ),
                ],
            ),
        ],
    )

    # supports_pic must be off — wasm doesn't have PIC the way ELF does.
    supports_pic_feature = feature(name = "supports_pic", enabled = False)

    features = [
        default_compile_flags,
        default_link_flags,
        opt_feature,
        dbg_feature,
        user_compile_flags,
        user_link_flags,
        sysroot_feature,
        supports_pic_feature,
    ]

    # tool_paths are package-relative.  The wrapper scripts in the
    # E3 package exec the real wasi-sdk binaries by absolute path.
    tool_paths = [
        tool_path(name = "gcc", path = ctx.attr.clang_path),
        tool_path(name = "ld", path = ctx.attr.clang_path),
        tool_path(name = "ar", path = ctx.attr.ar_path),
        tool_path(name = "cpp", path = ctx.attr.clang_path),
        tool_path(name = "gcov", path = "/bin/false"),
        tool_path(name = "nm", path = ctx.attr.nm_path),
        tool_path(name = "objdump", path = "/bin/false"),
        tool_path(name = "strip", path = "/bin/false"),
    ]

    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        features = features,
        action_configs = [],
        cxx_builtin_include_directories = ctx.attr.builtin_include_directories,
        toolchain_identifier = "wasm32_wasi_toolchain",
        host_system_name = "local",
        target_system_name = "wasm32-wasi",
        target_cpu = "wasm32",
        target_libc = "wasi",
        compiler = "clang",
        abi_version = "wasi",
        abi_libc_version = "wasi",
        tool_paths = tool_paths,
        builtin_sysroot = sysroot_path,
    )

cc_toolchain_config = rule(
    implementation = _impl,
    attrs = {
        "clang_path": attr.string(mandatory = True),
        "ar_path": attr.string(mandatory = True),
        "nm_path": attr.string(mandatory = True),
        "sysroot_path": attr.string(mandatory = True),
        "builtin_include_directories": attr.string_list(default = []),
    },
    provides = [CcToolchainConfigInfo],
)
