struct Vec3 {
	x f32
	y f32
	z f32
}

fn resize(oldSize uint, newSize uint, align uint, size uint) uint {
	return oldSize + newSize + align + size
}

fn rotate(v *Vec3, x f32, y f32, z f32) {
	v.x += x
	v.y += y
	v.z += z
}

fn main() i32 {
	var v Vec3
	rotate(&v, 1.0, 2.0, 3.0)
	return resize(1, 2, 3, 4) as i32
}
