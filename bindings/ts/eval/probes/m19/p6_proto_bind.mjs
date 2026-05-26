// M19 Probe P-6 — a real protobuf-es message bound in activation, read
// via DESCRIPTOR REFLECTION (not obj[name]).
//
// This is the genuine proto path P-9 stopped short of: the externref
// backing holds a protobuf-es `Message` + its `DescMessage`, and
// cel_get_field resolves cel.abi.fields[id].field_number against the
// descriptor and reads the value via `reflect(desc, msg).get(field)`.
// Everything else (externref table, slot wiring, runtime) is identical
// to P-9 — only the backing's field accessor changes, proving the
// trampoline harness is backing-agnostic.
//
// Byte-parity check: the proto-backed read of `u.name` must produce the
// SAME result value as P-9's object-backed read ("Ann"), confirming the
// descriptor-reflection backing is interchangeable with the object one.
// (Full byte-diff vs the C++ cel_host.cel_get_field out_slot is the
// Slice-C follow-up; the runtime wasm here is the same C++ build.)
//
// Deps: @bufbuild/protobuf (throwaway — node_modules deleted at closeout).
// Descriptor: customer.fds.bin (bazel e2e_fixture descriptor set).
//
// Run:
//   npm install   # in this dir, once
//   node compiler_v2/api/eval/ts/probes/m19/p6_proto_bind.mjs \
//        bazel-bin/compiler_v2/runtime/cel_runtime_wasm.wasm

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { createFileRegistry, fromBinary, create } from "@bufbuild/protobuf";
import { FileDescriptorSetSchema } from "@bufbuild/protobuf/wkt";
import { reflect } from "@bufbuild/protobuf/reflect";

const HERE = dirname(fileURLToPath(import.meta.url));
const runtimePath = process.argv[2] ?? "bazel-bin/compiler_v2/runtime/cel_runtime_wasm.wasm";
const LE = true;
const CEL_INT = 2, CEL_STRING = 5, CEL_MESSAGE = 10;
const CELWASM_ARENA_CAPACITY_BYTES = 64 * 1024;

// ── cel.abi decode (vars + fields), trimmed from P-5/P-9 ─────────────
function customSection(b, want) {
  let p = 8; const v = () => { let s = 0, r = 0; for (;;) { const x = b[p++]; r |= (x & 0x7f) << s; if (!(x & 0x80)) break; s += 7; } return r >>> 0; };
  while (p < b.length) { const id = b[p++], sz = v(), end = p + sz; if (id === 0) { const nl = v(); if (new TextDecoder().decode(b.subarray(p, p + nl)) === want) return b.subarray(p + nl, end); } p = end; }
  return null;
}
function reader(buf) { let p = 0; const vi = () => { let s = 0n, r = 0n; for (;;) { const x = BigInt(buf[p++]); r |= (x & 0x7fn) << s; if (!(x & 0x80n)) break; s += 7n; } return r; }; return { end: () => p >= buf.length, tag: () => { const t = vi(); return { f: Number(t >> 3n), w: Number(t & 7n) }; }, u32: () => Number(vi()), bytes: () => { const n = Number(vi()); const o = buf.subarray(p, p + n); p += n; return o; } }; }
function decodeCelAbi(payload) {
  const r = reader(payload), vars = new Map(), fields = new Map();
  while (!r.end()) { const { f, w } = r.tag();
    if (f === 2 && w === 2) { const s = reader(r.bytes()), e = { name: "", slotOffset: 0 }; while (!s.end()) { const t = s.tag(); if (t.f === 1) e.name = new TextDecoder().decode(s.bytes()); else if (t.f === 3) e.slotOffset = s.u32(); else if (t.w === 2) s.bytes(); else s.u32(); } vars.set(e.name, e); }
    else if (f === 3 && w === 2) { const s = reader(r.bytes()), e = { id: 0, fieldNumber: 0, name: "" }; while (!s.end()) { const t = s.tag(); if (t.f === 1) e.id = s.u32(); else if (t.f === 2) e.fieldNumber = s.u32(); else if (t.f === 3) e.name = new TextDecoder().decode(s.bytes()); else if (t.w === 2) s.bytes(); else s.u32(); } fields.set(e.id, e); }
    else if (w === 2) r.bytes(); else r.u32(); }
  return { vars, fields };
}

const trapNs = (mod, ov = {}) => new Proxy(ov, { get: (t, p) => p in t ? t[p] : (...a) => { throw new Error(`stub ${mod}.${String(p)}`); }, has: () => true });

