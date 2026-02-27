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
