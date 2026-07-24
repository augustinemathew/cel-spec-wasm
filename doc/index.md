---
hide:
  - navigation
---

# cel-wasm

**[CEL](https://github.com/google/cel-spec) is the expression language
Kubernetes, Envoy, and IAM already use to decide *"is this allowed?"*.
cel-wasm compiles it *ahead of time* into a sandboxed WebAssembly artifact —
so you can evaluate sensitive or untrusted policy expressions at native speed,
on every host, with no way for the expression to escape the sandbox, read host
memory, do I/O, or hang the process.**

**0 conformance failures**, honest two-sided benchmarks, and one portable
artifact that runs byte-for-byte everywhere. Stock CEL is a tree-walking
interpreter, re-implemented per host language; cel-wasm compiles instead —
no AST walk and no interpreter at eval time, just Cranelift-emitted native
code in a bounded, syscall-free sandbox.

!!! abstract "Built for two shapes of workload"

    **🔐 Security-critical & regulated** — banking, fintech, healthcare,
    multi-tenant SaaS. Evaluate sensitive business rules or *customer-authored*
    predicates (a fraud check, an entitlement rule, a transaction-limit policy)
    without the expression seeing more than you marshal in, escaping the
    sandbox, or being able to crash or hang your service. Even custom functions
    can run as isolated WebAssembly components you don't have to trust.

    **⚡ Lightweight & at the edge** — Envoy / API-gateway filters, request
    routing, rate-limit decisions, feature flags. One tiny, deterministic
    `.wasm`: compile once and run identical bytes at every proxy or node at
    native speed, with no per-host interpreter to drift.

```bash
bazel build //tools/cel:cel   # once
bazel-bin/tools/cel/cel eval 'age >= 18 && country in ["US","CA"]' \
      --var age:int=25 --var 'country:string="US"'
# => true     # parsed → checked → wasm → Cranelift JIT → native
```

<div class="grid cards" markdown>

-   :material-rocket-launch: **[Get started](user-guide/getting-started.md)**

    From `fetch_cel_cpp.sh` to a running `Eval` — the first embed, step by step.

-   :material-book-open-variant: **[User guide](user-guide/index.md)**

    The embedder's guide: compile → plan → eval, activations, custom functions.

-   :material-sitemap: **[Architecture](design/00-architecture.md)**

    The four-role lifecycle, the link-mode fork, the ABI, the memory map.

-   :material-shield-check: **[Security model](user-guide/security-model.md)**

    What the sandbox guarantees, who you trust, the known limits.

</div>

## Why it's different

- **Two compilers deep, zero interpreters.** The CEL compiler lowers the
  expression to wasm ahead of time (Binaryen); [wasmtime](https://wasmtime.dev/)'s
  [Cranelift](https://cranelift.dev/) JITs that wasm to native code at `Plan`
  time, amortized across evals. By `Eval()` there is no AST walk, no tree
  dispatch, no wasm interpreter — just native code.
- **One semantic implementation.** The compiler is the only component that
  knows CEL. Every host runs the same bytes, so cross-language semantic drift
  is structurally impossible.
- **Sandboxed by construction.** Bounded linear memory, no syscalls, no I/O, no
  recursion. Even *custom functions* can come from code you don't fully trust
  (sandboxed WebAssembly components with their own linear memory).
- **A `Program` is pure bytes.** Compile it in one process, write it to disk,
  evaluate it in a process that never links the compiler.

## Status

**Beta.** The pipeline, sandbox, benchmarks, and conformance numbers are real
and reproducible. **0 conformance failures** — every attempted row of the
upstream CEL corpus passes, in both link modes. Development is heavily
AI-assisted — designed and pair-programmed with Claude (Anthropic),
with every change gated by conformance, differential fuzzing against
cel-cpp, and the benchmark harness. The remaining hardening gaps
are listed honestly in the
[security model](user-guide/security-model.md) and the repo's *Production
readiness* section.

Need the full dynamic surface — every extension, every `dyn` case? Use
[`cel-cpp`](https://github.com/google/cel-cpp) or
[`cel-go`](https://github.com/google/cel-go). cel-wasm trades that surface for
AOT speed, portability, and the sandbox; it's not a drop-in replacement.

---

Built on Google's [cel-cpp](https://github.com/google/cel-cpp) (parser +
type-checker), [wasmtime](https://wasmtime.dev/) / [Cranelift](https://cranelift.dev/)
(JIT), and [Binaryen](https://github.com/WebAssembly/binaryen) (wasm codegen).