async function main() {
  // Build the protobuf-es Customer from the bazel descriptor set.
  const fds = fromBinary(FileDescriptorSetSchema, readFileSync(join(HERE, "customer.fds.bin")));
  const registry = createFileRegistry(fds);
  const Customer = registry.getMessage("celwasm.testdata.Customer");
  if (!Customer) throw new Error("Customer descriptor not found in FDS");
  const customer = create(Customer, { name: "Ann", age: 20 });

  // Proto backing: read a field by cel.abi field_number via reflection.
  const ctx = { mem: null, rx: null, abi: null, backing: null };
  const dv = () => new DataView(ctx.mem.buffer);
  function readProtoField(backing, fieldNumber) {
    const field = backing.desc.fields.find((f) => f.number === fieldNumber);
    if (!field) throw new Error(`no field #${fieldNumber} on ${backing.desc.typeName}`);
    return reflect(backing.desc, backing.msg).get(field); // string|number|bigint|...
  }
  function writeResult(off, val) {
    if (typeof val === "string") {
      const enc = new TextEncoder().encode(val);
      const ptr = ctx.rx.arena_alloc(Math.max(enc.length, 1));
      new Uint8Array(ctx.mem.buffer, ptr, enc.length).set(enc);
      dv().setUint32(off, CEL_STRING, LE); dv().setUint32(off + 4, 0, LE);
      dv().setUint32(off + 8, ptr, LE); dv().setUint32(off + 12, enc.length, LE);
    } else {
      dv().setUint32(off, CEL_INT, LE); dv().setUint32(off + 4, 0, LE);
      dv().setBigInt64(off + 8, BigInt(val), LE);
    }
  }
  const host = {
    cel_get_field: (out, msgSlot, fieldRefId) => {
      const ref = dv().getUint32(msgSlot + 8, LE);   // externref idx
      const backing = ctx.backing[ref];
      const fnum = ctx.abi.fields.get(fieldRefId).fieldNumber;
      writeResult(out, readProtoField(backing, fnum));
    },
  };

  // Instantiate runtime + expr (same wiring as P-4/P-9).
  const runtimeBytes = readFileSync(runtimePath);
  const exprBytes = readFileSync(join(HERE, "field.wasm"));
  ctx.abi = decodeCelAbi(customSection(exprBytes, "cel.abi"));
  const runtime = (await WebAssembly.instantiate(runtimeBytes, {
    cel_env: { cel_log: () => {} },
    cel_host: trapNs("cel_host", host),
    wasi_snapshot_preview1: trapNs("wasi", { proc_exit: () => {}, random_get: (p, n) => { crypto.getRandomValues(new Uint8Array(ctx.mem.buffer, p, n)); return 0; } }),
  })).instance;
  ctx.rx = runtime.exports; ctx.mem = ctx.rx.memory;
  ctx.rx.arena_init(CELWASM_ARENA_CAPACITY_BYTES);
  const celNs = { memory: ctx.mem };
  for (const [k, val] of Object.entries(ctx.rx)) if (typeof val === "function") celNs[k] = val;
  const expr = (await WebAssembly.instantiate(exprBytes, { cel: celNs, cel_host: trapNs("cel_host", host) })).instance;

  // Bind the proto message at u's slot as CEL_MESSAGE -> externref idx 1.
  ctx.backing = [null, { desc: Customer, msg: customer }];
  const slot = ctx.abi.vars.get("u").slotOffset;
  dv().setUint32(slot, CEL_MESSAGE, LE); dv().setUint32(slot + 4, 0, LE); dv().setUint32(slot + 8, 1, LE);

  const off = expr.exports.eval();
  const k = dv().getUint32(off, LE);
  const got = k === CEL_STRING ? new TextDecoder().decode(new Uint8Array(ctx.mem.buffer, dv().getUint32(off + 8, LE), dv().getUint32(off + 12, LE))) : `kind${k}`;
  const pass = k === CEL_STRING && got === "Ann";
  console.log(`  protobuf-es Customer{name:"Ann"} bound; u.name read via descriptor reflection (field #1) -> ${JSON.stringify(got)}`);
  console.log(pass ? "P-6 PASS ✅  real protobuf-es message bound in activation, field read via reflection == object-backed result"
                   : "P-6 FAILED ❌  expected CEL_STRING \"Ann\"");
  process.exit(pass ? 0 : 1);
}
main().catch((e) => { console.error("P-6 ERROR ❌:", e); process.exit(1); });
