fn main() {
	var x struct { a int; b bool }     = { a: 123, b: true }
	var y union { i int; f f64 }       = { i: x.a }
	var p struct { x, y int; ok bool } = { x: 1, y: 2, ok: true }
	assert y.i == 123
	assert p.ok
}
