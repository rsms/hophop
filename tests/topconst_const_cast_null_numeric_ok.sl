// Verifies topconst const cast null numeric is accepted.
const I int = null as int

const F f64 = null as f64

fn main() {
	assert I == 0
	assert F == 0.0
}
