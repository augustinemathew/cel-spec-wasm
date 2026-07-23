# Security model

You're about to run an expression you may not have written — a customer's
access policy, a tenant's routing rule, an analyst's fraud predicate —
over data you can't afford to leak, on a host you can't afford to hang.
This doc is the precise answer to *"what can that expression actually do
to me?"*

The short version: a compiled expression becomes native code trapped in a
sandbox. It can compute over **only what you hand it** and return **only a
value**. It *cannot* read your memory, make a syscall, touch the disk or
network, read the clock, or loop forever. There are exactly **three trust
decisions** you make, and **one** known sharp edge. The rest is detail.

*Updated 2026-06-11; every claim is checked against the code cited next
to it.*

## 1. The boundary — what the expression can and can't do

A compiled expression runs as Cranelift-JIT'd native code inside a
wasmtime store. Picture the boundary:

```
 ┌─ YOUR process ──────────────────────────────────────────────────────┐
 │                                                                      │
 │   your heap · your secrets · your file handles · your network        │
 │        ▲                                                             │
 │        │  in:  only the values you Bind()  ───────────────┐         │
 │        │  out: only the result Value  ◄───────────────────┤         │
 │   ┌────┴─────────────  wasmtime sandbox  ──────────────────┴─────┐   │
 │   │                                                              │   │
 │   │   the compiled expression  (native code, via Cranelift)      │   │
 │   │                                                              │   │
 │   │   ✓ CAN:   arithmetic, comparisons, field reads, read        │   │
 │   │            marshaled-in vars, build values in its own arena  │   │
 │   │                                                              │   │
 │   │   ✗ CANNOT: read/write your memory · call a syscall ·        │   │
 │   │            open a file · hit the network · read a clock ·    │   │
 │   │            recurse · loop unbounded · hang the process       │   │
 │   │                                                              │   │
 │   └──────────────────────────────────────────────────────────────┘   │
 │                                                                      │
 │   the ONE door out: @host functions you registered (see §2) ─────────┤
 └──────────────────────────────────────────────────────────────────────┘
```

The guarantees, precisely:

- **Bounded linear memory.** The expression and the runtime kernel share
  one wasm linear memory, sized by `CompilerOptions::mem_size_bytes`
  (default 128 KiB). Every load/store is bounds-checked by wasmtime — the
  expression physically cannot read or write your process memory.
- **No I/O, no syscalls.** The runtime links wasi-libc, so it *imports* a
  few WASI calls — but the engine wires them to a deliberately empty WASI
  context: no filesystem preopens, no environment, no stdio
  (`eval/engine.cc::InitStore`). There is no path from CEL evaluation to
  the filesystem, network, clock-as-capability, or process control.
- **No unbounded control flow.** CEL is a *total* language. Comprehensions
  lower to counted loops; recursive `@native` bodies are rejected at
  compile. An expression cannot spin forever.
- **Failures unwind, they don't corrupt.** A guest trap or a panic in a
  component surfaces as a non-OK `absl::Status` or a CEL error value — the
  host stays intact. (One residual exception, at *compile* time, in §3.)

## 2. The trust model — three decisions

Everything you feed cel-wasm falls into one of four buckets, and only one
of them needs real thought:

| What you provide | Trust required | The one-liner |
|---|---|---|
| **CEL expression source** | 🟡 **Semi-trusted** | Sandboxed once compiled — but *compilation* runs in your process, and one input shape can still crash the compiler (§3). Compile untrusted source in a separate worker (§4). |
| **`@host` functions** | 🔴 **Fully trusted** | Your C++ lambdas, your address space, your privileges. The sandbox does nothing for you here. |
| **`@component` functions** | 🟢 **Untrusted OK** | Own memory, trap-stubbed imports, failures become CEL errors. The path for third-party / customer code. |
| **`Program` bytes** | 🔴 **Trusted compiler only** | Treat like a shared library; see below. |

The decision that matters most is **how you let an expression call back
into your code** — and that's exactly the `@host` vs `@component` split:

```
   @host  function                     @component  function
   ════════════════                    ══════════════════════
   your C++ lambda                     a WebAssembly component
   ┌──────────────────────┐            ┌──────────────────────┐
   │ runs in YOUR memory  │            │ runs in its OWN linear│
   │ with YOUR privileges │            │ memory — can't see    │
   │                      │            │ the expression's, let │
   │ can do anything your │            │ alone yours           │
   │ process can do       │            │                      │
   │                      │            │ every import is a     │
   │ = same posture as    │            │ trap stub (no I/O)    │
   │   stock cel-cpp fns  │            │                      │
   └──────────────────────┘            └──────────────────────┘
        FULLY TRUSTED                       UNTRUSTED OK
   (you wrote it, you own it)          (3rd-party plugins,
                                        customer-authored predicates)
```

