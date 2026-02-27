import "feature/optional"

import "math/basic" as math { Add as Sum, Sub }

pub type IntVec [int .n]

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
pub struct Foo {
	// leading comment about field x
	x int // trailing comment about field x
	// leading comment across multiple lines
	// about field y
	y int = 2
	// comment about field z
	z ?int // trailing comment about Foo
}

pub union Value {
	x, y, z int  // comment
	text    &str // comment
}

pub enum Mode int {
	Unknown
	A   = 1
	Bee = 3
}

fn metrics { value.size, value.capacity }

pub fn add(a int, b int) int {
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
	sum = (sum * 2) + ((x - 1) / 3)
	sum = sizeof(int) + sizeof(sum)
	sum = sum + (new [int len(arr)] with context.alloc) as int
	_ = add(sum, 1) with context
	_ = add(sum, 1) with { alloc = context.alloc }
	_ = Foo{ x = 1, y = 2 }.x
	_ = (&arr[0])!
	return sum
}

fn declarations_only(a int) int
