// Author-side impls for the proto-bearing demo fixture
// (`demo_plugin_proto`).  See the matching fns_proto.idl.

#include "user_fns.h"

#include <cctype>
#include <string>

#include "e2e/plugin_fixtures/cel_wasm_plugin_demo/user.pb.h"

namespace customfn {

bool IsAdult(const acme::User& u) {
  return u.age() >= 18;
}

acme::User Capitalize(const acme::User& u) {
  acme::User r = u;
  std::string name = r.name();
  if (!name.empty()) {
    name[0] = static_cast<char>(
        std::toupper(static_cast<unsigned char>(name[0])));
  }
  r.set_name(name);
  return r;
}

}  // namespace customfn
