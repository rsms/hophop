// Verifies embedded struct field defaults cannot reference later outer fields.
struct Base {
	x i32
}

struct Bad {
	Base = { x: y }
	y i32 = 1
}
