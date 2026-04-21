// Verifies context overlay with equals is rejected.
fn f() context struct {
	x i32
} {}

fn main() {
	f() context { x = 1 }
}
