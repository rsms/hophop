// Verifies MIR runtime behavior for for in.
fn main() {
	var a [i32 4]
	a[0] = 1
	a[1] = 2
	a[2] = 3
	a[3] = 4

	var sum i32
	for item in a {
		sum += item
	}

	var weighted i32
	for i, item in a {
		weighted += item * i as i32
	}

	var count int
	for _ in a {
		count += 1
	}

	var text  &str = "ab"
	var chars u32
	for ch in text {
		chars += ch as u32
	}

	assert sum == 10
	assert weighted == 20
	assert count == 4
	assert chars == 'a' as u32 + 'b' as u32
}
