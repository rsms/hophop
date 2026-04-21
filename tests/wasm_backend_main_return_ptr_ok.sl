// Verifies the Wasm backend accepts main return pointer.
fn main() *i32 {
	var ma     = context.mem
	var p *i32 = new i32 context ma
	*p = 7
	assert *p == 7
	return p
}
