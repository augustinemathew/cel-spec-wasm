# L1 — Memory layout

Status: **draft, unvalidated.** Every claim is tagged `[ASSUMED
ABI-A###]`. Tags become `[VALIDATED]` once a test pins the value.
See `README.md` for process.

This layer describes the byte-level layout of values and aggregates
in the wasm linear memory shared by the expr module (the compiled
program) and the runtime module (`cel_runtime.wasm`). A correct TS
or Go evaluator must read and write these exact byte shapes.

## 1. Endianness, alignment, pointer width

  - **A001 [ASSUMED]** Wasm linear memory is little-endian. The host
    must run on a little-endian platform; multi-byte values cross
    the host↔wasm boundary by raw memcpy with no byte-swap.
    *(WASM spec mandates LE; mirrored on host so memcpy works.)*

  - **A002 [ASSUMED]** All `CelValue` instances are 8-byte aligned in
    linear memory. Codegen emits each value at an 8-aligned offset;
    the arena allocator only returns 8-aligned offsets.

  - **A003 [ASSUMED]** Linear-memory offsets are unsigned 32-bit
    (`u32`). Wasm32 only. Offset `0` is reserved as the universal
    "absent" sentinel — no live `CelValue` ever sits at offset 0.

  - **A004 [ASSUMED]** All scalar widths follow C99 `<stdint.h>`:
    `i32`/`u32` are 4 bytes, `i64`/`u64` are 8 bytes, `double` is
    IEEE-754 binary64 (8 bytes).

## 2. Linear-memory regions

The expr module owns the memory and exports it; the runtime module
imports it. Lifetime is per-`Instance`. Layout (low offset → high):

```
[0x0000 .. 0x0008)   reserved sentinel (8 bytes; offset 0 = absent)
[0x0008 .. 0x000C)   u32 arena bump cursor
[0x000C .. 0x0010)   u32 arena limit
[0x0010 .. R)        .rodata (compiler-emitted constant CelValues, strings)
[R      .. limit)    bump arena (cel_alloc carves from here)
[limit  .. mem_size) tail (unused, reserved for memory.grow headroom)
```

  - **A010 [ASSUMED]** Bytes `[0, 8)` are reserved and never written
    by emitted code or the runtime. The 8-byte width matches
    `CelValue` alignment so the cursor slot at offset 8 is aligned.

  - **A011 [ASSUMED]** The arena cursor lives at fixed offset `0x08`
    as a `u32`. `cel_reset` writes it; `cel_alloc` reads-modify-writes
    it. Hosts MUST NOT touch it directly.

  - **A012 [ASSUMED]** The arena limit lives at fixed offset `0x0C`
    as a `u32`. Set once by `cel_reset` per-eval; read by `cel_alloc`
    on every allocation.

  - **A013 [ASSUMED]** `.rodata` begins at offset `0x10` (16). The
    compiler emits a wasm `(data ...)` segment whose memory offset
    starts at 16 and runs to `R = 16 + sizeof(rodata)`.

  - **A014 [ASSUMED]** `R` (rodata end / arena base) is a
    compile-time constant baked into the wasm as the first arg of
    `cel_reset`. The host does not compute it.

  - **A015 [ASSUMED]** Initial linear-memory size is `ceil(mem_size_bytes
    / 65536)` wasm pages, where `mem_size_bytes` is a `CompileOptions`
    field defaulting to `128 * 1024` → 2 pages. The minimum is 2
    pages: `cel_runtime.wasm` is built with
    `--import-memory=cel,memory` and links against an imported
    memory of `min=2` (`compiler_v2/runtime/BUILD.bazel:191`,
    confirmed by `cel_runtime_wasm_test.cc:159`); a single-page
    expr module would fail to instantiate against it. The
    `mem_size_bytes` value is also passed as the second arg of the
    `cel_reset(base, limit)` call emitted at the top of `eval()`,
    fixing the arena ceiling per Instance.

