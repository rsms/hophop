// Verifies formatter output for rewrite literal casts.
var x u32 = 3

var xf f64 = 3.5

fn f() u32 {
	return 3
}

fn ff() f64 {
	return 3.5
}

fn generic_return[T]() T {
	return 5
}

fn g(v i16) {
	var y i16 = 4 * 9
	_ = v + 4
	var z      u32 = 3
	var assign i16 = 7
	assign = 8
	var p *i16 = &assign
	*p = 9
}

fn takes_pair(a i32, b f64) {}

fn named(a u64, b i32, c f64) {}

fn variadic(nums ...i32) {}

fn variadic_pair(prefix i32, nums ...i32) {}

fn h() {
	takes_pair(1, b: 2.5)
	named(1, b: 2, c: 3.5)
	variadic(1)
	variadic_pair(0, 1)
}
