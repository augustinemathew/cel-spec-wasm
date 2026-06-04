// GENERATED glue — the author never sees this. Lifts wit-bindgen
// {ptr,len}/{f0,f1} structs into native std:: containers and lowers back.
#pragma once
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include "author.h"
namespace codec {
inline std::string_view          lift(const author_string_t& s){ return {(const char*)s.ptr, s.len}; }
inline std::vector<int64_t>      lift(const author_list_s64_t& l){ return {l.ptr, l.ptr + l.len}; }
inline std::vector<uint8_t>      lift(const author_list_u8_t& l){ return {l.ptr, l.ptr + l.len}; }
inline std::map<std::string,int64_t> lift(const author_list_tuple2_string_s64_t& m){
  std::map<std::string,int64_t> r;
  for (size_t i=0;i<m.len;i++) r.emplace(std::string((const char*)m.ptr[i].f0.ptr,m.ptr[i].f0.len), m.ptr[i].f1);
  return r;
}
inline std::vector<std::vector<int64_t>> lift(const author_list_list_s64_t& l){
  std::vector<std::vector<int64_t>> r; r.reserve(l.len);
  for (size_t i=0;i<l.len;i++) r.emplace_back(l.ptr[i].ptr, l.ptr[i].ptr + l.ptr[i].len);
  return r;
}
inline std::map<std::string,std::vector<int64_t>> lift(const author_list_tuple2_string_list_s64_t& m){
  std::map<std::string,std::vector<int64_t>> r;
  for (size_t i=0;i<m.len;i++){ auto& t=m.ptr[i];
    r.emplace(std::string((const char*)t.f0.ptr,t.f0.len), std::vector<int64_t>(t.f1.ptr,t.f1.ptr+t.f1.len)); }
  return r;
}
inline void lower(author_string_t* ret, std::string_view s){ author_string_dup_n(ret, s.data(), s.size()); }
}  // namespace codec
