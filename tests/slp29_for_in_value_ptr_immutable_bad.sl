fn main() {
	var a  [i32 2]
	var ro &[i32] = a[:]
	for *v in ro {
		*v += 1
	}
}
