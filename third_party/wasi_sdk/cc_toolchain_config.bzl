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

Cross-platform.  One `toolchain()` is registered per build host
(darwin/linux × arm64/x86_64), each with `exec_compatible_with` pinned
to that host and a `cc_toolchain_config` whose sysroot + builtin
include paths spell out that host's `@wasi_sdk_<host>` external repo.
Bazel toolchain resolution auto-selects the one matching the build
host — no `select()` in the toolchain itself.  The
`wasm_wasi_toolchains` macro at the bottom of this file generates all
four from one call; `//third_party/wasi_sdk:BUILD.bazel` invokes it.
See `phase-c-plan.md` §7.6.
"""

load("@rules_cc//cc:defs.bzl", "cc_toolchain")
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
    target_triple = ctx.attr.target_triple
    threads = ctx.attr.threads

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
    compile_target_flags = ["--target=" + target_triple]
    if threads:
        compile_target_flags.append("-pthread")
    default_compile_flags = feature(
        name = "default_compile_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = _ALL_COMPILE_ACTIONS,
                flag_groups = [
                    flag_group(
                        flags = compile_target_flags + [
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

    link_target_flags = ["--target=" + target_triple]
    if threads:
        link_target_flags += ["-pthread", "-Wl,--shared-memory",
                              "-Wl,--max-memory=67108864"]
    default_link_flags = feature(
        name = "default_link_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = _ALL_LINK_ACTIONS,
                flag_groups = [
                    flag_group(
                        flags = link_target_flags + [
                            "-nostartfiles",
                            "-Wl,--no-entry",
                            "-Wl,--allow-undefined",
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
        toolchain_identifier = ctx.attr.toolchain_identifier,
        host_system_name = "local",
        target_system_name = target_triple,
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
        # New: parameterize the wasi target triple + threading flavor.
        # Defaults preserve the pre-existing wasm32-wasi-threads stack
        # for backward compatibility (cel_runtime, every existing
        # wasm_cc_binary callsite).
        "target_triple": attr.string(default = "wasm32-wasi-threads"),
        "threads": attr.bool(default = True),
        "toolchain_identifier": attr.string(
            default = "wasm32_wasi_threads_toolchain"),
    },
    provides = [CcToolchainConfigInfo],
)

# Build hosts we cross-compile wasm from.  Each entry maps a short host
# id to the matching `@wasi_sdk_<id>` external repo and the
# `@platforms//{os,cpu}` constraints that gate `exec_compatible_with`.
_HOSTS = {
    "darwin_arm64": struct(
        os = "@platforms//os:macos",
        cpu = "@platforms//cpu:arm64",
    ),
    "darwin_x86_64": struct(
        os = "@platforms//os:macos",
        cpu = "@platforms//cpu:x86_64",
    ),
    "linux_arm64": struct(
        os = "@platforms//os:linux",
        cpu = "@platforms//cpu:arm64",
    ),
    "linux_x86_64": struct(
        os = "@platforms//os:linux",
        cpu = "@platforms//cpu:x86_64",
    ),
}

# Canonical external-repo path prefix for a module-extension http_archive
# under bazel 7.x.  The `~` separator is bazel-version-specific (bazel 8
# switched to `+`); if the build's bazel version changes, this is the
# one literal to revisit.  Only the resolved host's archive is fetched,
# so a stale prefix for a non-host arm is harmless until that host builds.
def _external_prefix(host):
    return "external/_main~_repo_rules~wasi_sdk_" + host

# Generates a per-host wasm32-wasi-threads cc_toolchain stack:
# cc_toolchain_config + cc_toolchain + toolchain (with
# `exec_compatible_with` pinned to the host).  Bazel resolves the one
# matching the build host at analysis time.
def wasm_wasi_toolchains(name):
    for host, plat in _HOSTS.items():
        prefix = _external_prefix(host)
        sysroot = prefix + "/share/wasi-sysroot"

        cc_toolchain_config(
            name = "%s_config_%s" % (name, host),
            clang_path = "wasm_clang.sh",
            ar_path = "wasm_ar.sh",
            nm_path = "wasm_nm.sh",
            sysroot_path = sysroot,
            builtin_include_directories = [
                # libc++ headers live under
                # <sysroot>/include/<target>/c++/v1/.  Listed FIRST so
                # they take precedence over the C-only sysroot.
                sysroot + "/include/wasm32-wasi-threads/c++/v1",
                # wasi-libc headers (per-target).
                sysroot + "/include/wasm32-wasi-threads",
                # Generic sysroot includes (target-independent
                # wasi-libc bits).
                sysroot + "/include",
                # Clang's internal builtin headers (stddef.h etc.).
                prefix + "/lib/clang/19/include",
            ],
        )

        # All wasi-sdk tools + sysroot + the wrapper scripts are needed
        # at action time.
        native.filegroup(
            name = "%s_tool_inputs_%s" % (name, host),
            srcs = [
                ":tool_wrappers",
                "@wasi_sdk_%s//:all" % host,
            ],
        )

        cc_toolchain(
            name = "%s_cc_toolchain_%s" % (name, host),
            all_files = ":%s_tool_inputs_%s" % (name, host),
            ar_files = ":%s_tool_inputs_%s" % (name, host),
            compiler_files = ":%s_tool_inputs_%s" % (name, host),
            dwp_files = ":%s_tool_inputs_%s" % (name, host),
            linker_files = ":%s_tool_inputs_%s" % (name, host),
            objcopy_files = ":%s_tool_inputs_%s" % (name, host),
            strip_files = ":%s_tool_inputs_%s" % (name, host),
            toolchain_config = ":%s_config_%s" % (name, host),
            toolchain_identifier = "wasm32_wasi_%s" % host,
        )

        native.toolchain(
            name = "%s_toolchain_%s" % (name, host),
            exec_compatible_with = [plat.os, plat.cpu],
            target_compatible_with = [
                "@platforms//cpu:wasm32",
                "@platforms//os:wasi",
                # Distinguish from the wasip2 toolchain below — same
                # @platforms cpu/os, different threading mode.
                ":wasi_threads_on",
            ],
            toolchain = ":%s_cc_toolchain_%s" % (name, host),
            toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
        )

        # ── wasm32-wasip2 (no threads) variant ────────────────────────
        # Used by the cel_wasm_component macro: targets the wasi-preview2
        # ABI the Component Model speaks natively, with no shared
        # memory (CM components are single-instance per component
        # instantiation).
        cc_toolchain_config(
            name = "%s_config_p2_%s" % (name, host),
            clang_path = "wasm_clang.sh",
            ar_path = "wasm_ar.sh",
            nm_path = "wasm_nm.sh",
            sysroot_path = sysroot,
            target_triple = "wasm32-wasip2",
            threads = False,
            toolchain_identifier = "wasm32_wasip2_toolchain",
            builtin_include_directories = [
                sysroot + "/include/wasm32-wasip2/c++/v1",
                sysroot + "/include/wasm32-wasip2",
                sysroot + "/include",
                prefix + "/lib/clang/19/include",
            ],
        )

        cc_toolchain(
            name = "%s_cc_toolchain_p2_%s" % (name, host),
            all_files = ":%s_tool_inputs_%s" % (name, host),
            ar_files = ":%s_tool_inputs_%s" % (name, host),
            compiler_files = ":%s_tool_inputs_%s" % (name, host),
            dwp_files = ":%s_tool_inputs_%s" % (name, host),
            linker_files = ":%s_tool_inputs_%s" % (name, host),
            objcopy_files = ":%s_tool_inputs_%s" % (name, host),
            strip_files = ":%s_tool_inputs_%s" % (name, host),
            toolchain_config = ":%s_config_p2_%s" % (name, host),
            toolchain_identifier = "wasm32_wasip2_%s" % host,
        )

        native.toolchain(
            name = "%s_toolchain_p2_%s" % (name, host),
            exec_compatible_with = [plat.os, plat.cpu],
            target_compatible_with = [
                "@platforms//cpu:wasm32",
                "@platforms//os:wasi",
                ":wasi_threads_off",
            ],
            toolchain = ":%s_cc_toolchain_p2_%s" % (name, host),
            toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
        )
