// Verifies fntype is accepted.
fn add1(x i32) i32 {
	return x + 1
}

fn apply(x i32, f fn(i32) i32) i32 {
	return f(x)
}

struct Wrapper {
	cb fn(i32) i32
}

fn main() {
	var f fn(i32) i32 = add1
	var w Wrapper
	w.cb = f
	assert apply(41, f: w.cb) == 42
	assert w.cb(1) == 2
}
