import "lib/mode" { Mode, is_mode_a }

fn main() {
	var m Mode = Mode.A
	assert is_mode_a(m)
}
