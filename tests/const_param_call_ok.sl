fn parse_base(const base i32, x i32) i32 {
	return base + x
}

fn main() {
	assert parse_base(1 + 2, x: 4) == 7
	assert parse_base(5, x: 0) == 5
}
