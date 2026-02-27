struct Disc {
	x      f32
	y      f32
	radius f32
}

fn __equal(a &Disc, b &Disc) bool {
	return a.x == b.x && a.y == b.y
}

fn __order(a &Disc, b &Disc) int {
	if a.radius < b.radius {
		return -1
	}
	if b.radius < a.radius {
		return 1
	}
	if a.x < b.x {
		return -1
	}
	if b.x < a.x {
		return 1
	}
	if a.y < b.y {
		return -1
	}
	if b.y < a.y {
		return 1
	}
	return 0
}

fn main() {
	var a = Disc{ x: 10.0, y: 10.0, radius: 12.0 }
	var b = Disc{ x: 10.0, y: 10.0, radius: 11.0 }
	var c = Disc{ x: 10.0, y: 10.0, radius: 15.0 }
	var d = Disc{ x: 10.0, y: 10.0, radius: 12.0 }

	assert (a == b)
	assert (!(a != b))
	assert (b < a)
	assert (a < c)
	assert (a <= d && a >= d)
	assert (!(a != d))
}
