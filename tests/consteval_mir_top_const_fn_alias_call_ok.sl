// Verifies const-eval MIR top-level const function alias call is accepted.
fn add1(x i32) i32 {
	return x + 1
}

const ADD1 = add1

fn run() i32 {
	return ADD1(41)
}

const VALUE = run()

fn main() {
	assert VALUE == 42
}
