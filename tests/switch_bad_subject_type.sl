// Verifies switch bad subject type.
fn f(x i32) i32 {
	switch x {
		case true { return 0 }
		default   { return 1 }
	}
}