## 3. Arena

  - **A020 [ASSUMED]** `cel_reset(base, limit)` is the only legal way
    to initialise the cursor slots. It is exported by the runtime
    module and is called as the first instruction of every emitted
    `eval()`.

  - **A021 [ASSUMED]** `cel_alloc(n)` returns the pre-bump offset of
    a zero-filled run of `align_up(n, 8)` bytes, or `0` on
    out-of-space. Allocations are 8-aligned.

  - **A022 [ASSUMED]** `cel_alloc(0)` returns the next 8-aligned
    offset (rounds the request up to 8). Two consecutive
    `cel_alloc(0)` calls return offsets that differ by 8.

  - **A023 [ASSUMED]** `cel_alloc` zero-fills the returned region.
    Callers may rely on freshly-allocated `CelValue` having
    `kind == CEL_NULL` and zero payload before they overwrite it.

  - **A024 [ASSUMED]** Arena allocations are not freed individually;
    `cel_reset` is the only way to reclaim space, and it discards
    every prior allocation in one shot.

## 4. CelValue layout (24 bytes)

  - **A030 [ASSUMED]** `sizeof(CelValue) == 24` on every supported
    platform. This is wire-stable; changing it is an ABI break.

  - **A031 [ASSUMED]** Layout, byte-by-byte:
    ```
    offset  size  field
    0       4     u32   kind          (one of the CelKind values, A040)
    4       4     u32   _pad          (always 0; reserved for future use)
    8       16    union payload       (interpretation by `kind`)
    ```

  - **A032 [ASSUMED]** The 4-byte `_pad` at offset 4 is always
    written as zero. Two write paths exist and both zero it:
    (1) `cel_alloc` zero-fills the returned region (A023), so any
    `CelValue` reached via `alloc_cv` starts at zero and the
    payload writes that follow never touch offsets [4,8); (2)
    `StaticMemoryBuilder::OpenFrame` (codegen-emitted rodata
    `CelValue`s) writes the `_pad` slot explicitly with
    `AppendU32LE(buf_, 0u);` immediately after the `kind` (see
    `static_memory_builder.cc:60-61`). Host decoders MAY ignore it;
    producers MUST zero it.

  - **A033 [ASSUMED]** Payload union starts at offset 8 and
    occupies 16 bytes. All payload variants fit in 16 bytes; no
    variant uses offset 0–7 of the payload region.

