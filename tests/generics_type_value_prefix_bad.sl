struct Vector[T] {
	x T
}

fn main() {
	var v Vector[i32] = Vector[i32]{ x: 1 }
	if typeof(v) == Vector[i32] {}
}
