struct Example {
	pos struct {
		x int
		y int
	}
	dims struct {
		w int
		h int
	}
	value union {
		i int
		f f64
	}
}

fn f_struct(pos struct {
	x int
	y int
}) {}

fn f_struct2(pos struct {
	x int
	y int
}) {}

fn f_union(value union {
	i int
	f f64
}) {}

fn main() {
	var ex Example = { pos = { x = 1, y = 2 }, dims = { w = 3, h = 4 }, value = { i = 5 } }
	f_struct(ex.pos)
	f_struct2(ex.pos)
	f_union(ex.value)
}
