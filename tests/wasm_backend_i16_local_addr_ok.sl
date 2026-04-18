// Verifies the Wasm backend accepts i16 local addr.
fn main() {
	var x i16  = 7
	var p *i16 = &x

	assert *p == 7 as i16
	*p = 9 as i16
	assert x == 9 as i16
}
