// Probe: manual Rust authoring of a cel-wasm plugin.
//
// The WIT contract (wit/fns.wit) is the one `cel generate` emits
// from rustadd.idl; wit-bindgen generates the canonical-ABI glue and
// the `Guest` trait below.  Export names are the kebab-cased CEL
// overload-ids (`add_int_int` -> `add-int-int`), which wit-bindgen
// maps back to snake_case Rust method names.
wit_bindgen::generate!({ world: "customfn", path: "wit" });

struct RustAdd;

impl exports::cel::rustadd::fns::Guest for RustAdd {
    fn add_int_int(a: i64, b: i64) -> i64 {
        a + b
    }
}

export!(RustAdd);
