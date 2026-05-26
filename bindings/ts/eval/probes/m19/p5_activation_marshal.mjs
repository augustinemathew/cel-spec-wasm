// M19 Probe P-5 — activation marshal round-trip.
//
// Proves the host can bind a declared variable into its workspace slot
// before $eval, matching instance.cc::MarshalActivation. Two cases:
//   (a) int    `x + 1`  with x=41  -> 42   (inline slot write)
//   (b) string `s + "!"` with s="hi" -> "hi!"  (malloc'd activation
//       buffer — string bytes can't live in the bump arena, which
//       $eval's arena_reset prologue would wipe; instance.cc:377)
//
// Includes a minimal `cel.abi` custom-section decoder (wasm section
// walk + protobuf parse) — the first real piece of abi.ts. Graduates
// into abi.test.ts + instance.test.ts.
//
// Run:
//   node compiler_v2/api/eval/ts/probes/m19/p5_activation_marshal.mjs \
//        bazel-bin/compiler_v2/runtime/cel_runtime_wasm.wasm
//
// Throwaway probe — delete at M19 closeout.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const runtimePath =
  process.argv[2] ?? "bazel-bin/compiler_v2/runtime/cel_runtime_wasm.wasm";

const LE = true;
// CelKind (cel_data.h:31) — the WIRE kind in a CelValue.
const CEL_INT = 2, CEL_STRING = 5;
// NB: cel.abi `repr` is the ir::Repr ordinal (annotations.h:17), which
// is DISTINCT from CelKind because Repr leads with kUnknown=0:
//   Null=1 Bool=2 Int=3 Uint=4 Double=5 String=6 Bytes=7 List=8 Map=9
//   Message=10 Enum=11 Duration=12 Timestamp=13 Type=14 Optional=15.
// So an int var carries repr=3 (not 2) and a string var repr=6 (not 5).
// abi.ts's repr→encoder table must use THESE values, not CelKind.
const CELWASM_ARENA_CAPACITY_BYTES = 64 * 1024;

// ── cel.abi decode (→ abi.ts) ────────────────────────────────────────

// Walk wasm sections, return the payload bytes of custom section `want`.
function customSection(bytes, want) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let p = 8; // skip magic(4) + version(4)
  const readVaru32 = () => {
    let shift = 0, result = 0;
    for (;;) {
      const b = bytes[p++];
      result |= (b & 0x7f) << shift;
      if ((b & 0x80) === 0) break;
      shift += 7;
    }
    return result >>> 0;
  };
  while (p < bytes.length) {
    const id = bytes[p++];
    const size = readVaru32();
    const end = p + size;
    if (id === 0) {
      const nameLen = readVaru32();
      const name = new TextDecoder().decode(bytes.subarray(p, p + nameLen));
      if (name === want) return bytes.subarray(p + nameLen, end);
    }
    p = end;
  }
  return null;
}

// Minimal protobuf reader over a Uint8Array.
function pbReader(buf) {
  let p = 0;
  const varint = () => {
    let shift = 0n, result = 0n;
    for (;;) {
      const b = BigInt(buf[p++]);
      result |= (b & 0x7fn) << shift;
      if ((b & 0x80n) === 0n) break;
      shift += 7n;
    }
    return result;
  };
  return {
    atEnd: () => p >= buf.length,
    tag: () => {
      const t = varint();
      return { field: Number(t >> 3n), wire: Number(t & 7n) };
    },
    u32: () => Number(varint()),
    bytes: () => {
      const len = Number(varint());
      const out = buf.subarray(p, p + len);
      p += len;
      return out;
    },
  };
}

// Decode CelAbi → { vars: Map<name,{localIndex,slotOffset,repr}>, version }
// CelAbi: field2 repeated VariableEntry; field6 runtime_abi_version.
// VariableEntry: f1 name(str) f2 local_index f3 slot_offset f4 repr.
function decodeCelAbi(payload) {
  const r = pbReader(payload);
  const vars = new Map();
  let runtimeAbiVersion = 0;
  while (!r.atEnd()) {
    const { field, wire } = r.tag();
    if (field === 2 && wire === 2) {
      const sub = pbReader(r.bytes());
      const e = { name: "", localIndex: 0, slotOffset: 0, repr: 0 };
      while (!sub.atEnd()) {
        const t = sub.tag();
        if (t.field === 1 && t.wire === 2) e.name = new TextDecoder().decode(sub.bytes());
        else if (t.field === 2) e.localIndex = sub.u32();
        else if (t.field === 3) e.slotOffset = sub.u32();
        else if (t.field === 4) e.repr = sub.u32();
        else if (t.wire === 2) sub.bytes();
        else sub.u32();
      }
      vars.set(e.name, e);
    } else if (field === 6 && wire === 0) {
      runtimeAbiVersion = r.u32();
    } else if (wire === 2) {
      r.bytes();
    } else {
      r.u32();
    }
  }
  return { vars, runtimeAbiVersion };
}

// ── instantiation (same wiring as P-4; → engine.ts) ──────────────────

