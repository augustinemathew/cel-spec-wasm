"""cc_toolchain_config for wasm32-wasi-threads using the wasi-sdk SDK.

Targets `wasm32-wasi-threads` (not vanilla `wasm32-wasi`) because cctz
in absl/time needs `std::mutex`, which wasi-sdk's libc++ only ships
when threading is enabled.  See `doc/implementation-plan/rewrite/
phase-c-probes/E4/RESULT.md` failure-mode #6.

Validated by Phase C probes E3 (canary `hello_wasm`), E4
(`@com_google_absl//absl/strings`, `@com_google_absl//absl/time`), E5
(C + C++ + absl link), E7 (Phase C's 4 absl kernels transitively
need 43 libabsl_*.a), and E8 (RE2 + absl).

The reference implementation is `@rules_cc//cc/private/toolchain:
unix_cc_toolchain_config.bzl` (~2k LoC, ~50 features).  This config
is intentionally minimal — 8 features only — because most of the
reference's surface (PIC variants, sanitizers, layering checks,
profile-guided opts) is irrelevant to a wasm32 cross-compile.

Cross-platform note.  Today the config + BUILD bake in
`wasi_sdk_darwin_arm64`, matching the alias pattern in
`//third_party/wasi_sdk:BUILD.bazel`.  Multi-host CI is a separable
follow-up: the bazel idiom is one `toolchain()` registration per
(host, target) pair with `exec_compatible_with` set; a Starlark
macro can compress the four variants into one call.  See
`phase-c-plan.md` §7.6.
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

# Action names — mirror the strings the cc rules use internally.

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

    # The wasi-sdk libc++ headers live under
    # `<sysroot>/include/wasm32-wasi-threads/c++/v1/`, NOT
    # `<sysroot>/include/c++/v1/`.  bazel's cc rules call clang directly
    # rather than relying on driver-mode header discovery, so without
    # explicit `-isystem` flags clang fails to find `<array>` etc.
    # Listed first so libc++ headers take precedence over the C-only
    # sysroot.
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
                            # wasi-emulation defines (mirrors
                            # exp1_re2's wasi-toolchain.cmake).  cctz
                            # uses `<chrono>` steady_clock — needs
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

    # `-flto` lives in its own feature so consumers can disable it for
    # debug builds via `--features=-lto`.  Enabled by default — Phase C
    # E9 measured this halves the stripped wasm size on the
    # absl + re2 link (1.5 MB → ~800 KB).
    lto_feature = feature(
        name = "lto",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = _ALL_COMPILE_ACTIONS,
                flag_groups = [flag_group(flags = ["-flto"])],
            ),
            flag_set(
                actions = _ALL_LINK_ACTIONS,
                flag_groups = [flag_group(flags = ["-flto"])],
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

    # wasm has no ELF-style PIC.
    supports_pic_feature = feature(name = "supports_pic", enabled = False)

    features = [
        default_compile_flags,
        default_link_flags,
        lto_feature,
        opt_feature,
        dbg_feature,
        user_compile_flags,
        user_link_flags,
        sysroot_feature,
        supports_pic_feature,
    ]

    # tool_paths are package-relative; the wrapper sh scripts exec the
    # real wasi-sdk binaries by absolute path computed at script
    # runtime.
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
        toolchain_identifier = "wasm32_wasi_threads_toolchain",
        host_system_name = "local",
        target_system_name = "wasm32-wasi-threads",
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
