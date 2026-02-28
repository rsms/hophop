struct Data {
	x int
	y &str
}

fn main() {
	var out *str = fmt("count: {i}, data: {r}", 123 as i64, Data{ x: 456, y: "hello" })
	assert out == "count: 123, data: { x: 456, y: \"hello\" }"
	free(out)
}
