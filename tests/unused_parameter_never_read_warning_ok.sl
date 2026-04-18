// Verifies unused parameter never read warning is accepted.
fn id_write_only(x int) int {
	x = 2
	return 0
}

fn main() {
	id_write_only(1)
}
