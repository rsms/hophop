// Verifies anytype pack forward is accepted.
fn sink(args ...anytype) int {
	return len(args)
}

fn forward(args ...anytype) int {
	return sink(args...)
}

fn main() {
	assert forward(1, true, "x") == 3
}