So the rule of thumb: **a function body you wrote → `@host`; a function
body you didn't → `@component`.** A component is supplied as bytes at
runtime (`Engine::AddComponent`), so swapping one means handing the engine
new bytes — no re-link, no redeploy. Its only capability is one
deterministic `wasi:random` stub (for libc++'s hash seed); touching
`wasi:filesystem` / `clocks` / `io` / `cli` traps with a named error.

**Why `Program` bytes need a compiler you trust.** `Engine::Plan`
validates *structure, not provenance* — it checks the wasm is valid, the
`cel.abi` decodes, the ABI version matches, the link-mode label is
consistent. Nothing verifies the wasm *code* matches what the ABI
*declares*. A malicious Program is therefore arbitrary wasm in the
sandbox: it still can't escape linear memory, but it can lie about its
ABI, return anything, and **call every host import you wired — including
your `@host` functions — with arguments of its choosing**. So:

```
   trusted compiler ──► Program(bytes) ──► Engine.Plan ──► Eval
        ▲                                                          
        └── load Program bytes ONLY from a build pipeline you control.
            They are executable input, like a .so — not data.
```

Signing/validation of Program artifacts is future work.

## 3. The honest list — known limitations

Every item here is pinned by a passing or skipped-with-reason regression
test, or a tracked backlog entry. None is silent.

**🔴 The one open host-crash — deeply *bracket*-nested source.** Source
like `[[[…]]]` nested ~2,000 levels overflows the 8 MiB parser thread
stack inside cel-cpp's ANTLR stage, *before* the depth gate can return a
graceful error (cleanup-backlog #47). It takes hostile, hand-built input
far beyond any real expression — but on a default-stack thread it's a
crash, which is exactly why §4's "compile in a separate worker" is
load-bearing. Fix: size the parse stack to the limit.

**🟢 Everything else fails *gracefully* — by design:**

- **Deep expressions are capped, not crashed.** The parser, codegen, and
  the JIT each recurse one stack frame per nesting level, so an unbounded
  `a+b+c+…` chain would overflow the native stack (~10k terms). The
  compiler caps nesting at `kMaxExpressionNestingDepth` (2048) and rejects
  deeper input with a graceful `ResourceExhausted` — it never reaches
  codegen or the JIT. Clears realistic policies with wide margin
  (cleanup-backlog #45).
- **Very large literal aggregates don't compile** (a capability limit).
  An oversized literal list/map is rejected at compile with a loud
  `ResourceExhausted`, never silently miscompiled. The current rodata
  window admits roughly 10,900 int list elements / 4,600 int map
  entries; every boundary is pinned just-inside / just-past in
  `e2e/limits_test.cc`. Put larger constant data in an
  activation-bound variable.
- **The arena grows on demand, bounded by memory.** A chained,
  malloc-backed bump allocator (`runtime/cel_arena.c`); exhaustion is a
  graceful `ResourceExhausted` / `FAILED_PRECONDITION`, never a crash.
  Residual gap: a bound `list<string>` of ≥10k strings scanned by `in`
  returns an arena-OOM error instead of `true` — graceful, but a
  functional gap for big permission sets (cleanup-backlog #17).

**🟡 Maturity, not safety:**

- **Fuzzing is differential, and runs nightly — but is not exhaustive.**
  A property-based suite (`e2e/fuzz/`) generates type-checked CEL,
  evaluates it through both this pipeline and the real cel-cpp oracle,
  and fails on any divergence; CI runs the sweep nightly
  (`.github/workflows/fuzz.yml`). The ABI decoder and runtime kernels
  still lack dedicated byte-level fuzz targets, and the conformance
  corpus (2035 passing rows) is broad but not adversarial — treat fuzz
  coverage as strong on semantics, in-progress on wire surfaces.
- **The 100k-codepoint parser cap is a coarse fence, not the boundary.**
  The real safety gates are the static-region and depth checks above; the
  codepoint cap just bounds source *length*. Size your own input cap to
  your workload.

## 4. If you embed this today

For expression source you don't fully control, in priority order:

```
   ① cap source length         ── bound compile cost (the gates do safety)
   ② bound vars > big literals  ── compiles, and runs faster
   ③ COMPILE IN A SEPARATE      ── contains the #47 parser crash + any
      WORKER  ◄── load-bearing      future compiler bug on hostile source
   ④ set mem_size_bytes         ── cap what one Instance can consume
   ⑤ audit @host functions      ── the Program can call them with
                                   arbitrary args; validate inside the
                                   lambda — or use @component instead
```

1. **Cap expression source length** well below the 100k default — real
   policies rarely exceed a few thousand codepoints. This bounds compile
   *cost*; safety is enforced regardless.
2. **Prefer activation-bound variables to large literal aggregates** — it
   sidesteps the static-region limit and evaluates faster.
3. **Compile untrusted source in a separate process.** This is the
   load-bearing one while #47 is open: a separate, short-lived worker
   contains the parser-crash blast radius (and insures against any
   future compiler bug on hostile input). The `Program` is pure bytes —
   compile in the worker, ship the bytes back, `Plan`/`Eval` in your
   serving process.
4. **Set `mem_size_bytes` deliberately.** The default 128 KiB is a floor;
   pin it to what your expressions need rather than raising it reflexively.
5. **Audit your `@host` functions as attack surface.** Anything you
   register is callable by the Program with arbitrary arguments — validate
   inputs inside the lambda, and prefer `@component` for any body you
   didn't write yourself.
