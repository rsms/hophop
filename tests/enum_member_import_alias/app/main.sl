// Supports enum member import alias by providing the app entrypoint.
import "lib/mode" as mode

fn main() {
	var m mode.Mode = mode.Mode.A
	assert m == mode.Mode.A
	assert mode.is_mode_a(m)
}
