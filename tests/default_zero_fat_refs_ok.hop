// Verifies read-only fat references have default zero initialization.
var top_s &str

var top_xs &[int]

struct Foo {
	s  &str
	xs &[int]
}

fn main() {
	var s  &str
	var xs &[int]
	var foo = Foo{}

	assert len(s) == 0
	assert len(xs) == 0
	assert len(top_s) == 0
	assert len(top_xs) == 0
	assert len(foo.s) == 0
	assert len(foo.xs) == 0
}
