// Verifies const parameter call nonconst is rejected.
fn parse_base(const base i32) i32 {
	return base
}

fn main() {
	var n i32 = 3
	_ = parse_base(n)
}
