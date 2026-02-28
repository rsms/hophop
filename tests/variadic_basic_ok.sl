fn sum(nums ...i32) i32 {
	var total i32 = 0
	var i     u32 = 0
	for i < len(nums) {
		total += nums[i]
		i += 1
	}
	return total
}

fn main() {
	assert sum() == 0
	assert sum(1, 2, 3, 4) == 10
}
