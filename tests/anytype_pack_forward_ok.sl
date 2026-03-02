fn sink(args ...anytype) u32 {
	return len(args)
}

fn forward(args ...anytype) u32 {
	return sink(args...)
}

fn main() {
	assert forward(1, true, "x") == 3
}
