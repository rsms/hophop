// compound literals, introduced in SLP-13
// - explicit typed literal: Type{ ... }
// - inferred literal: { ... } from expected type (init/assign/call/field)
// - nested literals
// - omitted fields (zero fill)
// - readonly ref binding to temporary
// - address-of typed literal
// - union single-field initialization
struct Vec2 {
	x i32
	y i32
}

struct Rect {
	pos  Vec2
	size Vec2
}

union Number {
	i i32
	f f32
}

fn sum(v Vec2) i32 {
	return v.x + v.y
}

fn y_of(v &Vec2) i32 {
	return v.y
}

fn main() {
	// explicit target type
	var a = Vec2{ x: 1, y: 2 }

	// inferred target type from declared type and assignment target
	var b Vec2 = { x: 3, y: 4 }
	b = { x: 5, y: 6 }

	// inferred target type at call sites
	var c i32 = sum({ x: 7, y: 8 })
	var d i32 = y_of({ x: 9, y: 10 })
	var e i32 = y_of(Vec2{ x: 11, y: 12 })

	// address-of typed literal
	var p &Vec2 = &Vec2{ x: 13, y: 14 }

	// nested inference and omitted fields (zero fill)
	var r      = Rect{ pos: { x: 21, y: 22 }, size: { x: 30 } }
	var z Vec2 = {}

	// union single-field init
	var n = Number{ i: 42 }

	assert a.x == 1
	assert a.y == 2
	assert b.x == 5
	assert b.y == 6
	assert c == 15
	assert d == 10
	assert e == 12
	assert p.x == 13
	assert p.y == 14
	assert r.pos.x == 21
	assert r.pos.y == 22
	assert r.size.x == 30
	assert r.size.y == 0
	assert z.x == 0
	assert z.y == 0
	assert n.i == 42
}
