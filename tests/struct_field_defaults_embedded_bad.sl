// Verifies struct field defaults embedded is rejected.
struct Base {
	x i32
}

struct Bad {
	Base = Base{ x: 1 }
	y i32
}
