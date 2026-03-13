fn main() {
	var s *str = concat("a", "b")
	assert len(s) == 2
	free(s)
}
