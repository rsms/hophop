// Verifies tagged enum zero initialization no zero tag is rejected.
enum E u8 {
	A = 1
	B = 2
}

fn main() {
	var e E
	_ = e
}
