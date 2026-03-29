struct Handler {
	handle fn(i32) i32
}

fn add1(x i32) i32 {
	return x + 1
}

fn main() i32 {
	var h Handler
	h.handle = add1
	return h.handle(41)
}
