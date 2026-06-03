// C++ driver (wasi command): times the custom-fn boundary, typed model.
#include "driver_world.h"
#include <cstdio>
#include <ctime>
#include <cstdint>
static inline uint64_t now_ns(){ timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000000000ull + (uint64_t)t.tv_nsec; }
volatile uint64_t sink=0;
int main(){
  const long N=300000;
  driver_world_string_t empty{nullptr,0};
  printf("%-36s %9s\n","operation","ns/call");

  // A: invoke_prim, 3 ints by value (recommended design) — ~1 boundary call
  cel_fn_types_primitive_t pr[3];
  for(int k=0;k<3;k++){ pr[k].tag=CEL_FN_TYPES_PRIMITIVE_INT; pr[k].val.int_=k+1; }
  cel_fn_custom_fn_list_primitive_t la{pr,3};
  { cel_fn_custom_fn_primitive_t o; uint64_t t0=now_ns();
    for(long i=0;i<N;i++){ cel_fn_custom_fn_invoke_prim(&empty,&la,&o); sink+=o.val.int_; }
    printf("%-36s %9.1f\n","invoke_prim(3 ints, by value)",(double)(now_ns()-t0)/N); }

  // B: pull one value via as_primitive (1 boundary call) — the PULL cost
  cel_fn_types_primitive_t one{}; one.tag=CEL_FN_TYPES_PRIMITIVE_INT; one.val.int_=42;
  cel_fn_types_own_value_t h = cel_fn_types_static_value_of_primitive(&one);
  { cel_fn_types_primitive_t g; uint64_t t0=now_ns();
    for(long i=0;i<N;i++){ cel_fn_types_method_value_as_primitive(cel_fn_types_borrow_value(h),&g); sink+=g.val.int_; }
    printf("%-36s %9.1f\n","as_primitive (pull 1 value)",(double)(now_ns()-t0)/N); }

  // C: construct + drop one handle
  { uint64_t t0=now_ns();
    for(long i=0;i<N;i++){ auto g=cel_fn_types_static_value_of_primitive(&one); cel_fn_types_value_drop_own(g); }
    printf("%-36s %9.1f\n","of_primitive + drop (1 handle)",(double)(now_ns()-t0)/N); }

  // D: full typed call with HANDLE args: build 3 handles + invoke
  { uint64_t t0=now_ns();
    for(long i=0;i<N;i++){
      cel_fn_types_own_value_t hs[3];
      for(int k=0;k<3;k++){ cel_fn_types_primitive_t p{}; p.tag=CEL_FN_TYPES_PRIMITIVE_INT; p.val.int_=k+1;
        hs[k]=cel_fn_types_static_value_of_primitive(&p); }
      cel_fn_custom_fn_list_own_value_t lo{hs,3};
      cel_fn_custom_fn_primitive_t o; cel_fn_custom_fn_invoke(&empty,&lo,&o); sink+=o.val.int_;
    }
    printf("%-36s %9.1f\n","invoke(3 handles): 3x build + call",(double)(now_ns()-t0)/N); }

  cel_fn_types_value_drop_own(h);
  printf("(sink=%llu)\n",(unsigned long long)sink);
  return 0;
}
