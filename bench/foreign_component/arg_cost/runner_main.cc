#include "runner_world.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>
static inline uint64_t now_ns(){ timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
  return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec; }
volatile uint64_t g_sink = 0;
static char buf[512];
int main(){
  const long N = 1000000;
  memset(buf,'x',sizeof(buf));
  runner_world_string_t s1{(uint8_t*)buf,23}, s2{(uint8_t*)buf,31},
                        s3{(uint8_t*)buf,44}, s4{(uint8_t*)buf,37}, s5{(uint8_t*)buf,29};
  runner_world_list_u8_t by{(uint8_t*)buf,35};
  uint64_t acc=0; for(int i=0;i<20000;i++) acc+=bench_abi_engine_noop(); g_sink+=acc;
  #define BENCH(label, expr) do{ uint64_t t0=now_ns(); uint64_t a=0; \
    for(long i=0;i<N;i++){ a+=(uint64_t)(expr); } uint64_t t1=now_ns(); g_sink+=a; \
    printf("%-18s %9.1f\n", label, (double)(t1-t0)/(double)N); }while(0)

  printf("== per arg count (strings 23-44 bytes) ==\n%-18s %9s\n","shape","ns/call");
  BENCH("noop",            bench_abi_engine_noop());
  BENCH("i64 x3",          bench_abi_engine_i64_3((int64_t)i,(int64_t)i,(int64_t)i));
  BENCH("i64 x5",          bench_abi_engine_i64_5((int64_t)i,(int64_t)i,(int64_t)i,(int64_t)i,(int64_t)i));
  BENCH("str x1",          bench_abi_engine_str_1(&s1));
  BENCH("str x3",          bench_abi_engine_str_3(&s1,&s2,&s3));
  BENCH("str x5",          bench_abi_engine_str_5(&s1,&s2,&s3,&s4,&s5));
  BENCH("bytes x1",        bench_abi_engine_bytes_1(&by));
  { uint64_t t0=now_ns(); uint64_t a=0; runner_world_string_t ret;
    for(long i=0;i<N;i++){ bench_abi_engine_str_3_ret(&s1,&s2,&s3,&ret); a+=ret.len; runner_world_string_free(&ret); }
    uint64_t t1=now_ns(); g_sink+=a; printf("%-18s %9.1f\n","str x3 ->str",(double)(t1-t0)/(double)N); }

  printf("\n== str x3, all args length L (per-call) ==\n%-18s %9s\n","length L","ns/call");
  size_t lens[] = {5,20,35,50,100,200};
  for(size_t k=0;k<6;k++){ size_t L=lens[k];
    runner_world_string_t a{(uint8_t*)buf,L},b{(uint8_t*)buf,L},c{(uint8_t*)buf,L};
    uint64_t t0=now_ns(); uint64_t acc2=0;
    for(long i=0;i<N;i++){ acc2+=bench_abi_engine_str_3(&a,&b,&c); }
    uint64_t t1=now_ns(); g_sink+=acc2;
    char lab[32]; snprintf(lab,sizeof lab,"L=%zu (x3)",L);
    printf("%-18s %9.1f\n",lab,(double)(t1-t0)/(double)N);
  }
  printf("(sink=%llu)\n",(unsigned long long)g_sink);
  return 0;
}
