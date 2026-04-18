// Verifies type function selector autoref is accepted.
struct S {
	x int
}

fn set_ptr(s *S, v int) {
	s.x = v
}

fn read_val(s S) int {
	return s.x
}

fn choose(s S) int {
	return 1
}

fn choose(s *S) int {
	return 2
}

fn main() {
	var s S
	s.set_ptr(7)
	assert s.read_val() == 7
	assert s.choose() == 1
}
