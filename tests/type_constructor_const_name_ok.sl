fn id_type(T type) type {
	return T
}

const IntType = id_type(i64)

fn add(a, b IntType) IntType {
	return a + b
}

fn main() {
	var x IntType = 40 as i64
	var y IntType = 2 as i64
	assert add(x, b: y) == 42 as i64
	assert type_name(typeof(x)) == "i64"
}
