# Probe E10: kernel-level integration sketch

**Status:** PASS — sketch only (no code committed under `compiler/`).

## What changes in `runtime/`

Add a new C++ TU `cel_time_parse.cc` (sibling to `cel_time.c`):

```cpp
// runtime/cel_time_parse.cc — runtime kernels for
// `timestamp(string) -> timestamp` etc.  Compiled with wasi-sdk
// clang++ via the wasm32-wasi-threads cc_toolchain probed in E3.

#include <cstdint>
#include "absl/strings/string_view.h"
#include "absl/time/time.h"

extern "C" {

// Existing C API the runtime exposes.
#include "runtime/cel_data.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_layout.h"

// `string_to_timestamp` overload id routes here (M7B.D shape).
// out_slot: CelValue * to populate (CEL_TIMESTAMP).
// str_slot: CelValue * holding the input string.
__attribute__((export_name("cel_timestamp_parse_at_v")))
void cel_timestamp_parse_at_v(CelValue* out, const CelValue* str) {
  if (str->kind != CEL_STRING) {
    *out = CelMakeError(kCelErrInvalidArgument,
                       "string expected, got non-string");
    return;
  }
  absl::Time t;
  std::string err;
  if (!absl::ParseTime(absl::RFC3339_full,
                       absl::string_view(str->payload.s.ptr,
                                         str->payload.s.len),
                       &t, &err)) {
    *out = CelMakeError(kCelErrParseTimestamp,
                       "invalid RFC3339 timestamp");
    return;
  }
  const int64_t ns = absl::ToUnixNanos(t);
  *out = CelMakeTimestamp(ns / 1'000'000'000, ns % 1'000'000'000);
}

// Same shape for duration parse + format paths.
__attribute__((export_name("cel_duration_parse_at_v")))
void cel_duration_parse_at_v(CelValue* out, const CelValue* str);

__attribute__((export_name("cel_timestamp_format_at_v")))
void cel_timestamp_format_at_v(CelValue* out, const CelValue* ts);

__attribute__((export_name("cel_duration_format_at_v")))
void cel_duration_format_at_v(CelValue* out, const CelValue* dur);

// `matches(string, string) -> bool` — RE2 backed.  Two arg variant.
// Per-instance regex compile cache to be added in the C2.D slice;
// this skeleton compiles every call.
__attribute__((export_name("cel_matches_at_vv")))
void cel_matches_at_vv(CelValue* out,
                       const CelValue* text,
                       const CelValue* pat) {
  if (text->kind != CEL_STRING || pat->kind != CEL_STRING) {
    *out = CelMakeError(kCelErrInvalidArgument, "string expected");
    return;
  }
  RE2 re(absl::string_view(pat->payload.s.ptr, pat->payload.s.len));
  if (!re.ok()) {
    *out = CelMakeError(kCelErrInvalidRegex, "regex compile failed");
    return;
  }
  bool m = RE2::PartialMatch(
      absl::string_view(text->payload.s.ptr, text->payload.s.len), re);
  *out = CelMakeBool(m);
}

}  // extern "C"
```

Add to `runtime/BUILD.bazel`:

```python
# C++ kernels that need abseil-cpp / RE2.  Linked into
# cel_runtime.wasm via the wasm32-wasi-threads cc_toolchain
# registered under //third_party/wasi_sdk/.
cc_library(
    name = "cel_time_parse",
    srcs = ["cel_time_parse.cc"],
    hdrs = [
        "cel_data.h",
        "cel_arena.h",
        "cel_layout.h",
    ],
    deps = [
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@re2",
    ],
)
```

And update `cel_runtime_wasm_file` to be a real `cc_binary` (not a
genrule), depending on `cel_time_parse` + the existing C srcs.

## What changes in `compiler/codegen/overload_table.cc`

Reroute four `kCelHost` seeds to `kCelRuntime`, and graduate
two ids out of `kExplicitlyUnimplementedIds`:

```diff
- Seed{"string_to_timestamp",
-      {ImportModule::kCelHost, "cel_timestamp_parse"}},
- Seed{"string_to_duration",
-      {ImportModule::kCelHost, "cel_duration_parse"}},
- Seed{"timestamp_to_string",
-      {ImportModule::kCelHost, "cel_timestamp_format"}},
- Seed{"duration_to_string",
-      {ImportModule::kCelHost, "cel_duration_format"}},
+ Seed{"string_to_timestamp",
+      {ImportModule::kCelRuntime, "cel_timestamp_parse_at_v"}},
+ Seed{"string_to_duration",
+      {ImportModule::kCelRuntime, "cel_duration_parse_at_v"}},
+ Seed{"timestamp_to_string",
+      {ImportModule::kCelRuntime, "cel_timestamp_format_at_v"}},
+ Seed{"duration_to_string",
+      {ImportModule::kCelRuntime, "cel_duration_format_at_v"}},
+ Seed{"matches",
+      {ImportModule::kCelRuntime, "cel_matches_at_vv"}},
+ Seed{"matches_string",
+      {ImportModule::kCelRuntime, "cel_matches_at_vv"}},
```

And in the kExplicitlyUnimplementedIds array, **delete**:

```diff
- "matches",
- "matches_string",
```

This drops the array size from 10 → 8.

