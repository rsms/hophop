@wasm_import("env", "twice")
fn twice(x i64) i64

fn main() i64 {
	return twice(1)
}
