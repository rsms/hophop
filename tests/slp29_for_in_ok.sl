// Verifies SLP 29 for in is accepted.
fn sum(items &[i32]) i32 {
	var acc i32
	for item in items {
		acc += item
	}
	return acc
}

fn weighted(items &[i32]) i32 {
	var acc i32
	for i, item in items {
		acc += item * i as i32
	}
	return acc
}

fn count(items &[i32]) int {
	var n int
	for i, _ in items {
		n = i + 1
	}
	return n
}
