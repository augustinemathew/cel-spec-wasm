// Guest-side `getentropy` for cel wasm plugins.
//
// WHY THIS EXISTS.  libc++ seeds its hash machinery lazily, on the
// first hashed container / string operation.  wasi-libc implements
// that seed by calling `getentropy`, which bottoms out in the
// `wasi:random/random@0.2.0::get-random-bytes` component IMPORT.
// When the lazy init happens to fire while the guest is inside a
// canonical-ABI lift or lower — which is exactly when an aggregate
// (`list<T>`, `bytes`, `map<K,V>`) argument or return is being
// marshalled — wasmtime refuses the import call and the whole
// evaluation dies with `wasm trap: cannot leave component instance`.
// Aggregate-carrying plugin fns therefore trapped, and (worse) the
// map carrier trapped only on some builds, because whether the seed
// was already initialised depended on what else had allocated first.
//
// Defining `getentropy` here preempts wasi-libc's copy: wasi-libc
// ships it in a static archive, so an archive member is only pulled
// in when the symbol is still undefined at link time.  With this
// strong definition present, libc never reaches for the import, so
// there is no import call left to forbid.
//
// DETERMINISM IS NOT A REGRESSION.  The host already answered this
// import with fixed bytes (`RandomGetBytesStub` in eval/engine.cc) —
// the wasmtime C API exposes no per-store WASI context to satisfy a
// real preview2 random impl.  This moves the identical deterministic
// answer into the guest, where it cannot trap.  The protection real
// entropy buys libc++ is against adversarial hash flooding; a plugin
// is embedder-supplied code running against embedder-supplied input
// in-process, so that threat model does not apply.  A plugin that
// genuinely needs randomness should take it as a function argument
// rather than reaching for the ambient RNG.

#include <stddef.h>
#include <stdint.h>

// Fill `length` bytes at `buffer` with the deterministic pattern the
// host stub used: non-zero and non-constant, which libc++'s seed
// handling wants, without pulling in a real RNG.
static void FillDeterministic(unsigned char* out, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    out[i] = (unsigned char)(((i * 0xA5u) + 0x5Au) & 0xFFu);
  }
}

// wasi-libc reaches the RNG through this symbol, which it otherwise
// declares as an `import_module("wasi_snapshot_preview1")` /
// `import_name("random_get")` function.  Defining it here means the
// linker resolves the reference to our code instead of emitting the
// import, so no import call survives to be forbidden mid-ABI.
// Signature is the preview1 ABI's: (buf_ptr, buf_len) -> errno.  The
// pointer arrives as an i32 wasm address, so the int-to-pointer cast
// is the ABI, not an optimization hazard; likewise the symbol must
// keep external linkage for the linker to bind libc's reference to
// it, which is the whole point.
// NOLINTNEXTLINE(misc-use-internal-linkage)
int32_t __imported_wasi_snapshot_preview1_random_get(int32_t buf,
                                                     int32_t buf_len);
// NOLINTNEXTLINE(misc-use-internal-linkage)
int32_t __imported_wasi_snapshot_preview1_random_get(int32_t buf,
                                                     int32_t buf_len) {
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  unsigned char* out = (unsigned char*)(uintptr_t)buf;
  FillDeterministic(out, (size_t)buf_len);
  return 0;  // __WASI_ERRNO_SUCCESS
}
