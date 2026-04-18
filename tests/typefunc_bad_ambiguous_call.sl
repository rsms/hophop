// Verifies type function bad ambiguous call.
fn f(v i32) {}

fn f(v i64) {}

fn main() {
	f(1)
}
