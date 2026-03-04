fn demo(const format &str, args ...anytype) {
	const {
		for var i uint = 0; i < len(format); i += 1 {
			if format[i] == 'x' {
				const t type = typeof(args[i])
				assert t == t
			}
		}
	}
}

fn main() {
	demo("xxxx", 1, "2", true)
}
