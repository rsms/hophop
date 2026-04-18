// Verifies array type const function call local const is accepted.
fn extent(x i32) i32 {
	var base i32 = x + 1
	var span     = base * 2
	return span + 3
}

const N = extent(4)

fn main() {
	var a [i32 N]
	assert N == 13
	assert a[0] == 0
}
