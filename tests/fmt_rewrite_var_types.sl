// Verifies formatter output for redundant var declaration types.
struct Foo {
	x int
}

struct Box[T] {
	value T
}

fn makeFoo() Foo {
	return Foo{ x: 456 }
}

fn main() {
	var compound Foo = Foo{ x: 123 }
	var generic Box[i64] = Box[i64]{ value: 4 }
	var fromCall Foo = makeFoo()
	var fromIdent Foo = compound
	var keepUntyped Foo = { x: 789 }
	var keepCast i32 = 3 as i32
}
