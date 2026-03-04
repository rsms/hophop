fn require_char(const s &str, n uint) {
	const {
		assert s[n] == 'b'
	}
}

fn main() {
	var n uint = 1
	require_char("abc", n)
}
