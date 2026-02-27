fn example3(a int, b int, c uint) int {
	return a + b + (c as int)
}

fn main() i32 {
	return example3(1, 2, 3) as i32
}
