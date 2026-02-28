fn sum(nums ...i32) i32 {
	return 0
}

fn takes_slice_fn(f fn([i32]) i32) i32 {
	return 0
}

fn main() {
	takes_slice_fn(sum)
}
