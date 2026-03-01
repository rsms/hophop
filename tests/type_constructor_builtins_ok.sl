const I64Ptr = ptr(i64)

const I64SlicePtr = ptr(slice(i64))

const U8x4 = array(u8, N: 4 as uint)

fn read_ptr(p I64Ptr) i64 {
	return *p
}

fn main() {
	var x i64    = 7
	var p I64Ptr = &x
	assert read_ptr(p) == 7 as i64

	var data [i64 2]
	data[0] = 1 as i64
	data[1] = 2 as i64
	var sp I64SlicePtr = data[0:2]
	assert (*sp)[1] == 2 as i64

	var bytes U8x4
	bytes[0] = 9 as u8
	assert bytes[0] == 9 as u8
}
