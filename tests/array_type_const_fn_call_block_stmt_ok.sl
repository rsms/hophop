// Verifies array type const function call block stmt is accepted.
fn pick(x i32) i32 {
	{
		var y = x + 1
		if y > 4 {
			return y
		}
	}
	return x
}

const N = pick(5)

fn main() {
	var arr [i32 N]
	assert N == 6
	assert arr[0] == 0
}
