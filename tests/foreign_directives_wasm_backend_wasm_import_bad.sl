@wasm_import("env", "twice")
fn double(v i32) i32

fn main() i32 {
	return double(4)
}
