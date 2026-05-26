// M19 Probe P-9 — host-backed aggregates bound in activation.
//
// Validates the WHOLE host-aggregate plumbing the TS host needs for
// Slice C, with ZERO npm deps, using plain JS backings:
//   - object  → struct/JSON message backing  (u.name via cel_get_field)
//   - array   → list backing                 (xs[1] / xs.size())
//   - Map     → map backing                  (m["k"] via cel_map_lookup)
//
// This is exactly the C++ "custom HostMessageBacking / HostMapBacking /
// HostListBacking" path (cel_host.h) — the embedder supplies non-proto
// data (JSON, struct-of-structs) and the trampolines read it. The
// proto-SHAPED field read (u.name) works here against a JS object
// because cel.abi.fields[] carries the field NAME; the protobuf-es
// reflection variant (reading by descriptor/field_number) is P-10 and
// reuses these exact trampolines.
//
// Exercises: externref table (3 independent namespaces) + cel_get_field
//   + cel_list_at + cel_list_size + cel_map_lookup + cel.abi
//   {variables,fields} decode + arena-allocated string results.
// Mirrors cel_host.cc CelGetFieldImpl / CelListAtImpl / CelListSizeImpl
//   / CelMapLookupImpl and instance.cc EncodeMessage/List/Map.
//
// Run:
//   node compiler_v2/api/eval/ts/probes/m19/p9_host_aggregates.mjs \
//        bazel-bin/compiler_v2/runtime/cel_runtime_wasm.wasm
//
// Throwaway probe — delete at M19 closeout; graduates into
// host/*.test.ts + externref.test.ts + abi.test.ts.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const runtimePath =
  process.argv[2] ?? "bazel-bin/compiler_v2/runtime/cel_runtime_wasm.wasm";

const LE = true;
// CelKind (cel_data.h:31)
const CEL_INT = 2, CEL_STRING = 5, CEL_MAP_HOST = 9, CEL_MESSAGE = 10,
  CEL_LIST_HOST = 17;
const CELWASM_ARENA_CAPACITY_BYTES = 64 * 1024;

// ── cel.abi decode (vars + fields) — → abi.ts ────────────────────────
function customSection(bytes, want) {
  let p = 8;
  const v = () => { let s = 0, r = 0; for (;;) { const b = bytes[p++]; r |= (b & 0x7f) << s; if (!(b & 0x80)) break; s += 7; } return r >>> 0; };
  while (p < bytes.length) {
    const id = bytes[p++], size = v(), end = p + size;
    if (id === 0) {
      const nl = v();
      if (new TextDecoder().decode(bytes.subarray(p, p + nl)) === want) return bytes.subarray(p + nl, end);
    }
    p = end;
  }
  return null;
}
function pbReader(buf) {
  let p = 0;
  const varint = () => { let s = 0n, r = 0n; for (;;) { const b = BigInt(buf[p++]); r |= (b & 0x7fn) << s; if (!(b & 0x80n)) break; s += 7n; } return r; };
  return {
    atEnd: () => p >= buf.length,
    tag: () => { const t = varint(); return { field: Number(t >> 3n), wire: Number(t & 7n) }; },
    u32: () => Number(varint()),
    bytes: () => { const n = Number(varint()); const o = buf.subarray(p, p + n); p += n; return o; },
  };
}
// CelAbi: f2 repeated VariableEntry{f1 name,f2 local_index,f3 slot_offset,f4 repr};
//         f3 repeated FieldEntry{f1 id,f2 field_number,f3 name,f4 owner_fqn}.
function decodeCelAbi(payload) {
  const r = pbReader(payload);
  const vars = new Map(), fields = new Map();
  while (!r.atEnd()) {
    const { field, wire } = r.tag();
    if (field === 2 && wire === 2) {
      const s = pbReader(r.bytes()), e = { name: "", slotOffset: 0, repr: 0 };
      while (!s.atEnd()) { const t = s.tag(); if (t.field === 1) e.name = new TextDecoder().decode(s.bytes()); else if (t.field === 3) e.slotOffset = s.u32(); else if (t.field === 4) e.repr = s.u32(); else if (t.wire === 2) s.bytes(); else s.u32(); }
      vars.set(e.name, e);
    } else if (field === 3 && wire === 2) {
      const s = pbReader(r.bytes()), e = { id: 0, fieldNumber: 0, name: "" };
      while (!s.atEnd()) { const t = s.tag(); if (t.field === 1) e.id = s.u32(); else if (t.field === 2) e.fieldNumber = s.u32(); else if (t.field === 3) e.name = new TextDecoder().decode(s.bytes()); else if (t.wire === 2) s.bytes(); else s.u32(); }
      fields.set(e.id, e);
    } else if (wire === 2) r.bytes(); else r.u32();
  }
  return { vars, fields };
}

