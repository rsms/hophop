fn must_be_positive(const n int) {
	const {
		assert n > 0
	}
}

fn main() {
	must_be_positive(0)
}
