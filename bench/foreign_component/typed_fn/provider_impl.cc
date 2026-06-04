// C++ provider: owns the value resource + implements the two invoke variants.
#include "provider_world.h"
#include <cstdlib>
// I define the resource representation (one CEL value = an int here).
struct exports_cel_fn_types_value_t { int64_t i; };

exports_cel_fn_types_own_value_t
exports_cel_fn_types_static_value_of_primitive(exports_cel_fn_types_primitive_t *p){
  exports_cel_fn_types_value_t* rep = (exports_cel_fn_types_value_t*)malloc(sizeof(exports_cel_fn_types_value_t));
  rep->i = (p->tag==EXPORTS_CEL_FN_TYPES_PRIMITIVE_INT)? p->val.int_ : 0;
  return exports_cel_fn_types_value_new(rep);
}
void exports_cel_fn_types_method_value_as_primitive(
    exports_cel_fn_types_borrow_value_t self, exports_cel_fn_types_primitive_t *ret){
  ret->tag = EXPORTS_CEL_FN_TYPES_PRIMITIVE_INT;
  ret->val.int_ = self->i;                 // borrow == rep pointer
}
void exports_cel_fn_types_value_destructor(exports_cel_fn_types_value_t *rep){ free(rep); }

void exports_cel_fn_custom_fn_invoke(provider_world_string_t *name,
    exports_cel_fn_custom_fn_list_own_value_t *args, exports_cel_fn_custom_fn_primitive_t *ret){
  int64_t s=0;
  for(size_t k=0;k<args->len;k++){ auto h=args->ptr[k];
    s += exports_cel_fn_types_value_rep(h)->i; exports_cel_fn_types_value_drop_own(h); }
  if(args->ptr) free(args->ptr); if(name->len) free(name->ptr);
  ret->tag=EXPORTS_CEL_FN_TYPES_PRIMITIVE_INT; ret->val.int_=s;
}
void exports_cel_fn_custom_fn_invoke_prim(provider_world_string_t *name,
    exports_cel_fn_custom_fn_list_primitive_t *args, exports_cel_fn_custom_fn_primitive_t *ret){
  int64_t s=0;
  for(size_t k=0;k<args->len;k++) if(args->ptr[k].tag==EXPORTS_CEL_FN_TYPES_PRIMITIVE_INT) s+=args->ptr[k].val.int_;
  if(args->ptr) free(args->ptr); if(name->len) free(name->ptr);
  ret->tag=EXPORTS_CEL_FN_TYPES_PRIMITIVE_INT; ret->val.int_=s;
}
