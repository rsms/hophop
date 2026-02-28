// variadic functions: fixed + variadic parameters and forwarding with spread
fn sum(nums ...i32) i32 {
	var total i32 = 0
	for var i u32 = 0; i < len(nums); i += 1 {
		total += nums[i]
	}
	return total
}

fn weighted_sum(weight i32, nums ...i32) i32 {
	return weight * sum(nums...)
}

fn main() {
	// zero variadic args
	assert sum() == 0

	// explicit variadic tail args
	assert sum(1, 2, 3, 4) == 10

	// fixed parameter + variadic tail
	assert weighted_sum(2) == 0
	assert weighted_sum(2, 1, 2, 3) == 12

	// named fixed arg with variadic tail
	assert weighted_sum(weight: 3, 4, 5) == 27
}
