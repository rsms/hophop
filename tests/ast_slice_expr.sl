fn main() i32 {
	var a [i32 4]
	var x i32    = a[1]
	var s &[i32] = a[:3]
	var t &[i32] = a[1:]
	var u &[i32] = a[:]
	return x + len(s) as i32 + len(t) as i32 + len(u) as i32
}
