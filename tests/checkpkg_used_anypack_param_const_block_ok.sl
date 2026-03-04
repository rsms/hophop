fn verify(args ...anytype) {
	const {
		assert len(args) == 3
		assert typeof(args[0]) == int
		assert typeof(args[1]) == typeof("" as &str)
		assert typeof(args[2]) == bool
	}
}

fn main() {
	verify(1, "x", true)
}
