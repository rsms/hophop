fn main() {
	var i        = 123
	var p *int   = &i
	var x rawptr = p
	_ = x
}
