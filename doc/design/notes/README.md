# Design-rebuild notes

Working notes for the 2026-06-10 design-documentation rebuild. Each
file is the output of an exhaustive read of one component — its code,
its tests, and its historical design docs together. The notes are the
*verified* raw material from which the new design-doc set
(`doc/design/`) is authored; nothing enters a design doc that isn't
backed by a citation or a validation item resolved here.

- `<component>.md` — per-component notes (verified architecture,
  doc-vs-code discrepancies, validation items, coverage observations).
- `9x-*.md` — cross-component lens passes (ABI/memory consistency,
  contract coherence, testing synthesis, benchmarking synthesis).
- `00-consolidated-findings.md` — the merged discrepancy register,
  validation backlog, and the proposed new design-doc set.

These are working artifacts: they get archived or deleted once the
design docs they feed have shipped.

## Authoring principles for the new design docs (owner guidance)

1. **The design must fit together cohesively.** Each doc has a spine —
   for the compiler, the pass pipeline itself (parse/check → resolve →
   layout → lower → optimize → emit); for the evaluator, the
   plan/instantiate/eval lifecycle. Every mechanism is explained at
   its spot in that spine, in the larger context that invokes it: the
   slot allocator inside the layout/lowering story (how workspace
   slots are assigned and reused per expression node), the static
   memory builder inside the rodata story, the overload table inside
   the lowering story. No orphaned component pages.
2. **Explain the passes as passes**: for each — what it consumes, what
   it produces, what invariant it establishes for the next pass, and
   what breaks if it's reordered. The pass contract chain IS the
   compiler design.
3. Every claim backed by the notes here (citation or resolved
   validation item). Dry register. Colorful generated diagrams
   (pipeline, dependency graph, memory map, trust boundary) carry the
   cohesion visually; one shared palette across all docs.
4. Reader path: a newcomer reads ARCHITECTURE/overview → subsystem
   design (compiler, evaluator) → testing strategy / benchmarking,
   and at no point needs a historical doc to understand the present.