### 4.1 Payload variants by kind

  - **A040 [ASSUMED]** `kind == CEL_NULL` (0): payload is unused (16
    zero bytes). Producers MUST zero the payload region.

  - **A041 [ASSUMED]** `kind == CEL_BOOL` (1): payload bytes 8–11
    hold an `i32` (0 = false, 1 = true). Bytes 12–23 are zero. No
    other `i32` values are valid (`2` is not a "true" bool).

  - **A042 [ASSUMED]** `kind == CEL_INT` (2): payload bytes 8–15
    hold an `i64` (CEL spec §"Numeric Values" — 64-bit signed).
    Bytes 16–23 are zero.

  - **A043 [ASSUMED]** `kind == CEL_UINT` (3): payload bytes 8–15
    hold a `u64`. Bytes 16–23 are zero.

  - **A044 [ASSUMED]** `kind == CEL_DOUBLE` (4): payload bytes 8–15
    hold an IEEE-754 binary64. Bytes 16–23 are zero. NaN payload
    bits are not preserved across host↔wasm transfer (memcpy
    preserves bytes; observers compare via `==`, which makes any
    two NaNs equal per CEL spec §"Numeric Values").

  - **A045 [ASSUMED]** `kind == CEL_STRING` (5): payload is a
    `CelSpan { u32 ptr; u32 len; }` at offset 8 (`ptr`) and offset
    12 (`len`). Bytes 16–23 are zero. `ptr` is a linear-memory
    offset to UTF-8 bytes; `len` is byte length. Empty string is
    `ptr=0, len=0`.

  - **A046 [ASSUMED]** `kind == CEL_BYTES` (6): identical wire
    layout to `CEL_STRING`. Distinguished only by `kind`.

  - **A047 [ASSUMED]** `kind == CEL_LIST_ARENA` (7): payload bytes
    8–11 hold `u32 header_ptr` — a linear-memory offset to an
    `ArenaListHeader` (§5.2). Bytes 12–23 are zero.

  - **A048 [ASSUMED]** `kind == CEL_MAP_ARENA` (8): payload bytes
    8–11 hold `u32 header_ptr` — a linear-memory offset to an
    `ArenaMapHeader` (§5.1). Bytes 12–23 are zero.

  - **A049 [ASSUMED]** Host-backed kinds — `CEL_MAP_HOST` (9),
    `CEL_MESSAGE` (10), `CEL_LIST_HOST` (17) — share an identical
    payload shape: bytes 8–11 hold `u32 ref_slot`, an index into
    the host-side `ExternrefTable`. Bytes 12–23 are zero. The host
    owns the backing object; wasm code MUST NOT dereference
    `ref_slot` directly. The CelKind tag is the only on-wire
    distinction; per L4 invariant (forthcoming), the three kinds
    are interchangeable from a CEL program's perspective and
    differ only in observer dispatch (map lookup vs list indexing
    vs field selection).

  - **A050 [RESERVED]** *(Was: separate CEL_MESSAGE assumption with
    a `msg_slot` union name. Collapsed into A049; the distinct
    `msg_slot` member was renamed to `ref_slot` in `cel_data.h`,
    making all three host-backed kinds share one union member. ID
    retained for table stability.)*

  - **A051 [ASSUMED]** `kind == CEL_TYPE` (11): payload bytes 8–11
    hold `u32 type_id` — an index into a compile-time type table.
    Bytes 12–23 are zero.

  - **A052 [ASSUMED]** `kind == CEL_DURATION` (12): payload is a
    `CelDurTs { i64 seconds; i32 nanos; i32 _pad; }` filling all 16
    payload bytes (offsets 8, 16, 20, 24-aligned). Semantics:
    `google.protobuf.Duration`.

  - **A053 [ASSUMED]** `kind == CEL_TIMESTAMP` (13): identical
    wire layout to `CEL_DURATION`. Semantics:
    `google.protobuf.Timestamp` (epoch-relative).

  - **A054 [ASSUMED]** `kind == CEL_OPTIONAL` (14): payload bytes
    8–11 hold `u32 opt` — an offset to the wrapped `CelValue`, or
    `0` for empty optional.

  - **A055 [ASSUMED]** `kind == CEL_UNKNOWN` (15): payload bytes
    8–11 hold `u32 unk` — an attribute-id (index into the
    compile-time attribute table). Bytes 12–23 are zero.

  - **A056 [ASSUMED]** `kind == CEL_ERROR` (16): payload bytes
    8–11 hold `u32 err` — one of the `CEL_ERR_*` values from §6.
    Bytes 12–23 are zero. No string message is carried on the wire;
    the host pretty-prints from the code.

  - **A057 [RESERVED]** *(Was: separate CEL_LIST_HOST assumption.
    Folded into A049 — `CEL_LIST_HOST` (17) shares the host-backed
    `ref_slot` payload shape with `CEL_MAP_HOST` and `CEL_MESSAGE`.
    ID retained for table stability.)*

## 5. Aggregate headers

### 5.1 ArenaMapHeader

  - **A060 [ASSUMED]** `sizeof(ArenaMapHeader) == 16`.
  - **A061 [ASSUMED]** Layout:
    ```
    offset  size  field
    0       4     u32 count           (number of populated entries)
    4       4     u32 capacity        (allocated slot count)
    8       4     u32 entries_offset  (linear-memory offset to entries run)
    12      4     u32 _pad            (zero)
    ```
  - **A062 [ASSUMED]** `entries_offset == 0` is legal and means "no
    entries allocated yet" (used during construction; final
    `count == 0` may keep `entries_offset == 0` or have an empty
    allocation).

### 5.2 ArenaListHeader

  - **A063 [ASSUMED]** `sizeof(ArenaListHeader) == 16`.
  - **A064 [ASSUMED]** Layout:
    ```
    offset  size  field
    0       4     u32 count            (number of populated elements)
    4       4     u32 capacity         (allocated slot count)
    8       4     u32 elements_offset  (linear-memory offset to elements run)
    12      4     u32 _pad             (zero)
    ```
  - **A065 [ASSUMED]** `elements_offset == 0` legal iff `capacity == 0`.

