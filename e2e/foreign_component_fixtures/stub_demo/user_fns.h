#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>
namespace user {
int64_t  AddInts(int64_t a, int64_t b);
std::string Shout(std::string_view s);
int64_t  SumList(const std::vector<int64_t>& xs);
int64_t  Total(const std::map<std::string, int64_t>& m);
int64_t  GrandTotal(const std::vector<std::vector<int64_t>>& xss);
int64_t  SumByKey(const std::map<std::string, std::vector<int64_t>>& m, std::string_view key);
uint64_t ByteLen(const std::vector<uint8_t>& b);
}  // namespace user
