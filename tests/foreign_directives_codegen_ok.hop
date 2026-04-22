@wasm_import("env", "twice")
fn double(v i32) i32

@c_import("helper")
fn helper(v i32) i32

@c_import("config_ro")
const config_ro i32

@export("mylib_quarter")
pub fn quarter(x i32) i32 {
	return x / 4
}

fn main() {
	assert double(helper(4)) == 8
	_ = config_ro
}
