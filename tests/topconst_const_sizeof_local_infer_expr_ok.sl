// Verifies topconst const sizeof local inference expression is accepted.
fn infer_int_size() i32 {
	var x = 1
	return sizeof(x) as i32
}

fn infer_float_size() i32 {
	const y = 1.0
	return sizeof(y) as i32
}

const INFER_INT_SIZE = infer_int_size()

const INFER_FLOAT_SIZE = infer_float_size()

fn main() {
	assert INFER_INT_SIZE == sizeof(1 as int) as i32
	assert INFER_FLOAT_SIZE == sizeof(1.0 as f64) as i32
}
