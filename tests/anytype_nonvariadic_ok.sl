fn accept(x anytype) {
	_ = x
}

fn main() {
	accept(1)
	accept(true)
	accept("hello")
}
