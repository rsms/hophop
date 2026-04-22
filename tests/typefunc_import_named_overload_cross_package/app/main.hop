// Supports type function import named overload cross package by providing the app entrypoint.
import "a" { foo }
import "b" { foo }

fn main() {
	var xi int = 2
	var yi int = 3
	var xf f32 = 1.5
	var yf f32 = 3.0

	assert foo(xi) == 3
	assert foo(xi, y: yi) == 5
	assert foo(xf) == 3.0
	assert foo(xf, y: yf) == 4.5
}
