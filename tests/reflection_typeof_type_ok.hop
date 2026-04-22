// Verifies reflection typeof type is accepted.
type MyInt i32

fn is_i32(T type) bool {
	return T == i32
}

fn main() {
	var x i32
	const tx type = typeof(x)
	const tt type = typeof(i32)
	const tm type = typeof(MyInt)

	assert tx == i32
	assert tt == type
	assert tm == type

	assert is_i32(i32)
	assert !is_i32(u32)

	assert typeof(u16) == type
	assert typeof(u16) != u16
}
