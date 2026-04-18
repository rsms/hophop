// Verifies parameter runs mixed is accepted.
fn example3(a, b int, c uint) int {
	return a + b + c as int
}

fn main() i32 {
	return example3(1, b: 2, c: 3) as i32
}
