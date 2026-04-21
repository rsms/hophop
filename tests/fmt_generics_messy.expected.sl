type Ptr[T] *T

struct Pair[A, B] {
	left  A
	right B
}

enum Tag[T] int {
	None = 0
	Some = 1
}

fn make_pair[T](x, y T) Pair[T, T] {
	return Pair[T, T]{ left: x, right: y }
}

fn same_type(x anytype) bool {
	return typeof(x) == type Pair[i32, i32]
}
