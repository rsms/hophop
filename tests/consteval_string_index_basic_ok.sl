fn validate_prefix(const s &str) {
	const {
		assert s[0] == 'x'
		assert s[1] == 'y'
	}
}

fn main() {
	validate_prefix("xyz")
}
