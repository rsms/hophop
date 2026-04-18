// Verifies switch nonexhaustive enum multi is rejected.
enum Shape u8 {
	Circle
	Rectangle
	Octagon
	Triangle
}

fn area(shape Shape) i32 {
	switch shape {
		case Shape.Circle { return 1 }
	}
	return 0
}
