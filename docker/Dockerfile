# Linux build environment for celwasmc — the CEL → WebAssembly AOT compiler.
#
# Captures the exact host toolset the Linux build needs.  The wasm
# cross-compile toolchain (wasi-sdk) and the wasmtime runtime are NOT
# installed here: Bazel fetches them itself from MODULE.bazel, keyed to
# the host arch.  Only the host C++ toolchain + a few CLI tools are
# system dependencies.
#
#   clang + lld   host C++ compiler + linker.  Both are required, not
#                 optional: the runtime C uses clang's __attribute__
#                 ((musttail)) and cel_layout.h relies on clang's C11
#                 _Static_assert handling (gcc rejects both); and GNU
#                 gold crashes linking a binary this large
#                 (set_xindex internal error), so .bazelrc pins
#                 -fuse-ld=lld on Linux.
#   tzdata + tzdata-legacy   IANA timezone database.  Named-timezone
#                 resolution (e.g.
#                 timestamp(...).getDayOfMonth('US/Central')) reads
#                 /usr/share/zoneinfo; without it those evaluate to
#                 errors (and 2 conformance rows fail).  On Ubuntu 24.04
#                 the legacy aliases (US/Central, US/Eastern, ...) the
#                 CEL corpus uses live in the separate tzdata-legacy
#                 package — tzdata alone ships only America/Chicago etc.
#   build-essential, python3, zip/unzip   Bazel + the compile-db
#                 fallback generator (scripts/refresh_compile_db.sh).
#   bazelisk      Pins Bazel to .bazelversion (7.3.2) at run time.
#
# This is a *build-environment* image — it does not bake in the source.
# Mount your checkout and build:
#
#   docker build -t celwasmc-linux -f docker/Dockerfile .
#   docker run --rm -v "$PWD":/src -w /src celwasmc-linux \
#     bash -c 'third_party/fetch_cel_cpp.sh && bazel test //compiler_v2/...'
#
# Builds and runs on both linux/arm64 and linux/amd64.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl git \
      build-essential clang lld \
      python3 \
      tzdata tzdata-legacy \
      zip unzip \
 && rm -rf /var/lib/apt/lists/*

# Bazelisk -> /usr/local/bin/bazel.  Arch-aware so the image works on
# both Apple-Silicon/ARM hosts and x86_64.  Bazelisk then resolves the
# Bazel version from the repo's .bazelversion (7.3.2) at run time.
ARG BAZELISK_VERSION=v1.25.0
RUN arch="$(dpkg --print-architecture)" \
 && case "$arch" in \
      arm64) bz=bazelisk-linux-arm64 ;; \
      amd64) bz=bazelisk-linux-amd64 ;; \
      *) echo "unsupported arch: $arch" >&2; exit 1 ;; \
    esac \
 && curl -fsSL -o /usr/local/bin/bazel \
      "https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/${bz}" \
 && chmod +x /usr/local/bin/bazel

# Make clang the default host compiler for any non-Bazel tooling too.
# (Bazel itself reads CC/CXX via --repo_env in .bazelrc.)
ENV CC=clang CXX=clang++

WORKDIR /src
CMD ["bash"]
