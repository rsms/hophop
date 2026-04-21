// Verifies runtime behavior for type constructor const function.
fn make_ptr(T type) type {
	return ptr(T)
}

fn make_slice_ptr(T type) type {
	return ptr(slice(T))
}

fn make_array(T type, N uint) type {
	return array(T, N)
}

const I64Ptr = make_ptr(i64)

const I64SlicePtr = make_slice_ptr(i64)

const N = 4 as uint

const U8x4 = make_array(u8, N)

fn read_ptr(p I64Ptr) i64 {
	return *p
}

fn main() {
	var x i64    = 7
	var p I64Ptr = &x
	assert read_ptr(p) == 7

	var data [i64 2]
	data[0] = 1 as i64
	data[1] = 2 as i64
	var sp I64SlicePtr = data[0:2]
	assert (*sp)[1] == 2 as i64

	var bytes U8x4
	bytes[0] = 9 as u8
	assert bytes[0] == 9 as u8
}
