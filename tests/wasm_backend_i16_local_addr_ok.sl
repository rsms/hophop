// Verifies the Wasm backend accepts i16 local addr.
fn main() {
	var x i16  = 7
	var p *i16 = &x

	assert *p == 7
	*p = 9
	assert x == 9
}
