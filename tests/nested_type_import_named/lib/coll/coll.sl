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
		One = 1
		Two = 2
	}
}
