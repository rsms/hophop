// Verifies the Wasm backend accepts function local.
fn add1(x i32) i32 {
	return x + 1
}

fn add2(x i32) i32 {
	return x + 2
}

fn main() i32 {
	var f = add1
	f = add2
	return f(40)
}
