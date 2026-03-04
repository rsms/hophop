fn validate(args ...anytype) {
	const {
		var i uint = 0
		for i < len(args) {
			const t type = typeof(args[i])
			assert t == t
			i += 1
		}
	}
}

fn main() {
	validate(7, true, "ok")
}
