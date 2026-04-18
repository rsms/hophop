// Supports type function import named overload cross package by providing the a package fixture.
pub fn foo(x int) int {
	return x + 1
}

pub fn foo(x, y int) int {
	return x + y
}
