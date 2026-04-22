// Verifies runtime behavior for comparison.
struct Vec3 {
	x f32
	y f32
	z f32
}

fn main() {
	var s = "sam"
	assert (s == "sam")

	var v1 = Vec3{ x: 1.0, y: 2.0, z: 3.0 }
	var v2 = Vec3{ x: 1.0, y: 2.0, z: 3.0 }
	var v3 = Vec3{ x: 1.0, y: 2.0, z: 4.0 }
	assert (v1 == v2)
	assert (!(v1 == v3))

	var a [i32 2]
	var b [i32 2]
	a[0] = 1
	a[1] = 2
	b[0] = 1
	b[1] = 2
	var x &[i32] = a[:]
	var y &[i32] = b[:]
	assert (!(x == y))
	assert (*x == *y)
}
