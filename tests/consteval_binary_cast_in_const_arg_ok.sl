fn pass_through(const value bool) bool {
	const {
		if value {}
	}
	return value
}

const RESULT bool = eval_result()

fn eval_result() bool {
	const x i64 = 64
	return pass_through(x < 16 as i64)
}

fn main() {
	assert !RESULT
}
