fn validate_scan(const s &str) {
	const {
		var i uint = 0
		for i < len(s) {
			if i + 1 < len(s) {
				assert s[i + 1] != 0
			}
			i += 1
		}
		assert s[0 + 2] == 'c'
	}
}

fn main() {
	validate_scan("abc")
}