### 5.3 Entry strides

  - **A066 [ASSUMED]** Map entries: 48 bytes each. Entry `i` lives at
    `entries_offset + i * 48`. Each entry is two back-to-back
    `CelValue`s: `[key:CelValue][value:CelValue]`. Key is at offset
    0 of the entry; value at offset 24.

  - **A067 [ASSUMED]** List elements: 24 bytes each. Element `i`
    lives at `elements_offset + i * 24`. Each element is a single
    `CelValue`. `kCelListEntryStride == sizeof(CelValue)`.

  - **A068 [ASSUMED]** Both strides are wire-stable; tied to
    `sizeof(CelValue)`. Changing `CelValue` size is forbidden
    (A030); strides therefore cannot change either.

## 6. Error code wire values

CEL spec §"Runtime Errors" defines `no_matching_overload` and
`no_such_field` as the only built-ins. Implementation extends this
with arithmetic and indexing errors. All codes are wire-stable.

  - **A070 [ASSUMED]** Error codes (carried in `CelValue.payload.err`
    when `kind == CEL_ERROR`):
    ```
    10   CEL_ERR_OVERFLOW              (arithmetic overflow)
    11   CEL_ERR_DIVIDE_BY_ZERO        (int / int = 0)
    12   CEL_ERR_MODULUS_BY_ZERO       (int % 0)
    13   CEL_ERR_TYPE_MISMATCH         (operand kind not allowed)
    14   CEL_ERR_TYPE_UNSUPPORTED      (M2 envelope: proto MAP/REPEATED)
    15   CEL_ERR_NO_SUCH_KEY           (langdef §"Indexing")
    16   CEL_ERR_DUPLICATE_KEY         (langdef §"Map literals")
    17   CEL_ERR_INDEX_OUT_OF_BOUNDS   (langdef §"Indexing")
    20   CEL_ERR_FIELD_NOT_FOUND       (= no_such_field)
    41   CEL_ERR_HOST_ADAPTER_ERROR    (externref slot stale / wrong gen)
    ```

  - **A071 [ASSUMED]** Codes are append-only; renumbering is
    forbidden. Gaps (18, 19, 21–40, 42…) are reserved for future use.

  - **A072 [ASSUMED]** The host-side `cel::ErrorCode` enum
    (`api/error.h`) MUST mirror these numerically. A `CelValue` decoded
    with `payload.err = N` produces a `cel::ErrorCode` whose underlying
    integer is `N`.

## 7. Mapping to CEL spec value types

Cross-check that L1 covers every type in langdef §"Values":

| CEL spec type      | CelKind                 | Notes                       |
| ------------------ | ----------------------- | --------------------------- |
| `int`              | CEL_INT (2)             | A042                        |
| `uint`             | CEL_UINT (3)            | A043                        |
| `double`           | CEL_DOUBLE (4)          | A044                        |
| `bool`             | CEL_BOOL (1)            | A041                        |
| `string`           | CEL_STRING (5)          | A045                        |
| `bytes`            | CEL_BYTES (6)           | A046                        |
| `list`             | CEL_LIST_ARENA (7) **or** CEL_LIST_HOST (17) | A047, A049; dispatch by origin |
| `map`              | CEL_MAP_ARENA (8) **or** CEL_MAP_HOST (9)    | A048, A049; dispatch by origin |
| `null_type`        | CEL_NULL (0)            | A040                        |
| message names      | CEL_MESSAGE (10)        | A049                        |
| `type`             | CEL_TYPE (11)           | A051                        |
| Duration (abstract) | CEL_DURATION (12)      | A052                        |
| Timestamp (abstract) | CEL_TIMESTAMP (13)    | A053                        |

Implementation extras not in langdef:

