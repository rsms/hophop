fn sum(nums ...i32) i32 {
	var total i32 = 0
	for var i uint = 0; i < len(nums); i += 1 {
		total += nums[i]
	}
	return total
}

fn forward(nums ...i32) i32 {
	return sum(nums...)
}

fn main() {
	assert forward(2, 4, 6) == 12
}
