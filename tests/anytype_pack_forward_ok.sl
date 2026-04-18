// Verifies anytype pack forward is accepted.
fn sink(args ...anytype) uint {
	return len(args)
}

fn forward(args ...anytype) uint {
	return sink(args...)
}

fn main() {
	assert forward(1, true, "x") == 3
}