| Internal type  | CelKind                 | Purpose                          |
| -------------- | ----------------------- | -------------------------------- |
| optional       | CEL_OPTIONAL (14)       | optional-value extension         |
| unknown        | CEL_UNKNOWN (15)        | partial-eval marker (A055)       |
| error          | CEL_ERROR (16)          | runtime error carrier (A056)     |

  - **A080 [ASSUMED]** Every CEL spec value type listed in langdef
    §"Values" maps to exactly one (or, for list/map, one of two
    origin-tagged) CelKind values. There is no spec value type
    without a CelKind.

  - **A081 [MOVED to L4]** *(Was: arena-backed and host-backed
    list/map kinds are observably equivalent under CEL semantics.
    This is a behavioural claim — it depends on how comparison,
    indexing, and `size()` dispatch — and properly belongs in the
    L4 semantics layer. Tracked as a forward reference; L1 only
    documents that the wire shapes coexist.)*

## 8. Out of scope for L1

The following live in other layers and are intentionally NOT
specified here:

  - The `cel.abi` custom-section proto schema → L2.
  - The list of host-imported and wasm-exported functions and their
    signatures → L3.
  - Behavioural semantics of operations on these values (3VL
    absorption, type-promotion rules, comparison, ordering across
    NaN, comprehension scope) → L4.
  - The two-phase `Engine::Plan` / `Instance::Eval` lifecycle → L4.

---

## Assumption table

