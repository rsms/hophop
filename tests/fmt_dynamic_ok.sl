struct Data {
	x int
	y &str
}

fn pick(flag bool) &str {
	if flag {
		return "value={i}, rec={r}"
	}
	return "value={r}"
}

fn main() {
	var out *str = fmt(pick(true), 7 as i64, Data{ x: 9, y: "z" })
	assert out == "value=7, rec={ x: 9, y: \"z\" }"
	free(out)
}
