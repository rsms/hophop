import "feature/optional"
import "math/basic" as math { Add as Sum, Sub }

struct Foo {
	x int // trailing comment about field x
	// leading comment about field y
	y int = 2
}

fn add(a int, b int) int {
	for var i int = 0; i < 10; i += 1 {
		if i % 2 == 0 {
			continue
		} else {
			break
		}
	}
	return a + b
}

fn shorthand_rewrite(y int) context Ctx {
	_ = add(1, y)
	_ = add(1, y: (y))
	_ = Foo{ x: 1, y }
	_ = Foo{ x: 1, y: (y) }
	_ = add(1, 2) with { mem }
	_ = add(1, 2) with { mem: (context).mem }
	_ = y < 16 as i64
}