function trapNamespace(modName, overrides = {}) {
  return new Proxy(overrides, {
    get: (t, prop) =>
      prop in t
        ? t[prop]
        : (...args) => {
            throw new Error(`UNEXPECTED stub ${modName}.${String(prop)}(${args})`);
          },
    has: () => true,
  });
}

async function plan(runtimeBytes, exprBytes) {
  let mem = null;
  const runtime = (
    await WebAssembly.instantiate(runtimeBytes, {
      cel_env: { cel_log: () => {} },
      cel_host: trapNamespace("cel_host"),
      wasi_snapshot_preview1: trapNamespace("wasi_snapshot_preview1", {
        proc_exit: () => {},
        random_get: (ptr, len) => {
          crypto.getRandomValues(new Uint8Array(mem.buffer, ptr, len));
          return 0;
        },
      }),
    })
  ).instance;
  const rx = runtime.exports;
  mem = rx.memory;
  rx.arena_init(CELWASM_ARENA_CAPACITY_BYTES);

  const celNs = { memory: mem };
  for (const [k, v] of Object.entries(rx)) if (typeof v === "function") celNs[k] = v;
  const expr = (
    await WebAssembly.instantiate(exprBytes, {
      cel: celNs,
      cel_host: trapNamespace("cel_host"),
    })
  ).instance;
  return { rx, mem, expr };
}

function decodeScalarOrString(view, mem, offset) {
  const kind = view.getUint32(offset, LE);
  if (kind === CEL_INT) return { kind, i: view.getBigInt64(offset + 8, LE) };
  if (kind === CEL_STRING) {
    const ptr = view.getUint32(offset + 8, LE);
    const len = view.getUint32(offset + 12, LE);
    const s = new TextDecoder().decode(new Uint8Array(mem.buffer, ptr, len));
    return { kind, s };
  }
  return { kind, raw: "undecoded" };
}

// Write a CEL_INT CelValue at `slot`.
function writeInt(view, slot, v) {
  view.setUint32(slot, CEL_INT, LE);
  view.setUint32(slot + 4, 0, LE);
  view.setBigInt64(slot + 8, v, LE);
}
// Write a CEL_STRING CelValue at `slot`, bytes malloc'd in linear memory
// (mirrors EnsureActivationBuffer — survives arena_reset).
function writeString(view, rx, mem, slot, str) {
  const enc = new TextEncoder().encode(str);
  const off = rx.malloc(Math.max(enc.length, 1));
  new Uint8Array(mem.buffer, off, enc.length).set(enc);
  view.setUint32(slot, CEL_STRING, LE);
  view.setUint32(slot + 4, 0, LE);
  view.setUint32(slot + 8, off, LE);
  view.setUint32(slot + 12, enc.length, LE);
}

async function main() {
  const runtimeBytes = readFileSync(runtimePath);
  let ok = true;

  // ── (a) int: x + 1, x = 41 -> 42 ──
  {
    const exprBytes = readFileSync(join(HERE, "xplus1.wasm"));
    const { mem, expr } = await plan(runtimeBytes, exprBytes);
    const { vars, runtimeAbiVersion } = decodeCelAbi(customSection(exprBytes, "cel.abi"));
    const x = vars.get("x");
    console.log(`  abi: runtime_abi_version=${runtimeAbiVersion} x=${JSON.stringify(x)} (repr 3 == ir::Repr::kInt)`);
    const view = new DataView(mem.buffer);
    writeInt(view, x.slotOffset, 41n);
    const r = decodeScalarOrString(view, mem, expr.exports.eval());
    const pass = r.kind === CEL_INT && r.i === 42n;
    ok &&= pass;
    console.log(`  ${pass ? "ok " : "FAIL"}  int  x+1 -> ${r.i} (want 42)`);
  }

  // ── (b) string: s + "!", s = "hi" -> "hi!" (activation buffer) ──
  {
    const exprBytes = readFileSync(join(HERE, "sconcat.wasm"));
    const { rx, mem, expr } = await plan(runtimeBytes, exprBytes);
    const s = decodeCelAbi(customSection(exprBytes, "cel.abi")).vars.get("s");
    console.log(`  abi: s=${JSON.stringify(s)} (repr 6 == ir::Repr::kString)`);
    const view = new DataView(mem.buffer);
    writeString(view, rx, mem, s.slotOffset, "hi");
    const r = decodeScalarOrString(view, mem, expr.exports.eval());
    const pass = r.kind === CEL_STRING && r.s === "hi!";
    ok &&= pass;
    console.log(`  ${pass ? "ok " : "FAIL"}  string s+"!" -> ${JSON.stringify(r.s)} (want "hi!")`);
  }

  console.log(ok ? "P-5 PASS ✅  activation marshal round-trips (int slot + string buffer)" : "P-5 FAILED ❌");
  process.exit(ok ? 0 : 1);
}

main().catch((e) => {
  console.error("P-5 ERROR ❌:", e);
  process.exit(1);
});
