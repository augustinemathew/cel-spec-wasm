# Post-m39 toolchain analysis — remaining simplification opportunities

Status: analysis — written 2026-08-05 against master `475cb74` (the PR #40
m39 merge).  Requested by `m39-component-removal.md` §"follow-ups" (:293-295).
ANALYSIS ONLY — no code or build changes accompany this doc.  All
recommendations are options for the owner, not decisions; per the brief,
**"keep wasm32-wasi-threads" is the default** and this doc quantifies the
alternatives.

Toolchain surface analysed: the single wasm32-wasi-threads cc_toolchain
(wasi-sdk 25.0) building `runtime/cel_runtime.wasm`, Binaryen (source-built,
pinned version_129) generating core expr modules, and wasmtime (prebuilt C
API, v43.0.1) evaluating them.

## Summary table

| # | Opportunity | Payoff | Effort | Recommendation | Risk |
|---|---|---|---|---|---|
| 1a | Trim the 14 preview1 imports of `cel_runtime.wasm` | None practical — every import is init/error/contention-path, none fires on the eval hot path; hosts already stub all 14 trivially | Upstream-patch scale (libc++ locale, wasi-libc SSP, absl) per import | **Do nothing.** The import set is the cheap part of the status quo | Removing any is a loud ABI change with no measurable win |
| 1b | Add a real import-golden test (none exists today — the brief's premise was wrong) | Pins the wire ABI; today a *removed* import leaves a silently-dead stub row in `cel_runtime_wasm_test.cc`, a *grown* one fails only by accident of instantiation | ~1 h: one test comparing `wasm-objdump`-style import enumeration against a checked-in list | **Worth doing** (smallest real item in this doc) | None |
| 1c | Fix or delete the stale `wasm_imports.txt` "tight allowlist" | It is toothless (toolchain also passes `-Wl,--allow-undefined`) and stale (13 entries vs 17 cel imports actually linked) | ~1 h either way | **Either enforce it** (drop `--allow-undefined` from the toolchain default, complete the list) **or delete it**; today it documents a false invariant | Low |
| 2 | Drop the `-threads` triple (→ vanilla wasm32-wasi) | Stripped -O3 binary shrinks ~400–600 KB of 971 KB; import set shrinks by ~4; runs on hosts without the threads/shared-memory proposal | **Very large**: requires de-absl-ing the runtime — RE2 out (matches() → host trampoline), absl-time out (hand-rolled RFC3339), cord/absl_check out of string.format; 3+ milestone-sized slices | **Keep threads (default).** Only revisit if a target host runtime materialises that lacks shared-memory support | Byte-exact conformance regressions in time formats; re-introduces per-call host round-trips for matches(); reverses Phase C's "no host work required" payoff |
| 3a | Delete the six legacy tool aliases (`:clang`, `:ar`, …) + their 4 `config_setting`s | −~55 lines in `third_party/wasi_sdk/BUILD.bazel`, −5 dead filegroups per `BUILD.external.bazel` | ~30 min | **Delete** — zero consumers confirmed repo-wide (only doc/ mentions) | None (git has them) |
| 3b | Collapse single-setting attrs in `cc_toolchain_config` (`clang_path`/`ar_path`/`nm_path` are always the same 3 wrapper scripts) | −3 attrs, minor readability | ~30 min | Optional cleanup-when-touched | None |
| 3c | Delete dead root `WORKSPACE` (5.1 KB, ignored under bzlmod with `WORKSPACE.bzlmod` present) | Removes a misleading file declaring rules_go/gazelle deps we don't have | ~10 min | **Delete** | None |
| 3d | wasmtime `darwin_x86_64` gap: wasi-sdk declares 4 hosts, wasmtime only 3, and the select has no default | Today an Intel-Mac checkout cross-compiles wasm fine but fails config-time on any wasmtime target | ~30 min either way | **Owner choice**: add the missing archive arm, or drop darwin_x86_64 from *both* (CI is linux-x86_64 + dev is darwin-arm64) | Low |
| 4a | Binaryen simplification | — | — | **No — already minimal.** Source-built via rules_foreign_cc but `BUILD_TOOLS=OFF BUILD_TESTS=OFF`, single `libbinaryen.a`, C API only | — |
| 4b | wasmtime simplification | Cosmetic: hdrs glob includes 13 dead `component/` headers + `*.hh`; archive ships 21 MB unused (dylib + `min/` tree) | ~30 min to tighten glob; a probe to evaluate `min/lib/libwasmtime.a` (2.0 MB vs 30.5 MB) | **No action needed**; glob-tighten opportunistically; `min/` probe only if link size ever matters | `min/` build may lack API we use — probe before switching |

The three most load-bearing facts, up front:

1. **No import-golden test exists.**  `runtime/cel_runtime_wasm_test.cc` has a
   *stub table*, not an assertion; `wasm_imports.txt` is a linker allowlist
   that doesn't enforce (see §1.3–1.4).
2. **There are THREE mutex paths pinning the threads triple, not two.**
   Besides the owner-supplied cctz/absl-time and RE2 paths, `:cel_string_format`
   reaches `absl/synchronization` via `absl/strings:cord` (cordz sampling), and
   `absl/log:absl_check` reaches it via `log_sink_set` (§2.1).  Dropping the
   triple is a de-absl project, not a two-library surgery.
3. **The whole absl-time dependency is 9 symbols across 2 files** (§2.2) — the
   hand-roll option is small in call-site count but conformance-risky in
   byte-exactness.

---

## 1. WASI import surface

### 1.1 The import set

Command (fastbuild and `-c opt` produce the identical set — verified both):

```
bazel build //runtime:cel_runtime_wasm
wasm-objdump -x bazel-out/darwin_arm64-fastbuild/bin/runtime/cel_runtime_wasm.wasm
bazel build -c opt //runtime:cel_runtime_wasm     # 30 s on a warm cache
wasm-objdump -x bazel-out/darwin_arm64-opt/bin/runtime/cel_runtime_wasm.wasm
```

31 imports total: 1 `cel_env.cel_log`, 16 `cel_host.*`, and **14
`wasi_snapshot_preview1.*`**: `environ_get`, `environ_sizes_get`,
`clock_time_get`, `fd_close`, `fd_fdstat_get`, `fd_prestat_get`,
`fd_prestat_dir_name`, `fd_read`, `fd_seek`, `fd_write`, `poll_oneoff`,
`proc_exit`, `sched_yield`, `random_get`.

### 1.2 Per-import provenance and hot-path status

Method: full disassembly (`wasm-objdump -d <wasm> > /tmp/celrt.dis`), then an
awk pass mapping every `call` of each import to its enclosing function, then
second/third-level caller tracing of those functions.  First-party sources
were also grepped: runtime C/C++ code calls **no** libc I/O directly except
one `std::snprintf` (`runtime/cel_time_parse.cc:132`); `malloc`/`free` appear
only in `runtime/cel_arena.c` (:65-137).  Every WASI import is transitive
from wasi-libc, libc++, absl, or RE2.

| Import | Dragged in by (observed call chain) | Eval hot path? | What removal would take |
|---|---|---|---|
| `environ_get`, `environ_sizes_get` | `getenv` ← `__get_locale` ← `__newlocale` ← libc++ locale facets (`num_get`/`num_put`/`ctype`), lazily built on first locale touch (number formatting inside libc++/absl) | No — init-once | Patch libc++'s locale to a C-locale-only build; upstream-scale, not worth it |
| `clock_time_get` | `absl::Now()` / `GetCurrentTimeNanosFromSystem`, `CycleClock::Now` via `steady_clock::now`, `Mutex::LockSlow`, `KernelTimeout`, `__pthread_cond_timedwait` | No — absl init + mutex slow paths | Leaves only with absl removal (§2) |
| `fd_write`, `fd_seek`, `fd_close`, `fd_fdstat_get`, `fd_read` | stdio machinery reached via the FILE vtable; kept alive by stderr-on-fatal paths: `abort_message`, `absl::raw_log`, `LogMessage::Fail*`, `printf_core`, libc++ `__stdoutbuf` | No — error paths only | Requires an abort-/stderr-free link, i.e. no absl logging and no libc++ verbose-abort; absl-removal-scale |
| `fd_prestat_get`, `fd_prestat_dir_name` | wasi-libc preopen scan on the stdio close path | No — init/teardown | Same as above |
| `poll_oneoff` | `nanosleep` ← `absl SleepOnce` / `AbslInternalSpinLockDelay` | No — lock-contention backoff, unreachable single-threaded | absl removal |
| `proc_exit` | `_Exit` ← `abort` ← `LogMessage::FailWithoutStackTrace` / `RawLogVA` / `abort_message` | No — fatal only | abort-free link |
| `sched_yield` | **dlmalloc/dlfree/realloc lock** (6 call sites inside `dlmalloc` — wasi-threads libc builds malloc with locking), `AbslInternalSpinLockDelay`, `absl::Mutex::{LockSlow,UnlockSlow,Block}`, libc++ `thread_yield` | **Reachable from the hot path** (every allocation takes the malloc lock) but the import only *fires* under contention, which never happens in our single-threaded instances | Drops automatically with a non-threads triple (§2) |
| `random_get` | `__wasilibc_init_ssp` ← `__wasm_call_ctors` (stack-smashing-protector seed) | No — one call at instantiation | `-fno-stack-protector` might drop it (unprobed); one-line experiment if ever wanted |

**Conclusion — minimal viable import set is the current set**, as long as
absl + RE2 + libc++ are linked: all 14 are referenced by live (error/init)
code that `wasm-ld --gc-sections` correctly keeps.  A pure-C runtime had 0
WASI imports (`doc/implementation-plan/rewrite/phase-c-plan.md:267` — "the
imports list grows from 0 … to 13" when absl/RE2 landed).  The hosts' cost
today is 14 two-line stubs (`runtime/cel_runtime_wasm_test.cc:290-326`), or
wasmtime's built-in WASI context in production — i.e. the import surface
costs approximately nothing to keep and upstream-patch effort per line to
shrink.

### 1.3 The import "golden" does not exist (negative finding)

The brief assumed the import set is pinned by an import-golden in
`runtime/cel_runtime_wasm_test.cc`.  **It is not.**  That file's 9 tests are
all arena-behavior tests; the WASI names appear only in the `kStubs` array
(`:290-326`) that makes instantiation possible.  Its comment (`:297-299`)
asks the maintainer to keep the list in sync by hand.  Failure asymmetry: a
*new* import fails instantiation loudly; a *removed* import leaves a dead
stub row silently.  The import-shape goldens that do exist all pin the
**expr-module** side (`eval/internal/module_imports_test.cc`,
`abi/cel_abi_emit_test.cc:247-249`, `compiler/internal/compile_test.cc:705`),
not the runtime side.

*Option 1b:* a ~1 h test that enumerates the runtime wasm's import section
(the bytes are already embedded via `//runtime:cel_runtime_wasm_bytes`) and
compares module/name pairs against a checked-in golden.  This is the one
cheap, real hardening item this analysis found.

### 1.4 `wasm_imports.txt` is stale and toothless

`runtime/BUILD.bazel:770` passes
`-Wl,--allow-undefined-file=$(location wasm_imports.txt)` as a "tight import
allowlist" — but the toolchain's default link flags *also* pass the blanket
`-Wl,--allow-undefined` (`third_party/wasi_sdk/cc_toolchain_config.bzl:166`),
so the file enforces nothing.  Empirical proof: the linked wasm imports 17
cel_env/cel_host symbols; the file lists 13 (`cel_set_field`,
`cel_message_is_zero`, `cel_list_iter_open`, `cel_map_iter_open` are absent)
and the link succeeds anyway.  The BUILD comment (`:766-769`) already admits
it is aspirational.  *Option 1c:* make it real (remove `--allow-undefined`
from the toolchain default, complete the list — this also turns the linker
into the import golden, partially subsuming 1b) or delete the file.

---

## 2. The threads triple: KEEP vs DROP

Owner-supplied ground truth (treated as such): `wasm32-wasi-threads` is
load-bearing via absl mutex on two paths — cctz/absl-time
(`third_party/wasi_sdk/cc_toolchain_config.bzl:3-6` header comment, probe E4
failure-mode #6: wasi-sdk libc++ ships `<mutex>` only when threading is
enabled) and RE2 self-hosted for `matches()`
(`runtime/BUILD.bazel:478-482`, probe E8).

### 2.1 (a) Which targets reach a mutex in the wasm build

Commands:

```
bazel query 'kind("cc_library", deps(//runtime:cel_runtime_wasm.bin))' | grep -i absl   # 126 targets
bazel query 'rdeps(deps(//runtime:cel_runtime_wasm.bin), @com_google_absl//absl/synchronization:synchronization, 1)'
bazel query 'somepath(//runtime:cel_matches, @com_google_absl//absl/synchronization:synchronization)'
grep -ln "std::mutex" <output_base>/external/abseil-cpp~/absl/time/internal/cctz/src/*.cc
```

Direct dependents of `absl/synchronization` (absl::Mutex) in the wasm deps:

- `@re2//:re2` — the owner's path #2.
- `absl/strings:cordz_handle` + `cordz_info` — pulled by `absl/strings:cord`,
  which `:cel_string_format` deps directly (`runtime/BUILD.bazel:573` via
  str_format's cord support).  **A third path the two-path framing misses.**
- `absl/log/internal:log_sink_set` + `vlog_config` — pulled by
  `absl/log:absl_check` (`:cel_string_format`, `runtime/BUILD.bazel:569`).
- `absl/container:hashtablez_sampler` + `profiling:sample_recorder` — pulled
  by `flat_hash_map` (transitively via RE2).

And the `std::mutex` path (the E4 one): exactly one cctz file,
`absl/time/internal/cctz/src/time_zone_impl.cc` (zone-cache mutex), reached
from `absl/time` by `:cel_time_parse` (`runtime/BUILD.bazel:474`) and
`:cel_string_format` (`:574`).

The `-c opt` disassembly confirms all of this is *linked live*:
`absl::Mutex::{LockSlow,UnlockSlow,Block,Enqueue}`, `GetMutexGlobals`,
`AbslInternalSpinLockDelay` all appear with `sched_yield`/`clock_time_get`
call sites (§1.2 table).

**Implication:** dropping `-threads` is not "remove two libraries"; any
remaining `absl_check`, `cord`, or `flat_hash_map` usage in wasm-side C++
re-pins absl::Mutex.  The realistic shape of DROP is *no absl in the wasm
runtime at all* (absl/strings-only targets would need auditing down to
raw_logging, which is mutex-free, but str_format/log/cord are not).

### 2.2 (b) Could absl-time be hand-rolled?

The entire runtime-side absl-time surface is **9 distinct symbols across 2
files** (grep of `runtime/*.cc`):

- `runtime/cel_time_parse.cc` — `absl::ParseTime(RFC3339_full)` (:166),
  `absl::ParseDuration` (:200), `absl::FormatTime(RFC3339_full, …,
  UTCTimeZone())` (:226), `IDivDuration` (:56, :58), `UnixEpoch` /
  `Seconds` / `Nanoseconds` (:223-225).
- `runtime/cel_string_format_render.cc` — `UnixEpoch + Seconds + Nanoseconds`
  (:101-102), `FormatTime(RFC3339_full)` (:103).

Notably `absl::FormatDuration` is *already* hand-rolled
(`FormatProtoDuration`, `cel_time_parse.cc:120-135`, via `std::snprintf`)
because proto duration format differs from absl's — precedent that the
hand-roll is viable.  No `LoadTimeZone`, no named-timezone parsing, no direct
cctz calls: only fixed-offset RFC3339 + UTC.  A hand-rolled
RFC3339 parse/format (fixed offsets only) is a bounded, oracle-verifiable
slice — the risk is byte-exactness against cel-cpp (canonical string forms
are conformance-scored; every edge — leap seconds excluded, `Z` vs `+00:00`,
sub-second precision trimming — must match).  Note the runtime never calls
`absl::Now()` on the CEL semantic path; the wall-clock import is absl
internals only.

### 2.3 (c) Could matches()/RE2 move behind a cel_host trampoline?

What the docs record (search: `doc/implementation-plan/rewrite/`):

- The *original* design kept the trampoline (`wasi/DESIGN.md:493` — "CelHost
  trampoline pattern stays"); Phase C reversed it
  (`phase-c-design.md:22-42`): self-hosting means "every wasi-libc-compatible
  host (Chrome, wasmtime, node, embedders in any language) inherits
  matches / parse / format from the runtime — **no host work required**."
- **No self-hosted-vs-trampoline A/B bench was ever run** — the trampoline
  variant was never built.  What exists: `m28-bench-results.md:45-64`
  (`str_matchesComplex` 186 ns vs cel-cpp 8 939 ns — explicitly caveated as
  cache-vs-no-cache), and the per-Eval floor of **62 ns per wasm boundary
  crossing** (`m28-bench-results.md:86`).  A trampoline `matches()` pays that
  boundary cost per *call* (inside comprehension loops, per element), vs zero
  today; against a 101–186 ns self-hosted kernel that plausibly doubles-or-
  worse hot-loop matches() cost, but no measured number exists — treat any
  estimate as a probe-pending guess.
- The accepted size cost of self-hosting: `phase-c-design.md:120-124` (~388 KB
  compressed absl+RE2, a ~3× blow of the 241 KB budget, accepted as "the
  architectural payoff").
- The rejected middle path is on the record: `phase-c-plan.md:264-273` —
  *patch cctz to no-op TimeZoneMutex on wasm, keeping vanilla wasm32-wasi* —
  deferred with a named revisit trigger (shared-memory restrictions on a
  future Chrome target).  That option only cures the std::mutex path, not
  absl::Mutex (§2.1), so post-Phase-C it is no longer sufficient on its own.

Mechanically, a trampoline is well-trodden: add `cel_host.cel_matches` to the
import ABI (WAT-first per repo rules), implement the eval-side RE2 call in
`eval/internal/cel_host*`, move the per-Instance pattern cache host-side, and
delete `:cel_matches` + `@re2` from the wasm link.  It reverses the Phase C
portability claim: every non-wasmtime embedder must then provide a regex
implementation with RE2 syntax/semantics (JS `RegExp` is *not*
semantics-identical — that's a conformance cliff for a Chrome host).

### 2.4 (d) Size and import-surface delta

Measured (commands):

```
bazel build -c opt //runtime:cel_runtime_wasm
cp <opt wasm> /tmp/rt_opt.wasm && wasm-strip /tmp/rt_opt.wasm && gzip -k9 …
wasm-objdump -d <opt wasm>  # + awk bucketing of code bytes by C++ namespace
```

- Stripped `-c opt` runtime today: **971 235 B** (gzip -9: 313 616 B) —
  matching probe E9's ~800 KB / ~300 KB projections; pre-absl/RE2 baseline
  was 241 386 B stripped (`wasi/POST_MIGRATION_BENCH.md:17`).
- Code-section attribution (opt, 760 KB total): re2 148 KB, absl-non-cctz
  138 KB, cctz 18 KB, libc++ 166 KB, libc/other 173 KB, first-party cel
  117 KB.
- **DROP-both delta:** re2 + cctz = 166 KB direct, plus the large share of
  absl-other and libc++ they anchor — realistic stripped-size win **~400–600
  KB of 971 KB**.  (The floor is not 241 KB: string_ext / string.format /
  base64 / math / net kernels landed after that measurement.)
- Import delta: `clock_time_get`, `poll_oneoff` disappear with absl;
  `sched_yield` disappears with the triple (non-threads dlmalloc has no
  lock); stdio/environ/proc_exit/random_get persist while any libc++ /
  std::string code remains (all the C++ extension kernels).  Realistic
  post-DROP set: **~10 of 14**.  Only a return to pure C reaches 0.

### 2.5 Cost table

| | KEEP `wasm32-wasi-threads` (default) | DROP → vanilla `wasm32-wasi` |
|---|---|---|
| Effort | 0 | 3+ milestone-sized slices: (1) matches() → host trampoline (ABI + WAT + eval trampoline + cache relocation + conformance re-verify of 9 rows); (2) hand-rolled RFC3339 parse/format, oracle-verified byte-exact; (3) de-absl `cel_string_format` (drop cord, absl_check, str_format's time path) + audit remaining C++ kernels down to mutex-free absl subsets; then flip triple, drop `-pthread`/`--shared-memory`/`--max-memory` and the 4 emulated libs |
| Binary size (stripped, opt) | 971 KB | ~400–570 KB (est.) |
| preview1 imports | 14 | ~10 |
| Eval perf | matches() in-module, 101–186 ns/call | + ≥62 ns boundary crossing per matches() call (unmeasured; no A/B exists) |
| Host requirements | wasm runtime must support threads/shared-memory proposal (wasmtime does; some embedded runtimes don't) | any preview1 runtime; but every embedder must supply an RE2-semantics regex host fn |
| What breaks if half-done | — | loudly: `<mutex>` missing at compile, `libpthread` symbols at link — no silent failure mode |
| Feature loss | none | none *if* all three slices land byte-exact; conformance risk concentrated in RFC3339 canonical forms and RE2-vs-host regex semantics |

**Bottom line:** the only real payoff of DROP is portability to
non-threads-capable hosts plus ~0.5 MB of binary.  Neither is a live
requirement today.  KEEP stands.  If a middle path is ever wanted, the
separable half is absl-time (small, 9 symbols — §2.2) — but as §2.1 shows,
that alone does not unlock the triple; RE2/cord/log still pin absl::Mutex.

---

## 3. Residual toolchain config

Post-A1 audit of `third_party/wasi_sdk/cc_toolchain_config.bzl` (419 lines)
and `BUILD.bazel` (137 lines):

- **Legacy tool aliases (`:clang`, `:clang++`, `:ar`, `:ranlib`, `:sysroot`,
  `:all`) — zero consumers.**  Verified:
  `grep -rnE "third_party/wasi_sdk:(clang\+\+|clang|ar|ranlib|sysroot|all)\b"`
  over the first-party tree hits only `doc/` milestone history
  (`wasi/milestones/M1.md`, `M2.md`, `phase-c-research.md`,
  `wasi/README.md:157`).  The alias block (`BUILD.bazel:84-102`) exists only
  to serve a "future genrule" (`BUILD.bazel:8-13`); it drags four
  `config_setting`s (`:106-135`) used by nothing else, and five of the six
  per-tool filegroups in `BUILD.external.bazel` (`:clang`, `:clang++`,
  `:ar`, `:ranlib`, `:sysroot`) are consumed by nothing else (`:all` stays —
  it feeds the toolchain's `tool_inputs` filegroups).
  *Option:* **delete** the aliases + their config_settings (~55 lines) and
  the five dead filegroups.  Pre-1.0 posture says delete-not-deprecate; git
  has them if a genrule ever wants them back.
- **Single-setting knobs A1 missed:** `clang_path`, `ar_path`, `nm_path`
  attrs of `cc_toolchain_config` (`cc_toolchain_config.bzl:315-317`) are
  passed identically for all four hosts (`:365-367`, always the three
  wrapper scripts) — could be hardcoded in `_impl`, and `sysroot_path` +
  `builtin_include_directories` are both pure functions of the host id
  (`:361-381`), so the rule could take a single `host` attr.  Cosmetic;
  cleanup-when-touched.
- **Not dead, checked:** the `dbg`/`opt` features are the standard
  compilation-mode hooks; `supports_pic` disabled is required; the
  `_external_prefix` `~` separator is bazel-7-pinned and already documented
  (`:346-351`); all four `_HOSTS` arms are plausibly live (CI = linux-x86_64,
  dev = darwin-arm64; linux-arm64 named in the repo's cross-platform claim) —
  though see the darwin_x86_64 asymmetry below.
- **wasmtime darwin_x86_64 gap (found by the deps audit, §4):**
  `MODULE.bazel:88-89` and `third_party/wasmtime/BUILD.bazel:3-4` claim four
  per-host archives; only three exist, and the dispatch `select()`
  (`BUILD.bazel:22-26`) has no darwin_x86_64 arm and no default.  wasi_sdk
  covers the host, wasmtime doesn't — an Intel Mac can build the wasm but not
  run any eval test.  *Option:* add the archive arm, or drop darwin_x86_64
  from both wasi_sdk `_HOSTS` and the claim comments (one host id, ~6 sites).
- **Dead root `WORKSPACE` (5.1 KB):** declares rules_go 0.35.0 / gazelle /
  old rules_proto; ignored entirely under bzlmod because `WORKSPACE.bzlmod`
  exists.  *Option:* delete.
- **m39 removal is clean:** `grep -rniE "wasip2|preview2|wit.bindgen"` over
  `MODULE.bazel`, `.bazelrc`, `third_party/`, `scripts/`, `bazel/` finds
  nothing live (one prose hit on a doc filename).  The
  `wit_bindgen_darwin_arm64` directory still sitting in the bazel output
  base (14 MB) is fetch-cache garbage, gone on `bazel clean --expunge` — no
  repo action.

## 4. Binaryen + wasmtime

**Binaryen — no simplification available; already minimal.**  It is a
source build (pinned `version_129`, `MODULE.bazel:76-84`, sole pin) driven by
rules_foreign_cc's `cmake()` (`third_party/binaryen/BUILD.external.bazel:20-35`)
with `BUILD_TOOLS=OFF BUILD_TESTS=OFF BUILD_STATIC_LIB=ON` — so wasm-opt /
wasm-as / wasm-dis are never compiled and the only artifact is
`libbinaryen.a` + `binaryen-c.h`.  All 12 first-party include sites use the C
API only; optimization runs in-process (`compiler/codegen/module.cc:263-265`).
The 8 consuming targets are exactly the codegen pipeline plus the
static-link-mode `strip_command_wrappers` tool.  rules_foreign_cc exists in
MODULE.bazel solely for this one rule — that is the irreducible cost of
building from source, and a prebuilt libbinaryen is not distributed upstream,
so there is nothing to switch to.  The source tarball's `test/`/`fuzz/` trees
are staged but not compiled (copy cost only).  One stale comment found:
`tools/wat_runner/wat_runner.h:8` says "validates via `wasm-as`" but the code
uses `wasmtime_wat2wasm` (`wat_runner.cc:458-467`) — fix opportunistically.

**wasmtime — no structural simplification; two cosmetic options and one bug.**
Prebuilt official C-API archives, v43.0.1, statically linked
(`lib/libwasmtime.a` is the only referenced artifact;
`third_party/wasmtime/BUILD.macos.bazel:9-27`).  The repo's `--link_mode`
flag is *not* a wasmtime knob — it selects how the CEL runtime wasm links
into the emitted expr module (`abi/cel_abi.proto:362`,
`bazel/link_mode_test.bzl`).  Unused extracted weight per archive (~21 MB of
50 MB): `libwasmtime.dylib` (17.5 MB) and the `min/` tree (3.8 MB) — never
referenced, disk-only cost.  The `hdrs` glob sweeps in 13
`include/wasmtime/component/*` headers and all C++ `*.hh` headers with zero
first-party includes post-m39 — tightening the glob is a ~30 min cosmetic
change.  A `min/lib/libwasmtime.a` (2.0 MB vs 30.5 MB) exists in the archive;
whether the min build covers our API surface (`wasmtime.h`, `wasm.h`,
`wasi.h`/`wasi_config_new`) is a five-minute link probe if host-binary size
ever matters — unprobed, so no recommendation.  The real defect found is the
missing darwin_x86_64 arm (§3).  Also stale: `MODULE.bazel:86-87` says
wasmtime is "used only by the e2e eval test" — it has 21 first-party
consumers; fix the comment when next touching the file.

---

## Appendix: command log

```
git log --oneline -1                                   # 475cb74 (PR #40 merge)
bazel build //runtime:cel_runtime_wasm //runtime:cel_runtime_stripped_wasm_bin
bazel build -c opt //runtime:cel_runtime_wasm          # 29.9 s, warm cache
bazel cquery [-c opt] --output=files //runtime:cel_runtime_wasm
wasm-objdump -x <wasm>                                 # import/export sections
wasm-objdump -d <wasm> > /tmp/celrt[_opt].dis          # + awk caller-mapping passes
wasm-strip /tmp/rt_opt.wasm && gzip -k9 /tmp/rt_opt.wasm
bazel query 'kind("cc_library", deps(//runtime:cel_runtime_wasm.bin))'
bazel query 'rdeps(deps(//runtime:cel_runtime_wasm.bin), @com_google_absl//absl/synchronization:synchronization, 1)'
bazel query 'rdeps(deps(//runtime:cel_runtime_wasm.bin), @com_google_absl//absl/time/internal/cctz:time_zone, 1)'
bazel query 'somepath(//runtime:{cel_matches,cel_time_parse}, @com_google_absl//absl/synchronization:synchronization)'
bazel query 'filter(absl, deps(//runtime:cel_{base64_ext,string_ext,string_format,time_parse}))'
bazel query --keep_going 'rdeps(//..., @binaryen//:binaryen, 1)'      # --keep_going required: @fuzztest~//centipede breaks bare //... queries
bazel query --keep_going 'rdeps(//..., //third_party/wasmtime:wasmtime, 1)'
grep -rnE "third_party/wasi_sdk:(clang\+\+|clang|ar|ranlib|sysroot|all)\b" .   # doc/ hits only
grep -rniE "wasip2|preview2|wit.bindgen" MODULE.bazel .bazelrc third_party/ scripts/ bazel/
grep -ln "std::mutex" <output_base>/external/abseil-cpp~/absl/time/internal/cctz/src/*.cc
grep -n "absl::(Parse|Format|IDiv|Unix|Seconds|Nano)" runtime/*.cc     # 9 symbols, 2 files
```
