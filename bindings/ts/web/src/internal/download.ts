// Save a compiled Program's wasm bytes to a local file — the literal
// portable artifact the demo's thesis is about.  A Blob URL + a synthetic
// anchor click is the standard browser save path; the URL is revoked
// after the click so it does not leak.

/** Trigger a download of `bytes` as `filename` (default `program.wasm`). */
export function downloadWasm(
  bytes: Uint8Array,
  filename = 'program.wasm',
): void {
  const copy = bytes.slice();
  const blob = new Blob([copy], { type: 'application/wasm' });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.download = filename;
  document.body.append(anchor);
  anchor.click();
  anchor.remove();
  URL.revokeObjectURL(url);
}
