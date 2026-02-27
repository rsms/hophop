// defer semantics and execution order at scope exit
fn cleanup() {}

fn main() {
	defer cleanup()
}
