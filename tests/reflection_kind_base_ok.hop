// Verifies reflection kind base is accepted.
type MyInt i32

struct Pair {
	a, b i32
}

const K_U32 = u32.kind()

fn kind_passthrough(T type) u8 {
	return kind(T) as u8
}

fn main() {
	assert i32.kind() == u32.kind()
	assert MyInt.kind() != i32.kind()
	assert kind(MyInt) == MyInt.kind()
	assert kind_passthrough(MyInt) == MyInt.kind() as u8

	assert MyInt.base() == i32
	assert base(MyInt) == i32

	assert Pair.kind() != i32.kind()
}
