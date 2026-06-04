// GENERATED — implements the wit exports by routing through the codec
// into the author's native-typed functions. Author never edits this.
#include "author.h"
#include "codec.h"
#include "user_fns.h"
int64_t exports_cel_customfn_fns_add_ints(int64_t a, int64_t b) {
  return user::AddInts(a, b);
}
void exports_cel_customfn_fns_shout(author_string_t* s, author_string_t* ret) {
  codec::lower(ret, user::Shout(codec::lift(*s)));
}
int64_t exports_cel_customfn_fns_sum_list(author_list_s64_t* xs) {
  return user::SumList(codec::lift(*xs));
}
int64_t exports_cel_customfn_fns_total(author_list_tuple2_string_s64_t* m) {
  return user::Total(codec::lift(*m));
}
int64_t exports_cel_customfn_fns_grand_total(author_list_list_s64_t* xss) {
  return user::GrandTotal(codec::lift(*xss));
}
int64_t exports_cel_customfn_fns_sum_by_key(
    author_list_tuple2_string_list_s64_t* m, author_string_t* key) {
  return user::SumByKey(codec::lift(*m), codec::lift(*key));
}
uint64_t exports_cel_customfn_fns_byte_len(author_list_u8_t* b) {
  return user::ByteLen(codec::lift(*b));
}
