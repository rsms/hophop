// Verifies formatter output for rewrite literal casts.
var x=3 as u32
var xf=3.5 as f64

fn f()u32{
return 3 as u32
}

fn ff()f64{
return 3.5 as f64
}

fn g(v i16){
var y i16=4 as i16*9 as i16
_ = v+4 as i16
var z = 3 as u32
}

fn takes_pair(a i32, b f64) {
}

fn named(a u64, b i32, c f64) {
}

fn variadic(nums ...i32) {
}

fn variadic_pair(prefix i32, nums ...i32) {
}

fn h() {
	takes_pair(1 as i32, b: 2.5 as f64)
	named(1 as u64, b: 2 as i32, c: 3.5 as f64)
	variadic(1 as i32)
	variadic_pair(0 as i32, 1 as i32)
}
