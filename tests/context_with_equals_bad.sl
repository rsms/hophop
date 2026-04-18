// Verifies context with equals is rejected.
fn f() context struct {
	x i32
} {}

fn main() {
	f() with { x = 1 }
}
