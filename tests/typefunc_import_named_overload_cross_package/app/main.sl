import "a" { foo }
import "b" { foo }

fn main() {
	var xi int = 2
	var yi int = 3
	var xf f32 = 1.5
	var yf f32 = 3.0

	assert foo(xi) == 3
	assert foo(xi, yi) == 5
	assert foo(xf) == 3.0
	assert foo(xf, yf) == 4.5
}
