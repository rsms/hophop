// Supports type function import overload by providing the foo package fixture.
pub struct B {
	y int
}

pub fn pick_b(v B) int {
	return v.y
}
