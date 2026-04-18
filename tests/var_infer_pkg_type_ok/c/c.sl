// Supports variable inference package type by providing the c package fixture.
import "b"

fn example() int {
	var result = b.make()
	return result.value
}
