# M30 — descriptor-pool plumbing through the C++ compiler APIs

Status: in progress — drafted 2026-06-11. Slice A (C++ core + public API)
implemented 2026-06-11 (uncommitted).

> **Plan-vs-execution delta (2026-06-11).** The original §3 below designed
> a *bytes-based schema variant* inside the frontend (`SchemaDescriptorSetBytes`
> on `CheckOptions`). During execution this was reshaped to a **pool-first**
> contract: the compiler is a pure descriptor-pool *consumer* — it never
> builds a pool from `.proto` sources or FDS bytes. The public surface is
> `Compiler::Builder::SetDescriptorPool(const DescriptorPool*)`; callers
> (the CLI, the C ABI) build the pool (layered over `generated_pool()` so
> the well-known types the checker needs resolve) and hand it in. The old
> path-based `schema` variant + all in-frontend pool building were
> **deleted**. §3.1–3.2 are rewritten to the as-built shape; §2 below
> describes the pre-m30 starting point (now removed).

## 1. Why

Killing the internal CLI compile backend (the `@cel-wasm/compiler`
binding's `CliBackend`, which shells out to the native `cel` binary) is
the last open m29 follow-up. The blocker is **protobuf descriptors**:

  - M29 conformance reached 1710 pass (+264) by type-checking proto
    expressions against a `FileDescriptorSet` — but it does so through
    the CLI's `--descriptor_set <path>` flag.
  - The in-browser `compiler.wasm` (and the C ABI it wraps) **cannot
    accept a descriptor set at all** today. So the moment we route
    `compile()` / conformance through the wasm backend, every proto row
    regresses from pass back to skip.

So before the CLI backend can die, the wasm compiler must learn to
type-check against a caller-supplied descriptor set. This doc designs
that plumbing — and it is the *prerequisite* for the CLI removal, not
the removal itself (that is the closing slice here).

## 2. Current state (grounded in the source)

The schema carrier is a variant on `CheckOptions`
(`compiler/frontend/parse_and_check.h:35-58`):

```cpp
// compiler/frontend/parse_and_check.h
struct SchemaProtoSource   { std::string path; };  // a .proto source file
struct SchemaDescriptorSet { std::string path; };  // a binary FileDescriptorSet

struct CheckOptions {
  std::variant<std::monostate, SchemaProtoSource, SchemaDescriptorSet> schema;
  std::vector<std::string> variable_specs;
  // ...
};
```

Both variants hold a **filesystem path**. The descriptor-set path is
read with `std::ifstream` and parsed into a `FileDescriptorSet`
(`compiler/frontend/parse_and_check.cc:137-161`,
`RegisterSchemaDescriptorSet`), then `LoadDescriptorPool`
(`parse_and_check.cc:164+`) merges the parsed files with the
process-wide generated pool via a `MergedDescriptorDatabase` and hands
the checker an owned `DescriptorPool`.

Two consumers build `CheckOptions::schema`:

  - **The CLI** (`tools/cel/cel.cc:233-247`, `BuildCompileOptions`) sets
    `opts.check.schema = SchemaDescriptorSet{fds_path}` from
    `--descriptor_set` (or `SchemaProtoSource{path}` from `--proto`).
  - **The C ABI** (`bindings/c/compiler/cel_capi.cc:231-237`,
    `MakeCompilerOptions`) sets `container`, `optimize_level`,
    `link_mode` — and **nothing for schema**. The C ABI has no schema
    surface, so `compiler_wasm_exports.cc` / `cew_compile_opts` cannot
    pass one.

**The browser constraint.** `compiler.wasm` runs under the hand-written
WASI shim in `bindings/ts/compiler/src/internal/wasm-backend.ts`, which
has **no filesystem** (`fd_prestat_get` → `EBADF`, no preopens). A
path-based `SchemaDescriptorSet` is therefore unusable in the browser —
the descriptor set has to arrive as **bytes through linear memory**, not
a path.

## 3. Design

### 3.1 The compiler accepts a descriptor pool (as built)

The compiler is a pure descriptor-pool **consumer**. The public surface
is a Builder setter (the pool is type-environment state, alongside the
message-typed variable declarations it resolves):

```cpp
// compiler/compiler.h
class Compiler::Builder {
  // Borrowed; nullptr → generated_pool(). The supplied pool resolves a
  // type first; types it lacks fall back to the generated pool — but that
  // fallback is the *pool's* property (the caller layers it over
  // generated_pool()), NOT something the compiler builds.
  Builder& SetDescriptorPool(const google::protobuf::DescriptorPool* pool);
};
```

The frontend `CheckOptions` carries the same borrowed pointer
(`const google::protobuf::DescriptorPool* descriptor_pool = nullptr`),
and `parse_and_check.cc` collapses to:

```cpp
const google::protobuf::DescriptorPool* SelectDescriptorPool(
    const CheckOptions& opts) {
  return opts.descriptor_pool != nullptr
             ? opts.descriptor_pool
             : google::protobuf::DescriptorPool::generated_pool();
}
```

That is the entire frontend descriptor logic. The deleted code — the
`.proto` parser, the FDS-file reader, the `SimpleDescriptorDatabase` /
`MergedDescriptorDatabase` / owned-`DescriptorPool` building, and the
`schema` variant — moved **out** of the compiler to the callers (the CLI
already builds a merged pool in `BuildPool`; the C ABI does in §3.2).
The compiler no longer touches a filesystem, which is what the wasm
build needs.

**Why the caller builds the fallback, not the compiler.** A bare
supplied pool (schema only, no generated underlay) fails: the cel-cpp
checker references the well-known types during setup, so the pool handed
to it must resolve them. The idiomatic fix is the caller layering the
schema over `generated_pool()` (a `DescriptorPool` over a
`MergedDescriptorDatabase{schema_db, generated_db}`) — exactly what the
CLI's `BuildPool` already does. The compiler stays dumb: it uses
whichever single pool resolves supplied-then-generated.

### 3.2 C ABI surface

```c
// bindings/c/compiler/cel_capi.h
// Supply a binary-serialized google.protobuf.FileDescriptorSet (the bytes
// `protoc --descriptor_set_out` emits) describing the message types a
// proto-typed expression references. Copied into `opts`; the caller may
// free `fds` after the call. Overrides any previously set descriptor set.
void cel_compile_opts_set_descriptor_set(
    CelCompileOpts* opts, const uint8_t* fds, int len);
```

The C ABI is where the FDS bytes become a pool (all building outside the
compiler): `cel_capi.cc` parses the bytes into a `FileDescriptorSet`,
builds an owning pool **layered over `generated_pool()`** (the same
`MergedDescriptorDatabase` shape the CLI uses), stashes the owning
intermediates on `CelCompileOpts` so they outlive the compile, and calls
`builder.SetDescriptorPool(pool)`. `CelCompileOpts` gains the owning
pool members + a `const DescriptorPool*`; when no descriptor set was
supplied it leaves the pool null (→ generated pool, unchanged).

### 3.3 `cew_compile_opts` records entry

The structured records format already added in m29
(`compiler_wasm_exports.cc`, `ApplyOptions`) gains one kind:

| kind | value | meaning |
|------|-------|---------|
| `'d'` | raw FDS bytes (`len`-delimited) | descriptor set → `cel_compile_opts_set_descriptor_set` |

`ApplyOneOption` gets a `case 'd'`. No new boundary shape — the records
format was designed length-prefixed precisely so a binary blob like an
FDS rides through with no escaping.

### 3.4 TypeScript backend

`CompileRequest.descriptorSet` is today an absolute **path** (consumed by
the CLI's `--descriptor_set`). The wasm backend needs **bytes**. Two
clean options — pick during implementation:

  - **(a)** Keep `descriptorSet: string` (path) in `CompileRequest`;
    `wasm-backend.ts` reads the file → bytes in Node, and the SPA passes
    a second `descriptorSetBytes?: Uint8Array`. Asymmetric.
  - **(b, preferred)** Add `descriptorSetBytes?: Uint8Array` to
    `CompileRequest`; the wasm backend encodes it as a `'d'` record. The
    CLI backend keeps using the path form; a small helper reads a path
    to bytes where a caller only has a path. Symmetric and
    browser-honest — the SPA fetches the FDS like it fetches the
    runtime.

The conformance harness already loads the FDS into memory
(`conformance/src/harness.ts`), so it has the bytes in hand — option (b)
lets it pass them straight to the wasm backend with no temp file.

### 3.5 Closing slice — retire the CLI backend

Once descriptors flow through the wasm path:

  1. `compile()`'s default backend (`compiler/src/index.ts`,
     `getDefaultBackend`) switches from `CliBackend` to
     `WasmCompileBackend`, loading the compiler.wasm asset. (Move the
     54 MB `compiler.wasm` to live with the `@cel-wasm/compiler` package
     rather than `web/public/`, so the compiler binding owns its own
     artifact and the web package depends on it — fixing the current
     compiler↔web layering inversion.)
  2. The conformance harness compiles via the wasm backend with
     `descriptorSetBytes`.
  3. Delete `cli-backend.ts` + `cli-backend.test.ts`; move the
     `CompileBackend` / `CompileRequest` interfaces to a backend-neutral
     module (they currently live in `cli-backend.ts`).
  4. Accept the known diagnostic gap: the stock-wasi-sdk `compiler.wasm`
     gives generic messages on *syntax* errors (the EH wall). Type-check
     errors keep full diagnostics. This is the already-accepted "wasm
     now, emscripten later" tradeoff; emscripten restores syntax
     diagnostics in a later milestone.

## 4. Testing

Per repo rules — probe the real behaviour before asserting it, and pin
it with the oracle where a value is in question.

  - **C++ unit (slice A, done)** — `parse_and_check_test.cc`: a supplied
    `descriptor_pool` (a `Widget` message defined only in that pool,
    layered over generated) resolves a field select and a nested field;
    with no pool the supplied-only type is undeclared. `compiler_test.cc`:
    the public `SetDescriptorPool` compiles `w.label` end-to-end, and
    without it the type is undeclared.
  - **C ABI (slice B)** — `cel_capi_test.cc`: `cel_compile_opts_set_descriptor_set`
    then `cel_compile` of a proto expression succeeds and the emitted
    Program decodes; null/zero-len leaves the pool null (→ generated pool).
  - **wasm e2e** — extend `web/src/run.test.ts` (or a compiler-package
    test once the asset moves): compile a proto expression through
    `compiler.wasm` with a `'d'` record and assert it type-checks where
    it previously failed `undeclared reference`.
  - **Conformance** — the existing corpus run is the integration gate:
    flipping the harness to the wasm backend must hold 1710 pass / 0
    fail (the ratchet enforces it).
  - **Oracle** — for any "does this proto expression type-check / what
    does it evaluate to" question that the bytes-vs-path change could
    perturb, add a case to `testdata/cel_cpp_oracle_test.cc` rather than
    reasoning it out.

## 5. Work breakdown (slices)

  - **A — C++ core + public API. ✅ DONE (2026-06-11, uncommitted).**
    `CheckOptions::descriptor_pool` + `SelectDescriptorPool` (frontend is a
    pure pool consumer); public `Compiler::Builder::SetDescriptorPool`
    threaded Builder → Compiler → `Compile` → `CheckOptions`; the path-based
    `schema` variant + all in-frontend pool building **deleted**; the CLI
    migrated to pass its `BuildPool` pool via `descriptor_pool`. Tests:
    `parse_and_check_test` (supplied-pool resolution, nested field,
    null-can't-resolve-supplied-only), `compiler_test` ×2
    (`SetDescriptorPool` compiles `w.label` end-to-end; without-pool
    undeclared). Broad `//compiler/... //eval/... //tools/... //conformance/...`
    build green.
  - **B — C ABI + wasm export.** `cel_compile_opts_set_descriptor_set`
    (parses FDS bytes → builds a pool over `generated_pool()` → owns it on
    `CelCompileOpts` → `builder.SetDescriptorPool`), the `'d'` record in
    `ApplyOneOption`, rebuild `compiler.wasm`, C ABI test. End-to-end
    testable via a Node harness driving the rebuilt wasm (the m29 pattern).
  - **C — TS backend. ✅ DONE.** `descriptorSetBytes` on `CompileRequest`
    + `CompileOptions`, `wasm-backend.ts` `'d'`-record encoder, Widget e2e
    in `web/run.test.ts`.  (Also: `buildCompileArgs` now passes
    `--link_mode` explicitly so the CliBackend stays static-by-default
    after the cel tool's dynamic-default flip.)
  - **D — retire the CLI. ✅ DONE.** The compiler.wasm asset now lives with
    the `@cel-wasm/compiler` package (`compiler/wasm/compiler.wasm`,
    git-ignored, installed by `scripts/build-wasm-assets.sh`, loaded lazily
    from the built path via `internal/compiler-loader.ts` — mirroring
    eval's `runtime-loader.ts`). `getDefaultBackend()` is now the
    `WasmCompileBackend` (lazy+cached `getDefaultWasmBackend()`); the
    `cli-backend.ts` + `cli-backend.test.ts` are deleted; the
    `CompileBackend` / `CompileRequest` / `LinkMode` interfaces moved to a
    backend-neutral `internal/backend.ts`; the path-based `descriptorSet` +
    `resolveCelCli` / `CEL_CLI` plumbing is gone (the wasm backend takes
    only `descriptorSetBytes`). Conformance compiles via the wasm backend
    with in-memory `descriptorSetBytes`. Gate: full corpus holds **1710
    pass / 744 skip / 0 fail** (was 1710/735/0), wall-clock ~5.3 min (vs
    the CLI backend's ~10 min — the in-process backend is faster, no
    subprocess fork per row).

    > **Plan-vs-execution delta (slice D).** The 9 `cli_limitation` rows
    > (embedded-NUL `b'\x00'` byte literals) did **not** convert to PASS:
    > the C ABI's source boundary is a NUL-terminated `const char*`
    > (`cew_compile_opts`), so an embedded NUL still truncates the source
    > (now tripping the EH-wall as a generic compile error rather than the
    > CLI's process-arg rejection). They remain a reasoned SKIP, renamed
    > `cli_limitation` → `embedded_nul` and detected on the expression
    > itself (the wasm diagnostic is generic). A length-delimited source
    > entry point (slice F's proto `CompileRequest`) would let these
    > compile. Pass floor held; the skip count rose 735 → 744 as the FAILs
    > that briefly surfaced were reclassified to the renamed SKIP.
  - **E — relocate the C ABI.** `bindings/c/` → `bindings/c/compiler/`
    (it is the *compiler* C ABI; leaves room for a future
    `bindings/c/eval`).  Move sources + BUILD targets + fix the
    `//bindings/c:...` label references across the tree.
  - **F — schema'd CompileRequest.** Replace the ad-hoc length-prefixed
    records blob (`cew_compile_opts`'s `'v'`/`'f'`/`'c'`/`'o'`/`'l'`/`'d'`)
    with a single **proto** `CompileRequest` message (source, repeated
    var decls, repeated fn decls, container, optimize_level, link_mode,
    descriptor_set bytes).  Proto over JSON: the request carries binary
    FDS bytes (JSON would force base64) and both sides already link
    protobuf.  The C ABI takes serialized `CompileRequest` bytes; the JS
    side serializes via protobufjs.

Slices A–C are additive and regression-free.  D is the behavioural switch
(carries the conformance gate); E is a mechanical relocation; F is a
wire-format change to the compile boundary.

Adjacent workstream (NOT m30): the **eval-binding conformance backlog**
(`m29-ts-conformance-backlog.md`) — the 357 fixable skips the breakdown
surfaced, led by WKT-typed field construction from a scalar (110 rows).
These are `@cel-wasm/eval` runtime-ABI gaps, scheduled after slice D.

## 6. Open questions

  - **Asset home & size.** `compiler.wasm` is 54 MB. Moving it into the
    published `@cel-wasm/compiler` package makes the npm tarball large;
    do we ship it as an optional/peer asset, fetch-on-demand, or accept
    the size? (The browser already lazy-fetches it; Node could too.)
  - **`SchemaProtoSource` for wasm?** Source-`.proto` compilation also
    needs an in-memory variant for the browser, but no current consumer
    needs it (conformance uses FDS). Deferred unless a driver appears.
  - **Descriptor-set merge order.** The CLI merges schema-over-generated;
    confirm the bytes path keeps the same precedence so a schema that
    re-declares a WKT can't shadow the generated one (probe before
    assuming).

## 7. Relationship to other docs

  - `m29-typescript-bindings.md` — the descriptorSet path-based wiring
    (CLI) shipped there; this doc supersedes it for the wasm/browser
    path and closes the CLI-backend-removal follow-up it opened.
  - `design.md` — the C ABI surface section gains
    `cel_compile_opts_set_descriptor_set` when slice B lands.
