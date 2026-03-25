const base = 41

fn helper() i32 {
	return base + 1
}

fn main() {
	assert (helper() == 42)
}
