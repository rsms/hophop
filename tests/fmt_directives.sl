@wasm_import("env", "twice")

fn double(v i32) i32

@c_import("helper")

fn helper(v i32) i32

@export("quarter")

pub fn quarter(x i32) i32 {
	return x / 4
}
