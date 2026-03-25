fn maybe_one(flag bool) ?int {
	if flag {
		return 1
	}
	return null
}

const VALUE = maybe_one(true)

fn main() {
	assert VALUE == 1
}
