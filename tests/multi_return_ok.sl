// Verifies multi return is accepted.
fn time_of_day() (uint, uint, uint) {
	return 14, 25, 30
}

fn named_single_value() uint {
	return 2026
}

fn main() {
	var hms (uint, uint, uint) = time_of_day()
	var h, m, second = time_of_day()
	var y             = named_single_value()
	var t (i32, bool) = (123, true)

	assert h == 14 && m == 25 && second == 30
	assert y == 2026
	_ = hms
	_ = t
}
