// comparison operators across built-in and aggregate types,
// including custom comparison hooks (__equal and __order).
struct Vec3 {
	x f32
	y f32
	z f32
}

union Token {
	i int
	f f32
}

enum Priority i32 {
	Low    = 0
	Medium = 1
	High   = 2
}

struct Disc {
	x      f32
	y      f32
	radius f32
}

fn __equal(a &Disc, b &Disc) bool {
	// Equality is based on center position only.
	return a.x == b.x && a.y == b.y
}

fn __order(a &Disc, b &Disc) int {
	// Ordering is radius first, then position.
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

fn scalar_examples() {
	var t bool = 1 == 1
	var f bool = 1 == 0
	assert t == (2 > 1)
	assert t != f

	var a int = 7
	var b int = 9
	assert a < b
	assert a <= a
	assert b > a
	assert b >= b

	var x f64 = 1.5
	var y f64 = 2.0
	assert x < y
	assert y >= x
}

fn string_examples() {
	var s1 &str = "abc"
	var s2 &str = "abd"
	var s3 &str = "abc"

	assert s1 == s3
	assert s1 != s2
	assert s1 < s2
}

fn pointer_examples() {
	var a  int  = 7
	var b  int  = 7
	var pa &int = &a
	var pb &int = &b

	// Pointer equality compares addresses.
	assert pa == pa
	assert pa != pb

	// Ordered pointer comparisons are address-based.
	assert pa <= pa
	assert pa >= pa
	assert !(pa < pa)
	assert !(pa > pa)

	// Dereference to compare pointed-to values.
	assert *pa == *pb
}

fn array_and_slice_examples() {
	var ax [int 3]
	var ay [int 3]
	var az [int 3]

	ax[0] = 1
	ax[1] = 2
	ax[2] = 3

	ay[0] = 1
	ay[1] = 2
	ay[2] = 3

	az[0] = 1
	az[1] = 2
	az[2] = 4

	// Array values compare element-wise.
	assert ax == ay
	assert ax != az

	var sx &[int] = ax[:]
	var sy &[int] = ay[:]
	var sz &[int] = sx

	// Slice refs compare by address (identity).
	assert sx == sz
	assert sx != sy

	// Dereference to compare slice values (len + element bytes).
	assert *sx == *sy
}

fn enum_struct_union_examples() {
	assert Priority.Low < Priority.Medium
	assert Priority.High > Priority.Low

	var v1 = Vec3{ x: 1.0, y: 2.0, z: 3.0 }
	var v2 = Vec3{ x: 1.0, y: 2.0, z: 3.0 }
	var v3 = Vec3{ x: 1.0, y: 2.0, z: 4.0 }

	// Struct values compare by fields.
	assert v1 == v2
	assert v1 != v3

	var t1 Token
	var t2 Token
	var t3 Token
	t1.i = 42
	t2.i = 42
	t3.i = 7

	// Union values compare by underlying bytes.
	assert t1 == t2
	assert t1 != t3
}

fn custom_hook_examples() {
	var a = Disc{ x: 0.0, y: 0.0, radius: 3.0 }
	var b = Disc{ x: 0.0, y: 0.0, radius: 1.0 }
	var c = Disc{ x: 0.0, y: 0.0, radius: 3.0 }

	// Uses __equal: same center means equal.
	assert a == b
	assert !(a != b)

	// Uses __order: radius then position.
	assert b < a
	assert a <= c
	assert a >= c
	assert !(a < c)
	assert !(a > c)
}

fn main() {
	scalar_examples()
	string_examples()
	pointer_examples()
	array_and_slice_examples()
	enum_struct_union_examples()
	custom_hook_examples()
}
