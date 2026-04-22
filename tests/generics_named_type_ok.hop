struct Vector[T] {
	x, y T
}

fn add[T](a, b Vector[T]) Vector[T] {
	return { x: a.x + b.x, y: a.y + b.y }
}

fn main() {
	var v1 = Vector[i64]{ x: -1 as i64, y: 4 }
	var v2 = Vector[i64]{ x: 5, y: -2 }
	var v3 = add(v1, b: v2)
	assert typeof(v3) == type Vector[i64]
	assert v3.x == 4
}
