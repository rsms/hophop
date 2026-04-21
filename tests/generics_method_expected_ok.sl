struct Factory {}

struct Vector[T] {
	x, y T
}

fn make[T](self Factory) T {
	if T == i64 {
		return 0
	} else {
		return 1
	}
}

fn add[T](self, other Vector[T]) Vector[T] {
	return { x: self.x + other.x, y: self.y + other.y }
}

fn main() {
	var f Factory = {}
	var x i64     = f.make()
	var v1        = Vector[i64]{ x: 1, y: 2 }
	var v2        = Vector[i64]{ x: 3, y: 4 }
	var v3        = v1.add(other: v2)
	assert x == 0
	assert typeof(v3) == type Vector[i64]
	assert v3.y == 6
}
