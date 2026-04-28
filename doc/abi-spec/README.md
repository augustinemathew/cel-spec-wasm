# CEL WASM ABI Spec

The contract a TS/Go (or any other) port of the evaluator must
implement. Every claim in this directory is tagged with a unique
`[ASSUMED ABI-A###]` or `[VALIDATED ABI-A###]` marker so the spec is
verifiable, not aspirational.

## Layers

  - **L1 — Memory layout** (`01-memory-layout.md`): byte-level layout
    of `CelValue`, `CelKind` enum, aggregate headers, arena state, the
    reserved cursor slot, and entry strides. The thing every host
    decoder reads.
  - **L2 — Wire format** (`02-wire-format.md`, TBD): the `cel.abi`
    custom-section proto schema and version byte.
  - **L3 — Wasm interface** (`03-wasm-interface.md`, TBD): exports the
    host calls (`cel_alloc`, `cel_reset`, `eval`, …); imports the wasm
    calls (the host trampolines).
  - **L4 — Semantics** (`04-semantics.md`, TBD): what each import does,
    error propagation rules, externref lifetime, two-phase Engine /
    Instance lifecycle.

L1 is the foundation: L2/L3 reference its types; L4 references its
error encoding.

## Process

Per repo policy (`CLAUDE.md` "WAT-first" rule, generalised):

  1. Draft the spec with every claim marked `[ASSUMED ABI-A###]`.
  2. Build the assumption table at the bottom of each layer doc.
  3. Validate: for every assumption, point to (or write) a test that
     pins the value. Code-side static_asserts count; runtime tests
     count; a prose paragraph does not.
  4. Update marker to `[VALIDATED ABI-A###]` and append the test ref.
  5. If a test fails, decide *spec wrong* or *code wrong* and reconcile
     in the same commit.

The end state: a reader can grep `[ASSUMED` in this directory and
expect zero matches. Every byte of the ABI is asserted somewhere.

## Sources of truth

  - **Code** (`compiler_v2/runtime/cel_data.h`, `cel_arena.h`,
    `cel_memory.h`, `cel_make.h`, `cel_runtime.c`, `api/error.h`) —
    the implementation. Where this doc disagrees with the code, one
    of them is wrong; reconcile.
  - **CEL language spec** (`doc/langdef.md`) — the semantic
    grounding. Where the doc claims behaviour (e.g. "missing map key
    is an error, not null"), it cites the langdef section.
  - **NOT** `doc/implementation-plan/rewrite/cel-host-surface.md` —
    that doc is aspirational and out of sync; treated as a hypothesis
    to verify against, not a foundation to build on.
