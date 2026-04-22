// Verifies topconst const float unused is accepted.
fn bump(x f64) f64 {
	var y = x
	y += 0.5
	return y
}

const PI f64 = 3.14

const MIX f64 = 2 + 0.5

const BUMP f64 = bump(1.0)

fn main() {
	assert PI > 3.0
	assert MIX > 2.0
	assert BUMP > 1.0
}
