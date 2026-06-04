// C++ engine: trivial bodies so we measure the ABI boundary, not work.
#include "engine_world.h"

uint32_t exports_bench_abi_engine_noop(void) { return 1; }
int64_t  exports_bench_abi_engine_i64_3(int64_t a,int64_t b,int64_t c){return a+b+c;}
int64_t  exports_bench_abi_engine_i64_5(int64_t a,int64_t b,int64_t c,int64_t d,int64_t e){return a+b+c+d+e;}
uint32_t exports_bench_abi_engine_str_1(engine_world_string_t*a){return (uint32_t)a->len;}
uint32_t exports_bench_abi_engine_str_3(engine_world_string_t*a,engine_world_string_t*b,engine_world_string_t*c){return (uint32_t)(a->len+b->len+c->len);}
uint32_t exports_bench_abi_engine_str_5(engine_world_string_t*a,engine_world_string_t*b,engine_world_string_t*c,engine_world_string_t*d,engine_world_string_t*e){return (uint32_t)(a->len+b->len+c->len+d->len+e->len);}
uint32_t exports_bench_abi_engine_bytes_1(engine_world_list_u8_t*a){return (uint32_t)a->len;}
void exports_bench_abi_engine_str_3_ret(engine_world_string_t*a,engine_world_string_t*b,engine_world_string_t*c,engine_world_string_t*ret){
  (void)b;(void)c;
  engine_world_string_dup_n(ret,(const char*)a->ptr,a->len); // result lives in engine memory; ABI lifts it back
}
