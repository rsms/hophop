// Verifies unused function warning is accepted.
fn helper_used() {}

fn helper_unused() {}

pub fn api() {}

fn main() {
	helper_used()
}
