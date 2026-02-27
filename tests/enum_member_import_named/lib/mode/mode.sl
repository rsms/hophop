pub enum Mode i32 {
	A = 0
	B = 1
}

pub fn is_mode_a(m Mode) bool {
	return m == Mode.A
}
