// Verifies the Wasm backend accepts loop uncond continue.
fn main() i32 {
	var i i32 = 0
	for {
		i = i + 1
		if i < 3 {
			continue
		}
		break
	}
	return i
}
