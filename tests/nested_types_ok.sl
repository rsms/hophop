pub struct Collection {
	union Value {
		i i64
		f f64
	}
	struct Item {
		parent *Collection
		value  Value
	}
	enum Tag i32 {
		Head = 0
		Tail = 1
	}
}

fn is_head(v Collection.Tag) bool {
	return v == Collection.Tag.Head
}

fn main() {
	var c  = Collection{}
	var v  = Collection.Value{ i: 123 }
	var it = Collection.Item{ parent: &c, value: v }
	assert it.value.i == 123
	assert is_head(Collection.Tag.Head)
}