// ── externref table — → externref.ts (3 independent namespaces) ──────
function newExternref() {
  const msg = [null], list = [null], map = [null]; // index 0 = sentinel
  return {
    internMsg: (o) => (msg.push(o), msg.length - 1),
    internList: (o) => (list.push(o), list.length - 1),
    internMap: (o) => (map.push(o), map.length - 1),
    msg, list, map,
  };
}

// ── CelValue read/write helpers ──────────────────────────────────────
const kind = (dv, off) => dv.getUint32(off, LE);
const refSlot = (dv, off) => dv.getUint32(off + 8, LE); // msg_slot / ref_slot
const readInt = (dv, off) => dv.getBigInt64(off + 8, LE);
const readStr = (dv, mem, off) => new TextDecoder().decode(new Uint8Array(mem.buffer, dv.getUint32(off + 8, LE), dv.getUint32(off + 12, LE)));
function writeInt(dv, off, v) { dv.setUint32(off, CEL_INT, LE); dv.setUint32(off + 4, 0, LE); dv.setBigInt64(off + 8, BigInt(v), LE); }
function writeStr(dv, ctx, off, str) {
  const enc = new TextEncoder().encode(str);
  const ptr = ctx.rx.arena_alloc(Math.max(enc.length, 1));
  new Uint8Array(ctx.mem.buffer, ptr, enc.length).set(enc);
  dv.setUint32(off, CEL_STRING, LE); dv.setUint32(off + 4, 0, LE);
  dv.setUint32(off + 8, ptr, LE); dv.setUint32(off + 12, enc.length, LE);
}
// Encode an arbitrary JS value (int|string) as a result CelValue.
function writeValue(dv, ctx, off, v) {
  if (typeof v === "bigint" || typeof v === "number") writeInt(dv, off, v);
  else if (typeof v === "string") writeStr(dv, ctx, off, v);
  else throw new Error(`probe can't encode ${typeof v}`);
}

// ── the cel_host trampolines — → host/*.ts ───────────────────────────
function makeHost(ctx) {
  const dv = () => new DataView(ctx.mem.buffer);
  return {
    // CelGetFieldImpl(out_slot, msg_slot, field_ref_id, attribute_id)
    cel_get_field: (out, msgSlot, fieldRefId) => {
      const obj = ctx.refs.msg[refSlot(dv(), msgSlot)];
      const name = ctx.abi.fields.get(fieldRefId).name;
      writeValue(dv(), ctx, out, obj[name]);
    },
    // CelListAtImpl(out_slot, list_slot, index_slot)
    cel_list_at: (out, listSlot, idxSlot) => {
      const arr = ctx.refs.list[refSlot(dv(), listSlot)];
      writeValue(dv(), ctx, out, arr[Number(readInt(dv(), idxSlot))]);
    },
    // CelListSizeImpl(out_slot, list_slot)
    cel_list_size: (out, listSlot) => {
      writeInt(dv(), out, ctx.refs.list[refSlot(dv(), listSlot)].length);
    },
    // CelMapLookupImpl(out_slot, map_slot, key_slot)
    cel_map_lookup: (out, mapSlot, keySlot) => {
      const m = ctx.refs.map[refSlot(dv(), mapSlot)];
      writeValue(dv(), ctx, out, m.get(readStr(dv(), ctx.mem, keySlot)));
    },
  };
}

function trapNamespace(modName, overrides = {}) {
  return new Proxy(overrides, {
    get: (t, p) => p in t ? t[p] : (...a) => { throw new Error(`UNEXPECTED stub ${modName}.${String(p)}(${a})`); },
    has: () => true,
  });
}