| ID | Claim | Source | Validation |
| -- | ----- | ------ | ---------- |
| A001 | LE memory, no byte-swap on host↔wasm | wasm spec; `cel_data.h:162` `#error` | TODO |
| A002 | `CelValue` 8-aligned in linear memory | `layout_pass.cc:271` `static_assert` | TODO |
| A003 | u32 offsets; offset 0 = absent | `cel_arena.h:33`; `cel_runtime.c:124` | TODO |
| A004 | C99 stdint widths | `cel_data.h` typedefs | TODO |
| A010 | bytes [0,8) reserved | `cel_memory.h:9`; `cel_arena_test.cc:18` | TODO |
| A011 | bump cursor at offset 8 | `cel_runtime.c:69` | TODO |
| A012 | limit at offset 12 | `cel_runtime.c:70` | TODO |
| A013 | rodata begins at offset 16 | `cel_memory.h:12-13` | TODO |
| A014 | rodata-end is compile-time const | `cel_arena.h:5-9` | TODO |
| A015 | initial memory = `ceil(mem_size_bytes/65536)` pages, default 2, min 2 | `compile.h:50`; `runtime/BUILD.bazel:191`; `cel_runtime_wasm_test.cc:159` | TODO |
| A020 | `cel_reset` is the only initialiser | `cel_runtime.c:100-104` | TODO |
| A021 | `cel_alloc` 8-aligned, 0 on OOM | `cel_runtime.c:106-116` | TODO |
| A022 | `cel_alloc(0)` returns 8-aligned | `cel_runtime.c:108-109` | TODO |
| A023 | `cel_alloc` zero-fills | `cel_runtime.c:114` | TODO |
| A024 | no individual free | `cel_arena.h:1` | TODO |
| A030 | `sizeof(CelValue) == 24` | `cel_data.h:137` `_Static_assert`; `cel_data_test.cc`; `cel_arena_test.cc:27` | DONE — references existing tests |
| A031 | byte layout 0/4/8 (kind/pad/payload) | `cel_data.h:108-135` | TODO |
| A032 | `_pad` always zero (cel_alloc memset + OpenFrame explicit write) | `cel_runtime.c:114`; `static_memory_builder.cc:60-61` | TODO |
| A033 | payload at offset 8, 16 bytes | `cel_data.h:112` | TODO |
| A040 | CEL_NULL = 0, no payload | `cel_data.h:32`; `cel_runtime.c:132-139` | TODO |
| A041 | CEL_BOOL = 1, i32 payload {0,1} | `cel_data.h:33`; `cel_runtime.c:141-149` | TODO |
| A042 | CEL_INT = 2, i64 payload | `cel_data.h:34`; `cel_runtime.c:151-159` | TODO |
| A043 | CEL_UINT = 3, u64 payload | `cel_data.h:35`; `cel_runtime.c:161-169` | TODO |
| A044 | CEL_DOUBLE = 4, f64 payload | `cel_data.h:36`; `cel_runtime.c:171-179` | TODO |
| A045 | CEL_STRING = 5, CelSpan payload | `cel_data.h:37`; `cel_runtime.c:181-200` | TODO |
| A046 | CEL_BYTES = 6, CelSpan payload | `cel_data.h:38`; `cel_runtime.c:202-205` | TODO |
| A047 | CEL_LIST_ARENA = 7, header_ptr | `cel_data.h:39`; `cel_data_test.cc:54-62` | DONE |
| A048 | CEL_MAP_ARENA = 8, header_ptr | `cel_data.h:40`; `cel_data_test.cc:44-52` | DONE |
| A049 | host-backed kinds (MAP_HOST/MESSAGE/LIST_HOST) share `ref_slot` payload | `cel_data.h:41-42, 49`; `cel_data_test.cc:64-72` | partial — covers MAP_HOST/LIST_HOST shape; needs MESSAGE shape test + cross-kind equivalence |
| A050 | RESERVED — folded into A049 (msg_slot/ref_slot collapse) | n/a | n/a |
| A051 | CEL_TYPE = 11, type_id | `cel_data.h:43` | TODO |
| A052 | CEL_DURATION = 12, CelDurTs | `cel_data.h:44, 102-106` | TODO |
| A053 | CEL_TIMESTAMP = 13, CelDurTs | `cel_data.h:45` | TODO |
| A054 | CEL_OPTIONAL = 14, opt offset | `cel_data.h:46` | TODO |
| A055 | CEL_UNKNOWN = 15, attr id | `cel_data.h:47` | TODO |
| A056 | CEL_ERROR = 16, err code | `cel_data.h:48` | TODO |
| A057 | RESERVED — folded into A049 | n/a | n/a |
| A060 | `sizeof(ArenaMapHeader) == 16` | `cel_data.h:76` `_Static_assert`; `cel_data_test.cc:29` | DONE |
| A061 | ArenaMapHeader field offsets | `cel_data.h:69-74`; `cel_data_test.cc:30-32` | DONE |
| A062 | `entries_offset == 0` legal | `cel_runtime.c:350` | TODO |
| A063 | `sizeof(ArenaListHeader) == 16` | `cel_data.h:95`; `cel_data_test.cc:37` | DONE |
| A064 | ArenaListHeader field offsets | `cel_data.h:88-93`; `cel_data_test.cc:38-40` | DONE |
| A065 | `elements_offset == 0` iff `capacity == 0` | `cel_runtime.c:511-518` | TODO |
| A066 | map stride = 48; `[key][value]` | `cel_data.h:142-144`; `cel_data_test.cc:33` | DONE |
| A067 | list stride = 24 = `sizeof(CelValue)` | `cel_data.h:149-154` `_Static_assert`; `cel_data_test.cc:41` | DONE |
| A068 | strides tied to sizeof(CelValue) | `cel_data.h:153` `_Static_assert` | DONE |
| A070 | error code wire values | `cel_data.h:172-209` | partial — `cel_data_test.cc:23-25` covers 3 of 10 |
| A071 | codes are append-only | doc claim | not testable as code; repo policy |
| A072 | `cel::ErrorCode` mirrors `CEL_ERR_*` | `api/error.h:24-51` | TODO — needs cross-enum equality test |
| A080 | every langdef value type maps to a CelKind | langdef §"Values" + §7 above | TODO — coverage test |
| A081 | MOVED to L4 — arena/host kinds are CEL-equivalent (behavioral) | `map-list-dispatch.md`, langdef §"Equality" | tracked in L4 |

**Status counts:** DONE 9, partial 3, TODO 33, RESERVED 2, MOVED 1 (of 47 active L1 assumptions; A050/A057 retained as table-stable RESERVED placeholders, A081 moved to L4).

The next pass writes / extends `compiler_v2/abi/spec_test.cc` to
turn each TODO into a concrete test, embedding the assumption ID in
the test name (`TEST(AbiL1, ABI_A011_ArenaCursorAtOffset8)`).
