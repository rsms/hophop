fn take(args ...anytype) {}

fn main() {
	var a  [i32 2]
	var xs &[i32] = a[:]
	take(xs...)
}
