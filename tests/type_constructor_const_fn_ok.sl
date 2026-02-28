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

fn read_slice(sp I64SlicePtr) i64 {
	return (*sp)[0]
}

fn first(b U8x4) u8 {
	return b[0]
}
