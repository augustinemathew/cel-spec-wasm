// The ONLY file a custom-fn author writes. Native C++ types throughout.
#include "user_fns.h"
#include <cctype>
#include <numeric>
namespace user {
int64_t AddInts(int64_t a, int64_t b) {
  return a + b;
}
std::string Shout(std::string_view s) {
  std::string r(s);
  for (char& c : r)
    c = std::toupper((unsigned char)c);
  return r + "!";
}
int64_t SumList(const std::vector<int64_t>& xs) {
  return std::accumulate(xs.begin(), xs.end(), int64_t{0});
}
int64_t Total(const std::map<std::string, int64_t>& m) {
  int64_t s = 0;
  for (auto& [k, v] : m)
    s += v;
  return s;
}
int64_t GrandTotal(const std::vector<std::vector<int64_t>>& xss) {
  int64_t s = 0;
  for (auto& v : xss)
    s += std::accumulate(v.begin(), v.end(), int64_t{0});
  return s;
}
int64_t SumByKey(const std::map<std::string, std::vector<int64_t>>& m,
                 std::string_view key) {
  auto it = m.find(std::string(key));
  if (it == m.end()) return -1;
  return std::accumulate(it->second.begin(), it->second.end(), int64_t{0});
}
uint64_t ByteLen(const std::vector<uint8_t>& b) {
  return b.size();
}
}  // namespace user
