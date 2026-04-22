struct Vector[T] {
	x T
}

fn is_vector_i32(t type) bool {
	return t == type Vector[i32]
}

fn main() {
	var v = Vector[i32]{ x: 1 }
	assert is_vector_i32(typeof(v))
}
