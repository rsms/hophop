fn bad(args ...anytype) {
	_ = args[1]
}

fn main() {
	bad(1)
}
