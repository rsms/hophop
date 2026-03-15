fn add1(x i32) i32 {
	return x + 1
}

const ADD1 = add1

fn run() i32 {
	const {
		assert ADD1(41) == 42
	}
	return 42
}

fn main() {
	assert run() == 42
}
