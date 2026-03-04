fn require_third(const s &str) {
	const {
		assert s[3] == 0
	}
}

fn main() {
	require_third("abc")
}
