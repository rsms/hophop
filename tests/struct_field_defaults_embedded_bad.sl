// Verifies embedded struct field defaults still require assignable values.
struct Base {
	x i32
}

struct Bad {
	Base = 1
	y i32
}
