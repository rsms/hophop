// Verifies direct null as optional cast is accepted.
fn main() {
	var x ?*int = null as ?*int
	var y ?int  = null as ?int
	assert x == null
	assert y == null
}
