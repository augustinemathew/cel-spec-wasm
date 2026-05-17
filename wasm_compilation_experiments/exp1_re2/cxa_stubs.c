// Stubs for the C++ exception ABI symbols.  wasi-sdk's libc++ is
// built without exception support; RE2 has rare throw paths
// (out_of_range etc.) that we don't hit in practice for our usage.
// Stubs trap instead of unwinding — fine for a prototype.

#include <stddef.h>

void* __cxa_allocate_exception(size_t n) { (void)n; __builtin_trap(); return 0; }
void __cxa_free_exception(void* p) { (void)p; __builtin_trap(); }
_Noreturn void __cxa_throw(void* p, void* t, void (*d)(void*)) {
  (void)p; (void)t; (void)d; __builtin_trap();
}
void __cxa_begin_catch(void* p) { (void)p; __builtin_trap(); }
void __cxa_end_catch(void) { __builtin_trap(); }
void __cxa_rethrow(void) { __builtin_trap(); }
void* __cxa_current_exception_type(void) { return 0; }
void* __cxa_get_exception_ptr(void* p) { return p; }
void _Unwind_Resume(void* p) { (void)p; __builtin_trap(); }

// typeinfo / RTTI placeholders — RE2 was built with -fno-rtti so
// these may not actually appear, but defensive coverage.
__attribute__((weak)) void* __dynamic_cast(const void* sub,
                                            const void* src_type,
                                            const void* dst_type,
                                            long src2dst_offset) {
  (void)sub; (void)src_type; (void)dst_type; (void)src2dst_offset;
  return 0;
}

// libc++abi's vtable for the std::exception class hierarchy lives
// somewhere; if any unresolved typeinfo shows up, we trap.
__attribute__((weak)) const void* _ZTVN10__cxxabiv120__si_class_type_infoE = 0;
__attribute__((weak)) const void* _ZTVN10__cxxabiv117__class_type_infoE = 0;
__attribute__((weak)) const void* _ZTVN10__cxxabiv121__vmi_class_type_infoE = 0;
