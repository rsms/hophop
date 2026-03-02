fn sum(const xs ...i32) i32 {
	return 0
}

fn forward(nums ...i32) i32 {
	return sum(nums...)
}

fn main() {
	_ = forward(1, 2)
}
