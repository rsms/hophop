fn main() {
	var a [i32 2]
	a[0] = 1
	a[1] = 2
for  i,  &v  in  &a {
	assert *v > i as i32
}
}
