// Verifies array type const function call switch expression is accepted.
fn kind(x i32) i32 {
	switch x {
		case 0    { return 1 }
		case 1, 2 { return 2 }
		default   { return 3 }
	}
}

fn main() {
	var a [i32 kind(2)]
	assert a[0] == 0
}
