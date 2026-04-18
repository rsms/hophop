// Verifies topconst const sizeof local expression is accepted.
fn local_size() i32 {
	var x i32 = 1
	return sizeof(x) as i32
}

const LOCAL_SIZE = local_size()

var STORE [u8 local_size()]

fn main() {
	assert LOCAL_SIZE == 4
	assert len(STORE) == 4
}
