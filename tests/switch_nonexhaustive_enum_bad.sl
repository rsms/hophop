// Verifies switch nonexhaustive enum is rejected.
enum E u8 {
	A
	B
}

fn f(e E) i32 {
	switch e {
		case E.A { return 1 }
	}
	return 0
}