## What gets deleted in `eval/internal/cel_host.cc`

Four `absl::Status Impl(...)` functions (lines 3031-3199 in current
master): `CelTimestampParseImpl`, `CelDurationParseImpl`,
`CelTimestampFormatImpl`, `CelDurationFormatImpl`, plus their
registration calls in `RegisterHost(...)`.  Approximately **180 LoC
of host-side wasmtime trampolines** removed.

Hosts (wasmtime, Chrome, embedders) no longer need to implement
timestamp/duration parse + format — they inherit it from
`cel_runtime.wasm`.

## What `strings.format` needs

The brief lists `strings.format` separately because:

  - The CEL spec defines it as taking a **runtime** format string
    (the first arg).  absl::StrFormat requires a `constexpr` format
    string, so we can't naively call it.  E7's probe sidestepped
    this with a `"%d"` literal.
  - Two viable approaches:
    - `absl::FormatUntyped(string_view fmt, span<const FormatArg> args)`
      — accepts a runtime format string + a span of pre-bound
      arg types.  This is the production-grade path.
    - Hand-rolled printf-equivalent — simpler if we don't need full
      `%g` floats, but cel's `format` spec supports `%d / %s / %f
      / %e / %x / %o / %t / %b`.  All of these have absl::FormatArg
      conversions; FormatUntyped's the cleaner choice.

Implementation sketch:

```cpp
__attribute__((export_name("cel_strings_format_at_vv")))
void cel_strings_format_at_vv(CelValue* out,
                               const CelValue* fmt,
                               const CelValue* args_list) {
  // Validate fmt is string, args_list is list.
  // Walk args_list; for each element, build an absl::FormatArg
  // matching its CelKind (int → %d, double → %f, string → %s, …).
  // absl::FormatUntyped writes into a string sink we then arena_alloc
  // into out->payload.s.
  // Validate format directives match args; populate the result
  // CelValue or an error if validation fails.
}
```

This is the C2.G slice in `phase-c-design.md`; the implementation
is non-trivial (~150-200 LoC).

## Runtime arena interaction

Each kernel that produces a string (`format_timestamp`,
`format_duration`, `format`) calls `arena_alloc` for the output
bytes — same shape as the existing `cel_string_concat_at_vv`.  No
new memory model.

The match kernel doesn't allocate beyond what RE2 internally needs
(which uses malloc — wasi-libc's dlmalloc, the runtime's existing
allocator after the WASI migration).  RE2's compiled-regex object
is stack-local; it's destroyed when the function returns.  A future
optimization (C2.D) adds a per-Instance regex compile cache.

## Build wiring delta

Today the runtime is built via a `genrule` invoking `@wasi_sdk//:clang`
directly.  Post-Phase-C, it becomes a `cc_binary` under the bazel
wasm32-wasi-threads cc_toolchain (the E3-probed config moved to
`//third_party/wasi_sdk/`).  All the existing `-Wl,--export=cel_*`
flags become `linkopts`; the C++ kernels link automatically via
their `cc_library` dependency.

## Pre-flight checks before implementation

1. **Reactor-mode init.**  E8 surfaced that bazel-built wasm
   doesn't call `_initialize` automatically.  The runtime's host
   loader needs to call `_initialize` (or `__wasm_call_ctors`)
   right after instantiate.  This is a 5-LoC change in
   `eval/internal/host_loader.cc`.
2. **Cross-platform CI.**  The toolchain config currently hardcodes
   `@wasi_sdk_darwin_arm64`.  Add platform selects so it dispatches
   on `@platforms//os` × `@platforms//cpu`.  Mirror the existing
   pattern in `//third_party/wasi_sdk:BUILD.bazel`.
3. **Lint backlog.**  `cc_toolchain_config.bzl` (~280 LoC) and the
   tool-wrapper sh scripts are not currently in `scripts/lint.sh`'s
   coverage.  Add them to the lint pre-flight; `bzl-style` /
   buildifier should run over the `.bzl` file.

## What this doesn't address

  - **Conformance gates.**  The plan's C1.F / C2.F / C2.H rows need
    to run after each kernel lands; this sketch doesn't enumerate
    the per-test expectations (those live in the plan doc).
  - **timestamp/duration parse strictness.**  cel-cpp's
    CelTimestampParseImpl post-validates against
    `RejectsAsTimestampPerCEL` for boundary cases (year 0,
    negative years, etc.).  The runtime kernels must replicate
    those validation rules — flagged in the plan as a spec-
    compliance risk.
  - **Regex security.**  RE2 is safe-by-default (linear-time);
    however per-Instance compile caching (C2.D) needs an LRU
    bound to prevent unbounded host memory growth from
    adversarial inputs.

## Next-step implication

The implementation order is:

  1. Move E3's `cc_toolchain_config.bzl` + wrappers into
     `//third_party/wasi_sdk/` (1d).
  2. Switch `cel_runtime.wasm` to `cc_binary` (0.5d).
  3. Land Slice 1 (C1.A-F) per phase-c-design.md (3d).
  4. Land Slice 2 (C2.A-H) per phase-c-design.md, with the
     `single_version_override` patch applied (5.5d).

Total: ~10 days for the implementation phase, mostly Slice 2.
