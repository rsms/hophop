// Verifies variable inference void is rejected.
fn noop() {}

fn example() {
	var x = noop()
	_ = x
}
