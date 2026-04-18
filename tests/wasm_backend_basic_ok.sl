// Verifies the Wasm backend accepts basic.
fn add1(x i32) i32 {
	return x + 1
}

fn main() i32 {
	return add1(41)
}
