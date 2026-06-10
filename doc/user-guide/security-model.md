# Security model

What the sandbox actually guarantees, who you have to trust, and the
known holes — stated precisely, so you can make a real deployment
decision. Updated 2026-06-09; every claim below is checked against the
code cited next to it.

## 1. What the sandbox gives you

A compiled expression executes as Cranelift-JIT'd native code inside a
wasmtime store. Concretely:

- **Bounded linear memory.** The expression and the runtime kernel
  share one wasm linear memory, sized by
  `CompilerOptions::mem_size_bytes` (default 128 KiB, two wasm pages —
  `compiler/compiler.h`). All wasm loads/stores are bounds-checked by
  wasmtime; the expression cannot read or write embedder memory.
- **No I/O, no syscalls.** The runtime kernel links wasi-libc, so its
  module *imports* a handful of WASI preview1 functions — but the
  engine wires them to a deliberately empty WASI context: no
  filesystem preopens, no inherited environment, no inherited stdio
  (`eval/engine.cc::InitStore`, `RegisterWasiStubs`). There is no path
  from CEL evaluation to the filesystem, network, clock-as-capability,
  or process control.
- **No recursion, no unbounded control flow in emitted code.** CEL is
  a total language; comprehensions lower to counted loops, and
  recursive `@native` function bodies are rejected at compile time.
- **`@component` custom functions are isolated harder still.** Each
  component instantiates with its **own linear memory** — it cannot
  read the expression's memory, let alone the embedder's. Every import
  it declares is wired as a trap stub
  (`wasmtime_component_linker_define_unknown_imports_as_traps`), with
  exactly one exception: `wasi:random/random@0.2.0#get-random-bytes`
  gets a deterministic stub so libc++'s hash-seed init works
  (`eval/engine.cc::InstallWasiRandomStubAndTrapStubs`). A component
  that tries to touch `wasi:filesystem`, `wasi:clocks`, `wasi:io`, or
  `wasi:cli` traps with a named error. Components are supplied as
  bytes at runtime (`Engine::AddComponent`), so updating one means
  handing the engine new bytes — no embedder re-link or redeploy.

A guest failure (trap, panic in a component) surfaces as a non-OK
`absl::Status` or a CEL error value — it unwinds, it does not corrupt
the host. (With one known exception; see §3.)

## 2. The trust model

| Input | Trust required | Why |
|---|---|---|
| **CEL expression source** | **Semi-trusted** | The sandbox contains the *compiled code*, but compilation itself runs in your process, and one known bug lets pathological source crash the host (§3). Until that's fixed, don't compile arbitrary hostile source in-process. |
| **`@host` functions** | **Fully trusted** | They are your C++ lambdas, running in your address space with your privileges. This is the same posture as stock cel-cpp/cel-go custom functions. The sandbox does nothing for you here. |
| **`@component` functions** | **Untrusted OK** | Own memory, trap-stubbed imports, failures become CEL errors. This is the path for third-party plugins and customer-authored predicates. |
| **`Program` bytes** | **Trusted compiler only** | See below. |

**Why Program bytes must come from a compiler you trust.**
`Engine::Plan` validates structure, not provenance: wasmtime validates
the wasm module, the `cel.abi` custom section is decoded as a proto
(`eval/internal/abi_decode.h`), its runtime-ABI version is checked,
and the link-mode label is cross-checked against the module's import
list (`eval/engine.cc`). Nothing verifies that the wasm *code* matches
what the ABI *declares*. A malicious Program is therefore arbitrary
wasm running inside the sandbox — it can't escape linear memory (the
host's slot reads and writes are bounds-checked against memory size,
`eval/instance.cc`), but it can:

- lie about its ABI (declared variables, slot offsets, result kind),
- return any value it likes for any input, and
- call every host import the engine wires — including the
  `cel_host.*` trampolines and **any `@host` function you registered**
  — with arguments of its choosing.

Treat Program bytes like you treat a shared library: load them only
from a build pipeline you control. Signing/validation of Program
artifacts is future work.

## 3. Known limitations — the honest list

Each of these is pinned by a skipped-with-reason regression test or a
tracked backlog entry; none is silent.

- **An oversized literal aggregate can crash the host process.** A
  literal int list of ~10,000 elements compiles and Plans fine, then
  **panics wasmtime on Eval** (`store.rs:2440 assertion failed:
  fault.is_none()`) — a Rust panic that aborts the embedding process,
  not a graceful status.
  (`e2e/known_bugs_test.cc::KnownBugs.LiteralIntListInScanTrapsAt10K`,
  cleanup-backlog #16, P0.) **This is why expression source is
  "semi-trusted" above**: until the arena-OOM fix lands, source you
  don't control can take down your process. Smaller cliffs in the same
  family fail gracefully: a ~4,000-element intermediate returns a CEL
  overflow error
  (`KnownBugs.ExpressionIntermediatesArenaCliff`), and a bound list of
  10,000 strings returns `FAILED_PRECONDITION: arena OOM`
  (`KnownBugs.BoundStringListInScanArenaOomAt10K`, backlog #17).
- **The per-Instance arena is a fixed 64 KiB** (`runtime/cel_layout.h`
  `CELWASM_ARENA_CAPACITY_BYTES`); it does not grow on demand. Heavy
  string concatenation or large aggregate construction inside a single
  Eval hits it. `CompilerOptions::mem_size_bytes` raises the linear
  memory, but the arena cap is separate and not yet configurable.
- **Fuzzing is early.** The m27 property-based-testing machinery
  (FuzzTest) has landed — the compiler pipeline and grammar have
  property/generator suites that run randomised iterations under
  `bazel test` and turn into coverage-guided fuzzers under
  `--config=fuzztest`. Coverage is not yet comprehensive (the ABI
  decoder and runtime kernel have no dedicated targets), and the
  conformance corpus (1966 passing rows) is broad but not adversarial
  input — so treat fuzz coverage as in-progress, not a guarantee.
- **Incidental guardrail, not a defense:** the vendored parser caps
  source at 100,000 codepoints
  (`KnownBugs.ParserSourceCodepointLimitNotConfigurable`, backlog
  #15). That bounds source *length*, but the #16 crash needs only
  ~10,000 list elements — well under the cap.

## 4. Hardening recommendations for embedders today

If you embed cel-wasm now, with expression source you don't fully
control:

1. **Cap expression source length** well below the parser's 100 k
   default — real policy expressions rarely exceed a few thousand
   codepoints.
2. **Restrict literal aggregate sizes before compiling.** A cheap
   pre-compile scan (or a post-parse AST walk) rejecting list/map
   literals beyond a few hundred elements keeps you clear of the #16
   crash and the arena cliffs — and large constant data belongs in an
   activation-bound variable anyway, which also evaluates faster.
3. **Compile untrusted source in a separate process.** The Program is
   pure bytes; run the Compiler in a short-lived sandboxed worker,
   ship the bytes back, and `Plan`/`Eval` in your serving process.
   This contains both compiler bugs and the Eval-time panic.
4. **Set `mem_size_bytes` deliberately.** The default 128 KiB is the
   floor; pin it to what your expressions actually need rather than
   raising it reflexively — it bounds what any one Instance can
   consume.
5. **Audit your `@host` functions as attack surface.** Anything you
   register is callable by the Program with arbitrary arguments;
   validate inputs inside the lambda. Prefer `@component` for any
   function body you didn't write yourself.
