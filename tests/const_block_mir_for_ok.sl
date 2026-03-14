fn main() {
	const {
		var i   int = 0
		var sum int = 0
		for i = 0; i < 4; i += 1 {
			sum += i
		}
		assert sum == 6
	}
}
