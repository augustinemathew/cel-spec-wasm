---
hide:
  - navigation
---

# cel-wasm

**An AOT compiler for [CEL](https://github.com/google/cel-spec): it lowers an
expression to a portable `.wasm` module, then JITs that to native machine code
inside a sandbox. Up to 25× faster than the `cel-cpp` interpreter on repeated
evaluation — and the *same artifact* runs, byte-for-byte, on every host.**

Stock CEL is a tree-walking interpreter, re-implemented per host language.
cel-wasm compiles instead: no AST walk and no interpreter at eval time, just
Cranelift-emitted native code in a bounded, syscall-free sandbox. Compile once;
every runtime executes identical bytes.

```bash
bazel run //tools/cel:cel -- eval 'age >= 18 && country in ["US","CA"]' \
      --var age:int=25 --var country:string=US
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
upstream CEL corpus passes, in both link modes. The remaining hardening gaps
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
