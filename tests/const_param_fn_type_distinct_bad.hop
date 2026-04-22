// Verifies const parameter function type distinct is rejected.
fn normal(x i32) i32 {
	return x
}

fn takes_const(f fn(const i32) i32) {}

fn main() {
	takes_const(normal)
}
