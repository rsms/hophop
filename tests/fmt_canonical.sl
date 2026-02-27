import "feature/optional"

import "math/basic" as math { Add as Sum, Sub }

type IntVec [int .n]

struct Pair {
	anne     int // comment
	beatrice int // comment
	cat      IntVec
	daniel   IntVec // different comment column since previous line has no comment
	erica    int    // comment

	fred int    // linebreak above starts a new "group" of fields
	gio  IntVec // comment
	// a line with just comments has the same effect as a blank line
	hans   int       // comment
	inger  int  = 4  // comment
	janice &int = 45 // comment
}

// comment about Foo
struct Foo {
	// leading comment about field x
	x int // trailing comment about field x
	// leading comment across multiple lines
	// about field y
	y int = 2
	// comment about field z
	z ?int // trailing comment about Foo
}

union Value {
	x, y, z int  // comment
	text    &str // comment
}

enum Mode int {
	Unknown
	A   = 1
	Bee = 3
}

struct StructWithAnonymousStructTypes {
	internet bool
	pos      struct {
		alice     i32 // comment
		bob       i32
		catherine struct {
			z1, z2 int
			z3     bool
		}
	}
	size struct { w, h i32 }
}

struct Example {
	pos struct {
		x i32
		y i32
	}
	size struct {
		w i32
		h i32
	}
	lol   int
	value union {
		i i32
		f f64
	}
}

fn metrics { value.size, value.capacity }

fn add(a int, b int) int {
	return a + b
}

fn use_context(x int) int context AppContext {
	var arr    [int 3]
	var maybe  ?int  = null            // comment
	var view   [int] = arr[0:len(arr)] // comment
	var second int   = arr[1]          // comment

	var sum int    = x
	var opt ?int   = maybe
	var precedence = 3 + 5*4*8 - 6

	defer {
		sum += 1
		sum += 1
	}
	defer sum += 1

	if x > 0 {
		sum += add(x, 1)
	} else if x < 0 {
		sum -= 1
	} else {
		sum += 0
	}
	for {
		break
	}
	for x < 10 {
		x += 1
		continue
	}
	for var i = 0; i < len(arr); i += 1 {
		sum += arr[i]
	}
	switch sum {
		case 0, 1 {
			sum += 10
			sum += 10
		}
		case 2          { sum += 20 }  // comment
		case 3, 30, 300 { sum += 300 } // comment
		case 4, 40      { sum += 40 }  // comment
		case 5          {
			sum += 5
			sum += 50
		}
		case 6, 60 { sum += 50 }

		case 7     { sum += 70 }
		case 8, 80 { sum += 80 }
		default    { sum += 900 }
	}
	switch {
		case sum > 0 { sum += 1 }
		default      { sum += 2 }
	}
	assert sum > 0, "sum must be positive", sum

	// when operators with higher precedence are mixed with operators
	// with lower precedence, the ones with higher precedence "pull in"
	// operands tightly. I.e.
	sum = sum + 3 - 1 // all operators have same precedence level
	sum = sum * 3 / 1 // all operators have same precedence level
	sum = sum*2 + x/3 - 1 // mixed precedence levels
	sum = (sum * 2) + ((x - 1) / 3)
	sum = sum*2 + ((x - 1) / 3)
	sum = sum*2 + (x - 1)/3
	sum = sum * 2 << (x - 1) / 3 // all operators have same precedence level

	sum = sizeof(int) + sizeof(sum)
	sum = sum + (new [int len(arr)] with context.alloc) as int
	_ = add(sum, 1) with context
	_ = add(sum, 1) with { alloc = context.alloc }
	_ = Foo{ x = 1, y = 2 }.x
	_ = (&arr[0])!
	return sum
}

fn declarations_only(a int) int

// Struct & union literals should have their "linebreak or comma" forms preserved
fn preserve_line_form() {
	var a Example = { pos = { x = 1, y = 2 }, size = { w = 3, h = 4 }, value = { i = 5 } }
	var b Example = {
		pos = { x = 1, y = 2 }
		size = { w = 3, h = 4 }
		value = { i = 5 }
	}
	var c Example = {
		pos = {
			x = 1
			y = 2
		}
		size = { w = 3, h = 4 }
		value = { i = 5 }
	}
	var d Example = {
		pos = {
			x = 1
			y = 2
		}
		size = { w = 3,
			h = 4 }
		value = { i = 5 }
	}
	var e Example = {
		pos = {
			x = 1
			y = 2
		}
		size = {
			w = 3
			h = 4
		}
		value = {
			i = 5
		}
	}
}