// Instantiate runtime+expr, wiring the REAL cel_host trampolines into
// both (the runtime's kHost dispatchers tail-call cel_host.*).
async function plan(runtimeBytes, exprBytes, ctx) {
  const host = makeHost(ctx);
  const runtime = (await WebAssembly.instantiate(runtimeBytes, {
    cel_env: { cel_log: () => {} },
    cel_host: trapNamespace("cel_host", host),
    wasi_snapshot_preview1: trapNamespace("wasi", {
      proc_exit: () => {},
      random_get: (ptr, len) => { crypto.getRandomValues(new Uint8Array(ctx.mem.buffer, ptr, len)); return 0; },
    }),
  })).instance;
  const rx = runtime.exports;
  ctx.mem = rx.memory; ctx.rx = rx;
  rx.arena_init(CELWASM_ARENA_CAPACITY_BYTES);
  const celNs = { memory: rx.memory };
  for (const [k, val] of Object.entries(rx)) if (typeof val === "function") celNs[k] = val;
  const expr = (await WebAssembly.instantiate(exprBytes, {
    cel: celNs, cel_host: trapNamespace("cel_host", host),
  })).instance;
  return { rx, expr };
}

// Bind a host backing into a variable's workspace slot.
const bindMsg = (dv, slot, idx) => { dv.setUint32(slot, CEL_MESSAGE, LE); dv.setUint32(slot + 4, 0, LE); dv.setUint32(slot + 8, idx, LE); };
const bindList = (dv, slot, idx) => { dv.setUint32(slot, CEL_LIST_HOST, LE); dv.setUint32(slot + 4, 0, LE); dv.setUint32(slot + 8, idx, LE); };
const bindMap = (dv, slot, idx) => { dv.setUint32(slot, CEL_MAP_HOST, LE); dv.setUint32(slot + 4, 0, LE); dv.setUint32(slot + 8, idx, LE); };

async function run(runtimeBytes, file, varName, bindKind, backing, wantKind, want) {
  const exprBytes = readFileSync(join(HERE, file));
  const abi = decodeCelAbi(customSection(exprBytes, "cel.abi"));
  const refs = newExternref();
  const ctx = { abi, refs };
  const { expr } = await plan(runtimeBytes, exprBytes, ctx);
  const dv = new DataView(ctx.mem.buffer);
  const slot = abi.vars.get(varName).slotOffset;
  if (bindKind === "msg") bindMsg(dv, slot, refs.internMsg(backing));
  else if (bindKind === "list") bindList(dv, slot, refs.internList(backing));
  else bindMap(dv, slot, refs.internMap(backing));
  const off = expr.exports.eval();
  const k = kind(dv, off);
  const got = k === CEL_STRING ? readStr(dv, ctx.mem, off) : k === CEL_INT ? readInt(dv, off) : `kind${k}`;
  const pass = k === wantKind && (typeof want === "bigint" ? got === want : got === want);
  console.log(`  ${pass ? "ok " : "FAIL"}  ${file.padEnd(14)} -> ${typeof got === "bigint" ? got : JSON.stringify(got)} (want ${typeof want === "bigint" ? want : JSON.stringify(want)})`);
  return pass;
}

async function main() {
  const rb = readFileSync(runtimePath);
  let ok = true;
  // object backing (JSON/struct): u.name -> "Ann"
  ok = (await run(rb, "field.wasm", "u", "msg", { name: "Ann", age: 20n }, CEL_STRING, "Ann")) && ok;
  // array backing (list): xs[1] -> 20
  ok = (await run(rb, "listidx.wasm", "xs", "list", [10n, 20n, 30n], CEL_INT, 20n)) && ok;
  // array backing: xs.size() -> 3
  ok = (await run(rb, "listsize.wasm", "xs", "list", [10n, 20n, 30n], CEL_INT, 3n)) && ok;
  // Map backing: m["k"] -> 7
  ok = (await run(rb, "maplookup.wasm", "m", "map", new Map([["k", 7n], ["j", 9n]]), CEL_INT, 7n)) && ok;
  console.log(ok ? "P-9 PASS ✅  proto-shaped/JSON object + list + map bound in activation, read via host trampolines" : "P-9 FAILED ❌");
  process.exit(ok ? 0 : 1);
}
main().catch((e) => { console.error("P-9 ERROR ❌:", e); process.exit(1); });
