// named arguments: explicit labels, identifier shorthand, reordering,
// leading-underscore positional prefixes, receiver sugar, and variadic tails
struct Vec2 {
	x i32
	y i32
}

fn digits(a, b, c i32) i32 {
	return a*100 + b*10 + c
}

fn color(_r, _g, _b i32) i32 {
	return _r*100 + _g*10 + _b
}

fn packet(version, _flags, payload_len, checksum i32) i32 {
	return version*1000 + _flags*100 + payload_len*10 + checksum
}

fn translate_score(v Vec2, dx, dy i32) i32 {
	return v.x*1000 + v.y*100 + dx*10 + dy
}

fn weighted_sum(weight i32, nums ...i32) i32 {
	var total i32 = 0
	for var i int = 0; i < len(nums); i += 1 {
		total += nums[i]
	}
	return weight * total
}

const CONST_PACKET = packet(1, 2, checksum: 4, payload_len: 3)

fn main() {
	// Explicit labels may name every fixed argument.
	assert digits(a: 1, b: 2, c: 3) == 123

	// After the first positional argument, named arguments may reorder
	// the remaining fixed parameters.
	assert digits(1, c: 3, b: 2) == 123

	// Identifier arguments after the first argument can act as shorthand
	// for their matching parameter names.
	var b i32 = 2
	var c i32 = 3
	assert digits(1, c, b) == 123

	// Leading-underscore parameters extend the positional prefix.
	assert color(1, 3, _b: 2) == 132
	assert color(_r: 1, _g: 3, _b: 2) == 132

	// Once a non-underscore parameter is reached, the remaining fixed
	// arguments are named, but can still be reordered.
	assert packet(1, 2, checksum: 4, payload_len: 3) == 1234
	assert CONST_PACKET == 1234

	// Receiver-call sugar supplies the first argument; named arguments
	// still apply to the explicit suffix.
	var v = Vec2{ x: 7, y: 8 }
	assert v.translate_score(dx: 3, dy: 4) == 7834

	// For variadic calls, named arguments apply only to fixed parameters;
	// the variadic tail remains positional.
	assert weighted_sum(weight: 3, 4, 5) == 27
}
